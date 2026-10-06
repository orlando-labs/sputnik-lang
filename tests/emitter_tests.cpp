#include "bytecode/emitter.h"
#include "bytecode/format.h"
#include "frontend/binder/binder.h"
#include "frontend/hir/hir.h"
#include "frontend/lexer/lexer.h"
#include "frontend/parser/parser.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "emitter test failed: " << message << "\n";
    std::exit(1);
  }
}

sputnik::bytecode::EmitResult emit_ok(const std::string &source) {
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
  sputnik::bytecode::EmitResult emit_result =
      sputnik::bytecode::emit_program(program, parse_result.module_name);
  if (!emit_result.ok()) {
    std::cerr << sputnik::lexer::diagnostics_to_json(emit_result.diagnostics);
    std::exit(1);
  }
  return emit_result;
}

sputnik::bytecode::EmitResult emit_allow_bytecode_diagnostics(
    const std::string &source) {
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
  return sputnik::bytecode::emit_program(program, parse_result.module_name);
}

const sputnik::bytecode::BcCode *
code_by_id(const sputnik::bytecode::BcModule &module, std::uint32_t code_id) {
  for (const sputnik::bytecode::BcCode &code : module.code_objects) {
    if (code.code_id == code_id) {
      return &code;
    }
  }
  return nullptr;
}

const sputnik::bytecode::BcCode *
code_by_kind(const sputnik::bytecode::BcModule &module,
             sputnik::bytecode::CodeKind kind) {
  for (const sputnik::bytecode::BcCode &code : module.code_objects) {
    if (code.kind == kind) {
      return &code;
    }
  }
  return nullptr;
}

bool contains_opcode(const sputnik::bytecode::BcCode &code,
                     sputnik::bytecode::Opcode opcode) {
  for (const sputnik::bytecode::Instruction &instruction : code.instructions) {
    if (instruction.opcode == opcode) {
      return true;
    }
  }
  return false;
}

std::size_t count_opcode(const sputnik::bytecode::BcCode &code,
                         sputnik::bytecode::Opcode opcode) {
  std::size_t count = 0;
  for (const sputnik::bytecode::Instruction &instruction : code.instructions) {
    if (instruction.opcode == opcode) {
      ++count;
    }
  }
  return count;
}

bool module_contains_opcode(const sputnik::bytecode::BcModule &module,
                            sputnik::bytecode::Opcode opcode) {
  for (const sputnik::bytecode::BcCode &code : module.code_objects) {
    if (contains_opcode(code, opcode)) {
      return true;
    }
  }
  return false;
}

bool module_contains_callsite_flag(const sputnik::bytecode::BcModule &module,
                                   std::uint32_t flag) {
  for (const sputnik::bytecode::BcCode &code : module.code_objects) {
    for (const sputnik::bytecode::CacheSiteEntry &entry :
         code.call_site_table) {
      if ((entry.flags & flag) != 0U) {
        return true;
      }
    }
  }
  return false;
}

bool has_diagnostic_code(const sputnik::bytecode::EmitResult &result,
                         const std::string &code) {
  for (const sputnik::lexer::Diagnostic &diagnostic : result.diagnostics) {
    if (diagnostic.code == code) {
      return true;
    }
  }
  return false;
}

bool contains_symbol(const sputnik::bytecode::BcModule &module,
                     const std::string &symbol) {
  for (const std::string &value : module.symbols) {
    if (value == symbol) {
      return true;
    }
  }
  return false;
}

bool contains_string(const sputnik::bytecode::BcModule &module,
                     const std::string &text) {
  for (const std::string &value : module.strings) {
    if (value == text) {
      return true;
    }
  }
  return false;
}

bool contains_attr(const sputnik::bytecode::BcModule &module,
                   const std::string &key, const std::string &value) {
  for (const sputnik::bytecode::AttrEntry &entry : module.attrs) {
    const std::string attr_key = entry.key_str_id < module.strings.size()
                                     ? module.strings[entry.key_str_id]
                                     : std::string();
    const std::string attr_value = entry.value_str_id < module.strings.size()
                                       ? module.strings[entry.value_str_id]
                                       : std::string();
    if (attr_key == key && attr_value == value) {
      return true;
    }
  }
  return false;
}

std::string path_constant_text(const sputnik::bytecode::BcModule &module,
                               std::uint32_t ref_id) {
  expect(ref_id < module.const_pool.size(), "path const ref is in range");
  const sputnik::bytecode::Constant &constant = module.const_pool[ref_id];
  expect(constant.kind == sputnik::bytecode::ConstantKind::Path,
         "constant is path");
  std::string out;
  for (std::size_t i = 0; i < constant.items.size(); ++i) {
    expect(constant.items[i] < module.symbols.size(), "path symbol in range");
    if (i != 0U) {
      out += ".";
    }
    out += module.symbols[constant.items[i]];
  }
  return out;
}

void test_if_and_round_trip() {
  const sputnik::bytecode::EmitResult emit_result = emit_ok("def choose(x):\n"
                                                          "  if x > 0:\n"
                                                          "    x\n"
                                                          "  else:\n"
                                                          "    0\n");
  expect(emit_result.module.methods.size() == 1, "expected one method");

  const sputnik::bytecode::BcCode *code = code_by_id(
      emit_result.module, emit_result.module.methods[0].entry_code_id);
  expect(code != nullptr, "choose code exists");
  expect(contains_opcode(*code, sputnik::bytecode::Opcode::Send),
         "if method emits SEND");
  expect(contains_opcode(*code, sputnik::bytecode::Opcode::JumpIfFalse),
         "if method emits JUMP_IF_FALSE");
  expect(contains_opcode(*code, sputnik::bytecode::Opcode::Return),
         "if method emits RETURN");

  const std::vector<std::uint8_t> bytes =
      sputnik::bytecode::serialize_module(emit_result.module);
  const sputnik::bytecode::DecodeResult decoded =
      sputnik::bytecode::deserialize_module(bytes);
  expect(decoded.ok(), sputnik::bytecode::verify_errors_to_json(decoded.errors));
}

void test_loop_and_safepoint() {
  const sputnik::bytecode::EmitResult emit_result = emit_ok("def step(x):\n"
                                                          "  while x < 3:\n"
                                                          "    x = x + 1\n"
                                                          "  x\n");
  const sputnik::bytecode::BcCode *code = code_by_id(
      emit_result.module, emit_result.module.methods[0].entry_code_id);
  expect(code != nullptr, "step code exists");
  expect(contains_opcode(*code, sputnik::bytecode::Opcode::Safepoint),
         "loop emits SAFEPOINT");
  expect(!code->safepoint_table.empty(), "loop records safepoint table");
  expect(contains_opcode(*code, sputnik::bytecode::Opcode::Jump),
         "loop emits back-edge jump");
}

