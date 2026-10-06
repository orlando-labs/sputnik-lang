#include "runtime/module_loader.h"
#include "runtime/vm.h"

#include "bytecode/format.h"

#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "module loader test failed: " << message << "\n";
    std::exit(1);
  }
}

std::vector<std::uint8_t>
serialized_module(const sputnik::bytecode::BcModule &module) {
  return sputnik::bytecode::serialize_module(module);
}

std::uint32_t append_string(sputnik::bytecode::BcModule *module,
                            const std::string &value) {
  module->strings.push_back(value);
  return static_cast<std::uint32_t>(module->strings.size() - 1U);
}

std::uint32_t append_symbol(sputnik::bytecode::BcModule *module,
                            const std::string &value) {
  module->symbols.push_back(value);
  return static_cast<std::uint32_t>(module->symbols.size() - 1U);
}

void add_dependency(sputnik::bytecode::BcModule *module,
                    const std::string &dep_name) {
  sputnik::bytecode::DepEntry dep;
  dep.module_name_str_id = append_string(module, dep_name);
  dep.required_format = {1, 0};
  dep.min_language_version = {1, 0};
  module->dependencies.push_back(dep);
}

void add_code_export(sputnik::bytecode::BcModule *module,
                     const std::string &public_name) {
  sputnik::bytecode::ExportEntry entry;
  entry.symbol_id = append_symbol(module, public_name);
  entry.target_kind_str_id = append_string(module, "code");
  entry.target_index = 1;
  entry.visibility_flags = 1;
  module->exports.push_back(entry);
}

void add_reexport(sputnik::bytecode::BcModule *module,
                  const std::string &public_name,
                  const std::string &dependency_name,
                  const std::string &source_name) {
  sputnik::bytecode::ExportEntry entry;
  entry.symbol_id = append_symbol(module, public_name);
  entry.target_kind_str_id = append_string(module, "reexport");
  entry.target_index = append_string(module, source_name);
  entry.visibility_flags = 1;
  entry.has_reexport_module_name = true;
  entry.reexport_module_name_str_id = append_string(module, dependency_name);
  module->exports.push_back(entry);
}

sputnik::bytecode::BcModule make_module(const std::vector<std::string> &deps,
                                      std::int64_t init_value,
                                      bool failing_init = false) {
  using namespace sputnik::bytecode;

  BcModule module;
  module.format_version = {1, 0};
  module.language_version = {1, 0};

  for (const std::string &dep_name : deps) {
    add_dependency(&module, dep_name);
  }

  BcCode init;
  init.code_id = 1;
  init.kind = CodeKind::Module;
  init.reg_count = 1;

  Constant constant;
  if (failing_init) {
    const std::uint32_t error_name_id =
        static_cast<std::uint32_t>(module.strings.size());
    module.strings.push_back("BoomInit");
    constant.kind = ConstantKind::StringRef;
    constant.ref_id = error_name_id;
    module.const_pool.push_back(constant);
    init.instructions.push_back({Opcode::LoadK, {{0, false}, {0, false}}});
    init.instructions.push_back({Opcode::Raise, {{0, false}}});
    init.instructions.push_back({Opcode::Return, {{0, false}}});
  } else {
    constant.kind = ConstantKind::Integer;
    constant.int_value = init_value;
    module.const_pool.push_back(constant);
    init.instructions.push_back({Opcode::LoadK, {{0, false}, {0, false}}});
    init.instructions.push_back({Opcode::Return, {{0, false}}});
  }

  module.code_objects.push_back(init);
  module.init = {true, 1, 0};
  return module;
}

sputnik::bytecode::BcModule make_runtime_string_module() {
  using namespace sputnik::bytecode;

  BcModule module;
  module.format_version = {1, 0};
  module.language_version = {1, 0};
  module.symbols.push_back("to_str");

  Constant value;
  value.kind = ConstantKind::Integer;
  value.int_value = 6;
  module.const_pool.push_back(value);

  BcCode init;
  init.code_id = 1;
  init.kind = CodeKind::Module;
  init.reg_count = 2;
  init.instructions.push_back({Opcode::LoadK, {{0, false}, {0, false}}});
  init.instructions.push_back({Opcode::Send,
                               {{1, false},
                                {0, false},
                                {0, false},
                                {0, false},
                                {0, false},
                                {-1, true},
                                {0, false}}});
  init.instructions.push_back({Opcode::Return, {{1, false}}});
  init.call_site_table.push_back({1, 0, 0, 0});
  module.code_objects.push_back(init);
  module.init = {true, 1, 0};
  return module;
}

