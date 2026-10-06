#include "bytecode/emitter.h"
#include "frontend/binder/binder.h"
#include "frontend/hir/hir.h"
#include "frontend/lexer/lexer.h"
#include "frontend/parser/parser.h"
#include "optimizer/mir.h"
#include "optimizer/native.h"
#include "runtime/native_bridge.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

struct CompiledArtifacts {
  sputnik::bytecode::BcModule bytecode_module;
  sputnik::mir::Module mir_module;
  sputnik::native::NativeModule native_module;
};

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "native test failed: " << message << "\n";
    std::exit(1);
  }
}

CompiledArtifacts compile_ok(const std::string &source) {
  sputnik::lexer::Lexer lexer(source, "<test>");
  sputnik::lexer::LexResult lex_result = lexer.lex();
  if (!lex_result.ok()) {
    std::cerr << sputnik::lexer::diagnostics_to_json(lex_result.diagnostics);
    std::exit(1);
  }

  sputnik::parser::Parser parser(lex_result.tokens);
  sputnik::parser::ParseModuleResult parse_result = parser.parse_module_unit();
  if (!parse_result.ok()) {
    std::cerr << sputnik::lexer::diagnostics_to_json(parse_result.diagnostics);
    std::exit(1);
  }

  sputnik::binder::BindResult bind_result =
      sputnik::binder::bind_module(parse_result.items, parse_result.module_name);
  if (!bind_result.ok()) {
    std::cerr << sputnik::lexer::diagnostics_to_json(bind_result.diagnostics);
    std::exit(1);
  }

  sputnik::hir::Program program = sputnik::hir::lower_module(
      parse_result.items, parse_result.module_name, bind_result.graph);
  sputnik::mir::Module mir_module =
      sputnik::mir::lower_program(program, parse_result.module_name);
  const sputnik::mir::ValidationResult mir_validation =
      sputnik::mir::validate_module(mir_module);
  if (!mir_validation.ok()) {
    std::cerr << sputnik::mir::validation_errors_to_json(mir_validation.errors);
    std::exit(1);
  }

  sputnik::bytecode::EmitResult emit_result =
      sputnik::bytecode::emit_program(program, parse_result.module_name);
  if (!emit_result.ok()) {
    std::cerr << sputnik::lexer::diagnostics_to_json(emit_result.diagnostics);
    std::exit(1);
  }
  const std::vector<std::uint8_t> bytes =
      sputnik::bytecode::serialize_module(emit_result.module);
  const sputnik::bytecode::DecodeResult decoded =
      sputnik::bytecode::deserialize_module(bytes);
  if (!decoded.ok()) {
    std::cerr << sputnik::bytecode::verify_errors_to_json(decoded.errors);
    std::exit(1);
  }

  sputnik::native::NativeModule native_module =
      sputnik::native::compile_native_module(decoded.module, mir_module);
  const sputnik::native::NativeValidationResult native_validation =
      sputnik::native::validate_native_module(native_module, &decoded.module);
  if (!native_validation.ok()) {
    std::cerr << sputnik::native::diagnostics_to_json(
        native_validation.diagnostics);
    std::exit(1);
  }

  return {decoded.module, std::move(mir_module), std::move(native_module)};
}

const sputnik::bytecode::BcMethod *
method_by_name(const sputnik::bytecode::BcModule &module,
               const std::string &name) {
  for (const sputnik::bytecode::BcMethod &method : module.methods) {
    if (method.selector_sym_id < module.symbols.size() &&
        module.symbols[method.selector_sym_id] == name) {
      return &method;
    }
  }
  return nullptr;
}

const sputnik::native::NativeCodeObject *
native_code_for_bc(const sputnik::native::NativeModule &module,
                   std::uint32_t code_id) {
  for (const sputnik::native::NativeCodeObject &code : module.code_objects) {
    if (code.source_bc_code_id == code_id) {
      return &code;
    }
  }
  return nullptr;
}

bool has_stub_kind(const sputnik::native::NativeCodeObject &code,
                   const std::string &kind, bool reflective) {
  for (const sputnik::native::NativeCallStub &stub : code.call_stub_table) {
    if (stub.kind == kind && stub.reflective == reflective) {
      return true;
    }
  }
  return false;
}