void test_closure_capture_emission() {
  const sputnik::bytecode::EmitResult emit_result =
      emit_ok("def offsetter(xs, δ):\n"
              "  xs.map: _1 + δ\n");
  expect(emit_result.module.methods.size() == 1,
         "expected one top-level method");
  expect(emit_result.module.code_objects.size() >= 3,
         "expected module init, method, and block code");

  const sputnik::bytecode::BcCode *method_code = code_by_id(
      emit_result.module, emit_result.module.methods[0].entry_code_id);
  expect(method_code != nullptr, "offsetter code exists");
  expect(contains_opcode(*method_code, sputnik::bytecode::Opcode::MakeClosure),
         "method emits MAKE_CLOSURE");
  expect(contains_opcode(*method_code, sputnik::bytecode::Opcode::Send),
         "method emits SEND for map");

  bool saw_upval_load = false;
  for (const sputnik::bytecode::BcCode &code : emit_result.module.code_objects) {
    if (code.code_id == method_code->code_id) {
      continue;
    }
    if (contains_opcode(code, sputnik::bytecode::Opcode::LoadUpval)) {
      saw_upval_load = true;
      break;
    }
  }
  expect(saw_upval_load, "closure body emits LOAD_UPVAL");
}

// Regression: a closure that references an imported module by name (e.g.
// `task` inside `task.spawn: task.sleep(5)`) must not capture the import alias
// as an upvalue. The alias resolves to a runtime constant and is loaded via
// LOOKUP_CONST everywhere, so its import-alias local slot is never stored;
// capturing it emitted a MAKE_CLOSURE that read an uninitialized register,
// tripping the bytecode verifier with BC1313.
void test_closure_module_import_reference_verifies() {
  const sputnik::bytecode::EmitResult emit_result =
      emit_ok("import task\n"
              "h = task.spawn: task.sleep(5)\n"
              "h.wait()\n");

  // The module is well-formed only if it survives verification (deserialize
  // re-runs the verifier); before the fix this failed with BC1313.
  const std::vector<std::uint8_t> bytes =
      sputnik::bytecode::serialize_module(emit_result.module);
  const sputnik::bytecode::DecodeResult decoded =
      sputnik::bytecode::deserialize_module(bytes);
  expect(decoded.ok(), sputnik::bytecode::verify_errors_to_json(decoded.errors));

  // The closure body should reach the module import through LOOKUP_CONST and
  // must not capture it as an upvalue.
  const sputnik::bytecode::BcCode *block_code =
      code_by_kind(emit_result.module, sputnik::bytecode::CodeKind::Block);
  expect(block_code != nullptr, "closure block code exists");
  expect(contains_opcode(*block_code, sputnik::bytecode::Opcode::LookupConst),
         "closure body loads the module import via LOOKUP_CONST");
  expect(!contains_opcode(*block_code, sputnik::bytecode::Opcode::LoadUpval),
         "closure body must not load the module import via LOAD_UPVAL");
  expect(block_code->capture_layout.empty(),
         "closure must not capture the module import alias");
}

// Regression: top-level references to the net/io stdlib namespaces and their
// nested types resolve to runtime constant paths (e.g. "net.tcp",
// "io.ByteBuffer") via LOOKUP_CONST. Their import-alias local slot is never
// stored, so an `import net` / `from io import ByteBuffer` reference that
// lowered to a plain local read tripped the bytecode verifier with BC1313.
void test_net_io_stdlib_alias_references_verify() {
  const sputnik::bytecode::EmitResult emit_result =
      emit_ok("import net\n"
              "from io import ByteBuffer\n"
              "l = net.tcp.listen(\"127.0.0.1\", 0)\n"
              "b = ByteBuffer.new(8)\n"
              "n = \"hi\".bytes().count()\n");

  // The module is well-formed only if it survives verification (deserialize
  // re-runs the verifier); before the fix this failed with BC1313.
  const std::vector<std::uint8_t> bytes =
      sputnik::bytecode::serialize_module(emit_result.module);
  const sputnik::bytecode::DecodeResult decoded =
      sputnik::bytecode::deserialize_module(bytes);
  expect(decoded.ok(), sputnik::bytecode::verify_errors_to_json(decoded.errors));

  // The stdlib aliases reach the runtime through LOOKUP_CONST rather than a
  // read of their (never-stored) import-alias slot.
  const sputnik::bytecode::BcCode *init_code =
      code_by_kind(emit_result.module, sputnik::bytecode::CodeKind::Module);
  expect(init_code != nullptr, "module init code exists");
  expect(contains_opcode(*init_code, sputnik::bytecode::Opcode::LookupConst),
         "net/io aliases load via LOOKUP_CONST");
}

void test_default_thunk_emission() {
  const sputnik::bytecode::EmitResult emit_result =
      emit_ok("def configure(x, y = x + 1):\n"
              "  y\n");
  expect(emit_result.module.methods.size() == 1,
         "expected one method with default thunk");
  const sputnik::bytecode::BcMethod &method = emit_result.module.methods[0];
  expect(method.params.size() == 2, "expected two serialized params");
  expect((method.params[0].flags &
          sputnik::bytecode::kMethodParamFlagHasDefault) == 0U,
         "required param carries no default flag");
  expect((method.params[1].flags &
          sputnik::bytecode::kMethodParamFlagHasDefault) != 0U,
         "defaulted param carries default flag");
  expect(method.default_thunk_ids.size() == 1, "expected one default thunk id");

  const sputnik::bytecode::BcCode *thunk_code =
      code_by_id(emit_result.module, method.default_thunk_ids[0]);
  expect(thunk_code != nullptr, "default thunk code exists");
  expect(thunk_code->kind == sputnik::bytecode::CodeKind::DefaultThunk,
         "default thunk code kind");
  expect(code_by_kind(emit_result.module,
                      sputnik::bytecode::CodeKind::DefaultThunk) == thunk_code,
         "default thunk kind lookup");
  expect(contains_opcode(*thunk_code, sputnik::bytecode::Opcode::Send),
         "default thunk emits SEND");
  expect(contains_opcode(*thunk_code, sputnik::bytecode::Opcode::Return),
         "default thunk emits RETURN");
}

