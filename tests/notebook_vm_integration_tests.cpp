#include "notebook/compiler.h"
#include "notebook/vm_cell_executor.h"

#include "bytecode/format.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using amber::bytecode::BcCode;
using amber::bytecode::BcModule;
using amber::bytecode::CodeKind;
using amber::bytecode::Constant;
using amber::bytecode::ConstantKind;
using amber::bytecode::Instruction;
using amber::bytecode::Opcode;
using amber::notebook::BindingKey;
using amber::notebook::CellCompileResult;
using amber::notebook::CellExecutionResult;
using amber::notebook::CellMode;
using amber::notebook::CellSource;
using amber::notebook::KernelWrite;
using amber::notebook::NotebookImage;
using amber::notebook::NotebookKernel;
using amber::notebook::NotebookSlotDirection;
using amber::notebook::NotebookVmCellExecutor;
using amber::notebook::VmCellDescriptorTable;
using amber::notebook::VmCellExecutionReport;
using amber::notebook::VmNotebookImageManifest;
using amber::notebook::VmSlotDescriptor;
using amber::runtime::RuntimeWorld;
using amber::runtime::Value;

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "notebook VM integration test failed: " << message << "\n";
    std::exit(1);
  }
}

CellSource cell(std::uint64_t id, std::string source) {
  return CellSource{id, std::move(source), "<notebook-vm-test>",
                    CellMode::Watch};
}

std::uint32_t append_integer(BcModule *module, std::int64_t value) {
  Constant constant;
  constant.kind = ConstantKind::Integer;
  constant.int_value = value;
  module->const_pool.push_back(constant);
  return static_cast<std::uint32_t>(module->const_pool.size() - 1U);
}

BcCode notebook_code(std::uint32_t code_id, bool fault_after_store) {
  BcCode code;
  code.code_id = code_id;
  code.kind = CodeKind::NotebookCell;
  code.reg_count = fault_after_store ? 2U : 3U;
  code.instructions.push_back(
      {Opcode::LoadNotebookSlot, {{0, false}, {0, false}}});
  if (fault_after_store) {
    code.instructions.push_back(
        {Opcode::StoreNotebookSlot, {{1, false}, {0, false}}});
    // Descriptor 99 is intentionally absent from the adapter table.  The
    // first STORE must still be discarded when this later load faults.
    code.instructions.push_back(
        {Opcode::LoadNotebookSlot, {{1, false}, {99, false}}});
    code.instructions.push_back({Opcode::Return, {{0, false}}});
    return code;
  }

  code.instructions.push_back({Opcode::LoadK, {{1, false}, {0, false}}});
  code.instructions.push_back(
      {Opcode::IAdd, {{2, false}, {0, false}, {1, false}}});
  code.instructions.push_back(
      {Opcode::StoreNotebookSlot, {{1, false}, {2, false}}});
  // Repeated STOREs update the staging map and must not create duplicate
  // KernelWrite entries.
  code.instructions.push_back(
      {Opcode::StoreNotebookSlot, {{1, false}, {2, false}}});
  code.instructions.push_back({Opcode::Return, {{2, false}}});
  return code;
}

BcCode passthrough_code() {
  BcCode code;
  code.code_id = 4;
  code.kind = CodeKind::NotebookCell;
  code.reg_count = 1;
  code.instructions = {
      {Opcode::LoadNotebookSlot, {{0, false}, {0, false}}},
      {Opcode::StoreNotebookSlot, {{1, false}, {0, false}}},
      {Opcode::StoreNotebookSlot, {{1, false}, {0, false}}},
      {Opcode::Safepoint, {}},
      {Opcode::Return, {{0, false}}},
  };
  return code;
}

