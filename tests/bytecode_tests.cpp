#include "bytecode/format.h"

#include "frontend/lexer/token.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "bytecode test failed: " << message << "\n";
    std::exit(1);
  }
}

bool has_error_code(const sputnik::bytecode::DecodeResult &result,
                    const std::string &code) {
  for (const sputnik::bytecode::VerifyError &error : result.errors) {
    if (error.code == code) {
      return true;
    }
  }
  return false;
}

void write_u32_le(std::vector<std::uint8_t> &bytes, std::size_t offset,
                  std::uint32_t value) {
  expect(offset + 4U <= bytes.size(), "byte mutation is in range");
  bytes[offset] = static_cast<std::uint8_t>(value & 0xffU);
  bytes[offset + 1U] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
  bytes[offset + 2U] = static_cast<std::uint8_t>((value >> 16U) & 0xffU);
  bytes[offset + 3U] = static_cast<std::uint8_t>((value >> 24U) & 0xffU);
}

std::string path_constant_text(const sputnik::bytecode::BcModule &module,
                               std::uint32_t ref_id) {
  expect(ref_id < module.const_pool.size(), "path ref in range");
  const sputnik::bytecode::Constant &constant = module.const_pool[ref_id];
  expect(constant.kind == sputnik::bytecode::ConstantKind::Path,
         "expected path constant");
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

sputnik::bytecode::BcModule sample_module() {
  using namespace sputnik::bytecode;

  BcModule module;
  module.format_version = {1, 0};
  module.language_version = {1, 0};
  module.profile_flags = 1;
  module.file_flags = 2;
  for (std::size_t i = 0; i < module.abi_hash.size(); ++i) {
    module.abi_hash[i] = static_cast<std::uint8_t>(i);
  }

  module.strings = {"x",        "param",      "local", "method",
                    "std/core", "build.mode", "debug"};
  module.symbols = {"compute"};
  Constant integer_constant;
  integer_constant.kind = ConstantKind::Integer;
  integer_constant.int_value = 42;
  Constant string_ref_constant;
  string_ref_constant.kind = ConstantKind::StringRef;
  string_ref_constant.ref_id = 0;
  Constant symbol_ref_constant;
  symbol_ref_constant.kind = ConstantKind::SymbolRef;
  symbol_ref_constant.ref_id = 0;
  module.const_pool = {integer_constant, string_ref_constant,
                       symbol_ref_constant};

  BcCode code;
  code.code_id = 7;
  code.kind = CodeKind::Method;
  code.reg_count = 2;
  code.local_layout.push_back({0, 0, 1, 2});
  code.instructions.push_back({Opcode::GetLast, {{0, false}}});
  code.instructions.push_back({Opcode::Jump, {InstructionOperand{0, false}}});
  code.instructions.push_back({Opcode::Return, {{0, false}}});
  code.safepoint_table.push_back({0, 0});
  code.source_spans.push_back(
      {0, 2, {"/tmp/sample.s", {1, 1, 0}, {1, 14, 13}}});
  module.code_objects.push_back(code);

  BcMethod method;
  method.selector_sym_id = 0;
  method.signature_blob_id = 0;
  method.clause_table.push_back({1, 7, 7, 7, 0});
  method.entry_code_id = 7;
  module.methods.push_back(method);

  sputnik::bytecode::DepEntry dependency;
  dependency.module_name_str_id = 4;
  dependency.required_format = {1, 0};
  dependency.min_language_version = {1, 0};
  module.dependencies.push_back(dependency);
  module.exports.push_back({0, 3, 0, 1});
  module.init = {true, 7, 0};
  module.pattern_programs.push_back({1, 1, 0});
  module.line_table.push_back({7, 0, 1});
  module.line_table.push_back({7, 2, 2});
  module.local_debug.push_back({7, 0, 0, 0, 3});
  module.attrs.push_back({5, 6});
  module.required_features = {"core.v1"};
  module.optional_features = {"typed.v1"};
  module.forbidden_features = {"ffi.v1"};
  module.capabilities.push_back(
      sputnik::capability::make_capability("fs.read", "./data"));
  module.effects.push_back(sputnik::effect::make_effect_summary(
      "compute", "function", {"fs"}, {"fs"}, true));
  module.observability_sites.push_back(
      {1,
       "task.started",
       "runtime",
       "compute",
       {"sample.s", 1, 1},
       sputnik::replay::kObservabilitySiteFlagRequired});
  module.replay_metadata.required_event_names = {"task.started",
                                                 "task.completed"};
  module.replay_metadata.deterministic_sources = {"time", "random"};
  module.replay_metadata.flags =
      sputnik::replay::kReplayMetadataFlagDeterministic;
  sputnik::data::SchemaDefinition order_v1;
  order_v1.name = "Order";
  order_v1.version = 1;
  order_v1.fields.push_back({"id",
                             "integer",
                             true,
                             false,
                             {},
                             sputnik::data::kSchemaFieldFlagPrimaryKey});
  order_v1.fields.push_back({"amount", "float", true, false, {}, 0});
  sputnik::data::SchemaDefinition order_v2 = order_v1;
  order_v2.version = 2;
  order_v2.fields.push_back({"status", "string", false, false, "new", 0});
  module.schemas = {order_v1, order_v2};
  module.schema_migrations.push_back(
      {"Order", 1, 2, "compatible",
       sputnik::data::kSchemaMigrationFlagCompatible});
  sputnik::data::TablePlan high_value;
  high_value.plan_id = "orders.high_value";
  high_value.op = "filter";
  high_value.input_refs = {"orders"};
  high_value.arguments = {"amount > 100"};
  high_value.column_dependencies = {{"orders", "amount"}};
  high_value.effect_row = {};
  high_value.flags = sputnik::data::kTablePlanFlagLazy;
  module.table_plans.push_back(high_value);
  sputnik::wasm_accel::WasmInterfaceEntry import_entry;
  import_entry.name = "fs.read";
  import_entry.kind = "resource";
  import_entry.type_signature = "resource";
  import_entry.capability =
      sputnik::capability::make_capability("fs.read", "./data");
  sputnik::wasm_accel::WasmInterfaceEntry export_entry;
  export_entry.name = "normalize";
  export_entry.kind = "func";
  export_entry.type_signature = "(Order) -> Order";
  export_entry.schema_name = "Order";
  sputnik::wasm_accel::WasmComponent component;
  component.name = "analytics.plugin";
  component.world = "analytics-plugin";
  component.flags = sputnik::wasm_accel::kWasmComponentFlagFrozenWorld |
                    sputnik::wasm_accel::kWasmComponentFlagRawFfiDenied |
                    sputnik::wasm_accel::kWasmComponentFlagWorldMutationDenied;
  component.imports.push_back(import_entry);
  component.exports.push_back(export_entry);
  module.wasm_components.push_back(component);
  sputnik::wasm_accel::AcceleratorKernel kernel;
  kernel.kernel_id = "scale.f32";
  kernel.entry = "scale";
  kernel.target = "gpu";
  kernel.effect_row = {"gpu"};
  kernel.params.push_back({"xs", "Tensor[F32]", "device", 0});
  kernel.params.push_back({"factor", "F32", "scalar", 0});
  kernel.flags = sputnik::wasm_accel::kAcceleratorKernelFlagPureHelpersOnly;
  module.accelerator_kernels.push_back(kernel);
  sputnik::modern::AgentSymbol symbol;
  symbol.symbol_id = "main::compute";
  symbol.name = "compute";
  symbol.kind = "function";
  symbol.module = "main";
  symbol.visibility = "public";
  symbol.source = {"sample.s", 1, 1};
  symbol.defined_in = "sample.s";
  symbol.effect_summary = "!{fs}";
  module.agent_symbols.push_back(symbol);
  sputnik::modern::AgentPatch patch;
  patch.patch_id = "patch.rename_compute";
  patch.intent = "rename_symbol";
  patch.tool = "agent";
  patch.request_digest = "abc123";
  patch.capabilities = {"refactor"};
  patch.operations.push_back({"rename", "main::compute", "calculate", 0});
  module.agent_patches.push_back(patch);
  sputnik::modern::ProvenanceRecord provenance;
  provenance.patch_id = "patch.rename_compute";
  provenance.tool = "agent";
  provenance.request_digest = "abc123";
  provenance.files_changed = {"sample.s"};
  provenance.symbols_changed = {"main::compute"};
  provenance.checks_run = {"bind", "typed"};
  module.provenance_records.push_back(provenance);
  sputnik::modern::ContractSpec contract;
  contract.owner = "Account.withdraw";
  contract.kind = "ensure";
  contract.expression = "result.balance >= 0";
  contract.effect_row = {"mut"};
  module.contracts.push_back(contract);
  sputnik::modern::PropertySpec property;
  property.name = "reverse_twice";
  property.owner = "Array";
  property.seed = 42;
  property.generator = "Array[Int]";
  property.profile_set = {"contracts", "replay"};
  module.properties.push_back(property);
  module.privacy_labels.push_back({"pii", "pii", 0});
  module.privacy_policies.push_back(
      {"PrivateAudit", "redact", "pii", {}, 0, 0});
  sputnik::modern::LineageNode lineage;
  lineage.node_id = "transform.users";
  lineage.kind = "transform";
  lineage.output = "users.redacted";
  lineage.labels = {"pii"};
  module.lineage_nodes.push_back(lineage);
  sputnik::modern::WorkflowStep step;
  step.workflow = "ImportOrders";
  step.name = "commit";
  step.effect_row = {"db"};
  step.idempotency_key = "batch-1";
  module.workflow_steps.push_back(step);
  sputnik::modern::WorkflowHistoryEvent event;
  event.workflow_id = "ImportOrders/1";
  event.workflow_version = 1;
  event.step = "commit";
  event.event = "commit";
  event.input_digest = "in-a";
  event.output_digest = "out-a";
  event.idempotency_key = "batch-1";
  module.workflow_history.push_back(event);
  module.hashes.push_back(
      {SectionKind::Code, std::vector<std::uint8_t>(32, 0xAB)});
  return module;
}

std::string bytes_hash(const std::vector<std::uint8_t> &bytes) {
  const std::string binary(bytes.begin(), bytes.end());
  return sputnik::lexer::sha256_hex(binary);
}

void test_round_trip_and_dump() {
  const sputnik::bytecode::BcModule module = sample_module();
  const std::vector<std::uint8_t> bytes =
      sputnik::bytecode::serialize_module(module);
  sputnik::bytecode::DecodeResult decoded =
      sputnik::bytecode::deserialize_module(bytes);
  expect(decoded.ok(), sputnik::bytecode::verify_errors_to_json(decoded.errors));

  const std::string artifact_hash = bytes_hash(bytes);
  const std::string dump1 = sputnik::bytecode::module_to_json(
      decoded.module, decoded.sections, artifact_hash);
  const std::vector<std::uint8_t> bytes2 =
      sputnik::bytecode::serialize_module(decoded.module);
  expect(bytes == bytes2,
         "serialize -> deserialize -> serialize is not stable");

  sputnik::bytecode::DecodeResult decoded2 =
      sputnik::bytecode::deserialize_module(bytes2);
  expect(decoded2.ok(),
         sputnik::bytecode::verify_errors_to_json(decoded2.errors));
  const std::string dump2 = sputnik::bytecode::module_to_json(
      decoded2.module, decoded2.sections, bytes_hash(bytes2));
  expect(dump1 == dump2, "JSON dump changed across round-trip");
  expect(decoded.sections.size() == 28,
         "expected required and optional sections");
}

void test_notebook_format_minor_version_boundary() {
  using namespace sputnik::bytecode;

  BcModule supported = sample_module();
  supported.format_version = {1, 1};
  const DecodeResult supported_decoded =
      deserialize_module(serialize_module(supported));
  expect(supported_decoded.ok(), "bytecode format 1.1 should be supported");

  BcModule future = sample_module();
  future.format_version = {1, 2};
  const DecodeResult future_decoded =
      deserialize_module(serialize_module(future));
  expect(!future_decoded.ok(), "future bytecode minor unexpectedly accepted");
  expect(has_error_code(future_decoded, "BC1003"),
         "expected BC1003 for unsupported future bytecode minor");
}

void test_notebook_cell_round_trip() {
  using namespace sputnik::bytecode;

  expect(static_cast<std::uint8_t>(Opcode::PFail) == 0x4cU,
         "existing opcode value changed");
  expect(static_cast<std::uint8_t>(Opcode::LoadNotebookSlot) == 0x4dU,
         "notebook slot load opcode has the wrong ABI value");
  expect(static_cast<std::uint8_t>(Opcode::StoreNotebookSlot) == 0x4eU,
         "notebook slot store opcode has the wrong ABI value");
  expect(static_cast<std::uint8_t>(CodeKind::Module) == 0U,
         "module code kind value changed");
  expect(static_cast<std::uint8_t>(CodeKind::Method) == 1U,
         "method code kind value changed");
  expect(static_cast<std::uint8_t>(CodeKind::Block) == 2U,
         "block code kind value changed");
  expect(static_cast<std::uint8_t>(CodeKind::Ensure) == 3U,
         "ensure code kind value changed");
  expect(static_cast<std::uint8_t>(CodeKind::Rescue) == 4U,
         "rescue code kind value changed");
  expect(static_cast<std::uint8_t>(CodeKind::DefaultThunk) == 5U,
         "default thunk code kind value changed");
  expect(static_cast<std::uint8_t>(CodeKind::NotebookCell) == 6U,
         "notebook cell code kind value is not the extension value");

  BcModule module = sample_module();
  module.format_version = {1, 1};
  BcCode notebook_provider = module.code_objects.front();
  notebook_provider.code_id = 8;
  notebook_provider.kind = CodeKind::NotebookCell;
  notebook_provider.instructions = {
      {Opcode::LoadK, {{0, false}, {0, false}}},
      {Opcode::StoreNotebookSlot, {{17, false}, {0, false}}},
      {Opcode::Return, {{0, false}}},
  };
  BcCode notebook_cell = module.code_objects.front();
  notebook_cell.code_id = 9;
  notebook_cell.kind = CodeKind::NotebookCell;
  notebook_cell.instructions = {
      {Opcode::LoadNotebookSlot, {{0, false}, {17, false}}},
      {Opcode::StoreNotebookSlot, {{18, false}, {0, false}}},
      {Opcode::Return, {{0, false}}},
  };
  module.code_objects.push_back(notebook_provider);
  module.code_objects.push_back(notebook_cell);
  module.file_flags |= kFileFlagNotebookOnly;
  NotebookMetadata notebook_metadata;
  notebook_metadata.descriptors.push_back({17, 41, 0});
  notebook_metadata.descriptors.push_back({18, 42, 0});
  notebook_metadata.cells.push_back({8, 41, {}, {17}});
  notebook_metadata.cells.push_back({9, 42, {17}, {18}});
  module.notebook_metadata = notebook_metadata;

  const std::vector<std::uint8_t> bytes = serialize_module(module);
  const DecodeResult decoded = deserialize_module(bytes);
  expect(decoded.ok(), verify_errors_to_json(decoded.errors));
  expect(decoded.module.code_objects.size() == 3U,
         "notebook cell code objects were not preserved");
  expect(decoded.module.code_objects[2].kind == CodeKind::NotebookCell,
         "notebook cell kind was not preserved");
  expect(decoded.module.code_objects[2].instructions[0].opcode ==
             Opcode::LoadNotebookSlot,
         "notebook slot load opcode was not preserved");
  expect(decoded.module.code_objects[2].instructions[1].opcode ==
             Opcode::StoreNotebookSlot,
         "notebook slot store opcode was not preserved");
  expect(decoded.module.notebook_metadata.has_value(),
         "notebook metadata sidecar was not preserved");
  expect(decoded.module.notebook_metadata->schema_version.major == 1U &&
             decoded.module.notebook_metadata->schema_version.minor == 0U &&
             decoded.module.notebook_metadata->descriptors.size() == 2U &&
             decoded.module.notebook_metadata->cells.size() == 2U,
         "notebook metadata sidecar record counts were not preserved");
  expect(decoded.module.notebook_metadata->cells[1].input_descriptor_ids ==
             std::vector<std::uint32_t>{17} &&
             decoded.module.notebook_metadata->cells[1].output_descriptor_ids ==
                 std::vector<std::uint32_t>{18},
         "notebook metadata ordered direction sets were not preserved");
  expect(opcode_name(Opcode::LoadNotebookSlot) == "LOAD_NOTEBOOK_SLOT" &&
             opcode_name(Opcode::StoreNotebookSlot) == "STORE_NOTEBOOK_SLOT",
         "notebook slot opcode has the wrong name");
  expect(code_kind_name(decoded.module.code_objects[1].kind) == "notebook_cell",
         "notebook cell kind has the wrong name");
  expect(module_to_disasm(decoded.module, decoded.sections, bytes_hash(bytes))
             .find("kind=notebook_cell") != std::string::npos,
         "notebook cell kind is missing from disassembly");
  expect(bytes == serialize_module(decoded.module),
         "notebook cell round-trip is not byte stable");
}

void test_unknown_code_kind_rejected() {
  using namespace sputnik::bytecode;

  BcModule module = sample_module();
  module.code_objects[0].kind = static_cast<CodeKind>(0xffU);
  const DecodeResult decoded = deserialize_module(serialize_module(module));
  expect(!decoded.ok(), "unknown code kind unexpectedly decoded");
  expect(has_error_code(decoded, "BC1308"),
         "expected BC1308 for unknown code kind");
}

void test_notebook_cell_cannot_be_an_ordinary_entry() {
  using namespace sputnik::bytecode;

  BcModule module = sample_module();
  BcCode notebook_cell = module.code_objects.front();
  notebook_cell.code_id = 8;
  notebook_cell.kind = CodeKind::NotebookCell;
  module.code_objects.push_back(notebook_cell);
  module.init.entry_code_id = notebook_cell.code_id;

  const DecodeResult decoded = deserialize_module(serialize_module(module));
  expect(!decoded.ok(),
         "notebook cell was accepted as an ordinary module entry");
  expect(has_error_code(decoded, "BC1316"),
         "expected BC1316 for an ordinary notebook-cell entry");
}

void test_notebook_slot_opcodes_require_notebook_cell() {
  using namespace sputnik::bytecode;

  BcModule module = sample_module();
  module.code_objects[0].instructions = {
      {Opcode::LoadNotebookSlot, {{0, false}, {17, false}}},
      {Opcode::Return, {{0, false}}},
  };
  module.line_table[1].pc = 1;
  const DecodeResult decoded = deserialize_module(serialize_module(module));
  expect(!decoded.ok(),
         "notebook slot opcode was accepted by ordinary code");
  expect(has_error_code(decoded, "BC1317"),
         "expected BC1317 for notebook slot opcode in ordinary code");
}

sputnik::bytecode::BcModule valid_notebook_metadata_module() {
  using namespace sputnik::bytecode;
  BcModule module = sample_module();
  module.format_version = {1, 1};
  BcCode provider = module.code_objects.front();
  provider.code_id = 8;
  provider.kind = CodeKind::NotebookCell;
  provider.instructions = {
      {Opcode::LoadK, {{0, false}, {0, false}}},
      {Opcode::StoreNotebookSlot, {{17, false}, {0, false}}},
      {Opcode::Return, {{0, false}}},
  };
  BcCode cell = module.code_objects.front();
  cell.code_id = 9;
  cell.kind = CodeKind::NotebookCell;
  cell.instructions = {
      {Opcode::LoadNotebookSlot, {{0, false}, {17, false}}},
      {Opcode::StoreNotebookSlot, {{18, false}, {0, false}}},
      {Opcode::Return, {{0, false}}},
  };
  module.code_objects.push_back(provider);
  module.code_objects.push_back(cell);
  module.file_flags |= kFileFlagNotebookOnly;
  NotebookMetadata metadata;
  metadata.descriptors = {{17, 41, 0}, {18, 42, 0}};
  metadata.cells = {{8, 41, {}, {17}}, {9, 42, {17}, {18}}};
  module.notebook_metadata = std::move(metadata);
  return module;
}

void test_notebook_metadata_negative_cases() {
  using namespace sputnik::bytecode;

  {
    BcModule module = valid_notebook_metadata_module();
    module.format_version = {1, 0};
    const DecodeResult decoded = deserialize_module(serialize_module(module));
    expect(!decoded.ok(), "NBMD in bytecode format 1.0 unexpectedly accepted");
    expect(has_error_code(decoded, "BC1444"),
           "expected BC1444 for NBMD before bytecode format 1.1");
  }
  {
    BcModule module = valid_notebook_metadata_module();
    module.file_flags &= ~kFileFlagNotebookOnly;
    const DecodeResult decoded = deserialize_module(serialize_module(module));
    expect(!decoded.ok(), "NBMD in ordinary image unexpectedly accepted");
    expect(has_error_code(decoded, "BC1420"),
           "expected BC1420 for NBMD in ordinary image");
  }
  {
    BcModule module = valid_notebook_metadata_module();
    module.notebook_metadata.reset();
    const DecodeResult decoded = deserialize_module(serialize_module(module));
    expect(!decoded.ok(), "notebook-only image without NBMD accepted");
    expect(has_error_code(decoded, "BC1421"),
           "expected BC1421 for missing NBMD");
  }
  {
    BcModule module = valid_notebook_metadata_module();
    module.notebook_metadata->schema_version.minor = 1;
    const DecodeResult decoded = deserialize_module(serialize_module(module));
    expect(!decoded.ok(), "unsupported NBMD schema unexpectedly accepted");
    expect(has_error_code(decoded, "BC1440"),
           "expected BC1440 for unsupported NBMD schema");
  }
  {
    BcModule module = valid_notebook_metadata_module();
    std::vector<std::uint8_t> bytes = serialize_module(module);
    const DecodeResult baseline = deserialize_module(bytes);
    expect(baseline.ok(), "valid NBMD baseline did not decode");
    std::size_t nbmd_offset = 0;
    for (const SectionEntry &section : baseline.sections) {
      if (section.kind == SectionKind::Nbmd) {
        nbmd_offset = static_cast<std::size_t>(section.offset);
        break;
      }
    }
    expect(nbmd_offset != 0U, "NBMD section offset was not found");

    // The count is syntactically valid but cannot fit even one 16-byte
    // descriptor in the section. The decoder must reject before reserve().
    write_u32_le(bytes, nbmd_offset + 4U,
                 std::numeric_limits<std::uint32_t>::max());
    DecodeResult decoded;
    bool threw = false;
    try {
      decoded = deserialize_module(bytes);
    } catch (...) {
      threw = true;
    }
    expect(!threw, "malformed NBMD descriptor count threw an exception");
    expect(!decoded.ok() && has_error_code(decoded, "BC1445"),
           "expected BC1445 for oversized NBMD descriptor count");
  }
  {
    BcModule module = valid_notebook_metadata_module();
    std::vector<std::uint8_t> bytes = serialize_module(module);
    const DecodeResult baseline = deserialize_module(bytes);
    expect(baseline.ok(), "valid NBMD baseline did not decode");
    std::size_t nbmd_offset = 0;
    std::size_t nbmd_directory_index = 0;
    bool found_nbmd = false;
    for (std::size_t index = 0; index < baseline.sections.size(); ++index) {
      if (baseline.sections[index].kind == SectionKind::Nbmd) {
        nbmd_offset = static_cast<std::size_t>(baseline.sections[index].offset);
        nbmd_directory_index = index;
        found_nbmd = true;
        break;
      }
    }
    expect(found_nbmd, "NBMD section directory entry was not found");

    // Keep only the schema and descriptor count, then advertise UINT32_MAX
    // descriptors. This exercises both truncation and the count guard.
    write_u32_le(bytes, nbmd_offset + 4U,
                 std::numeric_limits<std::uint32_t>::max());
    write_u32_le(bytes, 56U + nbmd_directory_index * 24U + 12U, 8U);
    bytes.resize(nbmd_offset + 8U);
    DecodeResult decoded;
    bool threw = false;
    try {
      decoded = deserialize_module(bytes);
    } catch (...) {
      threw = true;
    }
    expect(!threw, "truncated NBMD count threw an exception");
    expect(!decoded.ok() && has_error_code(decoded, "BC1445"),
           "expected BC1445 for truncated oversized NBMD count");
  }
  {
    BcModule module = valid_notebook_metadata_module();
    std::vector<std::uint8_t> bytes = serialize_module(module);
    const DecodeResult baseline = deserialize_module(bytes);
    expect(baseline.ok(), "valid NBMD baseline did not decode");
    std::size_t nbmd_offset = 0;
    for (const SectionEntry &section : baseline.sections) {
      if (section.kind == SectionKind::Nbmd) {
        nbmd_offset = static_cast<std::size_t>(section.offset);
        break;
      }
    }
    expect(nbmd_offset != 0U, "NBMD section offset was not found");

    // Cell 2's input descriptor array starts at offset 80 in this compact
    // two-cell fixture. Its UINT32_MAX count must not trigger a huge reserve.
    write_u32_le(bytes, nbmd_offset + 80U,
                 std::numeric_limits<std::uint32_t>::max());
    DecodeResult decoded;
    bool threw = false;
    try {
      decoded = deserialize_module(bytes);
    } catch (...) {
      threw = true;
    }
    expect(!threw, "malformed NBMD array count threw an exception");
    expect(!decoded.ok() && has_error_code(decoded, "BC1445"),
           "expected BC1445 for oversized NBMD array count");
  }
  {
    BcModule module = valid_notebook_metadata_module();
    module.notebook_metadata->descriptors.push_back({17, 43, 0});
    const DecodeResult decoded = deserialize_module(serialize_module(module));
    expect(!decoded.ok(), "duplicate descriptor id unexpectedly accepted");
    expect(has_error_code(decoded, "BC1422"),
           "expected BC1422 for duplicate descriptor id");
  }
  {
    BcModule module = valid_notebook_metadata_module();
    module.notebook_metadata->descriptors[0].cell_id = 0;
    const DecodeResult decoded = deserialize_module(serialize_module(module));
    expect(!decoded.ok(), "zero descriptor CellId unexpectedly accepted");
    expect(has_error_code(decoded, "BC1423"),
           "expected BC1423 for zero descriptor CellId");
  }
  {
    BcModule module = valid_notebook_metadata_module();
    module.notebook_metadata->descriptors[1].name_str_id = 99;
    const DecodeResult decoded = deserialize_module(serialize_module(module));
    expect(!decoded.ok(), "invalid descriptor name ref unexpectedly accepted");
    expect(has_error_code(decoded, "BC1424"),
           "expected BC1424 for invalid descriptor name ref");
  }
  {
    BcModule module = valid_notebook_metadata_module();
    module.notebook_metadata->descriptors[1].name_str_id = 0;
    module.notebook_metadata->descriptors[1].cell_id = 41;
    const DecodeResult decoded = deserialize_module(serialize_module(module));
    expect(!decoded.ok(), "duplicate BindingKey unexpectedly accepted");
    expect(has_error_code(decoded, "BC1426"),
           "expected BC1426 for duplicate BindingKey");
  }
  {
    BcModule module = valid_notebook_metadata_module();
    module.notebook_metadata->cells[1].output_descriptor_ids = {17};
    const DecodeResult decoded = deserialize_module(serialize_module(module));
    expect(!decoded.ok(), "output descriptor from another cell accepted");
    expect(has_error_code(decoded, "BC1432"),
           "expected BC1432 for output CellId mismatch");
  }
  {
    BcModule module = valid_notebook_metadata_module();
    module.notebook_metadata->cells[1].input_descriptor_ids.clear();
    const DecodeResult decoded = deserialize_module(serialize_module(module));
    expect(!decoded.ok(), "undeclared LOAD descriptor unexpectedly accepted");
    expect(has_error_code(decoded, "BC1433") &&
               has_error_code(decoded, "BC1438"),
           "expected exact input direction validation errors");
  }
  {
    BcModule module = valid_notebook_metadata_module();
    module.notebook_metadata->descriptors[0].cell_id = 99;
    const DecodeResult decoded = deserialize_module(serialize_module(module));
    expect(!decoded.ok(), "missing notebook provider cell unexpectedly accepted");
    expect(has_error_code(decoded, "BC1441"),
           "expected BC1441 for missing notebook provider cell");
  }
  {
    BcModule module = valid_notebook_metadata_module();
    module.notebook_metadata->cells[0].output_descriptor_ids.clear();
    const DecodeResult decoded = deserialize_module(serialize_module(module));
    expect(!decoded.ok(), "input without provider output unexpectedly accepted");
    expect(has_error_code(decoded, "BC1442") &&
               has_error_code(decoded, "BC1443"),
           "expected provider output and orphan descriptor validation errors");
  }
  {
    BcModule module = valid_notebook_metadata_module();
    module.strings.push_back("orphan");
    module.notebook_metadata->descriptors.push_back(
        {19, 41, static_cast<std::uint32_t>(module.strings.size() - 1U)});
    const DecodeResult decoded = deserialize_module(serialize_module(module));
    expect(!decoded.ok(), "orphan notebook descriptor unexpectedly accepted");
    expect(has_error_code(decoded, "BC1443"),
           "expected BC1443 for orphan notebook descriptor");
  }
  {
    BcModule module = valid_notebook_metadata_module();
    module.notebook_metadata->cells.clear();
    const DecodeResult decoded = deserialize_module(serialize_module(module));
    expect(!decoded.ok(), "NotebookCell without manifest unexpectedly accepted");
    expect(has_error_code(decoded, "BC1430"),
           "expected BC1430 for missing cell manifest");
  }
  {
    BcModule module = valid_notebook_metadata_module();
    module.notebook_metadata->cells[0].code_id = 7;
    const DecodeResult decoded = deserialize_module(serialize_module(module));
    expect(!decoded.ok(), "ordinary code manifest unexpectedly accepted");
    expect(has_error_code(decoded, "BC1429"),
           "expected BC1429 for non-notebook manifest target");
  }
}

void test_disasm_is_stable() {
  const sputnik::bytecode::BcModule module = sample_module();
  const std::vector<std::uint8_t> bytes =
      sputnik::bytecode::serialize_module(module);
  sputnik::bytecode::DecodeResult decoded =
      sputnik::bytecode::deserialize_module(bytes);
  expect(decoded.ok(), sputnik::bytecode::verify_errors_to_json(decoded.errors));

  const std::string disasm = sputnik::bytecode::module_to_disasm(
      decoded.module, decoded.sections, bytes_hash(bytes));
  expect(disasm.find(".header format=1.0 language=1.0") != std::string::npos,
         "missing header line in disasm");
  expect(disasm.find("0001 JUMP 0") != std::string::npos,
         "missing canonical jump line in disasm");
  expect(
      disasm.find("export y0(compute) kind=s3(method) target=0 visibility=1") !=
          std::string::npos,
      "missing export line in disasm");
  expect(disasm.find(".hash\n  CODE sha256=") != std::string::npos,
         "missing hash section in disasm");
  expect(disasm.find(".prof\n  required=\"core.v1\"") != std::string::npos &&
             disasm.find("optional=\"typed.v1\"") != std::string::npos,
         "missing profile section in disasm");
  expect(disasm.find(".caps\n  fs.read target=\"./data\"") != std::string::npos,
         "missing capability section in disasm");
  expect(disasm.find(".efct\n  compute kind=\"function\" declared=\"!{fs}\"") !=
             std::string::npos,
         "missing effect section in disasm");
  expect(disasm.find(".obsv\n  site=1 event=\"task.started\"") !=
             std::string::npos,
         "missing observability section in disasm");
  expect(disasm.find(".rply flags=1") != std::string::npos &&
             disasm.find("deterministic_source=\"random\"") !=
                 std::string::npos,
         "missing replay section in disasm");
  expect(disasm.find(".scma\n  schema Order version=1") != std::string::npos &&
             disasm.find("migration Order 1->2 kind=\"compatible\"") !=
                 std::string::npos,
         "missing schema section in disasm");
  expect(disasm.find(".tabl\n  plan orders.high_value op=\"filter\"") !=
                 std::string::npos &&
             disasm.find("dep=\"orders.amount\"") != std::string::npos,
         "missing table section in disasm");
  expect(disasm.find(".wasm\n  component analytics.plugin") !=
                 std::string::npos &&
             disasm.find("export normalize kind=\"func\"") != std::string::npos,
         "missing wasm section in disasm");
  expect(disasm.find(".accl\n  kernel scale.f32 entry=\"scale\"") !=
                 std::string::npos &&
             disasm.find("param xs type=\"Tensor[F32]\"") != std::string::npos,
         "missing accelerator section in disasm");
  expect(disasm.find(".agnt\n  symbol main::compute") != std::string::npos &&
             disasm.find("patch patch.rename_compute") != std::string::npos,
         "missing agent tooling section in disasm");
  expect(disasm.find(".cntr\n  contract Account.withdraw") != std::string::npos,
         "missing contract section in disasm");
  expect(disasm.find(".priv\n  label pii") != std::string::npos &&
             disasm.find("lineage transform.users") != std::string::npos,
         "missing privacy section in disasm");
  expect(disasm.find(".wflw\n  step ImportOrders.commit") != std::string::npos,
         "missing workflow section in disasm");
}

void test_bad_magic_rejected() {
  std::vector<std::uint8_t> bytes =
      sputnik::bytecode::serialize_module(sample_module());
  bytes[0] = 'X';
  const sputnik::bytecode::DecodeResult decoded =
      sputnik::bytecode::deserialize_module(bytes);
  expect(!decoded.ok(), "bad magic unexpectedly decoded");
  expect(has_error_code(decoded, "BC1001"), "expected BC1001 for bad magic");
}

void test_missing_required_section_rejected() {
  std::vector<std::uint8_t> bytes =
      sputnik::bytecode::serialize_module(sample_module());
  expect(bytes.size() > 60, "fixture too small for section directory mutation");
  bytes[56] = 'X';
  bytes[57] = 'X';
  bytes[58] = 'X';
  bytes[59] = 'X';
  const sputnik::bytecode::DecodeResult decoded =
      sputnik::bytecode::deserialize_module(bytes);
  expect(!decoded.ok(), "unknown section tag unexpectedly decoded");
  expect(has_error_code(decoded, "BC1105"),
         "expected BC1105 for unknown section");
  expect(has_error_code(decoded, "BC1102"),
         "expected BC1102 for missing required section");
}

void test_invalid_code_ref_rejected() {
  sputnik::bytecode::BcModule module = sample_module();
  module.init.entry_code_id = 99;
  const sputnik::bytecode::DecodeResult decoded =
      sputnik::bytecode::deserialize_module(
          sputnik::bytecode::serialize_module(module));
  expect(!decoded.ok(), "invalid init code ref unexpectedly decoded");
  expect(has_error_code(decoded, "BC1204"), "expected BC1204 for bad code ref");
}

void test_back_edge_requires_safepoint() {
  sputnik::bytecode::BcModule module = sample_module();
  module.code_objects[0].safepoint_table.clear();
  const sputnik::bytecode::DecodeResult decoded =
      sputnik::bytecode::deserialize_module(
          sputnik::bytecode::serialize_module(module));
  expect(!decoded.ok(), "missing safepoint unexpectedly accepted");
  expect(has_error_code(decoded, "BC1303"),
         "expected BC1303 for back-edge without safepoint");
}

void test_register_range_rejected() {
  sputnik::bytecode::BcModule module = sample_module();
  module.code_objects[0].instructions.clear();
  module.code_objects[0].instructions.push_back(
      {sputnik::bytecode::Opcode::LoadK, {{2, false}, {0, false}}});
  module.code_objects[0].instructions.push_back(
      {sputnik::bytecode::Opcode::Return, {{0, false}}});
  module.code_objects[0].safepoint_table.clear();
  const sputnik::bytecode::DecodeResult decoded =
      sputnik::bytecode::deserialize_module(
          sputnik::bytecode::serialize_module(module));
  expect(!decoded.ok(), "out-of-range register unexpectedly accepted");
  expect(has_error_code(decoded, "BC1311"),
         "expected BC1311 for out-of-range register");
}

void test_uninitialized_register_read_rejected() {
  sputnik::bytecode::BcModule module = sample_module();
  module.code_objects[0].instructions.clear();
  module.code_objects[0].instructions.push_back(
      {sputnik::bytecode::Opcode::Move, {{1, false}, {0, false}}});
  module.code_objects[0].instructions.push_back(
      {sputnik::bytecode::Opcode::Return, {{1, false}}});
  module.code_objects[0].local_layout.clear();
  module.code_objects[0].safepoint_table.clear();
  const sputnik::bytecode::DecodeResult decoded =
      sputnik::bytecode::deserialize_module(
          sputnik::bytecode::serialize_module(module));
  expect(!decoded.ok(), "uninitialized register read unexpectedly accepted");
  expect(has_error_code(decoded, "BC1313"),
         "expected BC1313 for uninitialized register read");
}

void test_branch_join_initializedness_rejected() {
  sputnik::bytecode::BcModule module = sample_module();
  module.code_objects[0].instructions.clear();
  module.code_objects[0].instructions.push_back(
      {sputnik::bytecode::Opcode::JumpIfFalse, {{0, false}, {3, false}}});
  module.code_objects[0].instructions.push_back(
      {sputnik::bytecode::Opcode::LoadK, {{1, false}, {0, false}}});
  module.code_objects[0].instructions.push_back(
      {sputnik::bytecode::Opcode::Jump, {{3, false}}});
  module.code_objects[0].instructions.push_back(
      {sputnik::bytecode::Opcode::Return, {{1, false}}});
  module.code_objects[0].safepoint_table.clear();
  const sputnik::bytecode::DecodeResult decoded =
      sputnik::bytecode::deserialize_module(
          sputnik::bytecode::serialize_module(module));
  expect(!decoded.ok(), "branch join uninitialized read unexpectedly accepted");
  expect(has_error_code(decoded, "BC1313"),
         "expected BC1313 for branch join initializedness");
}

void test_class_descriptor_round_trip() {
  using namespace sputnik::bytecode;

  BcModule module = sample_module();
  module.symbols.push_back("Trackable");
  module.symbols.push_back("Owner");
  module.symbols.push_back("Base");
  module.symbols.push_back("Serializable");

  Constant empty_keyset;
  empty_keyset.kind = ConstantKind::KeySet;
  const std::uint32_t keyset_id =
      static_cast<std::uint32_t>(module.const_pool.size());
  module.const_pool.push_back(empty_keyset);

  Constant trackable_path;
  trackable_path.kind = ConstantKind::Path;
  trackable_path.items = {1};
  const std::uint32_t trackable_path_id =
      static_cast<std::uint32_t>(module.const_pool.size());
  module.const_pool.push_back(trackable_path);

  Constant base_path;
  base_path.kind = ConstantKind::Path;
  base_path.items = {3};
  const std::uint32_t base_path_id =
      static_cast<std::uint32_t>(module.const_pool.size());
  module.const_pool.push_back(base_path);

  Constant serializable_path;
  serializable_path.kind = ConstantKind::Path;
  serializable_path.items = {4};
  const std::uint32_t serializable_path_id =
      static_cast<std::uint32_t>(module.const_pool.size());
  module.const_pool.push_back(serializable_path);

  BcClass mixin;
  mixin.class_name_sym_id = 1;
  mixin.ivar_schema_id = keyset_id;
  mixin.flags = kClassFlagMixin;

  BcClass klass;
  klass.class_name_sym_id = 2;
  klass.has_superclass_ref = true;
  klass.superclass_ref = base_path_id;
  klass.ivar_schema_id = keyset_id;
  klass.direct_include_refs.push_back(trackable_path_id);
  klass.direct_extend_refs.push_back(serializable_path_id);

  module.classes = {mixin, klass};

  const std::vector<std::uint8_t> bytes = serialize_module(module);
  const DecodeResult decoded = deserialize_module(bytes);
  expect(decoded.ok(), verify_errors_to_json(decoded.errors));
  expect(decoded.module.classes.size() == 2, "expected two class descriptors");
  expect((decoded.module.classes[0].flags & kClassFlagMixin) != 0U,
         "mixin flag preserved");
  expect(decoded.module.classes[1].has_superclass_ref,
         "superclass ref preserved");
  expect(path_constant_text(decoded.module,
                            decoded.module.classes[1].superclass_ref) == "Base",
         "superclass path round-trips");
  expect(decoded.module.classes[1].direct_include_refs.size() == 1,
         "include refs round-trip");
  expect(path_constant_text(decoded.module,
                            decoded.module.classes[1].direct_include_refs[0]) ==
             "Trackable",
         "include path round-trips");
  expect(decoded.module.classes[1].direct_extend_refs.size() == 1,
         "extend refs round-trip");
  expect(path_constant_text(decoded.module,
                            decoded.module.classes[1].direct_extend_refs[0]) ==
             "Serializable",
         "extend path round-trips");
}

void test_invalid_class_path_ref_rejected() {
  using namespace sputnik::bytecode;

  BcModule module = sample_module();
  Constant empty_keyset;
  empty_keyset.kind = ConstantKind::KeySet;
  const std::uint32_t keyset_id =
      static_cast<std::uint32_t>(module.const_pool.size());
  module.const_pool.push_back(empty_keyset);

  BcClass klass;
  klass.class_name_sym_id = 0;
  klass.has_superclass_ref = true;
  klass.superclass_ref = 0;
  klass.ivar_schema_id = keyset_id;
  module.classes.push_back(klass);

  const DecodeResult decoded = deserialize_module(serialize_module(module));
  expect(!decoded.ok(), "non-path superclass unexpectedly accepted");
  expect(has_error_code(decoded, "BC1206"),
         "expected BC1206 for non-path superclass ref");
}

void test_invalid_schema_metadata_rejected() {
  sputnik::bytecode::BcModule module = sample_module();
  module.schemas[0].fields[0].type = "not a type";
  const sputnik::bytecode::DecodeResult decoded =
      sputnik::bytecode::deserialize_module(
          sputnik::bytecode::serialize_module(module));
  expect(!decoded.ok(), "invalid schema metadata unexpectedly accepted");
  expect(has_error_code(decoded, "BC1406"),
         "expected BC1406 for invalid schema metadata");
}

void test_invalid_table_plan_metadata_rejected() {
  sputnik::bytecode::BcModule module = sample_module();
  module.table_plans[0].op = "teleport";
  const sputnik::bytecode::DecodeResult decoded =
      sputnik::bytecode::deserialize_module(
          sputnik::bytecode::serialize_module(module));
  expect(!decoded.ok(), "invalid table plan metadata unexpectedly accepted");
  expect(has_error_code(decoded, "BC1407"),
         "expected BC1407 for invalid table metadata");
}

void test_invalid_wasm_metadata_rejected() {
  sputnik::bytecode::BcModule module = sample_module();
  module.wasm_components[0].flags &=
      ~sputnik::wasm_accel::kWasmComponentFlagWorldMutationDenied;
  const sputnik::bytecode::DecodeResult decoded =
      sputnik::bytecode::deserialize_module(
          sputnik::bytecode::serialize_module(module));
  expect(!decoded.ok(), "invalid wasm metadata unexpectedly accepted");
  expect(has_error_code(decoded, "BC1408"),
         "expected BC1408 for invalid wasm metadata");
}

void test_invalid_accelerator_metadata_rejected() {
  sputnik::bytecode::BcModule module = sample_module();
  module.accelerator_kernels[0].forbidden_features.push_back(
      "dynamic_dispatch");
  const sputnik::bytecode::DecodeResult decoded =
      sputnik::bytecode::deserialize_module(
          sputnik::bytecode::serialize_module(module));
  expect(!decoded.ok(), "invalid accelerator metadata unexpectedly accepted");
  expect(has_error_code(decoded, "BC1409"),
         "expected BC1409 for invalid accelerator metadata");
}

void test_invalid_agent_metadata_rejected() {
  sputnik::bytecode::BcModule module = sample_module();
  module.agent_patches[0].operations[0].symbol_id = "main::stale";
  const sputnik::bytecode::DecodeResult decoded =
      sputnik::bytecode::deserialize_module(
          sputnik::bytecode::serialize_module(module));
  expect(!decoded.ok(), "invalid agent metadata unexpectedly accepted");
  expect(has_error_code(decoded, "BC1410"),
         "expected BC1410 for invalid agent metadata");
}

void test_invalid_contract_metadata_rejected() {
  sputnik::bytecode::BcModule module = sample_module();
  module.properties[0].seed = 0;
  const sputnik::bytecode::DecodeResult decoded =
      sputnik::bytecode::deserialize_module(
          sputnik::bytecode::serialize_module(module));
  expect(!decoded.ok(), "invalid contract metadata unexpectedly accepted");
  expect(has_error_code(decoded, "BC1411"),
         "expected BC1411 for invalid contract metadata");
}

void test_invalid_privacy_metadata_rejected() {
  sputnik::bytecode::BcModule module = sample_module();
  module.lineage_nodes[0].kind = "export";
  module.privacy_policies[0].action = "deny";
  const sputnik::bytecode::DecodeResult decoded =
      sputnik::bytecode::deserialize_module(
          sputnik::bytecode::serialize_module(module));
  expect(!decoded.ok(), "invalid privacy metadata unexpectedly accepted");
  expect(has_error_code(decoded, "BC1412"),
         "expected BC1412 for invalid privacy metadata");
}

void test_invalid_workflow_metadata_rejected() {
  sputnik::bytecode::BcModule module = sample_module();
  sputnik::modern::WorkflowHistoryEvent conflict = module.workflow_history[0];
  conflict.input_digest = "in-b";
  conflict.output_digest = "out-b";
  module.workflow_history.push_back(conflict);
  const sputnik::bytecode::DecodeResult decoded =
      sputnik::bytecode::deserialize_module(
          sputnik::bytecode::serialize_module(module));
  expect(!decoded.ok(), "invalid workflow metadata unexpectedly accepted");
  expect(has_error_code(decoded, "BC1413"),
         "expected BC1413 for invalid workflow metadata");
}

} // namespace

int main() {
  test_round_trip_and_dump();
  test_notebook_format_minor_version_boundary();
  test_notebook_cell_round_trip();
  test_unknown_code_kind_rejected();
  test_notebook_cell_cannot_be_an_ordinary_entry();
  test_notebook_slot_opcodes_require_notebook_cell();
  test_notebook_metadata_negative_cases();
  test_disasm_is_stable();
  test_bad_magic_rejected();
  test_missing_required_section_rejected();
  test_invalid_code_ref_rejected();
  test_back_edge_requires_safepoint();
  test_register_range_rejected();
  test_uninitialized_register_read_rejected();
  test_branch_join_initializedness_rejected();
  test_class_descriptor_round_trip();
  test_invalid_class_path_ref_rejected();
  test_invalid_schema_metadata_rejected();
  test_invalid_table_plan_metadata_rejected();
  test_invalid_wasm_metadata_rejected();
  test_invalid_accelerator_metadata_rejected();
  test_invalid_agent_metadata_rejected();
  test_invalid_contract_metadata_rejected();
  test_invalid_privacy_metadata_rejected();
  test_invalid_workflow_metadata_rejected();
  std::cout << "bytecode_tests: ok\n";
  return 0;
}