void test_type_hook_emission() {
  const sputnik::bytecode::EmitResult emit_result =
      emit_ok("def f(x as Int) -> Int:\n"
              "  x\n");
  expect(emit_result.module.methods.size() == 1,
         "expected one typed method");
  const sputnik::bytecode::BcMethod &method = emit_result.module.methods[0];
  expect(method.type_hook_ids.size() == 2,
         "parameter and return type hooks emitted");

  for (std::uint32_t hook_code_id : method.type_hook_ids) {
    const sputnik::bytecode::BcCode *hook_code =
        code_by_id(emit_result.module, hook_code_id);
    expect(hook_code != nullptr, "type hook code exists");
    expect(hook_code->kind == sputnik::bytecode::CodeKind::Block,
           "type hook code kind");
    expect(hook_code->reg_count == 1, "type hook value register exists");
    expect(contains_opcode(*hook_code, sputnik::bytecode::Opcode::TypeCheck),
           "type hook emits TYPECHECK");
    expect(contains_opcode(*hook_code, sputnik::bytecode::Opcode::Return),
           "type hook emits RETURN");
  }

  const sputnik::bytecode::DecodeResult decoded =
      sputnik::bytecode::deserialize_module(
          sputnik::bytecode::serialize_module(emit_result.module));
  expect(decoded.ok(), "typed method bytecode round-trips through verifier");
  expect(decoded.module.methods.size() == 1,
         "typed method survives round-trip");
  expect(decoded.module.methods[0].type_hook_ids.size() == 2,
         "type hook ids survive round-trip");
}

void test_keyword_param_emission() {
  const sputnik::bytecode::EmitResult emit_result =
      emit_ok("class Config:\n"
              "  class_method def build(x, α:, β: 2):\n"
              "    β\n");
  expect(emit_result.module.methods.size() == 1,
         "expected one keyword-bearing method");
  const sputnik::bytecode::BcMethod &method = emit_result.module.methods[0];
  expect(method.params.size() == 3, "serialized keyword params count");
  expect((method.params[0].flags & sputnik::bytecode::kMethodParamFlagKeyword) ==
             0U,
         "positional param has no keyword flag");
  expect((method.params[1].flags & sputnik::bytecode::kMethodParamFlagKeyword) !=
             0U,
         "required keyword param flagged");
  expect((method.params[1].flags &
          sputnik::bytecode::kMethodParamFlagHasDefault) == 0U,
         "required keyword param has no default flag");
  expect((method.params[2].flags & sputnik::bytecode::kMethodParamFlagKeyword) !=
             0U,
         "optional keyword param flagged");
  expect((method.params[2].flags &
          sputnik::bytecode::kMethodParamFlagHasDefault) != 0U,
         "optional keyword param carries default flag");
}

void test_v20_9_named_callable_param_emission() {
  const sputnik::bytecode::EmitResult emit_result = emit_ok(
      "def request(value, &success:, &error: null):\n"
      "  value\n"
      "request(1) with:\n"
      "  success:\n"
      "    _1\n");
  expect(emit_result.module.methods.size() == 1,
         "expected one named-callable method");
  const sputnik::bytecode::BcMethod &method = emit_result.module.methods[0];
  expect(method.params.size() == 3, "named callable bytecode param count");
  expect((method.params[1].flags &
          sputnik::bytecode::kMethodParamFlagKeyword) != 0U &&
             (method.params[1].flags &
              sputnik::bytecode::kMethodParamFlagNamedCallable) != 0U &&
             (method.params[1].flags &
              sputnik::bytecode::kMethodParamFlagNamedCallableNullable) == 0U,
         "required named callable flags");
  expect((method.params[2].flags &
          sputnik::bytecode::kMethodParamFlagKeyword) != 0U &&
             (method.params[2].flags &
              sputnik::bytecode::kMethodParamFlagHasDefault) != 0U &&
             (method.params[2].flags &
              sputnik::bytecode::kMethodParamFlagNamedCallable) != 0U &&
             (method.params[2].flags &
              sputnik::bytecode::kMethodParamFlagNamedCallableNullable) != 0U,
         "nullable optional named callable flags");
  expect(module_contains_opcode(emit_result.module,
                                sputnik::bytecode::Opcode::MakeClosure),
         "multiblock entry emits an ordinary closure");

  const sputnik::bytecode::DecodeResult decoded =
      sputnik::bytecode::deserialize_module(
          sputnik::bytecode::serialize_module(emit_result.module));
  expect(decoded.ok(), "named callable bytecode round-trips");
  expect((decoded.module.methods[0].params[1].flags &
          sputnik::bytecode::kMethodParamFlagNamedCallable) != 0U &&
             (decoded.module.methods[0].params[2].flags &
              sputnik::bytecode::kMethodParamFlagNamedCallableNullable) != 0U,
         "named callable flags survive serialization");
}

void test_case_emission() {
  const sputnik::bytecode::EmitResult emit_result = emit_ok("def choose(x):\n"
                                                          "  case x:\n"
                                                          "    when 0:\n"
                                                          "      1\n"
                                                          "    else:\n"
                                                          "      2\n");
  const sputnik::bytecode::BcCode *code = code_by_id(
      emit_result.module, emit_result.module.methods[0].entry_code_id);
  expect(code != nullptr, "choose case code exists");
  expect(contains_opcode(*code, sputnik::bytecode::Opcode::PCheckEq),
         "case emits P_CHECK_EQ");
  expect(contains_opcode(*code, sputnik::bytecode::Opcode::Jump),
         "case emits jumps");
  expect(!contains_opcode(*code, sputnik::bytecode::Opcode::GetLast),
         "case without explicit last-value avoids GETLAST");
}

void test_last_result_elision_without_explicit_last_value() {
  const sputnik::bytecode::EmitResult emit_result = emit_ok("def compute():\n"
                                                          "  x = 1\n"
                                                          "  x + 2\n");
  const sputnik::bytecode::BcCode *code = code_by_id(
      emit_result.module, emit_result.module.methods[0].entry_code_id);
  expect(code != nullptr, "compute code exists");
  expect(!contains_opcode(*code, sputnik::bytecode::Opcode::SetLast),
         "method without $_ avoids SETLAST");
  expect(!contains_opcode(*code, sputnik::bytecode::Opcode::GetLast),
         "method without $_ avoids GETLAST");
  expect(!code->instructions.empty() &&
             code->instructions.front().opcode ==
                 sputnik::bytecode::Opcode::LoadK &&
             code->instructions.front().operands.size() >= 2 &&
             code->instructions.front().operands[0].value == 0,
         "constant assignment loads directly into local slot");
}