BcCode conditional_dependency_code() {
  BcCode code;
  code.code_id = 5;
  code.kind = CodeKind::NotebookCell;
  code.reg_count = 2;
  code.instructions = {
      {Opcode::LoadNotebookSlot, {{0, false}, {2, false}}},
      {Opcode::JumpIfFalse, {{0, false}, {5, false}}},
      {Opcode::LoadNotebookSlot, {{1, false}, {0, false}}},
      // A repeated executed read must still produce one dynamic BindingKey.
      {Opcode::LoadNotebookSlot, {{1, false}, {0, false}}},
      {Opcode::Jump, {{6, false}}},
      {Opcode::LoadNotebookSlot, {{1, false}, {3, false}}},
      {Opcode::StoreNotebookSlot, {{1, false}, {1, false}}},
      {Opcode::Return, {{1, false}}},
  };
  return code;
}

BcCode dependency_capture_code(std::uint32_t code_id, bool fault_after_read) {
  BcCode code;
  code.code_id = code_id;
  code.kind = CodeKind::NotebookCell;
  code.reg_count = fault_after_read ? 2U : 3U;
  code.instructions = {
      {Opcode::LoadNotebookSlot, {{0, false}, {0, false}}},
      {Opcode::Move, {{1, false}, {0, false}}},
  };
  if (fault_after_read) {
    code.instructions.push_back(
        {Opcode::LoadNotebookSlot, {{1, false}, {99, false}}});
    code.instructions.push_back({Opcode::Return, {{1, false}}});
  } else {
    code.instructions.push_back({Opcode::Move, {{2, false}, {0, false}}});
    code.instructions.push_back({Opcode::Return, {{2, false}}});
  }
  return code;
}

BcModule make_module() {
  BcModule module;
  module.format_version = {1, 0};
  module.language_version = {1, 0};
  append_integer(&module, 1);
  module.code_objects.push_back(notebook_code(2, false));
  module.code_objects.push_back(notebook_code(3, true));
  module.code_objects.push_back(passthrough_code());
  module.code_objects.push_back(conditional_dependency_code());
  module.code_objects.push_back(dependency_capture_code(6, false));
  module.code_objects.push_back(dependency_capture_code(7, true));
  return module;
}

VmCellDescriptorTable input_x_output_y() {
  VmCellDescriptorTable table;
  table.slots = {
      VmSlotDescriptor{0, BindingKey{1, "x"}, NotebookSlotDirection::Input},
      VmSlotDescriptor{1, BindingKey{2, "y"}, NotebookSlotDirection::Output},
  };
  return table;
}

VmCellDescriptorTable conditional_inputs_output_y() {
  VmCellDescriptorTable table;
  table.slots = {
      VmSlotDescriptor{0, BindingKey{1, "x"}, NotebookSlotDirection::Input},
      VmSlotDescriptor{2, BindingKey{1, "condition"},
                       NotebookSlotDirection::Input},
      VmSlotDescriptor{3, BindingKey{1, "fallback"},
                       NotebookSlotDirection::Input},
      VmSlotDescriptor{1, BindingKey{2, "y"}, NotebookSlotDirection::Output},
  };
  return table;
}

CellExecutionResult
publish_x(const std::vector<amber::notebook::KernelInput> &inputs) {
  expect(inputs.empty(), "provider cell unexpectedly received inputs");
  CellExecutionResult result;
  result.ok = true;
  result.writes.push_back(KernelWrite{{1, "x"}, Value::integer(2), {}});
  return result;
}

void test_consumer_executes_and_publishes_atomically() {
  NotebookKernel kernel({cell(1, "x = 2\n"), cell(2, "y = x + 1\n")});
  kernel.update_cells({cell(1, "x = 2\n"), cell(2, "y = x + 1\n")}, 1);
  expect(kernel.run_cell(1, publish_x).ok,
         "provider should publish x through the kernel callback");

  RuntimeWorld world(make_module());
  NotebookVmCellExecutor adapter(&world, 2, input_x_output_y());
  expect(adapter.valid(), "consumer descriptor table should be valid");
  const auto consumer = kernel.run_cell(2, adapter.as_executor());
  expect(consumer.ok, consumer.error);
  expect(consumer.publication.events.size() == 1U,
         "repeated STORE must produce one publication event");
  const auto y = kernel.read_slot({2, "y"});
  expect(y.has_value() && y->is_integer() && y->as_integer() == 3,
         "consumer should publish y = x + 1 atomically");
}