const sputnik::runtime::RuntimeModuleSnapshot &
snapshot_named(const sputnik::runtime::RuntimeModuleLoadResult &result,
               const std::string &name) {
  for (const sputnik::runtime::RuntimeModuleSnapshot &snapshot : result.modules) {
    if (snapshot.name == name) {
      return snapshot;
    }
  }
  std::cerr << "module loader test failed: missing snapshot for " << name
            << "\n";
  std::exit(1);
}

void add_ok(sputnik::runtime::RuntimeModuleLoader &loader,
            const std::string &name, const sputnik::bytecode::BcModule &module) {
  const sputnik::runtime::RuntimeModuleLoadResult added =
      loader.add_serialized_module(name, serialized_module(module));
  expect(added.ok, "module add failed for " + name + ": " + added.message);
}

void test_loader_initializes_dependencies_once_in_order() {
  sputnik::runtime::RuntimeModuleLoader loader;
  add_ok(loader, "app.main", make_module({"core.util", "core.base"}, 3));
  add_ok(loader, "core.base", make_module({}, 1));
  add_ok(loader, "core.util", make_module({"core.base"}, 2));

  const sputnik::runtime::RuntimeModuleLoadResult initialized =
      loader.initialize_module("app.main");
  expect(initialized.ok, "app.main initialization should succeed");
  expect(initialized.has_execution_result,
         "root module initialization should expose execution result");
  expect(initialized.value.is_integer() && initialized.value.as_integer() == 3,
         "root module initialization should expose init return value");
  expect(initialized.init_order.size() == 3, "expected three init records");
  expect(initialized.init_order[0] == "core.base",
         "base module should initialize first");
  expect(initialized.init_order[1] == "core.util",
         "util module should initialize after base");
  expect(initialized.init_order[2] == "app.main",
         "requested module should initialize last");

  expect(snapshot_named(initialized, "core.base").state ==
             sputnik::runtime::RuntimeModuleState::Ready,
         "base module should be ready");
  expect(snapshot_named(initialized, "core.util").init_runs == 1,
         "dependency init should run once");
  expect(snapshot_named(initialized, "app.main").init_runs == 1,
         "root init should run once");

  const sputnik::runtime::RuntimeModuleLoadResult second =
      loader.initialize_module("app.main");
  expect(second.ok, "second app.main initialization should succeed");
  expect(snapshot_named(second, "core.base").init_runs == 1,
         "base init should not rerun");
  expect(snapshot_named(second, "core.util").init_runs == 1,
         "util init should not rerun");
  expect(snapshot_named(second, "app.main").init_runs == 1,
         "root init should not rerun");
}

void test_loader_preserves_runtime_string_table_for_results() {
  const sputnik::bytecode::BcModule module = make_runtime_string_module();
  sputnik::runtime::RuntimeModuleLoader loader;
  add_ok(loader, "runtime.string", module);

  const sputnik::runtime::RuntimeModuleLoadResult initialized =
      loader.initialize_module("runtime.string");
  expect(initialized.ok, "runtime string module initialization should succeed");
  expect(initialized.has_execution_result,
         "runtime string module should expose execution result");
  expect(sputnik::runtime::value_to_debug_string(
             initialized.value, &module, &initialized.runtime_strings,
             &initialized.runtime_symbols) == "\"6\"",
         "loader should preserve runtime-created strings for display");
}

void test_loader_reports_missing_dependency() {
  sputnik::runtime::RuntimeModuleLoader loader;
  add_ok(loader, "app.main", make_module({"core.missing"}, 1));

  const sputnik::runtime::RuntimeModuleLoadResult linked = loader.link();
  expect(!linked.ok, "link should fail for missing dependency");
  expect(linked.error_name == "ImportError",
         "missing dependency should report ImportError");
  expect(linked.message.find("core.missing") != std::string::npos,
         "missing dependency message should name dependency");
}

void test_loader_rejects_unverified_bytecode() {
  sputnik::runtime::RuntimeModuleLoader loader;
  sputnik::bytecode::BcModule module = make_module({}, 1);
  module.init.entry_code_id = 99;

  const sputnik::runtime::RuntimeModuleLoadResult added =
      loader.add_serialized_module("bad.module", serialized_module(module));
  expect(!added.ok, "bad bytecode should be rejected at load time");
  expect(added.error_name == "BytecodeVerificationError",
         "bad bytecode should report BytecodeVerificationError");
  expect(snapshot_named(added, "bad.module").state ==
             sputnik::runtime::RuntimeModuleState::Failed,
         "bad module should snapshot as failed");
}