void test_explicit_last_value_preserves_last_result_updates() {
  const sputnik::bytecode::EmitResult emit_result = emit_ok("def compute():\n"
                                                          "  1\n"
                                                          "  $_ + 2\n");
  const sputnik::bytecode::BcCode *code = code_by_id(
      emit_result.module, emit_result.module.methods[0].entry_code_id);
  expect(code != nullptr, "last-value code exists");
  expect(count_opcode(*code, sputnik::bytecode::Opcode::SetLast) >= 1,
         "$_ method emits SETLAST for prior expression");
  expect(contains_opcode(*code, sputnik::bytecode::Opcode::GetLast),
         "$_ method emits GETLAST");
}

void test_pattern_assignment_emission() {
  const sputnik::bytecode::EmitResult emit_result =
      emit_ok("def unpack(values):\n"
              "  [head, *tail] = values\n");
  const sputnik::bytecode::BcCode *code = code_by_id(
      emit_result.module, emit_result.module.methods[0].entry_code_id);
  expect(code != nullptr, "unpack code exists");
  expect(contains_opcode(*code, sputnik::bytecode::Opcode::PPrepSeq),
         "pattern assignment emits P_PREP_SEQ");
  expect(contains_opcode(*code, sputnik::bytecode::Opcode::PCheckLenGte),
         "pattern assignment emits P_CHECK_LEN_GTE");
  expect(contains_opcode(*code, sputnik::bytecode::Opcode::PGetIndex),
         "pattern assignment emits P_GET_INDEX");
  expect(contains_opcode(*code, sputnik::bytecode::Opcode::PBind),
         "pattern assignment emits P_BIND");
  expect(contains_opcode(*code, sputnik::bytecode::Opcode::PCommit),
         "pattern assignment emits P_COMMIT");
}

void test_optional_index_emission() {
  const sputnik::bytecode::EmitResult emit_result = emit_ok("def lookup(xs, i):\n"
                                                          "  xs[?i]\n");
  const sputnik::bytecode::BcCode *code = code_by_id(
      emit_result.module, emit_result.module.methods[0].entry_code_id);
  expect(code != nullptr, "optional index code exists");
  expect(contains_opcode(*code, sputnik::bytecode::Opcode::Send),
         "optional index emits SEND");
  expect(contains_symbol(emit_result.module, "[]?"),
         "optional index interns []? selector");
}

void test_matcher_expr_emission() {
  const sputnik::bytecode::EmitResult emit_result =
      emit_ok("def choose(x, limit):\n"
              "  case x:\n"
              "    when limit + 1:\n"
              "      x\n"
              "    else:\n"
              "      0\n");
  const sputnik::bytecode::BcCode *code = code_by_id(
      emit_result.module, emit_result.module.methods[0].entry_code_id);
  expect(code != nullptr, "matcher-expression code exists");
  expect(contains_opcode(*code, sputnik::bytecode::Opcode::PTripleEq),
         "matcher-expression emits P_TRIPLE_EQ");
}

void test_dynamic_pattern_emission() {
  const sputnik::bytecode::EmitResult emit_result =
      emit_ok("def classify(shape):\n"
              "  case shape:\n"
              "    when pattern(route(\"/users/:id\")) with {id:, **null}:\n"
              "      id\n"
              "    else:\n"
              "      shape\n");
  const sputnik::bytecode::BcCode *code = code_by_id(
      emit_result.module, emit_result.module.methods[0].entry_code_id);
  expect(code != nullptr, "dynamic pattern code exists");
  expect(contains_opcode(*code, sputnik::bytecode::Opcode::Send),
         "dynamic pattern emits SEND");
  expect(contains_opcode(*code, sputnik::bytecode::Opcode::PPrepMap),
         "dynamic pattern emits P_PREP_MAP");
  expect(contains_opcode(*code, sputnik::bytecode::Opcode::PHasKey),
         "dynamic pattern emits P_HAS_KEY");
  expect(contains_opcode(*code, sputnik::bytecode::Opcode::PGetKey),
         "dynamic pattern emits P_GET_KEY");
  expect(contains_opcode(*code, sputnik::bytecode::Opcode::PBind),
         "dynamic pattern emits P_BIND");
  expect(contains_opcode(*code, sputnik::bytecode::Opcode::Raise),
         "dynamic pattern emits protocol RAISE path");
  expect(contains_symbol(emit_result.module, "match"),
         "dynamic pattern interns :match");
  expect(contains_symbol(emit_result.module, "success"),
         "dynamic pattern interns :success");
  expect(contains_symbol(emit_result.module, "bindings"),
         "dynamic pattern interns :bindings");
  expect(!has_diagnostic_code(emit_result, "BC2007"),
         "dynamic pattern no longer emits BC2007");
}

void test_clause_method_emission() {
  const sputnik::bytecode::EmitResult emit_result =
      emit_ok("def fact(0): 1\n"
              "def fact(n) if n > 0: n\n");
  expect(emit_result.module.methods.size() == 1,
         "expected one clause-style method");
  const sputnik::bytecode::BcMethod &method = emit_result.module.methods[0];
  expect(method.clause_table.size() == 2, "expected two clause entries");
  expect(emit_result.module.pattern_programs.size() == 2,
         "expected two pattern program descriptors");
  expect(emit_result.module.pattern_programs[0].binding_count == 0,
         "literal clause should not bind");
  expect(emit_result.module.pattern_programs[1].binding_count == 1,
         "binder clause should expose one binding");
  expect(code_by_id(emit_result.module, method.entry_code_id) != nullptr,
         "clause else-body code exists");
  expect(code_by_id(emit_result.module,
                    method.clause_table[0].pattern_code_id) != nullptr,
         "clause pattern code exists");
  expect(code_by_id(emit_result.module, method.clause_table[0].guard_code_id) !=
             nullptr,
         "clause guard code exists");
  expect(code_by_id(emit_result.module, method.clause_table[0].body_code_id) !=
             nullptr,
         "clause body code exists");
  const sputnik::bytecode::BcCode *pattern_code =
      code_by_id(emit_result.module, method.clause_table[1].pattern_code_id);
  expect(pattern_code != nullptr, "binder clause pattern code exists");
  expect(contains_opcode(*pattern_code, sputnik::bytecode::Opcode::PBind),
         "binder clause pattern emits P_BIND");
  expect(contains_opcode(*pattern_code, sputnik::bytecode::Opcode::PCommit),
         "binder clause pattern emits P_COMMIT");
}