bool has_patchpoint_kind(const sputnik::native::NativeCodeObject &code,
                         const std::string &kind) {
  for (const sputnik::native::NativePatchpoint &patchpoint : code.patchpoints) {
    if (patchpoint.kind == kind) {
      return true;
    }
  }
  return false;
}

bool has_slowpath_kind(const sputnik::native::NativeCodeObject &code,
                       const std::string &kind) {
  for (const sputnik::native::NativeSlowPath &slowpath : code.slowpath_table) {
    if (slowpath.kind == kind) {
      return true;
    }
  }
  return false;
}

bool has_root_map_for_ip(const sputnik::native::NativeCodeObject &code,
                         std::uint32_t ip) {
  for (const sputnik::native::NativeRootMap &root : code.root_maps) {
    if (root.ip_offset == ip) {
      return true;
    }
  }
  return false;
}

bool has_safepoint_kind(const sputnik::native::NativeCodeObject &code,
                        const std::string &kind) {
  for (const sputnik::native::NativeSafepointMap &safepoint :
       code.safepoint_maps) {
    if (safepoint.kind == kind) {
      return true;
    }
  }
  return false;
}

bool safepoint_kind_has_root_map(const sputnik::native::NativeCodeObject &code,
                                 const std::string &kind) {
  for (const sputnik::native::NativeSafepointMap &safepoint :
       code.safepoint_maps) {
    if (safepoint.kind == kind &&
        has_root_map_for_ip(code, safepoint.ip_offset)) {
      return true;
    }
  }
  return false;
}

bool has_native_diagnostic(const sputnik::native::NativeValidationResult &result,
                           const std::string &code) {
  for (const sputnik::native::NativeDiagnostic &diagnostic : result.diagnostics) {
    if (diagnostic.code == code) {
      return true;
    }
  }
  return false;
}

void test_native_metadata_preserves_call_and_root_maps() {
  const CompiledArtifacts artifacts = compile_ok("def add(x, y):\n"
                                                 "  x + y\n");
  const sputnik::bytecode::BcMethod *method =
      method_by_name(artifacts.bytecode_module, "add");
  expect(method != nullptr, "add method exists");
  const sputnik::native::NativeCodeObject *code =
      native_code_for_bc(artifacts.native_module, method->entry_code_id);
  expect(code != nullptr, "native code object exists for add");
  expect(!code->machine_code_blob.empty(), "native blob is recorded");
  expect(!code->root_maps.empty(), "root maps are emitted");
  expect(!code->safepoint_maps.empty(), "safepoint maps are emitted");
  expect(has_stub_kind(*code, "send", false), "SEND lowers to call stub");
  expect(has_patchpoint_kind(*code, "call_ic"),
         "SEND gets JIT call-IC patchpoint");
  expect(has_slowpath_kind(*code, "send"),
         "SEND records an explicit runtime slowpath");
  expect(has_slowpath_kind(*code, "assumption_invalidation"),
         "frozen native code records bytecode fallback invalidation slowpath");
  const std::string json =
      sputnik::native::module_to_json(artifacts.native_module, "test-hash");
  expect(json.find("\"slowpath_table\"") != std::string::npos,
         "native JSON exposes slowpath_table");
  const std::string dump =
      sputnik::native::module_to_dump(artifacts.native_module, "test-hash");
  expect(dump.find("slowpath q") != std::string::npos,
         "native dump exposes slowpath entries");
}

void test_reflective_send_dyn_uses_slow_stub_metadata() {
  const CompiledArtifacts artifacts =
      compile_ok("def invoke(recv, selector, value):\n"
                 "  send(recv, selector, value)\n");
  const sputnik::bytecode::BcMethod *method =
      method_by_name(artifacts.bytecode_module, "invoke");
  expect(method != nullptr, "invoke method exists");
  const sputnik::native::NativeCodeObject *code =
      native_code_for_bc(artifacts.native_module, method->entry_code_id);
  expect(code != nullptr, "native code object exists for invoke");
  expect(has_stub_kind(*code, "send_dyn", true),
         "SEND_DYN uses reflective slow stub");
  expect(has_patchpoint_kind(*code, "reflective_send_dyn"),
         "SEND_DYN gets reflective patchpoint");
  expect(has_slowpath_kind(*code, "send_dyn"),
         "SEND_DYN records reflective slowpath metadata");
}