void test_loader_detects_init_cycles() {
  sputnik::runtime::RuntimeModuleLoader loader;
  add_ok(loader, "cycle.a", make_module({"cycle.b"}, 1));
  add_ok(loader, "cycle.b", make_module({"cycle.a"}, 2));

  const sputnik::runtime::RuntimeModuleLoadResult linked = loader.link();
  expect(linked.ok, "dependency cycles should link before init access");

  const sputnik::runtime::RuntimeModuleLoadResult initialized =
      loader.initialize_all();
  expect(!initialized.ok, "cyclic module init should fail");
  expect(initialized.error_name == "ModuleInitError",
         "cyclic module init should report ModuleInitError");
  expect(snapshot_named(initialized, "cycle.a").state ==
             sputnik::runtime::RuntimeModuleState::Failed,
         "cycle.a should fail");
  expect(snapshot_named(initialized, "cycle.b").state ==
             sputnik::runtime::RuntimeModuleState::Failed,
         "cycle.b should fail");
}

void test_loader_marks_failed_init() {
  sputnik::runtime::RuntimeModuleLoader loader;
  add_ok(loader, "bad.init", make_module({}, 1, true));

  const sputnik::runtime::RuntimeModuleLoadResult initialized =
      loader.initialize_module("bad.init");
  expect(!initialized.ok, "raising module init should fail");
  expect(initialized.error_name == "ModuleInitError",
         "raising module init should report ModuleInitError");
  const sputnik::runtime::RuntimeModuleSnapshot &snapshot =
      snapshot_named(initialized, "bad.init");
  expect(snapshot.state == sputnik::runtime::RuntimeModuleState::Failed,
         "raising init should mark module failed");
  expect(snapshot.init_runs == 0, "failed init should not count as successful");
  expect(snapshot.message.find("BoomInit") != std::string::npos,
         "failed init message should preserve VM fault");

  const sputnik::runtime::RuntimeModuleLoadResult retried =
      loader.initialize_module("bad.init");
  expect(!retried.ok, "failed init should stay failed on retry");
  expect(retried.error_name == "ModuleInitError",
         "retry should preserve ModuleInitError");
  expect(retried.message == initialized.message,
         "retry should preserve original failure message");
  const sputnik::runtime::RuntimeModuleSnapshot &retry_snapshot =
      snapshot_named(retried, "bad.init");
  expect(retry_snapshot.state == sputnik::runtime::RuntimeModuleState::Failed,
         "retry should leave failed module failed");
  expect(retry_snapshot.init_runs == 0,
         "failed module init should not rerun on retry");
}

void test_loader_materializes_exports_and_import_aliases() {
  sputnik::runtime::RuntimeModuleLoader loader;
  sputnik::bytecode::BcModule core = make_module({}, 42);
  add_code_export(&core, "Answer");
  sputnik::bytecode::BcModule app = make_module({"core.values"}, 1);

  add_ok(loader, "core.values", core);
  add_ok(loader, "app.main", app);
  const sputnik::runtime::RuntimeModuleLoadResult alias_added =
      loader.add_import_alias("app.main", "Answer", "core.values", "Answer");
  expect(alias_added.ok, "import alias registration should succeed");

  const sputnik::runtime::RuntimeModuleLoadResult linked = loader.link();
  expect(linked.ok, "export/import link should succeed");
  const std::optional<sputnik::runtime::RuntimeExportCellSnapshot> before =
      loader.export_snapshot("core.values", "Answer");
  expect(before.has_value(), "linked export cell should be visible");
  expect(before->state == sputnik::runtime::RuntimeExportCellState::Uninitialized,
         "linked export should be uninitialized before module init");

  const sputnik::runtime::RuntimeModuleLoadResult early_read =
      loader.read_import_alias("app.main", "Answer");
  expect(!early_read.ok, "early import alias read should fail");
  expect(early_read.error_name == "ModuleInitError",
         "early import alias read should report ModuleInitError");

  const sputnik::runtime::RuntimeModuleLoadResult initialized =
      loader.initialize_all();
  expect(initialized.ok, "export/import modules should initialize");
  const std::optional<sputnik::runtime::RuntimeImportAliasSnapshot> alias =
      loader.import_alias_snapshot("app.main", "Answer");
  expect(alias.has_value(), "import alias snapshot should be visible");
  expect(alias->read_only, "import alias should be read-only");
  expect(alias->export_cell.state ==
             sputnik::runtime::RuntimeExportCellState::Ready,
         "import alias should observe ready export cell after init");
  expect(alias->export_cell.resolved_module_name == "core.values",
         "import alias should resolve to exporting module");
}