void test_w13_operator_emission() {
  const sputnik::bytecode::EmitResult emit_result =
      emit_ok("def ops(x, xs, a, b):\n"
              "  x in xs\n"
              "  a and b\n"
              "  a or b\n"
              "  1_000 + 0x10\n"
              "  1..10\n");
  const sputnik::bytecode::BcCode *code = code_by_id(
      emit_result.module, emit_result.module.methods[0].entry_code_id);
  expect(code != nullptr, "ops code exists");
  expect(contains_opcode(*code, sputnik::bytecode::Opcode::JumpIfFalse),
         "and emits JUMP_IF_FALSE");
  expect(contains_opcode(*code, sputnik::bytecode::Opcode::JumpIfTrue),
         "or emits JUMP_IF_TRUE");
  expect(contains_symbol(emit_result.module, "contains?"),
         "in interns contains?");
  expect(contains_symbol(emit_result.module, "Range"), "range interns Range");
  expect(contains_symbol(emit_result.module, "new"),
         "range interns constructor selector");
  expect(contains_symbol(emit_result.module, "inclusive_end"),
         "range interns inclusive_end keyword");

  bool saw_1000 = false;
  bool saw_16 = false;
  for (const sputnik::bytecode::Constant &constant :
       emit_result.module.const_pool) {
    if (constant.kind != sputnik::bytecode::ConstantKind::Integer) {
      continue;
    }
    saw_1000 = saw_1000 || constant.int_value == 1000;
    saw_16 = saw_16 || constant.int_value == 16;
  }
  expect(saw_1000, "underscored integer canonicalized");
  expect(saw_16, "hex integer canonicalized");
}

void test_native_source_metadata_emission() {
  const sputnik::bytecode::EmitResult emit_result = emit_ok(
      "package sqlite3\n"
      "class Error < NativeError\n"
      "class StepError < Error\n"
      "native class Conn from \"sqlite3.Conn\" owned:\n"
      "  def init(path as Str) from \"amsq_conn_open\"\n"
      "  def destroy!() from \"amsq_conn_free\"\n");

  expect(contains_attr(emit_result.module, "sputnik.native.type:sqlite3.Conn",
                       "sqlite3.Conn\towned\tamsq_conn_free"),
         "native class emits source-contained type metadata");
  expect(contains_attr(emit_result.module, "sputnik.native.error:Sqlite3.Error",
                       "NativeError"),
         "source error emits native error metadata");
  expect(contains_attr(emit_result.module,
                       "sputnik.native.error:Sqlite3.StepError",
                       "Sqlite3.Error"),
         "inherited native error parent metadata");
  expect((emit_result.module.classes[0].flags &
          sputnik::bytecode::kClassFlagNativeError) != 0U,
         "NativeError ancestry marks the root class");
  expect((emit_result.module.classes[1].flags &
          sputnik::bytecode::kClassFlagNativeError) != 0U,
         "NativeError ancestry is inherited");
}

void test_collection_literal_emission() {
  const sputnik::bytecode::EmitResult emit_result =
      emit_ok("[1, 2]\n"
              "(3, 4)\n"
              "{5}\n"
              "{id: :ok}\n"
              "{\"name\": 5, 1: 6}\n");
  const sputnik::bytecode::BcCode *code =
      code_by_kind(emit_result.module, sputnik::bytecode::CodeKind::Module);
  expect(code != nullptr, "module init code exists");
  expect(contains_opcode(*code, sputnik::bytecode::Opcode::MakeList),
         "list literal emits MAKE_LIST");
  expect(contains_opcode(*code, sputnik::bytecode::Opcode::MakeTuple),
         "tuple literal emits MAKE_TUPLE");
  expect(contains_opcode(*code, sputnik::bytecode::Opcode::MakeSet),
         "set literal emits MAKE_SET");
  expect(contains_opcode(*code, sputnik::bytecode::Opcode::MakeMap),
         "symbol-only map literal emits MAKE_MAP");
  expect(contains_opcode(*code, sputnik::bytecode::Opcode::MakeMapDyn),
         "expression-key map literal emits MAKE_MAP_DYN");
  expect(contains_symbol(emit_result.module, "ok"),
         "symbol literal interns symbol");
  expect(contains_symbol(emit_result.module, "id"),
         "identifier map key interns symbol");
  expect(contains_string(emit_result.module, "name"),
         "string map key interns string constant");
}

void test_v20_7_spread_emission() {
  const sputnik::bytecode::EmitResult emit_result =
      emit_ok("def take(a, b:, c:):\n"
              "  a\n"
              "args = [2, 3]\n"
              "opts = {b: 2, c: 3}\n"
              "take(1, *args)\n"
              "take(1, **opts)\n"
              "[0, *args]\n"
              "{0, *args}\n"
              "{a: 1, **opts}\n");
  const sputnik::bytecode::BcCode *code =
      code_by_kind(emit_result.module, sputnik::bytecode::CodeKind::Module);
  expect(code != nullptr, "module init code exists");
  expect(contains_opcode(*code, sputnik::bytecode::Opcode::CallSpread),
         "spread call emits CALL_SPREAD");
  expect(contains_opcode(*code, sputnik::bytecode::Opcode::MakeListSpread),
         "array spread emits MAKE_LIST_SPREAD");
  expect(contains_opcode(*code, sputnik::bytecode::Opcode::MakeSetSpread),
         "set spread emits MAKE_SET_SPREAD");
  expect(contains_opcode(*code, sputnik::bytecode::Opcode::MakeMapSpread),
         "map spread emits MAKE_MAP_SPREAD");

  const sputnik::bytecode::DecodeResult decoded =
      sputnik::bytecode::deserialize_module(
          sputnik::bytecode::serialize_module(emit_result.module));
  expect(decoded.ok(), sputnik::bytecode::verify_errors_to_json(decoded.errors));
}