void test_exception_edges_have_native_root_and_slowpath_metadata() {
  sputnik::bytecode::BcModule module;
  module.format_version = {1, 0};
  module.language_version = {1, 0};

  sputnik::bytecode::BcCode code;
  code.code_id = 1;
  code.kind = sputnik::bytecode::CodeKind::Method;
  code.reg_count = 1;
  code.instructions.push_back({sputnik::bytecode::Opcode::Raise, {{0, false}}});
  code.instructions.push_back({sputnik::bytecode::Opcode::GetLast, {{0, false}}});
  code.instructions.push_back({sputnik::bytecode::Opcode::Return, {{0, false}}});
  code.handler_table.push_back({0, 1, 1, 1, 0});
  module.code_objects.push_back(code);

  sputnik::mir::Module mir_module;
  mir_module.module_name = "native.manual";
  sputnik::native::NativeModule native_module =
      sputnik::native::compile_native_module(module, mir_module);
  const sputnik::native::NativeValidationResult validation =
      sputnik::native::validate_native_module(native_module, &module);
  expect(validation.ok(),
         sputnik::native::diagnostics_to_json(validation.diagnostics));

  const sputnik::native::NativeCodeObject *native_code =
      native_code_for_bc(native_module, 1);
  expect(native_code != nullptr, "manual native code object exists");
  expect(has_stub_kind(*native_code, "raise", false),
         "RAISE records runtime raise stub");
  expect(has_slowpath_kind(*native_code, "raise"),
         "RAISE records language-error preserving slowpath");
  expect(has_root_map_for_ip(*native_code, 1),
         "exception handler pc has a native root map");
}

void test_throw_edges_have_native_slowpath_metadata() {
  sputnik::bytecode::BcModule module;
  module.format_version = {1, 0};
  module.language_version = {1, 0};

  sputnik::bytecode::BcCode code;
  code.code_id = 1;
  code.kind = sputnik::bytecode::CodeKind::Method;
  code.reg_count = 2;
  code.instructions.push_back(
      {sputnik::bytecode::Opcode::Throw, {{0, false}, {1, false}}});
  code.instructions.push_back({sputnik::bytecode::Opcode::Return, {{1, false}}});
  module.code_objects.push_back(code);

  sputnik::mir::Module mir_module;
  mir_module.module_name = "native.manual";
  sputnik::native::NativeModule native_module =
      sputnik::native::compile_native_module(module, mir_module);
  const sputnik::native::NativeValidationResult validation =
      sputnik::native::validate_native_module(native_module, &module);
  expect(validation.ok(),
         sputnik::native::diagnostics_to_json(validation.diagnostics));

  const sputnik::native::NativeCodeObject *native_code =
      native_code_for_bc(native_module, 1);
  expect(native_code != nullptr, "manual throw native code object exists");
  expect(has_stub_kind(*native_code, "throw", false),
         "THROW records runtime throw stub");
  expect(has_slowpath_kind(*native_code, "throw"),
         "THROW records language-control slowpath");
}

