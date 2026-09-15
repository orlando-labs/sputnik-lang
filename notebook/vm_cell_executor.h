#pragma once

#include "notebook/compiler.h"
#include "notebook/kernel.h"
#include "runtime/world.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace amber::notebook {

// A bytecode descriptor is deliberately opaque to the runtime.  The notebook
// compiler/adapter owns the mapping from that descriptor to a persistent
// BindingKey and declares whether the descriptor may be loaded or stored.
enum class NotebookSlotDirection {
  Input,
  Output,
};

struct VmSlotDescriptor {
  std::uint32_t descriptor = 0;
  BindingKey key;
  NotebookSlotDirection direction = NotebookSlotDirection::Input;
};

struct VmCellDescriptorTable {
  std::vector<VmSlotDescriptor> slots;

  bool validate(std::string *error = nullptr) const;
};

struct VmNotebookCellManifest {
  CellId cell_id = 0;
  std::uint32_t code_id = 0;
  VmCellDescriptorTable descriptors;
};

// Immutable adapter metadata reconstructed from a serialized notebook image.
// Keeping this distinct from CellCompileResult makes loading/restarting an
// image independent of compiler-temporary objects.
struct VmNotebookImageManifest {
  std::vector<VmNotebookCellManifest> cells;

  const VmNotebookCellManifest *cell_for_id(CellId cell_id) const;
  const VmNotebookCellManifest *cell_for_code(std::uint32_t code_id) const;
};

std::optional<VmNotebookImageManifest>
vm_notebook_image_manifest(const bytecode::BcModule &module,
                           std::string *error = nullptr);

// The VM returns more than the staged notebook outputs.  Keep this report
// channel separate from CellExecutionResult so the generic kernel stays
// independent of runtime presentation details (and so callers can retain
// the expression result for cells that do not write a binding).
struct VmCellExecutionReport {
  runtime::Value value = runtime::Value::null();
  std::vector<runtime::ExecutionLocal> locals;
  std::uint64_t watch_epoch = 0;
  std::size_t watch_event_count = 0;
  std::vector<std::string> runtime_strings;
  std::vector<std::string> runtime_symbols;
  // Rich runtime dependencies remain separate from the notebook's legacy
  // vector<BindingKey> ObserveSlot snapshot.  This is populated only after
  // the complete cell execution (including output validation) succeeds.
  runtime::RuntimeDependencySet dependency_capture;
};

// Build the runtime descriptor table emitted by compile_cell.  Inputs and
// outputs use the same descriptor identity, but direction is per cell use.
VmCellDescriptorTable vm_slot_table(const CellCompileResult &compiled);

// UI-independent bridge from the persistent notebook kernel to one compiled
// CodeKind::NotebookCell.  The RuntimeWorld only executes and stages values;
// NotebookKernel remains the sole owner of publication and invalidation.
class NotebookVmCellExecutor {
public:
  NotebookVmCellExecutor(runtime::RuntimeWorld *world, std::uint32_t code_id,
                         VmCellDescriptorTable descriptors);
  // New callers provide the consumer identity used by runtime dependency
  // capture.  Keep the old constructor above as a no-capture default.
  NotebookVmCellExecutor(runtime::RuntimeWorld *world, std::uint32_t code_id,
                         VmCellDescriptorTable descriptors,
                         CellId consumer_cell_id);

  bool valid() const noexcept { return validation_error_.empty(); }
  const std::string &validation_error() const noexcept {
    return validation_error_;
  }

  CellExecutionResult
  execute(const std::vector<KernelInput> &inputs,
          VmCellExecutionReport *report = nullptr) const;
  CellExecutor as_executor(VmCellExecutionReport *report = nullptr) const;

private:
  const VmSlotDescriptor *find(std::uint32_t descriptor) const;
  static std::string binding_label(const BindingKey &key);

  // RuntimeWorld is a cheap shared-Impl handle. Retaining a copy keeps the
  // execution boundary alive when a queued CellExecutor outlives the UI/session
  // object that originally constructed it.
  mutable std::optional<runtime::RuntimeWorld> world_;
  std::uint64_t world_epoch_ = 0;
  std::uint32_t code_id_ = 0;
  CellId consumer_cell_id_ = 0;
  VmCellDescriptorTable descriptors_;
  std::map<std::uint32_t, VmSlotDescriptor> by_descriptor_;
  std::string validation_error_;
};

} // namespace amber::notebook