void test_kernel_watch_emission() {
  const sputnik::bytecode::EmitResult local_result =
      emit_ok("x = 1\n"
              "Kernel.watch(x)\n");
  expect(local_result.module.init.has_entry_code_id,
         "watch local module init exists");
  const sputnik::bytecode::BcCode *init_code =
      code_by_id(local_result.module, local_result.module.init.entry_code_id);
  expect(init_code != nullptr, "watch local init code exists");
  expect(contains_opcode(*init_code, sputnik::bytecode::Opcode::WatchLocal),
         "Kernel.watch(local) emits WATCH_LOCAL");
  const sputnik::bytecode::DecodeResult local_decoded =
      sputnik::bytecode::deserialize_module(
          sputnik::bytecode::serialize_module(local_result.module));
  expect(local_decoded.ok(),
         sputnik::bytecode::verify_errors_to_json(local_decoded.errors));

  const sputnik::bytecode::EmitResult capture_result =
      emit_ok("def watch_each(xs, x):\n"
              "  xs.map: Kernel.watch(x)\n");
  expect(module_contains_opcode(capture_result.module,
                                sputnik::bytecode::Opcode::WatchUpval),
         "Kernel.watch(capture) emits WATCH_UPVAL");

  const sputnik::bytecode::EmitResult ivar_result =
      emit_ok("class Particle:\n"
              "  def observe():\n"
              "    Kernel.watch(@mass)\n");
  expect(module_contains_opcode(ivar_result.module,
                                sputnik::bytecode::Opcode::WatchIvar),
         "Kernel.watch(ivar) emits WATCH_IVAR");
  const sputnik::bytecode::DecodeResult ivar_decoded =
      sputnik::bytecode::deserialize_module(
          sputnik::bytecode::serialize_module(ivar_result.module));
  expect(ivar_decoded.ok(),
         sputnik::bytecode::verify_errors_to_json(ivar_decoded.errors));

  const sputnik::bytecode::EmitResult invalid_result =
      emit_allow_bytecode_diagnostics("x = 1\n"
                                      "Kernel.watch(x + 1)\n");
  expect(!invalid_result.ok(), "invalid watch target emits diagnostic");
  expect(has_diagnostic_code(invalid_result, "BC2008"),
         "invalid watch target diagnostic code");
}

void test_block_param_pattern_emission() {
  const sputnik::bytecode::EmitResult emit_result =
      emit_ok("def transform(xs):\n"
              "  xs.map |[head, *tail]|: head\n");
  expect(emit_result.module.code_objects.size() >= 3,
         "expected block code for pattern-param closure");

  const sputnik::bytecode::BcCode *method_code = code_by_id(
      emit_result.module, emit_result.module.methods[0].entry_code_id);
  expect(method_code != nullptr, "transform code exists");

  const sputnik::bytecode::BcCode *closure_code = nullptr;
  for (const sputnik::bytecode::BcCode &code : emit_result.module.code_objects) {
    if (code.code_id != method_code->code_id &&
        contains_opcode(code, sputnik::bytecode::Opcode::PPrepSeq)) {
      closure_code = &code;
      break;
    }
  }
  expect(closure_code != nullptr, "closure code with pattern prologue exists");
  expect(contains_opcode(*closure_code, sputnik::bytecode::Opcode::PCheckLenGte),
         "block param pattern emits P_CHECK_LEN_GTE");
  expect(contains_opcode(*closure_code, sputnik::bytecode::Opcode::PBind),
         "block param pattern emits P_BIND");
  expect(contains_opcode(*closure_code, sputnik::bytecode::Opcode::PCommit),
         "block param pattern emits P_COMMIT");
  expect(contains_opcode(*closure_code, sputnik::bytecode::Opcode::PFail),
         "block param mismatch path emits P_FAIL");
}

void test_simple_block_param_emission() {
  const sputnik::bytecode::EmitResult emit_result = emit_ok("def transform(xs):\n"
                                                          "  xs.map |x|: x\n");
  expect(emit_result.module.code_objects.size() >= 3,
         "expected block code for simple closure param");

  const sputnik::bytecode::BcCode *method_code = code_by_id(
      emit_result.module, emit_result.module.methods[0].entry_code_id);
  expect(method_code != nullptr, "simple transform code exists");

  const sputnik::bytecode::BcCode *closure_code = nullptr;
  for (const sputnik::bytecode::BcCode &code : emit_result.module.code_objects) {
    if (code.code_id != method_code->code_id &&
        contains_opcode(code, sputnik::bytecode::Opcode::PCommit)) {
      closure_code = &code;
      break;
    }
  }
  expect(closure_code != nullptr, "simple closure code exists");
  expect(contains_opcode(*closure_code, sputnik::bytecode::Opcode::PBind),
         "simple block param emits P_BIND");
  expect(contains_opcode(*closure_code, sputnik::bytecode::Opcode::PCommit),
         "simple block param emits P_COMMIT");
}

void test_object_model_emission() {
  const sputnik::bytecode::EmitResult emit_result =
      emit_ok("package physics.core\n"
              "export Timestamped, Particle\n"
              "\n"
              "mixin Timestamped:\n"
              "  def touch!()\n"
              "\n"
              "class Particle < Entity:\n"
              "  include Timestamped\n"
              "  extend Serializable\n"
              "  class_method def find(id):\n"
              "    id\n"
              "  def init(@масса, α = 1)\n");

  expect(emit_result.module.classes.size() == 2,
         "expected mixin and class descriptors");
  expect(emit_result.module.methods.size() == 3,
         "expected mixin method plus two class methods");

  const sputnik::bytecode::BcClass &mixin = emit_result.module.classes[0];
  expect((mixin.flags & sputnik::bytecode::kClassFlagMixin) != 0U,
         "mixin descriptor carries mixin flag");
  expect(mixin.method_range_start == 0 && mixin.method_range_count == 1,
         "mixin method range");
  expect(mixin.direct_include_refs.empty(),
         "mixin has no direct includes here");

  const sputnik::bytecode::BcClass &klass = emit_result.module.classes[1];
  expect((klass.flags & sputnik::bytecode::kClassFlagMixin) == 0U,
         "class descriptor has no mixin flag");
  expect(klass.has_superclass_ref, "class keeps superclass ref");
  expect(path_constant_text(emit_result.module, klass.superclass_ref) ==
             "Entity",
         "superclass path preserved");
  expect(klass.direct_include_refs.size() == 1, "class preserves include refs");
  expect(path_constant_text(emit_result.module, klass.direct_include_refs[0]) ==
             "Timestamped",
         "include path preserved");
  expect(klass.direct_extend_refs.size() == 1, "class preserves extend refs");
  expect(path_constant_text(emit_result.module, klass.direct_extend_refs[0]) ==
             "Serializable",
         "extend path preserved");
  expect(klass.method_range_start == 1 && klass.method_range_count == 2,
         "class method range");

  expect(emit_result.module.methods[0].owner_dispatch_ref == 0 &&
             emit_result.module.methods[0].flags == 1,
         "mixin method owned by mixin instance side");
  expect(emit_result.module.methods[1].owner_dispatch_ref == 1 &&
             emit_result.module.methods[1].flags == 2,
         "class-side method owned by class descriptor");
  expect(emit_result.module.methods[2].owner_dispatch_ref == 1 &&
             emit_result.module.methods[2].flags == 1,
         "instance method owned by class descriptor");

  expect(emit_result.module.exports.size() == 2,
         "mixin and class exports emitted");
  expect(emit_result.module.exports[0].target_index == 0,
         "mixin export targets first descriptor");
  expect(emit_result.module.exports[1].target_index == 1,
         "class export targets second descriptor");
}