void test_native_root_maps_cover_gc_boundary_kinds() {
  sputnik::bytecode::BcModule module;
  module.format_version = {1, 0};
  module.language_version = {1, 0};

  sputnik::bytecode::Constant one;
  one.kind = sputnik::bytecode::ConstantKind::Integer;
  one.int_value = 1;
  module.const_pool.push_back(one);

  sputnik::bytecode::BcCode code;
  code.code_id = 1;
  code.kind = sputnik::bytecode::CodeKind::Method;
  code.reg_count = 5;
  code.instructions.push_back(
      {sputnik::bytecode::Opcode::LoadK, {{0, false}, {0, false}}});
  code.instructions.push_back({sputnik::bytecode::Opcode::MakeList,
                               {{1, false}, {0, false}, {1, false}}});
  code.instructions.push_back({sputnik::bytecode::Opcode::MakeClosure,
                               {{2, false}, {2, false}, {0, false}}});
  code.instructions.push_back({sputnik::bytecode::Opcode::Call,
                               {{3, false},
                                {2, false},
                                {0, false},
                                {0, false},
                                {-1, true},
                                {0, false}}});
  code.instructions.push_back(
      {sputnik::bytecode::Opcode::LoadBool, {{4, false}, {1, false}}});
  code.instructions.push_back({sputnik::bytecode::Opcode::Safepoint, {}});
  code.instructions.push_back(
      {sputnik::bytecode::Opcode::JumpIfFalse, {{4, false}, {9, false}}});
  code.instructions.push_back(
      {sputnik::bytecode::Opcode::LoadBool, {{4, false}, {0, false}}});
  code.instructions.push_back({sputnik::bytecode::Opcode::Jump, {{5, false}}});
  code.instructions.push_back({sputnik::bytecode::Opcode::Return, {{1, false}}});
  code.safepoint_table.push_back({5, 0});

  sputnik::bytecode::BcCode callee;
  callee.code_id = 2;
  callee.kind = sputnik::bytecode::CodeKind::Block;
  callee.reg_count = 1;
  callee.instructions.push_back(
      {sputnik::bytecode::Opcode::LoadK, {{0, false}, {0, false}}});
  callee.instructions.push_back(
      {sputnik::bytecode::Opcode::Return, {{0, false}}});
  module.code_objects = {code, callee};

  sputnik::mir::Module mir_module;
  mir_module.module_name = "native.boundaries";
  const sputnik::native::NativeModule native_module =
      sputnik::native::compile_native_module(module, mir_module);
  const sputnik::native::NativeValidationResult validation =
      sputnik::native::validate_native_module(native_module, &module);
  expect(validation.ok(),
         sputnik::native::diagnostics_to_json(validation.diagnostics));

  const sputnik::native::NativeCodeObject *native_code =
      native_code_for_bc(native_module, 1);
  expect(native_code != nullptr, "boundary native code object exists");
  expect(has_safepoint_kind(*native_code, "allocation"),
         "native metadata records allocation safepoint");
  expect(has_safepoint_kind(*native_code, "call"),
         "native metadata records call safepoint");
  expect(has_safepoint_kind(*native_code, "backedge"),
         "native metadata records backedge safepoint");
  expect(safepoint_kind_has_root_map(*native_code, "allocation"),
         "allocation safepoint should have a root map");
  expect(safepoint_kind_has_root_map(*native_code, "call"),
         "call safepoint should have a root map");
  expect(safepoint_kind_has_root_map(*native_code, "backedge"),
         "backedge safepoint should have a root map");
}

void test_native_trampoline_safepoint_preserves_heap_argument_root() {
  sputnik::bytecode::BcModule module;
  module.format_version = {1, 0};
  module.language_version = {1, 0};

  sputnik::bytecode::Constant one;
  one.kind = sputnik::bytecode::ConstantKind::Integer;
  one.int_value = 1;
  module.const_pool.push_back(one);

  sputnik::bytecode::BcCode identity;
  identity.code_id = 1;
  identity.kind = sputnik::bytecode::CodeKind::Method;
  identity.reg_count = 1;
  identity.instructions.push_back({sputnik::bytecode::Opcode::Safepoint, {}});
  identity.instructions.push_back(
      {sputnik::bytecode::Opcode::Return, {{0, false}}});
  identity.safepoint_table.push_back({0, 0});

  sputnik::bytecode::BcCode make_list;
  make_list.code_id = 2;
  make_list.kind = sputnik::bytecode::CodeKind::Method;
  make_list.reg_count = 2;
  make_list.instructions.push_back(
      {sputnik::bytecode::Opcode::LoadK, {{0, false}, {0, false}}});
  make_list.instructions.push_back({sputnik::bytecode::Opcode::MakeList,
                                    {{1, false}, {0, false}, {1, false}}});
  make_list.instructions.push_back(
      {sputnik::bytecode::Opcode::Return, {{1, false}}});
  module.code_objects = {identity, make_list};

  sputnik::mir::Module mir_module;
  mir_module.module_name = "native.trampoline_roots";
  const sputnik::native::NativeModule native_module =
      sputnik::native::compile_native_module(module, mir_module);
  const sputnik::native::NativeValidationResult validation =
      sputnik::native::validate_native_module(native_module, &module);
  expect(validation.ok(),
         sputnik::native::diagnostics_to_json(validation.diagnostics));
  const sputnik::native::NativeCodeObject *native_code =
      native_code_for_bc(native_module, 1);
  expect(native_code != nullptr, "trampoline root native code object exists");

  sputnik::runtime::RuntimeWorld world(module);
  sputnik::runtime::ExecutionResult made = world.execute(2);
  expect(made.ok() && made.value.is_list(),
         "trampoline root probe should allocate a world list");
  const sputnik::runtime::IntrusivePtr<sputnik::runtime::ListValue> list = made.value.as_list();

  expect(world.freeze_world().ok(), "trampoline root world should freeze");
  const sputnik::native::NativeModule bound =
      sputnik::runtime::bind_native_module_to_world(native_module,
                                                  world.world_mirror());
  world.request_garbage_collection(sputnik::runtime::RuntimeGcCycle::Full);
  const sputnik::runtime::ExecutionResult result =
      sputnik::runtime::execute_native_code(world, bound, native_code->native_id,
                                          {made.value});
  expect(result.ok(), "native trampoline safepoint should execute");
  expect(result.value.is_list() && result.value.as_list() == list,
         "native trampoline should return original heap argument");
  expect(list->header.lifetime_state ==
             sputnik::runtime::ObjectLifetimeState::Live,
         "native trampoline safepoint should preserve argument root");
  const sputnik::runtime::RuntimeHeapStats stats = world.heap_stats();
  expect(stats.gc_safepoint_collections == 1,
         "native trampoline safepoint should run requested GC");
  expect(stats.gc_reclaimed_objects == 0,
         "native trampoline safepoint should not reclaim argument root");
}

