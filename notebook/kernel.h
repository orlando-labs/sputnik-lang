#pragma once

#include "notebook/dependency_graph.h"
#include "notebook/scheduler.h"
#include "notebook/slot_table.h"
#include "runtime/watch.h"

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace amber::notebook {

struct KernelInput {
  BindingKey key;
  runtime::Value value = runtime::Value::null();
  std::uint64_t revision = 0;
};

struct KernelWrite {
  BindingKey key;
  runtime::Value value = runtime::Value::null();
  std::optional<SlotProviderMetadata> provider;
};

// Result returned by a compiler/VM backend.  Writes remain ordinary values
// until NotebookKernel validates the complete batch and commits it atomically.
struct CellExecutionResult {
  bool ok = false;
  std::string error;
  std::vector<KernelWrite> writes;
  std::vector<BindingKey> dynamic_dependencies;
  // Optional rich runtime dependency snapshot.  nullopt means that this
  // executor did not request capture; an engaged, empty set is a successful
  // capture that intentionally clears the previous snapshot.  The kernel
  // validates both consumer and RuntimeWorld identities and publishes this
  // set only with the same successful slot transaction as
  // dynamic_dependencies. The first successful namespaced capture may bind
  // an otherwise unbound kernel atomically with that publication.
  std::optional<runtime::RuntimeDependencySet> runtime_dependencies;
  // Optional lifetime fence for backend-owned resources (for example runtime
  // heap pins protecting staged values). NotebookKernel keeps the execution
  // result alive through validation and the atomic publication transaction.
  std::shared_ptr<void> keepalive;
  std::optional<std::set<std::string>> input_dependencies;
};

using CellExecutor =
    std::function<CellExecutionResult(const std::vector<KernelInput> &)>;

struct CellRunResult {
  bool ok = false;
  std::string error;
  std::vector<KernelInput> inputs;
  SlotCommitResult publication;
  std::vector<BindingKey> dynamic_dependencies;
  std::optional<runtime::RuntimeDependencySet> runtime_dependencies;
};

using RuntimeDependencySourceKey = runtime::RuntimeDependencySourceKey;

enum class RuntimeEventBatchPlanStatus {
  Accepted,
  InvalidBatch,
  SourceMismatch,
};

struct RuntimeEventBatchPlanResult {
  RuntimeEventBatchPlanStatus status =
      RuntimeEventBatchPlanStatus::InvalidBatch;
  std::string error;
  std::vector<EvaluationStep> plan;
  // Present only for Accepted. Hosts publish this validated value rather than
  // reaching back into the untrusted poll result after planning.
  std::optional<runtime::RuntimeWatchCursor> acknowledgement_cursor;

  bool accepted() const noexcept {
    return status == RuntimeEventBatchPlanStatus::Accepted;
  }
};

// UI-independent orchestration boundary for persistent notebook execution.
// The current kernel accepts an executor callback; the VM integration can
// later implement that callback with RuntimeWorld::execute_notebook_cell.
class NotebookKernel {
public:
  // Move-only two-phase graph/dependency update.  Preparation owns the
  // kernel execution lock but does not mutate published state, allowing a
  // host to install a candidate VM image before committing the matching
  // graph generation.  Destruction without commit is a safe cancellation.
  // As with a slot-table transaction, the owning NotebookKernel must outlive
  // every prepared update because the update retains its execution lock.
  class PreparedCellUpdate {
  public:
    PreparedCellUpdate() = default;
    PreparedCellUpdate(const PreparedCellUpdate &) = delete;
    PreparedCellUpdate &operator=(const PreparedCellUpdate &) = delete;
    PreparedCellUpdate(PreparedCellUpdate &&) noexcept = default;
    PreparedCellUpdate &operator=(PreparedCellUpdate &&) noexcept = default;

    bool active() const noexcept;
    bool committed() const noexcept { return committed_; }
    const std::vector<EvaluationStep> &plan() const noexcept { return plan_; }
    std::vector<EvaluationStep> take_plan() noexcept;
    void cancel() noexcept;

  private:
    friend class NotebookKernel;
    PreparedCellUpdate(
        NotebookKernel *owner, std::unique_lock<std::mutex> execution_lock,
        std::vector<CellSource> sources, DependencyGraph graph,
        std::vector<EvaluationStep> plan,
        std::vector<BindingKey> stale_outputs,
        std::map<CellId, std::vector<BindingKey>> dynamic_dependencies,
        std::map<CellId, runtime::RuntimeDependencySet> runtime_dependencies,
        std::map<RuntimeDependencySourceKey, std::set<CellId>>
            runtime_dependency_consumers);

    NotebookKernel *owner_ = nullptr;
    std::unique_lock<std::mutex> execution_lock_;
    std::vector<CellSource> sources_;
    DependencyGraph graph_;
    std::vector<EvaluationStep> plan_;
    std::vector<BindingKey> stale_outputs_;
    std::map<CellId, std::vector<BindingKey>> dynamic_dependencies_;
    std::map<CellId, runtime::RuntimeDependencySet> runtime_dependencies_;
    std::map<RuntimeDependencySourceKey, std::set<CellId>>
        runtime_dependency_consumers_;
    bool committed_ = false;
  };

  NotebookKernel() = default;
  explicit NotebookKernel(const std::vector<CellSource> &cells);
  // Ambient names are an immutable environment-generation contract.  Hosts
  // that change the environment must construct a new kernel/world rather
  // than mutating a live kernel.
  NotebookKernel(const std::vector<CellSource> &cells,
                 std::set<std::string> ambient_names);
  NotebookKernel(const NotebookKernel &) = delete;
  NotebookKernel &operator=(const NotebookKernel &) = delete;

  const std::set<std::string> &ambient_names() const noexcept {
    return ambient_names_;
  }

  // Rebuilds the source graph, invalidates outputs reachable in either graph
  // version, and returns the watch/manual execution plan for this edit.
  std::vector<EvaluationStep> update_cells(const std::vector<CellSource> &cells,
                                           const std::set<CellId> &changed,
                                           bool force_all = false);
  std::vector<EvaluationStep> update_cells(const std::vector<CellSource> &cells,
                                           CellId changed,
                                           bool force_all = false);
  PreparedCellUpdate
  prepare_update_cells(const std::vector<CellSource> &cells,
                       const std::set<CellId> &changed,
                       bool force_all = false);
  PreparedCellUpdate prepare_update_cells(const std::vector<CellSource> &cells,
                                          CellId changed,
                                          bool force_all = false);
  bool commit_update_cells(PreparedCellUpdate &update) noexcept;

  // Runs one cell under the kernel serialization lock.  State is not locked
  // while the executor runs, so a RuntimeWorld safepoint may call gc_roots().
  // Failed or structurally invalid executions do not publish writes or replace
  // either previous dependency snapshot.  An executor that leaves
  // runtime_dependencies disengaged is compatible with legacy callers and
  // leaves the previous rich snapshot untouched.
  CellRunResult run_cell(CellId id, const CellExecutor &executor);

  std::optional<SlotState> slot_state(const BindingKey &key) const;
  std::optional<runtime::Value> read_slot(const BindingKey &key) const;
  std::size_t slot_count() const;
  bool slots_empty() const { return slot_count() == 0U; }
  std::vector<CellId> cell_order() const;
  std::vector<BindingKey> dynamic_dependencies(CellId id) const;
  // Returns the last rich runtime dependency snapshot for a cell.  nullopt
  // means that no capture has been published for that cell.
  std::optional<runtime::RuntimeDependencySet>
  runtime_dependencies(CellId id) const;

  // Pure runtime-event invalidation.  The caller must keep all dependency
  // source IDs and events within one runtime domain: numeric runtime object
  // and watch-cell identities are intentionally opaque here.  These methods
  // acquire the kernel execution/state locks, never poll or call
  // RuntimeWorld, and conservatively schedule through the static graph.
  std::vector<EvaluationStep>
  plan_runtime_events(const std::vector<runtime::RuntimeWatchEvent> &events);
  std::vector<EvaluationStep>
  plan_runtime_events(const runtime::RuntimeWatchEvent &event);

  // Validates and plans one read-only RuntimeWorld poll result as a single
  // acknowledgement unit, including its exact requested cursor and contiguous
  // event range. A timed-out wait result is not an acknowledgement batch. The
  // caller may publish poll.next_cursor only when this returns Accepted and
  // downstream execution has applied the plan.
  RuntimeEventBatchPlanResult plan_runtime_event_batch(
      const runtime::RuntimeWatchPollResult &poll);

  // Binds opaque runtime IDs in this kernel to one world stream namespace.
  // Rebinding to a different source is rejected. Hosts may bind explicitly
  // before execution; otherwise the first successful namespaced rich capture
  // binds atomically with its slot publication. Every engaged capture must
  // carry a valid source identity.
  bool bind_runtime_watch_source(
      const runtime::RuntimeWatchStreamIdentity &source);

  // Explicit source-change recovery. Runtime numeric identities cannot cross
  // stream namespaces, so this clears rich dependency snapshots/reverse edges
  // while preserving sources and the static graph. Published slots are
  // cleared because their watch cells/heap objects may belong to the old
  // world and cannot safely become inputs or GC roots of the new one. A host
  // must follow this with plan_runtime_resync().
  bool reset_runtime_watch_source(
      const runtime::RuntimeWatchStreamIdentity &source);

  // Conservative recovery for a host that fell behind a bounded runtime
  // event stream. Every current cell is treated as an automatic root: watch
  // cells rerun, manual cells stay stale, and their descendants are blocked.
  // This never treats stream recovery as an explicit user edit.
  std::vector<EvaluationStep> plan_runtime_resync();

  // Snapshot of consumers currently indexed for one source identity.  This
  // is useful to host adapters/tests and is independent of source revisions.
  std::vector<CellId> runtime_dependency_consumers(
      const RuntimeDependencySourceKey &source) const;
  std::vector<runtime::Value> gc_roots() const;

  // Installs a failure barrier in the currently published graph.  The
  // kernel keeps historical slot values for GC/debugging, but every output
  // reachable from a failed root becomes stale and therefore cannot be used
  // as an input by a subsequent run.  This is used when a document rebuild
  // fails before update_cells() can publish the new graph.
  void mark_failure_barrier(const std::set<CellId> &roots);
  void mark_failure_barrier(CellId root);

  // Clears cells, slots, and runtime snapshots while preserving the immutable
  // ambient-name configuration.  Changing that configuration requires a new
  // NotebookKernel/world generation.
  void reset();
  // Project-scoped external input events, with normal Watch/Manual barriers.
  std::vector<EvaluationStep> plan_input_changes(const std::set<std::string> &keys);

private:
  std::vector<BindingKey> expected_writes(CellId id) const;
  std::vector<EvaluationStep> plan_runtime_events_locked(
      const std::vector<runtime::RuntimeWatchEvent> &events);
  void mark_plan_outputs_stale(const DependencyGraph *previous,
                               const DependencyGraph &current,
                               const std::vector<EvaluationStep> &plan);
  static std::vector<BindingKey>
  plan_outputs_to_stale(const DependencyGraph *previous,
                        const DependencyGraph &current,
                        const std::vector<EvaluationStep> &plan);

  // execution_mutex_ serializes runs with graph updates/reset. state_mutex_
  // protects short metadata snapshots and is deliberately released around a
  // VM/backend call so its external GC-root callback can re-enter gc_roots().
  mutable std::mutex execution_mutex_;
  mutable std::mutex state_mutex_;
  std::set<std::string> ambient_names_;
  std::vector<CellSource> sources_;
  DependencyGraph graph_;
  NotebookSlotTable slots_;
  std::map<CellId, std::vector<BindingKey>> dynamic_dependencies_;
  std::map<CellId, runtime::RuntimeDependencySet> runtime_dependencies_;
  std::map<RuntimeDependencySourceKey, std::set<CellId>>
      runtime_dependency_consumers_;
  std::optional<runtime::RuntimeWatchStreamIdentity> runtime_watch_source_;
  std::map<CellId, std::set<std::string>> input_dependencies_;
  // Failed attempts may have read a new branch. Retry conservatively without
  // replacing the last successful dependency snapshot on a failed transaction.
  std::set<CellId> input_retry_consumers_;
};

} // namespace amber::notebook