void test_bodyless_class_emission() {
  const sputnik::bytecode::EmitResult emit_result =
      emit_ok("class Empty\n");
  expect(emit_result.ok(), "bodyless class emits cleanly");
  expect(emit_result.module.classes.size() == 1U,
         "bodyless class is emitted");
  expect(emit_result.module.classes[0].method_range_count == 0U,
         "bodyless class has no synthetic body members");
}

void test_property_emission() {
  const sputnik::bytecode::EmitResult emit_result =
      emit_ok("prop answer: 42\n"
              "class User:\n"
              "  prop full_name:\n"
              "    get: @first\n"
              "    set(value): @first = value\n"
              "class Box:\n"
              "  attr var value\n"
              "user.full_name\n"
              "user.full_name = \"Ada\"\n");

  expect(emit_result.module.methods.size() == 5,
         "module getter, property, and attr methods emitted");
  expect((emit_result.module.methods[0].flags &
          sputnik::bytecode::kMethodFlagPropertyGetter) != 0U,
         "module property getter flag emitted");
  expect((emit_result.module.methods[1].flags &
          sputnik::bytecode::kMethodFlagPropertyGetter) != 0U,
         "instance property getter flag emitted");
  expect((emit_result.module.methods[1].flags &
          sputnik::bytecode::kMethodFlagInstance) != 0U,
         "instance property keeps dispatch flag");
  expect((emit_result.module.methods[2].flags &
          sputnik::bytecode::kMethodFlagPropertySetter) != 0U,
         "instance property setter flag emitted");
  expect((emit_result.module.methods[2].flags &
          sputnik::bytecode::kMethodFlagInstance) != 0U,
         "instance property setter keeps dispatch flag");
  expect((emit_result.module.methods[3].flags &
          sputnik::bytecode::kMethodFlagPropertyGetter) != 0U,
         "attr getter flag emitted");
  expect((emit_result.module.methods[3].flags &
          sputnik::bytecode::kMethodFlagAttrReader) != 0U,
         "attr getter direct-reader flag emitted");
  const sputnik::bytecode::BcCode *attr_getter = code_by_id(
      emit_result.module, emit_result.module.methods[3].entry_code_id);
  expect(attr_getter != nullptr && attr_getter->instructions.size() == 4U,
         "attr getter keeps canonical direct-ivar bytecode shape");
  expect(attr_getter->instructions[0].opcode == sputnik::bytecode::Opcode::LoadSelf &&
             attr_getter->instructions[1].opcode ==
                 sputnik::bytecode::Opcode::LoadIvar &&
             attr_getter->instructions[2].opcode ==
                 sputnik::bytecode::Opcode::CloseUpvalues &&
             attr_getter->instructions[3].opcode == sputnik::bytecode::Opcode::Return,
         "attr getter bytecode is safe for the VM direct-reader path");
  expect((emit_result.module.methods[4].flags &
          sputnik::bytecode::kMethodFlagPropertySetter) != 0U,
         "attr setter flag emitted");
  expect(module_contains_callsite_flag(
             emit_result.module,
             sputnik::bytecode::kCallSiteFlagPropertyAccess),
         "bare member property access callsite flag emitted");
  expect(module_contains_callsite_flag(
             emit_result.module,
             sputnik::bytecode::kCallSiteFlagPropertyAssignment),
         "member property assignment callsite flag emitted");
}

void test_try_rescue_ensure_emission() {
  const sputnik::bytecode::EmitResult emit_result =
      emit_ok("try:\n"
              "  raise \"boom\"\n"
              "rescue TypeError:\n"
              "  1\n"
              "rescue:\n"
              "  2\n"
              "ensure:\n"
              "  3\n");

  expect(emit_result.module.init.has_entry_code_id,
         "try module init emitted");
  const sputnik::bytecode::BcCode *code =
      code_by_id(emit_result.module, emit_result.module.init.entry_code_id);
  expect(code != nullptr, "try module init code exists");
  expect(code->handler_table.size() == 2,
         "try emits rescue plus a direct ensure for non-rescuable unwinds");
  expect(sputnik::bytecode::handler_kind(code->handler_table[0].flags) ==
             sputnik::bytecode::kHandlerKindRescue,
         "try rescue handler kind is encoded");
  expect(sputnik::bytecode::handler_kind(code->handler_table[1].flags) ==
             sputnik::bytecode::kHandlerKindEnsure &&
             code->handler_table[0].protected_from == code->handler_table[1].protected_from &&
             code->handler_table[0].protected_to == code->handler_table[1].protected_to,
         "direct ensure covers the same region, after exception-priority rescue");
  expect(contains_opcode(*code, sputnik::bytecode::Opcode::Raise),
         "try body emits RAISE");

  const sputnik::bytecode::BcCode *rescue =
      code_by_kind(emit_result.module, sputnik::bytecode::CodeKind::Rescue);
  expect(rescue != nullptr, "rescue handler code exists");
  expect(contains_opcode(*rescue, sputnik::bytecode::Opcode::TripleEq),
         "typed rescue matcher emits TRIPLE_EQ");

  const sputnik::bytecode::BcCode *ensure =
      code_by_kind(emit_result.module, sputnik::bytecode::CodeKind::Ensure);
  expect(ensure != nullptr, "ensure handler code exists");
}

void test_throw_catch_emission() {
  const sputnik::bytecode::EmitResult emit_result =
      emit_ok("catch(:enough):\n"
              "  throw :enough, 42\n");

  expect(emit_result.module.init.has_entry_code_id,
         "catch module init emitted");
  const sputnik::bytecode::BcCode *code =
      code_by_id(emit_result.module, emit_result.module.init.entry_code_id);
  expect(code != nullptr, "catch module init code exists");
  expect(contains_opcode(*code, sputnik::bytecode::Opcode::Throw),
         "throw body emits THROW");
  expect(code->handler_table.size() == 1,
         "catch emits one protected handler entry");
  expect(sputnik::bytecode::handler_kind(code->handler_table[0].flags) ==
             sputnik::bytecode::kHandlerKindCatch,
         "catch handler kind is encoded");

  const sputnik::bytecode::DecodeResult decoded =
      sputnik::bytecode::deserialize_module(
          sputnik::bytecode::serialize_module(emit_result.module));
  expect(decoded.ok(), sputnik::bytecode::verify_errors_to_json(decoded.errors));
}