void test_native_validation_rejects_missing_slowpath_metadata() {
  const CompiledArtifacts artifacts = compile_ok("def add(x, y):\n"
                                                 "  x + y\n");
  sputnik::native::NativeModule broken = artifacts.native_module;
  broken.code_objects[0].slowpath_table.clear();
  const sputnik::native::NativeValidationResult validation =
      sputnik::native::validate_native_module(broken, &artifacts.bytecode_module);
  expect(!validation.ok(), "missing slowpath metadata should be rejected");
  expect(has_native_diagnostic(validation, "NATIVE1017") ||
             has_native_diagnostic(validation, "NATIVE1013"),
         "native validation should report missing W15 slowpath metadata");
}

void test_native_trampoline_requires_frozen_world_and_executes() {
  const CompiledArtifacts artifacts = compile_ok("def add(x, y):\n"
                                                 "  x + y\n");
  const sputnik::bytecode::BcMethod *method =
      method_by_name(artifacts.bytecode_module, "add");
  expect(method != nullptr, "add method exists");
  const sputnik::native::NativeCodeObject *code =
      native_code_for_bc(artifacts.native_module, method->entry_code_id);
  expect(code != nullptr, "native code object exists for add");

  sputnik::runtime::RuntimeWorld world(artifacts.bytecode_module);
  const sputnik::runtime::ExecutionResult open_result =
      sputnik::runtime::execute_native_code(world, artifacts.native_module,
                                          code->native_id,
                                          {sputnik::runtime::Value::integer(9),
                                           sputnik::runtime::Value::integer(4)});
  expect(!open_result.ok(), "open world native execution is rejected");
  expect(open_result.fault->error_name == "WorldFrozenError",
         "open world rejection uses WorldFrozenError");

  expect(world.freeze_world().ok(), "freeze_world should succeed");
  const sputnik::native::NativeModule bound =
      sputnik::runtime::bind_native_module_to_world(artifacts.native_module,
                                                  world.world_mirror());
  const sputnik::runtime::ExecutionResult native_result =
      sputnik::runtime::execute_native_code(world, bound, code->native_id,
                                          {sputnik::runtime::Value::integer(9),
                                           sputnik::runtime::Value::integer(4)});
  expect(native_result.ok(), "bound native trampoline execution succeeds");
  expect(native_result.value.is_integer() &&
             native_result.value.as_integer() == 13,
         "native trampoline returns bytecode-equivalent result");
}

