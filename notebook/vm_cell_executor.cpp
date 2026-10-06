#include "notebook/vm_cell_executor.h"

#include "runtime/objects.h"
#include "runtime/context.h"

#include <algorithm>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <utility>

namespace amber::notebook {

namespace {

void set_error(std::string *error, std::string message) {
  if (error != nullptr) {
    *error = std::move(message);
  }
}

} // namespace

struct NotebookVmExecutionState {
  explicit NotebookVmExecutionState(runtime::RuntimeWorld *runtime_world)
      : heap(runtime_world == nullptr ? runtime::RuntimeHeap{}
                                      : runtime_world->heap_handle()) {}

  ~NotebookVmExecutionState() {
    std::vector<runtime::RuntimePinToken> retained;
    {
      std::lock_guard<std::mutex> lock(mutex);
      retained.swap(pins);
      staged_roots.clear();
    }
    for (runtime::RuntimePinToken &pin : retained) {
      if (pin.active) {
        (void)heap.unpin(&pin);
      }
    }
  }

  // RuntimeHeap is a copyable handle over the shared heap implementation.  It
  // deliberately replaces the RuntimeWorld pointer here: an execution result
  // may outlive the world that performed the execution, but staged pins still
  // need a live heap implementation in order to release their tokens.
  runtime::RuntimeHeap heap;
  mutable std::mutex mutex;
  std::map<std::uint32_t, runtime::Value> staged_roots;
  std::set<BindingKey> observed_dependencies;
  std::vector<runtime::RuntimePinToken> pins;

  std::vector<runtime::Value> snapshot() const {
    std::vector<runtime::Value> roots;
    std::lock_guard<std::mutex> lock(mutex);
    roots.reserve(staged_roots.size());
    for (const auto &entry : staged_roots) {
      roots.push_back(entry.second);
    }
    return roots;
  }

  std::optional<runtime::Value> staged_value(std::uint32_t descriptor) const {
    std::lock_guard<std::mutex> lock(mutex);
    const auto found = staged_roots.find(descriptor);
    return found == staged_roots.end()
               ? std::nullopt
               : std::optional<runtime::Value>(found->second);
  }

  void observe(BindingKey key) {
    std::lock_guard<std::mutex> lock(mutex);
    observed_dependencies.insert(std::move(key));
  }