void test_fault_after_store_discards_partial_batch() {
  NotebookKernel kernel({cell(1, "x = 2\n"), cell(2, "y = x + 1\n")});
  kernel.update_cells({cell(1, "x = 2\n"), cell(2, "y = x + 1\n")}, 1);
  expect(kernel.run_cell(1, publish_x).ok, "provider publication failed");

  RuntimeWorld world(make_module());
  NotebookVmCellExecutor adapter(&world, 3, input_x_output_y());
  const auto failed = kernel.run_cell(2, adapter.as_executor());
  expect(!failed.ok &&
             failed.error.find("NotebookCellError") != std::string::npos,
         "runtime fault should reach the kernel as a failed execution");
  expect(!kernel.read_slot({2, "y"}).has_value(),
         "STORE before a later runtime fault must not publish y");
}

void test_fault_after_observation_preserves_dynamic_snapshot() {
  NotebookKernel kernel({cell(1, "x = 2\n"), cell(2, "y = x + 1\n")});
  kernel.update_cells({cell(1, "x = 2\n"), cell(2, "y = x + 1\n")}, 1);
  expect(kernel.run_cell(1, publish_x).ok, "provider publication failed");

  RuntimeWorld world(make_module());
  NotebookVmCellExecutor successful(&world, 2, input_x_output_y());
  expect(kernel.run_cell(2, successful.as_executor()).ok,
         "initial consumer capture should succeed");
  expect(kernel.dynamic_dependencies(2) ==
             std::vector<BindingKey>{{1, "x"}},
         "successful load should publish the observed dependency snapshot");

  NotebookVmCellExecutor failing(&world, 3, input_x_output_y());
  const auto failed = kernel.run_cell(2, failing.as_executor());
  expect(!failed.ok,
         "faulting rerun should fail after observing the input");
  expect(kernel.dynamic_dependencies(2) ==
             std::vector<BindingKey>{{1, "x"}},
         "faulting rerun must preserve the last successful dependency set");
}

void test_heap_staging_is_rooted_across_forced_safepoint() {
  NotebookKernel kernel({cell(1, "x = 2\n"), cell(2, "y = x + 1\n")});
  kernel.update_cells({cell(1, "x = 2\n"), cell(2, "y = x + 1\n")}, 1);
  amber::runtime::RuntimeWorldOptions options;
  options.external_gc_root_provider = [&kernel]() { return kernel.gc_roots(); };
  RuntimeWorld world(make_module(), std::move(options));
  const Value input = world.list_value({Value::integer(41)});
  expect(kernel
             .run_cell(1,
                       [&input](const auto &inputs) {
                         expect(inputs.empty(),
                                "heap provider unexpectedly received inputs");
                         CellExecutionResult result;
                         result.ok = true;
                         result.writes.push_back(
                             KernelWrite{{1, "x"}, input, {}});
                         return result;
                       })
             .ok,
         "heap provider should publish x");
  NotebookVmCellExecutor adapter(&world, 4, input_x_output_y());
  world.request_garbage_collection();
  const auto consumer = kernel.run_cell(2, adapter.as_executor());
  expect(consumer.ok, consumer.error);
  const auto y = kernel.read_slot({2, "y"});
  expect(y.has_value() && y->is_list(),
         "heap passthrough should publish a list output");
  expect(world.pin_count(*y) == 0U,
         "run-local pins should be released after kernel commit");
  (void)world.collect_garbage();
  const auto retained = kernel.read_slot({2, "y"});
  expect(retained.has_value() && retained->is_list(),
         "published heap output should remain rooted by the kernel");
}