void test_loader_live_alias_snapshots_track_export_updates() {
  sputnik::runtime::RuntimeModuleLoader loader;
  sputnik::bytecode::BcModule core = make_module({}, 42);
  add_code_export(&core, "Answer");
  sputnik::bytecode::BcModule app = make_module({"core.values"}, 1);

  add_ok(loader, "core.values", core);
  add_ok(loader, "app.main", app);
  const sputnik::runtime::RuntimeModuleLoadResult alias_added =
      loader.add_import_alias("app.main", "Answer", "core.values", "Answer");
  expect(alias_added.ok, "live alias registration should succeed");

  const sputnik::runtime::RuntimeModuleLoadResult linked = loader.link();
  expect(linked.ok, "live alias modules should link");

  const std::optional<sputnik::runtime::RuntimeImportAliasSnapshot> alias_before =
      loader.import_alias_snapshot("app.main", "Answer");
  expect(alias_before.has_value(),
         "live alias snapshot should be visible before init");
  expect(alias_before->export_cell.state ==
             sputnik::runtime::RuntimeExportCellState::Uninitialized,
         "live alias should observe uninitialized export before init");

  const sputnik::runtime::RuntimeModuleLoadResult early_read =
      loader.read_import_alias("app.main", "Answer");
  expect(!early_read.ok, "live alias read before export init should fail");
  expect(early_read.error_name == "ModuleInitError",
         "early live alias read should report ModuleInitError");

  const sputnik::runtime::RuntimeModuleLoadResult core_initialized =
      loader.initialize_module("core.values");
  expect(core_initialized.ok, "exporting module should initialize");

  const std::optional<sputnik::runtime::RuntimeImportAliasSnapshot> alias_after =
      loader.import_alias_snapshot("app.main", "Answer");
  expect(alias_after.has_value(),
         "live alias snapshot should stay visible after init");
  expect(alias_after->export_cell.state ==
             sputnik::runtime::RuntimeExportCellState::Ready,
         "live alias should observe ready export after init");
  expect(alias_after->export_cell.resolved_module_name == "core.values",
         "live alias should retain resolved exporting module");

  const sputnik::runtime::RuntimeModuleLoadResult ready_read =
      loader.read_import_alias("app.main", "Answer");
  expect(ready_read.ok, "live alias read should succeed after export init");
}