void test_integer_specialized_send_emission() {
  const sputnik::bytecode::EmitResult emit_result =
      emit_ok("def fast():\n"
              "  x = 10\n"
              "  y = 3\n"
              "  z = x + y\n"
              "  q = z - 2\n"
              "  product = x * y\n"
              "  quotient = product / y\n"
              "  remainder = product % y\n"
              "  floored = product // y\n"
              "  anded = x & y\n"
              "  ored = x | 4\n"
              "  xored = x ^ y\n"
              "  shifted = x << 2\n"
              "  unshifted = shifted >> 1\n"
              "  cmp = x <=> y\n"
              "  le = q <= x\n"
              "  ge = q >= 0\n"
              "  eq = x == 10\n"
              "  ne = x != y\n"
              "  (q < x) and (q > 0) and le and ge and eq and ne and "
              "(cmp > 0) and (floored > 0) and (remainder >= 0) and "
              "(quotient > 0) and (anded > 0) and (ored > 0) and "
              "(xored > 0) and (shifted > unshifted)\n");

  const sputnik::bytecode::BcCode *code = code_by_id(
      emit_result.module, emit_result.module.methods[0].entry_code_id);
  expect(code != nullptr, "fast code exists");
  expect(contains_opcode(*code, sputnik::bytecode::Opcode::IAdd),
         "integer local addition emits IADD");
  expect(contains_opcode(*code, sputnik::bytecode::Opcode::ISubK),
         "integer literal subtraction emits ISUBK");
  expect(contains_opcode(*code, sputnik::bytecode::Opcode::IMul),
         "integer local multiplication emits IMUL");
  expect(contains_opcode(*code, sputnik::bytecode::Opcode::IDiv),
         "integer local division emits IDIV");
  expect(contains_opcode(*code, sputnik::bytecode::Opcode::IMod),
         "integer local modulo emits IMOD");
  expect(contains_opcode(*code, sputnik::bytecode::Opcode::IFloorDiv),
         "integer local floor division emits IFLOORDIV");
  expect(contains_opcode(*code, sputnik::bytecode::Opcode::IBitAnd),
         "integer local bit and emits IBITAND");
  expect(contains_opcode(*code, sputnik::bytecode::Opcode::IBitOrK),
         "integer literal bit or emits IBITORK");
  expect(contains_opcode(*code, sputnik::bytecode::Opcode::IBitXor),
         "integer local xor emits IBITXOR");
  expect(contains_opcode(*code, sputnik::bytecode::Opcode::IShlK),
         "integer literal left shift emits ISHLK");
  expect(contains_opcode(*code, sputnik::bytecode::Opcode::IShrK),
         "integer literal right shift emits ISHRK");
  expect(contains_opcode(*code, sputnik::bytecode::Opcode::ILt),
         "integer local comparison emits ILT");
  expect(contains_opcode(*code, sputnik::bytecode::Opcode::IGtK),
         "integer literal comparison emits IGTK");
  expect(contains_opcode(*code, sputnik::bytecode::Opcode::ILe),
         "integer <= comparison emits ILE");
  expect(contains_opcode(*code, sputnik::bytecode::Opcode::IGeK),
         "integer >= literal comparison emits IGEK");
  expect(contains_opcode(*code, sputnik::bytecode::Opcode::IEqK),
         "integer equality literal comparison emits IEQK");
  expect(contains_opcode(*code, sputnik::bytecode::Opcode::INe),
         "integer inequality local comparison emits INE");
  expect(contains_opcode(*code, sputnik::bytecode::Opcode::ICmp),
         "integer spaceship comparison emits ICMP");
  expect(!contains_opcode(*code, sputnik::bytecode::Opcode::Send),
         "integer-only method avoids generic SEND");
}

void test_unknown_integer_send_stays_dynamic() {
  const sputnik::bytecode::EmitResult emit_result =
      emit_ok("def add(x, y):\n"
              "  x + y\n");

  const sputnik::bytecode::BcCode *code = code_by_id(
      emit_result.module, emit_result.module.methods[0].entry_code_id);
  expect(code != nullptr, "add code exists");
  expect(contains_opcode(*code, sputnik::bytecode::Opcode::Send),
         "unknown parameter addition keeps SEND");
  expect(!contains_opcode(*code, sputnik::bytecode::Opcode::IAdd),
         "unknown parameter addition does not emit IADD");
}

} // namespace

int main() {
  {
    const auto emitted = emit_ok("package facade\nfrom provider import Thing as Alias\nexport Alias\n");
    const auto &module = emitted.module;
    expect(module.exports.size() == 1, "one imported alias export");
    const auto &entry = module.exports.front();
    expect(entry.has_reexport_module_name &&
           module.strings.at(entry.target_kind_str_id) == "reexport" &&
           module.strings.at(entry.reexport_module_name_str_id) == "provider" &&
           module.strings.at(entry.target_index) == "Thing" &&
           module.symbols.at(entry.symbol_id) == "Alias", "re-export preserves provider and original name");
    expect(sputnik::bytecode::deserialize_module(sputnik::bytecode::serialize_module(module)).ok(),
           "re-export bytecode round trip");
  }
  test_if_and_round_trip();
  test_loop_and_safepoint();
  test_closure_capture_emission();
  test_closure_module_import_reference_verifies();
  test_net_io_stdlib_alias_references_verify();
  test_default_thunk_emission();
  test_type_hook_emission();
  test_keyword_param_emission();
  test_v20_9_named_callable_param_emission();
  test_case_emission();
  test_last_result_elision_without_explicit_last_value();
  test_explicit_last_value_preserves_last_result_updates();
  test_pattern_assignment_emission();
  test_optional_index_emission();
  test_matcher_expr_emission();
  test_dynamic_pattern_emission();
  test_clause_method_emission();
  test_w13_operator_emission();
  test_native_source_metadata_emission();
  test_collection_literal_emission();
  test_v20_7_spread_emission();
  test_kernel_watch_emission();
  test_block_param_pattern_emission();
  test_simple_block_param_emission();
  test_object_model_emission();
  test_bodyless_class_emission();
  test_property_emission();
  test_try_rescue_ensure_emission();
  test_throw_catch_emission();
  test_integer_specialized_send_emission();
  test_unknown_integer_send_stays_dynamic();
  std::cout << "emitter_tests: ok\n";
  return 0;
}