void test_execution_result_releases_pins_after_world_destroyed() {
  CellExecutionResult result;
  amber::runtime::RuntimeHeap heap;
  {
    auto world = std::make_unique<RuntimeWorld>(make_module());
    heap = world->heap_handle();
    NotebookVmCellExecutor adapter(&*world, 4, input_x_output_y());
    const Value input = world->list_value({Value::integer(7)});
    result = adapter.execute(
        {amber::notebook::KernelInput{{1, "x"}, input, 0}});
    expect(result.ok, "heap-output execution should succeed");
    expect(result.writes.size() == 1U && result.writes[0].value.is_list(),
           "heap-output execution should stage the list");
    expect(heap.stats().active_pins != 0U,
           "successful execution should retain a pin until commit lifetime ends");
  }
  // The result still owns the run-local execution state, but its release path
  // must use the shared heap handle rather than the destroyed RuntimeWorld.
  result.keepalive.reset();
  expect(heap.stats().active_pins == 0U,
         "destroying the result should release pins after world destruction");
}

void test_executor_keeps_runtime_world_handle_alive() {
  std::optional<NotebookVmCellExecutor> executor;
  {
    auto world = std::make_unique<RuntimeWorld>(make_module());
    executor.emplace(&*world, 2, input_x_output_y());
    expect(executor->valid(), "retained-world executor should be valid");
  }

  const CellExecutionResult result = executor->execute(
      {amber::notebook::KernelInput{{1, "x"}, Value::integer(2), 0}});
  expect(result.ok && result.writes.size() == 1U &&
             result.writes.front().value.is_integer() &&
             result.writes.front().value.as_integer() == 3,
         "executor should safely run after the original world handle dies");
}

void test_incomplete_store_is_rejected_before_kernel_commit() {
  NotebookKernel kernel(
      {cell(1, "x = 2\n"), cell(2, "y = x + 1\nz = x + 2\n")});
  kernel.update_cells({cell(1, "x = 2\n"), cell(2, "y = x + 1\nz = x + 2\n")},
                      1);
  expect(kernel.run_cell(1, publish_x).ok, "provider publication failed");

  RuntimeWorld world(make_module());
  VmCellDescriptorTable table = input_x_output_y();
  table.slots.push_back(
      VmSlotDescriptor{2, BindingKey{2, "z"}, NotebookSlotDirection::Output});
  NotebookVmCellExecutor adapter(&world, 2, std::move(table));
  const auto failed = kernel.run_cell(2, adapter.as_executor());
  expect(!failed.ok && failed.error.find("notebook output was not staged") !=
                           std::string::npos,
         "missing output store should fail the adapter");
  expect(!kernel.read_slot({2, "y"}).has_value() &&
             !kernel.read_slot({2, "z"}).has_value(),
         "incomplete output batch must publish no slots");
}

void test_executor_rejects_replaced_image_generation() {
  RuntimeWorld world(make_module());
  NotebookVmCellExecutor stale(&world, 2, input_x_output_y());
  expect(stale.valid(), "pre-install executor should be valid");

  auto replacement = std::make_shared<const BcModule>(make_module());
  const auto installed = world.install_notebook_image(replacement);
  expect(installed.ok && installed.swapped,
         "notebook image replacement should succeed");
  const CellExecutionResult result = stale.execute(
      {amber::notebook::KernelInput{{1, "x"}, Value::integer(2), 0}});
  expect(!result.ok &&
             result.error.find("StaleNotebookImageError") != std::string::npos,
         "executor from the old image must fail after image replacement");
}