void test_native_trampoline_executes_time_stdlib_module() {
  const CompiledArtifacts artifacts = compile_ok(
      "def time_stdlib_roundtrip():\n"
      "  t = Time.parse(\"2026-07-02T12:00:00Z\")\n"
      "  ny = TimeZone[\"America/New_York\"]\n"
      "  shifted = t.in_tz(ny)\n"
      "  assumed = t.as_tz(ny)\n"
      "  parsed = Time.parse(\"2026-03-08 02:30\", "
      "format: :datetime, zone: \"America/New_York\", on_gap: :forward)\n"
      "  period = 1.day + 2.hours\n"
      "  boundary = shifted.start_of_day\n"
      "  if Time === t and TimeZone === ny and TimePeriod === period and "
      "ny.fixed? == false and "
      "shifted.to_str(:iso8601) == \"2026-07-02T08:00:00-04:00\" and "
      "assumed.in_tz(:utc).to_str(:iso8601) == "
      "\"2026-07-02T16:00:00Z\" and "
      "parsed.to_str(:iso8601) == \"2026-03-08T03:30:00-04:00\" and "
      "boundary.to_str(:datetime) == \"2026-07-02 00:00:00\" and "
      "period.days == 1 and period.nanoseconds == 7200000000000:\n"
      "    42\n"
      "  else:\n"
      "    0\n");
  const sputnik::bytecode::BcMethod *method =
      method_by_name(artifacts.bytecode_module, "time_stdlib_roundtrip");
  expect(method != nullptr, "time_stdlib_roundtrip method exists");
  const sputnik::native::NativeCodeObject *code =
      native_code_for_bc(artifacts.native_module, method->entry_code_id);
  expect(code != nullptr, "native code object exists for time stdlib method");

  sputnik::runtime::RuntimeWorld world(artifacts.bytecode_module);
  expect(world.freeze_world().ok(), "freeze_world should succeed");
  const sputnik::native::NativeModule bound =
      sputnik::runtime::bind_native_module_to_world(artifacts.native_module,
                                                  world.world_mirror());
  const sputnik::runtime::ExecutionResult native_result =
      sputnik::runtime::execute_native_code(world, bound, code->native_id, {});
  expect(native_result.ok(), "native time stdlib execution succeeds");
  expect(native_result.value.is_integer() &&
             native_result.value.as_integer() == 42,
         "native time stdlib execution returns expected result");
}

void test_stale_native_assumption_can_fall_back_to_bytecode() {
  const CompiledArtifacts artifacts = compile_ok("def id(x):\n"
                                                 "  x\n");
  const sputnik::bytecode::BcMethod *method =
      method_by_name(artifacts.bytecode_module, "id");
  expect(method != nullptr, "id method exists");
  const sputnik::native::NativeCodeObject *code =
      native_code_for_bc(artifacts.native_module, method->entry_code_id);
  expect(code != nullptr, "native code object exists for id");

  sputnik::runtime::RuntimeWorld world(artifacts.bytecode_module);
  const sputnik::native::NativeModule stale =
      sputnik::runtime::bind_native_module_to_world(artifacts.native_module,
                                                  world.world_mirror());
  expect(world.freeze_world().ok(), "freeze_world should succeed");

  const sputnik::runtime::ExecutionResult rejected =
      sputnik::runtime::execute_native_code(world, stale, code->native_id,
                                          {sputnik::runtime::Value::integer(5)});
  expect(!rejected.ok(), "stale native assumptions are rejected");
  expect(rejected.fault->error_name == "NativeInvalidationError",
         "stale native assumption reports invalidation");

  sputnik::runtime::NativeExecutionOptions options;
  options.allow_bytecode_fallback = true;
  const sputnik::runtime::ExecutionResult fallback =
      sputnik::runtime::execute_native_code(
          world, stale, code->native_id, {sputnik::runtime::Value::integer(5)},
          sputnik::runtime::Value::null(), sputnik::runtime::Value::null(),
          options);
  expect(fallback.ok(), "stale native code can re-enter bytecode");
  expect(fallback.value.is_integer() && fallback.value.as_integer() == 5,
         "bytecode fallback preserves result");
}

} // namespace

int main() {
  test_native_metadata_preserves_call_and_root_maps();
  test_reflective_send_dyn_uses_slow_stub_metadata();
  test_exception_edges_have_native_root_and_slowpath_metadata();
  test_throw_edges_have_native_slowpath_metadata();
  test_native_root_maps_cover_gc_boundary_kinds();
  test_native_trampoline_safepoint_preserves_heap_argument_root();
  test_native_validation_rejects_missing_slowpath_metadata();
  test_native_trampoline_requires_frozen_world_and_executes();
  test_native_trampoline_executes_time_stdlib_module();
  test_stale_native_assumption_can_fall_back_to_bytecode();
  return 0;
}