  std::vector<BindingKey> dependency_snapshot() const {
    std::lock_guard<std::mutex> lock(mutex);
    return {observed_dependencies.begin(), observed_dependencies.end()};
  }
};

bool VmCellDescriptorTable::validate(std::string *error) const {
  std::map<std::uint32_t, const VmSlotDescriptor *> descriptors;
  std::map<BindingKey, NotebookSlotDirection> keys;
  for (const VmSlotDescriptor &slot : slots) {
    if (slot.key.cell_id == 0U) {
      set_error(error, "notebook slot descriptor has a zero cell id");
      return false;
    }
    if (slot.key.name.empty()) {
      set_error(error, "notebook slot descriptor has an empty binding name");
      return false;
    }
    if (!descriptors.emplace(slot.descriptor, &slot).second) {
      set_error(error, "duplicate notebook slot descriptor: " +
                           std::to_string(slot.descriptor));
      return false;
    }
    const auto found = keys.find(slot.key);
    if (found != keys.end()) {
      set_error(error, "duplicate notebook binding in descriptor table: " +
                           std::to_string(slot.key.cell_id) + "@" +
                           slot.key.name);
      return false;
    }
    keys.emplace(slot.key, slot.direction);
  }
  return true;
}

const VmNotebookCellManifest *
VmNotebookImageManifest::cell_for_id(CellId cell_id) const {
  const auto found =
      std::find_if(cells.begin(), cells.end(), [cell_id](const auto &cell) {
        return cell.cell_id == cell_id;
      });
  return found == cells.end() ? nullptr : &*found;
}

const VmNotebookCellManifest *
VmNotebookImageManifest::cell_for_code(std::uint32_t code_id) const {
  const auto found =
      std::find_if(cells.begin(), cells.end(), [code_id](const auto &cell) {
        return cell.code_id == code_id;
      });
  return found == cells.end() ? nullptr : &*found;
}

std::optional<VmNotebookImageManifest>
vm_notebook_image_manifest(const bytecode::BcModule &module,
                           std::string *error) {
  if ((module.file_flags & bytecode::kFileFlagNotebookOnly) == 0U) {
    set_error(error, "module is not marked as a notebook-only image");
    return std::nullopt;
  }
  if (!module.notebook_metadata.has_value()) {
    set_error(error, "notebook image has no NBMD metadata");
    return std::nullopt;
  }
  if (module.format_version.major != 1U ||
      module.format_version.minor != 1U) {
    set_error(error,
              "notebook metadata requires supported bytecode format 1.1");
    return std::nullopt;
  }
  if (module.notebook_metadata->schema_version.major !=
          bytecode::kNotebookMetadataSchemaMajor ||
      module.notebook_metadata->schema_version.minor >
          bytecode::kNotebookMetadataSchemaMinor) {
    set_error(error, "unsupported notebook image metadata schema");
    return std::nullopt;
  }

  std::map<std::uint32_t, BindingKey> descriptors;
  std::map<BindingKey, std::uint32_t> binding_descriptors;
  for (const bytecode::NotebookDescriptorEntry &entry :
       module.notebook_metadata->descriptors) {
    if (entry.cell_id == 0U) {
      set_error(error, "notebook image descriptor has a zero cell id");
      return std::nullopt;
    }
    if (entry.name_str_id >= module.strings.size() ||
        module.strings[entry.name_str_id].empty()) {
      set_error(error,
                "notebook image descriptor has an invalid binding name");
      return std::nullopt;
    }
    BindingKey key{entry.cell_id, module.strings[entry.name_str_id]};
    if (!descriptors.emplace(entry.descriptor_id, key).second) {
      set_error(error, "duplicate notebook image descriptor id: " +
                           std::to_string(entry.descriptor_id));
      return std::nullopt;
    }
    if (!binding_descriptors.emplace(key, entry.descriptor_id).second) {
      set_error(error, "duplicate notebook image binding descriptor: " +
                           std::to_string(key.cell_id) + "@" + key.name);
      return std::nullopt;
    }
  }

  VmNotebookImageManifest manifest;
  manifest.cells.reserve(module.notebook_metadata->cells.size());
  std::map<CellId, std::uint32_t> cell_ids;
  std::map<std::uint32_t, CellId> code_ids;
  for (const bytecode::NotebookCellEntry &entry :
       module.notebook_metadata->cells) {
    if (entry.cell_id == 0U || entry.code_id == 0U) {
      set_error(error,
                "notebook image cell manifest has a zero cell or code id");
      return std::nullopt;
    }
    if (!cell_ids.emplace(entry.cell_id, entry.code_id).second) {
      set_error(error, "duplicate notebook image cell id: " +
                           std::to_string(entry.cell_id));
      return std::nullopt;
    }
    if (!code_ids.emplace(entry.code_id, entry.cell_id).second) {
      set_error(error, "duplicate notebook image cell code id: " +
                           std::to_string(entry.code_id));
      return std::nullopt;
    }
    const auto code = std::find_if(
        module.code_objects.begin(), module.code_objects.end(),
        [&entry](const bytecode::BcCode &candidate) {
          return candidate.code_id == entry.code_id;
        });
    if (code == module.code_objects.end() ||
        code->kind != bytecode::CodeKind::NotebookCell) {
      set_error(error,
                "notebook image cell manifest references invalid code id: " +
                    std::to_string(entry.code_id));
      return std::nullopt;
    }

    VmNotebookCellManifest cell;
    cell.cell_id = entry.cell_id;
    cell.code_id = entry.code_id;
    cell.descriptors.slots.reserve(entry.input_descriptor_ids.size() +
                                   entry.output_descriptor_ids.size());
    for (const std::uint32_t descriptor : entry.input_descriptor_ids) {
      const auto found = descriptors.find(descriptor);
      if (found == descriptors.end()) {
        set_error(error, "notebook input references unknown descriptor: " +
                             std::to_string(descriptor));
        return std::nullopt;
      }
      cell.descriptors.slots.push_back(
          {descriptor, found->second, NotebookSlotDirection::Input});
    }
    for (const std::uint32_t descriptor : entry.output_descriptor_ids) {
      const auto found = descriptors.find(descriptor);
      if (found == descriptors.end()) {
        set_error(error, "notebook output references unknown descriptor: " +
                             std::to_string(descriptor));
        return std::nullopt;
      }
      if (found->second.cell_id != entry.cell_id) {
        set_error(error,
                  "notebook output descriptor belongs to another cell");
        return std::nullopt;
      }
      cell.descriptors.slots.push_back(
          {descriptor, found->second, NotebookSlotDirection::Output});
    }
    std::string table_error;
    if (!cell.descriptors.validate(&table_error)) {
      set_error(error, "invalid notebook cell descriptor table: " +
                           table_error);
      return std::nullopt;
    }
    manifest.cells.push_back(std::move(cell));
  }

  // A descriptor's BindingKey names its provider. Requiring every descriptor
  // to be one provider output (and every input to point at that exact output)
  // prevents a stale or spliced manifest from manufacturing dependencies that
  // do not exist in the decoded image.
  std::map<std::uint32_t, CellId> output_providers;
  for (const VmNotebookCellManifest &cell : manifest.cells) {
    for (const VmSlotDescriptor &slot : cell.descriptors.slots) {
      if (slot.direction == NotebookSlotDirection::Output) {
        output_providers.emplace(slot.descriptor, cell.cell_id);
      }
    }
  }
  for (const auto &descriptor : descriptors) {
    if (output_providers.find(descriptor.first) == output_providers.end()) {
      set_error(error, "orphan notebook image descriptor: " +
                           std::to_string(descriptor.first));
      return std::nullopt;
    }
  }
  for (const VmNotebookCellManifest &cell : manifest.cells) {
    for (const VmSlotDescriptor &slot : cell.descriptors.slots) {
      if (slot.direction != NotebookSlotDirection::Input) {
        continue;
      }
      const auto provider = output_providers.find(slot.descriptor);
      if (provider == output_providers.end() ||
          provider->second != slot.key.cell_id) {
        set_error(error,
                  "notebook input descriptor has no matching provider output");
        return std::nullopt;
      }
    }
  }

  for (const bytecode::BcCode &code : module.code_objects) {
    if (code.kind == bytecode::CodeKind::NotebookCell &&
        code_ids.find(code.code_id) == code_ids.end()) {
      set_error(error, "notebook cell code has no manifest entry: " +
                           std::to_string(code.code_id));
      return std::nullopt;
    }
  }
  return manifest;
}

VmCellDescriptorTable vm_slot_table(const CellCompileResult &compiled) {
  VmCellDescriptorTable table;
  table.slots.reserve(compiled.inputs.size() + compiled.outputs.size());
  for (const CellSlotUse &input : compiled.inputs) {
    table.slots.push_back(VmSlotDescriptor{
        input.descriptor.id, input.descriptor.key, NotebookSlotDirection::Input});
  }
  for (const CellSlotUse &output : compiled.outputs) {
    table.slots.push_back(VmSlotDescriptor{
        output.descriptor.id, output.descriptor.key,
        NotebookSlotDirection::Output});
  }
  return table;
}

NotebookVmCellExecutor::NotebookVmCellExecutor(
    runtime::RuntimeWorld *world, std::uint32_t code_id,
    VmCellDescriptorTable descriptors)
    : NotebookVmCellExecutor(world, code_id, std::move(descriptors), 0) {}

NotebookVmCellExecutor::NotebookVmCellExecutor(
    runtime::RuntimeWorld *world, std::uint32_t code_id,
    VmCellDescriptorTable descriptors, CellId consumer_cell_id)
    : world_(world == nullptr
                 ? std::nullopt
                 : std::optional<runtime::RuntimeWorld>(*world)),
      code_id_(code_id), consumer_cell_id_(consumer_cell_id),
      descriptors_(std::move(descriptors)) {
  if (!world_.has_value()) {
    validation_error_ = "notebook VM executor has no runtime world";
    return;
  }
  world_epoch_ = world_->world_epoch();
  if (!descriptors_.validate(&validation_error_)) {
    return;
  }
  for (const VmSlotDescriptor &slot : descriptors_.slots) {
    by_descriptor_.emplace(slot.descriptor, slot);
  }
}

const VmSlotDescriptor *
NotebookVmCellExecutor::find(std::uint32_t descriptor) const {
  const auto found = by_descriptor_.find(descriptor);
  return found == by_descriptor_.end() ? nullptr : &found->second;
}

std::string NotebookVmCellExecutor::binding_label(const BindingKey &key) {
  return key.name + "@" + std::to_string(key.cell_id);
}

CellExecutionResult
NotebookVmCellExecutor::execute(const std::vector<KernelInput> &inputs,
                                VmCellExecutionReport *report) const {
  CellExecutionResult result;
  if (report != nullptr) {
    *report = VmCellExecutionReport{};
  }
  if (!valid()) {
    result.error = validation_error_;
    return result;
  }

  std::map<BindingKey, runtime::Value> input_values;
  for (const KernelInput &input : inputs) {
    if (!input_values.emplace(input.key, input.value).second) {
      result.error = "duplicate notebook VM input: " + binding_label(input.key);
      return result;
    }
  }

  const std::shared_ptr<NotebookVmExecutionState> state =
      std::make_shared<NotebookVmExecutionState>(&*world_);
  runtime::RuntimeNotebookCellContext context;
  context.live_store = runtime::NotebookLiveScope::current();
  context.live_cell_id = consumer_cell_id_;
  context.project_inputs = project_inputs_;
  context.load_slot =
      [&input_values,
       this](std::uint32_t descriptor) -> std::optional<runtime::Value> {
    const VmSlotDescriptor *slot = find(descriptor);
    if (slot == nullptr || slot->direction != NotebookSlotDirection::Input) {
      return std::nullopt;
    }
    const auto value = input_values.find(slot->key);
    if (value == input_values.end()) {
      return std::nullopt;
    }
    return value->second;
  };
  context.observe_slot = [state, this](std::uint32_t descriptor) {
    const VmSlotDescriptor *slot = find(descriptor);
    if (slot == nullptr || slot->direction != NotebookSlotDirection::Input) {
      return false;
    }
    state->observe(slot->key);
    return true;
  };
  context.stage_slot = [state, this](std::uint32_t descriptor,
                                     runtime::Value value) {
    const VmSlotDescriptor *slot = find(descriptor);
    if (slot == nullptr || slot->direction != NotebookSlotDirection::Output) {
      return false;
    }

    runtime::RuntimePinResult pin = state->heap.pin(value);
    if (!pin.ok && !(pin.error_name == "TypeError" &&
                     !runtime::value_has_heap_payload_tag(value))) {
      return false;
    }
    // Assignment is intentional: a repeated STORE is an update of the
    // staged descriptor, and the last value is the one that gets published.
    try {
      std::lock_guard<std::mutex> lock(state->mutex);
      if (pin.token.active) {
        state->pins.push_back(pin.token);
        pin.token.active = false;
      }
      state->staged_roots[descriptor] = value;
    } catch (...) {
      if (pin.token.active) {
        (void)state->heap.unpin(&pin.token);
      }
      throw;
    }
    return true;
  };
  context.gc_roots = [state]() { return state->snapshot(); };
  if (consumer_cell_id_ != 0U) {
    context.dependency_capture.consumer_cell_id = consumer_cell_id_;
  }

  runtime::ExecutionResult runtime_result;
  try {
    runtime_result =
        world_->execute_notebook_cell(code_id_, std::move(context), {},
                                      world_epoch_);
  } catch (const std::exception &error) {
    result.error = std::string("notebook VM execution failed: ") + error.what();
    return result;
  } catch (...) {
    result.error = "notebook VM execution failed with an unknown exception";
    return result;
  }

  if (report) for (const auto &event : runtime_result.live_events)
    if (event.kind == runtime::NotebookLiveEventKind::Progress) report->progress.push_back(event);
  if (!runtime_result.ok()) {
    if (runtime_result.fault.has_value()) {
      result.error = runtime_result.fault->error_name;
      if (!runtime_result.fault->message.empty()) {
        result.error += ": ";
        result.error += runtime_result.fault->message;
      }
    } else {
      result.error = "notebook VM execution failed";
    }
    // The RuntimeWorld callback may have observed STOREs before a later
    // fault.  Never expose that partial batch to the kernel.
    return result;
  }

  if (runtime::runtime_run_cancel_requested()) {
    result.error = "CancelledError: execution cancelled before publication";
    return result;
  }

  for (const VmSlotDescriptor &slot : descriptors_.slots) {
    if (slot.direction != NotebookSlotDirection::Output) {
      continue;
    }
    const std::optional<runtime::Value> staged_value =
        state->staged_value(slot.descriptor);
    if (!staged_value.has_value()) {
      result.error =
          "notebook output was not staged: " + binding_label(slot.key);
      result.writes.clear();
      return result;
    }
    result.writes.push_back(KernelWrite{slot.key, *staged_value, std::nullopt});
  }
  // Keep the rich capture on the kernel result, rather than exposing a
  // partially observed set through the report or result when VM execution or
  // adapter output validation failed.  RuntimeWorld tags an explicitly
  // requested capture with the adapter's consumer CellId; reject a malformed
  // result before transferring it across the adapter boundary.
  if (consumer_cell_id_ != 0U &&
      runtime_result.dependency_capture.notebook_cell_id !=
          consumer_cell_id_) {
    result.error =
        "notebook VM runtime dependency capture has an unexpected consumer";
    result.writes.clear();
    return result;
  }
  result.dynamic_dependencies = state->dependency_snapshot();
  result.input_dependencies = std::move(runtime_result.input_dependencies);
  if (consumer_cell_id_ != 0U) {
    result.runtime_dependencies = runtime_result.dependency_capture;
  }
  if (report != nullptr) {
    report->value = runtime_result.value;
    report->displays = std::move(runtime_result.displays);
    report->locals = runtime_result.locals;
    report->watch_epoch = runtime_result.watch_epoch;
    report->watch_event_count = runtime_result.watch_events.size();
    report->runtime_strings = runtime_result.runtime_strings;
    report->runtime_symbols = runtime_result.runtime_symbols;
    report->dependency_capture = runtime_result.dependency_capture;
  }
  result.keepalive = state;
  result.ok = true;
  return result;
}

CellExecutor
NotebookVmCellExecutor::as_executor(VmCellExecutionReport *report) const {
  const NotebookVmCellExecutor copy = *this;
  return [copy, report](const std::vector<KernelInput> &inputs) {
    return copy.execute(inputs, report);
  };
}

} // namespace amber::notebook