void test_executor_captures_only_executed_notebook_slot_reads() {
  RuntimeWorld world(make_module());
  NotebookVmCellExecutor adapter(&world, 5, conditional_inputs_output_y());
  expect(adapter.valid(), "conditional descriptor table should be valid");

  const std::vector<amber::notebook::KernelInput> true_inputs{
      {{1, "x"}, Value::integer(7), 0},
      {{1, "condition"}, Value::boolean(true), 0},
      {{1, "fallback"}, Value::integer(9), 0},
  };
  const CellExecutionResult true_result = adapter.execute(true_inputs);
  expect(true_result.ok &&
             true_result.dynamic_dependencies ==
                 std::vector<BindingKey>{{1, "condition"}, {1, "x"}},
         "true branch should capture condition and deduplicated x only");

  std::vector<amber::notebook::KernelInput> false_inputs = true_inputs;
  false_inputs[1].value = Value::boolean(false);
  const CellExecutionResult false_result = adapter.execute(false_inputs);
  expect(false_result.ok &&
             false_result.dynamic_dependencies ==
                 std::vector<BindingKey>{{1, "condition"}, {1, "fallback"}},
         "false branch should replace capture with condition and fallback");
}

void test_executor_reports_run_local_runtime_dependency_capture() {
  RuntimeWorld world(make_module());
  const amber::runtime::RuntimeWatchStreamIdentity source =
      world.watch_cursor().source;
  VmCellDescriptorTable input_only;
  input_only.slots = {
      VmSlotDescriptor{0, BindingKey{1, "x"}, NotebookSlotDirection::Input}};
  NotebookVmCellExecutor adapter(&world, 6, input_only, 77);
  expect(adapter.valid(), "runtime dependency descriptor table should be valid");

  auto watched = std::make_shared<amber::runtime::RuntimeWatchCell>(
      Value::integer(9), 901, "x");
  watched->enable_watch();
  world.begin_dependency_capture(1234);
  VmCellExecutionReport report;
  const CellExecutionResult success = adapter.execute(
      {amber::notebook::KernelInput{{1, "x"}, Value::watch_cell(watched), 0}},
      &report);
  const amber::runtime::RuntimeDependencySet legacy_capture =
      world.end_dependency_capture();
  expect(success.ok && success.runtime_dependencies.has_value() &&
             success.runtime_dependencies->notebook_cell_id == 77U &&
             success.runtime_dependencies->source == source &&
             success.runtime_dependencies->dependencies.size() == 1U,
         "watched notebook input should execute and transfer its rich capture");
  expect(report.dependency_capture.notebook_cell_id == 77 &&
             report.dependency_capture.source == source &&
             report.dependency_capture.dependencies.size() == 1U,
         "successful execution should report its rich runtime capture");
  expect(report.dependency_capture.dependencies.front().kind ==
             amber::runtime::RuntimeDependencyKind::Binding,
         "watched notebook input should report a binding dependency");
  expect(report.dependency_capture.dependencies.front().cell_id == 901,
         "runtime dependency should retain the watched provider cell id");
  expect(report.dependency_capture.dependencies.front().target_name == "x",
         "runtime dependency should retain the watched target name");
  expect(legacy_capture.notebook_cell_id == 1234 &&
             legacy_capture.source == source &&
             legacy_capture.dependencies.size() == 1U &&
             legacy_capture.dependencies.front().cell_id == 901,
         "run-local capture must not clobber a simultaneous legacy capture");

  // The same watched cell is read twice, but the run-local rich set is
  // deduplicated independently of the legacy ObserveSlot BindingKey vector.
  expect(success.dynamic_dependencies ==
             std::vector<BindingKey>{{1, "x"}},
         "legacy slot observation remains a separate deduplicated snapshot");

  NotebookVmCellExecutor empty(&world, 6, input_only, 78);
  VmCellExecutionReport empty_report;
  const CellExecutionResult empty_result = empty.execute(
      {amber::notebook::KernelInput{{1, "x"}, Value::integer(9), 0}},
      &empty_report);
  expect(empty_result.ok && empty_report.dependency_capture.notebook_cell_id ==
                               78 &&
             empty_report.dependency_capture.source == source &&
             empty_report.dependency_capture.dependencies.empty() &&
             empty_result.runtime_dependencies.has_value() &&
             empty_result.runtime_dependencies->notebook_cell_id == 78U &&
             empty_result.runtime_dependencies->source == source &&
             empty_result.runtime_dependencies->dependencies.empty(),
         "successful execution should report an explicitly empty capture");

  NotebookVmCellExecutor failing(&world, 7, input_only, 79);
  VmCellExecutionReport failed_report;
  const CellExecutionResult failed = failing.execute(
      {amber::notebook::KernelInput{{1, "x"}, Value::watch_cell(watched), 0}},
      &failed_report);
  expect(!failed.ok && !failed.runtime_dependencies.has_value() &&
             failed_report.dependency_capture.notebook_cell_id == 0 &&
             failed_report.dependency_capture.dependencies.empty(),
         "faulting adapter execution must not surface its replacement capture");
}