void test_loader_cyclic_import_aliases_fail_and_stay_failed() {
  sputnik::runtime::RuntimeModuleLoader loader;
  sputnik::bytecode::BcModule cycle_a = make_module({"cycle.b"}, 1);
  add_code_export(&cycle_a, "A");
  sputnik::bytecode::BcModule cycle_b = make_module({"cycle.a"}, 2);
  add_code_export(&cycle_b, "B");

  add_ok(loader, "cycle.a", cycle_a);
  add_ok(loader, "cycle.b", cycle_b);
  expect(loader.add_import_alias("cycle.a", "B", "cycle.b", "B").ok,
         "cycle.a import alias registration should succeed");
  expect(loader.add_import_alias("cycle.b", "A", "cycle.a", "A").ok,
         "cycle.b import alias registration should succeed");

  const sputnik::runtime::RuntimeModuleLoadResult linked = loader.link();
  expect(linked.ok, "cyclic aliases should link before init access");

  const std::optional<sputnik::runtime::RuntimeImportAliasSnapshot> alias_before =
      loader.import_alias_snapshot("cycle.a", "B");
  expect(alias_before.has_value(),
         "cyclic alias snapshot should be visible before init");
  expect(alias_before->export_cell.state ==
             sputnik::runtime::RuntimeExportCellState::Uninitialized,
         "cyclic alias should observe uninitialized export before init");

  const sputnik::runtime::RuntimeModuleLoadResult early_read =
      loader.read_import_alias("cycle.a", "B");
  expect(!early_read.ok, "cyclic alias read before init should fail");
  expect(early_read.error_name == "ModuleInitError",
         "cyclic early alias read should report ModuleInitError");

  const sputnik::runtime::RuntimeModuleLoadResult initialized =
      loader.initialize_all();
  expect(!initialized.ok, "cyclic import aliases should fail during init");
  expect(initialized.error_name == "ModuleInitError",
         "cyclic import aliases should report ModuleInitError");
  expect(initialized.message.find("cyclic module initialization") !=
             std::string::npos,
         "cyclic import aliases should include cycle context");
  expect(snapshot_named(initialized, "cycle.a").state ==
             sputnik::runtime::RuntimeModuleState::Failed,
         "cycle.a should stay failed after cyclic alias init");
  expect(snapshot_named(initialized, "cycle.b").state ==
             sputnik::runtime::RuntimeModuleState::Failed,
         "cycle.b should stay failed after cyclic alias init");

  const std::optional<sputnik::runtime::RuntimeImportAliasSnapshot> alias_after =
      loader.import_alias_snapshot("cycle.a", "B");
  expect(alias_after.has_value(),
         "cyclic alias snapshot should remain visible after failure");
  expect(alias_after->export_cell.state ==
             sputnik::runtime::RuntimeExportCellState::Failed,
         "cyclic alias should observe failed export cell after init failure");
  expect(alias_after->export_cell.message.find(
             "cyclic module initialization") != std::string::npos,
         "failed cyclic alias should retain cycle message");

  const sputnik::runtime::RuntimeModuleLoadResult retried =
      loader.initialize_all();
  expect(!retried.ok, "cyclic init failure should stay failed on retry");
  expect(retried.error_name == "ModuleInitError",
         "cyclic retry should preserve ModuleInitError");
  expect(snapshot_named(retried, "cycle.a").init_runs == 0,
         "cycle.a init should not rerun after cyclic failure");
  expect(snapshot_named(retried, "cycle.b").init_runs == 0,
         "cycle.b init should not rerun after cyclic failure");
}

void test_loader_reports_missing_export() {
  sputnik::runtime::RuntimeModuleLoader loader;
  add_ok(loader, "core.values", make_module({}, 1));
  add_ok(loader, "app.main", make_module({"core.values"}, 1));
  const sputnik::runtime::RuntimeModuleLoadResult alias_added =
      loader.add_import_alias("app.main", "Missing", "core.values", "Missing");
  expect(alias_added.ok,
         "missing export alias registration should be accepted");

  const sputnik::runtime::RuntimeModuleLoadResult linked = loader.link();
  expect(!linked.ok, "link should fail for missing export");
  expect(linked.error_name == "ImportError",
         "missing export should report ImportError");
  expect(linked.message.find("Missing") != std::string::npos,
         "missing export message should name export");
  expect(!linked.diagnostics.empty(),
         "missing export should include diagnostic");
  expect(linked.diagnostics[0].module_name == "app.main",
         "missing export diagnostic should name importer");
  expect(linked.diagnostics[0].dependency_name == "core.values",
         "missing export diagnostic should name dependency");
  expect(linked.diagnostics[0].export_name == "Missing",
         "missing export diagnostic should name export");
}

void test_loader_reports_version_and_abi_mismatch() {
  sputnik::runtime::RuntimeModuleLoader version_loader;
  sputnik::bytecode::BcModule versioned_app = make_module({"core.versioned"}, 1);
  versioned_app.dependencies[0].required_format = {1, 1};
  add_ok(version_loader, "core.versioned", make_module({}, 1));
  add_ok(version_loader, "app.versioned", versioned_app);

  const sputnik::runtime::RuntimeModuleLoadResult version_linked =
      version_loader.link();
  expect(!version_linked.ok, "link should fail for dependency format mismatch");
  expect(version_linked.error_name == "ImportError",
         "version mismatch should report ImportError");
  expect(version_linked.message.find("1.1") != std::string::npos,
         "version mismatch should include required version");

  sputnik::runtime::RuntimeModuleLoader abi_loader;
  sputnik::bytecode::BcModule abi_app = make_module({"core.abi"}, 1);
  abi_app.dependencies[0].has_abi_requirement = true;
  abi_app.dependencies[0].abi_requirement.fill(0xAB);
  add_ok(abi_loader, "core.abi", make_module({}, 1));
  add_ok(abi_loader, "app.abi", abi_app);

  const sputnik::runtime::RuntimeModuleLoadResult abi_linked = abi_loader.link();
  expect(!abi_linked.ok, "link should fail for dependency ABI mismatch");
  expect(abi_linked.error_name == "ImportError",
         "ABI mismatch should report ImportError");
  expect(abi_linked.message.find("ABI") != std::string::npos,
         "ABI mismatch should include ABI context");
}

void test_loader_rejects_unsupported_required_profile() {
  sputnik::runtime::RuntimeModuleLoader loader;
  sputnik::bytecode::BcModule module = make_module({}, 1);
  module.required_features = {"ffi.v1"};
  add_ok(loader, "profile.bad", module);

  const sputnik::runtime::RuntimeModuleLoadResult linked = loader.link();
  expect(!linked.ok, "link should fail for unsupported required profile");
  expect(linked.error_name == "UnsupportedProfileError",
         "unsupported profile should report UnsupportedProfileError");
  expect(linked.message.find("ffi.v1") != std::string::npos,
         "unsupported profile message should name feature");
}

void test_loader_resolves_reexport_chain() {
  sputnik::runtime::RuntimeModuleLoader loader;
  sputnik::bytecode::BcModule leaf = make_module({}, 1);
  add_code_export(&leaf, "Thing");

  sputnik::bytecode::BcModule facade = make_module({"core.leaf"}, 2);
  add_reexport(&facade, "Thing", "core.leaf", "Thing");

  sputnik::bytecode::BcModule app = make_module({"core.facade"}, 3);

  add_ok(loader, "core.leaf", leaf);
  add_ok(loader, "core.facade", facade);
  add_ok(loader, "app.main", app);
  const sputnik::runtime::RuntimeModuleLoadResult alias_added =
      loader.add_import_alias("app.main", "Thing", "core.facade", "Thing");
  expect(alias_added.ok, "re-export import alias registration should succeed");

  const sputnik::runtime::RuntimeModuleLoadResult initialized =
      loader.initialize_all();
  expect(initialized.ok, "re-export module chain should initialize");
  const std::optional<sputnik::runtime::RuntimeImportAliasSnapshot> alias =
      loader.import_alias_snapshot("app.main", "Thing");
  expect(alias.has_value(), "re-export alias snapshot should be visible");
  expect(alias->export_cell.has_reexport,
         "facade export should be marked as re-export");
  expect(alias->export_cell.resolved_module_name == "core.leaf",
         "re-export should resolve to leaf module");
  expect(alias->export_cell.resolved_export_name == "Thing",
         "re-export should resolve target export name");
  expect(alias->export_cell.state ==
             sputnik::runtime::RuntimeExportCellState::Ready,
         "re-export alias should observe ready leaf export");
}

void test_loader_reports_source_mapped_init_failure() {
  sputnik::runtime::RuntimeModuleLoader loader;
  sputnik::bytecode::BcModule module = make_module({}, 1, true);
  module.code_objects[0].source_spans.push_back(
      {1, 2, {"bad_init.s", {9, 3, 80}, {9, 12, 89}}});
  module.line_table.push_back({1, 1, 9});
  add_ok(loader, "bad.init", module);

  const sputnik::runtime::RuntimeModuleLoadResult initialized =
      loader.initialize_module("bad.init");
  expect(!initialized.ok, "source-mapped failing init should fail");
  expect(initialized.error_name == "ModuleInitError",
         "source-mapped failing init should report ModuleInitError");
  expect(!initialized.diagnostics.empty(),
         "source-mapped failing init should include diagnostic");
  expect(initialized.diagnostics[0].location.file == "bad_init.s",
         "loader diagnostic should preserve source file");
  expect(initialized.diagnostics[0].location.line == 9,
         "loader diagnostic should preserve source line");
  expect(initialized.message.find("bad_init.s") != std::string::npos,
         "loader message should include trace text");
}

} // namespace

int main() {
  test_loader_initializes_dependencies_once_in_order();
  test_loader_preserves_runtime_string_table_for_results();
  test_loader_reports_missing_dependency();
  test_loader_rejects_unverified_bytecode();
  test_loader_detects_init_cycles();
  test_loader_marks_failed_init();
  test_loader_materializes_exports_and_import_aliases();
  test_loader_live_alias_snapshots_track_export_updates();
  test_loader_cyclic_import_aliases_fail_and_stay_failed();
  test_loader_reports_missing_export();
  test_loader_reports_version_and_abi_mismatch();
  test_loader_rejects_unsupported_required_profile();
  test_loader_resolves_reexport_chain();
  test_loader_reports_source_mapped_init_failure();
  return 0;
}