void test_kernel_stores_adapter_runtime_dependencies_transactionally() {
  const std::vector<CellSource> cells{cell(1, "x = 2\n"), cell(2, "x\n")};
  NotebookKernel kernel(cells);
  kernel.update_cells(cells, 1);

  RuntimeWorld world(make_module());
  VmCellDescriptorTable input_only;
  input_only.slots = {
      VmSlotDescriptor{0, BindingKey{1, "x"}, NotebookSlotDirection::Input}};
  NotebookVmCellExecutor adapter(&world, 6, input_only, 2);
  auto watched = std::make_shared<amber::runtime::RuntimeWatchCell>(
      Value::integer(9), 901, "x");
  watched->enable_watch();
  expect(kernel
             .run_cell(1, [&watched](const auto &) {
               CellExecutionResult result;
               result.ok = true;
               result.writes.push_back(KernelWrite{
                   {1, "x"}, Value::watch_cell(watched), {}});
               return result;
             })
             .ok,
         "watched provider should publish before rich capture");

  const auto first = kernel.run_cell(2, adapter.as_executor());
  expect(first.ok && first.runtime_dependencies.has_value(),
         "kernel run should carry the adapter's rich capture");
  const auto stored = kernel.runtime_dependencies(2);
  expect(stored.has_value() && stored->notebook_cell_id == 2U &&
             stored->source == world.watch_cursor().source &&
             stored->dependencies.size() == 1U &&
             stored->dependencies.front().cell_id == 901U,
         "kernel should store runtime provider identity separately from slots");
  expect(stored->dependencies.front().cell_id !=
             first.inputs.front().key.cell_id,
         "runtime dependency cell id must not be rewritten as BindingKey cell id");

  RuntimeWorld foreign_world(make_module());
  NotebookVmCellExecutor foreign_adapter(&foreign_world, 6, input_only, 2);
  const auto foreign = kernel.run_cell(2, foreign_adapter.as_executor());
  expect(!foreign.ok &&
             foreign.error.find("another world") != std::string::npos &&
             kernel.runtime_dependencies(2).has_value() &&
             kernel.runtime_dependencies(2)->source == stored->source,
         "a foreign-world capture must not replace the published namespace");

  expect(kernel
             .run_cell(1, [](const auto &) {
               CellExecutionResult result;
               result.ok = true;
               result.writes.push_back(
                   KernelWrite{{1, "x"}, Value::integer(9), {}});
               return result;
             })
             .ok,
         "plain provider rerun should publish before empty capture");
  const auto empty = kernel.run_cell(2, adapter.as_executor());
  expect(empty.ok && empty.runtime_dependencies.has_value() &&
             empty.runtime_dependencies->notebook_cell_id == 2U &&
             empty.runtime_dependencies->dependencies.empty() &&
             kernel.runtime_dependencies(2).has_value() &&
             kernel.runtime_dependencies(2)->dependencies.empty(),
         "successful empty adapter capture should clear the stored set");
}

void test_compiled_source_runs_through_runtime_and_kernel() {
  const std::vector<CellSource> cells{cell(1, "x = 2\n"),
                                      cell(2, "y = x + 1\n")};
  const amber::notebook::DependencyGraph graph(cells);
  NotebookImage image;
  const CellCompileResult provider =
      amber::notebook::compile_cell(cells[0], graph, image);
  const CellCompileResult consumer =
      amber::notebook::compile_cell(cells[1], graph, image);
  expect(provider.ok && consumer.ok,
         "source cells should compile into NotebookCell bytecode");
  expect(consumer.inputs.size() == 1U && consumer.outputs.size() == 1U,
         "compiled consumer should expose one input and one output");

  // Exercise the persistence boundary: execution metadata must be rebuilt
  // solely from a serialized and decoded image, without CellCompileResult.
  const std::vector<std::uint8_t> bytes =
      amber::bytecode::serialize_module(*image.snapshot());
  amber::bytecode::DecodeResult decoded =
      amber::bytecode::deserialize_module(bytes);
  expect(decoded.ok(), "serialized notebook image should verify and decode");
  std::string manifest_error;
  const std::optional<VmNotebookImageManifest> manifest =
      amber::notebook::vm_notebook_image_manifest(decoded.module,
                                                   &manifest_error);
  expect(manifest.has_value(), manifest_error);
  const auto *provider_manifest = manifest->cell_for_id(cells[0].id);
  const auto *consumer_manifest = manifest->cell_for_id(cells[1].id);
  expect(provider_manifest != nullptr && consumer_manifest != nullptr,
         "decoded manifest should preserve both CellIds");
  expect(provider_manifest->code_id == provider.code_id &&
             consumer_manifest->code_id == consumer.code_id,
         "decoded manifest should preserve CellId-to-code_id identity");

  NotebookKernel kernel(cells);
  amber::runtime::RuntimeWorldOptions options;
  options.external_gc_root_provider = [&kernel]() { return kernel.gc_roots(); };
  auto decoded_image =
      std::make_shared<const BcModule>(std::move(decoded.module));
  amber::runtime::RuntimeWorld world(decoded_image, std::move(options));
  NotebookVmCellExecutor provider_executor(
      &world, provider_manifest->code_id, provider_manifest->descriptors);
  NotebookVmCellExecutor consumer_executor(
      &world, consumer_manifest->code_id, consumer_manifest->descriptors);
  expect(provider_executor.valid() && consumer_executor.valid(),
         "compiled descriptor tables should be valid");
  expect(kernel.run_cell(cells[0].id, provider_executor.as_executor()).ok,
         "compiled provider should publish x");
  const auto result =
      kernel.run_cell(cells[1].id, consumer_executor.as_executor());
  expect(result.ok, result.error);
  const auto y = kernel.read_slot({2, "y"});
  expect(y.has_value() && y->is_integer() && y->as_integer() == 3,
         "compiled consumer should publish y = 3");
}

} // namespace

int main() {
  test_consumer_executes_and_publishes_atomically();
  test_fault_after_store_discards_partial_batch();
  test_fault_after_observation_preserves_dynamic_snapshot();
  test_heap_staging_is_rooted_across_forced_safepoint();
  test_execution_result_releases_pins_after_world_destroyed();
  test_executor_keeps_runtime_world_handle_alive();
  test_incomplete_store_is_rejected_before_kernel_commit();
  test_executor_rejects_replaced_image_generation();
  test_executor_captures_only_executed_notebook_slot_reads();
  test_executor_reports_run_local_runtime_dependency_capture();
  test_kernel_stores_adapter_runtime_dependencies_transactionally();
  test_compiled_source_runs_through_runtime_and_kernel();
  std::cout << "notebook_vm_integration_tests ok\n";
  return 0;
}
