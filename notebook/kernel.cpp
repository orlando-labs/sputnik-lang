#include "notebook/kernel.h"
#include "runtime/context.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace amber::notebook {

namespace {

std::string binding_label(const BindingKey &key) {
  return key.name + "@" + std::to_string(key.cell_id);
}

bool is_binding_mutation_kind(const std::string &kind) {
  return kind == "watch.write";
}

bool is_ivar_mutation_kind(const std::string &kind) {
  return kind == "watch.ivar.write";
}

bool is_object_mutation_kind(const std::string &kind) {
  // RuntimeWatch currently has no standalone object writer, but preserve a
  // future object mutation event as a deliberately named extension point.
  return kind == "watch.object.write";
}

bool is_binding_mutation(const runtime::RuntimeWatchEvent &event) {
  if (event.cell_id == 0U || event.object_id != 0U ||
      !event.field_name.empty()) {
    return false;
  }
  // RuntimeWatch emits watch.write; registration events such as
  // watch.binding remain quiet.
  return is_binding_mutation_kind(event.kind);
}

bool is_ivar_mutation(const runtime::RuntimeWatchEvent &event) {
  return event.cell_id == 0U && event.object_id != 0U &&
         !event.field_name.empty() &&
         is_ivar_mutation_kind(event.kind);
}

bool is_object_mutation(const runtime::RuntimeWatchEvent &event) {
  return event.cell_id == 0U && event.object_id != 0U &&
         event.field_name.empty() && is_object_mutation_kind(event.kind);
}

bool valid_runtime_dependency(const runtime::RuntimeDependency &dependency) {
  switch (dependency.kind) {
  case runtime::RuntimeDependencyKind::Binding:
    return dependency.cell_id != 0U;
  case runtime::RuntimeDependencyKind::Ivar:
    return dependency.object_id != 0U && !dependency.field_name.empty();
  case runtime::RuntimeDependencyKind::Object:
    return dependency.object_id != 0U;
  }
  return false;
}

runtime::RuntimeDependency canonical_runtime_dependency(
    runtime::RuntimeDependency dependency) {
  switch (dependency.kind) {
  case runtime::RuntimeDependencyKind::Binding:
    dependency.object_id = 0U;
    dependency.field_name.clear();
    dependency.object_revision = 0U;
    break;
  case runtime::RuntimeDependencyKind::Ivar:
    dependency.cell_id = 0U;
    break;
  case runtime::RuntimeDependencyKind::Object:
    dependency.cell_id = 0U;
    dependency.field_name.clear();
    dependency.revision = 0U;
    break;
  }
  return dependency;
}

runtime::RuntimeDependencySet normalize_runtime_dependencies(
    const runtime::RuntimeDependencySet &capture) {
  runtime::RuntimeDependencySet normalized;
  normalized.notebook_cell_id = capture.notebook_cell_id;
  normalized.source = capture.source;
  std::map<RuntimeDependencySourceKey, runtime::RuntimeDependency> by_source;
  for (const runtime::RuntimeDependency &raw : capture.dependencies) {
    if (!valid_runtime_dependency(raw)) {
      // The caller validates this condition and reports the structural error;
      // keeping this helper total makes its candidate state exception-safe.
      continue;
    }
    runtime::RuntimeDependency dependency =
        canonical_runtime_dependency(raw);
    const RuntimeDependencySourceKey source = dependency.source_key();
    const auto found = by_source.find(source);
    if (found == by_source.end()) {
      by_source.emplace(source, std::move(dependency));
      continue;
    }
    runtime::RuntimeDependency &current = found->second;
    current.revision = std::max(current.revision, dependency.revision);
    current.object_revision =
        std::max(current.object_revision, dependency.object_revision);
    // target_name is metadata only.  Selecting the lexicographically larger
    // spelling makes duplicate captures independent of input order.
    if (dependency.target_name > current.target_name) {
      current.target_name = std::move(dependency.target_name);
    }
  }
  normalized.dependencies.reserve(by_source.size());
  for (auto &entry : by_source) {
    normalized.dependencies.push_back(std::move(entry.second));
  }
  return normalized;
}

RuntimeDependencySourceKey binding_source_key(std::uint64_t cell_id) {
  RuntimeDependencySourceKey key;
  key.kind = runtime::RuntimeDependencyKind::Binding;
  key.cell_id = cell_id;
  return key;
}

RuntimeDependencySourceKey ivar_source_key(std::uint64_t object_id,
                                           const std::string &field_name) {
  RuntimeDependencySourceKey key;
  key.kind = runtime::RuntimeDependencyKind::Ivar;
  key.object_id = object_id;
  key.field_name = field_name;
  return key;
}

RuntimeDependencySourceKey object_source_key(std::uint64_t object_id) {
  RuntimeDependencySourceKey key;
  key.kind = runtime::RuntimeDependencyKind::Object;
  key.object_id = object_id;
  return key;
}

const runtime::RuntimeDependency *find_runtime_dependency(
    const runtime::RuntimeDependencySet &capture,
    const RuntimeDependencySourceKey &source) {
  for (const runtime::RuntimeDependency &dependency : capture.dependencies) {
    if (dependency.source_key() == source) {
      return &dependency;
    }
  }
  return nullptr;
}

std::string validate_runtime_event_batch(
    const runtime::RuntimeWatchPollResult &poll) {
  if (poll.timed_out) {
    return "runtime event batch is a wait timeout, not a poll batch";
  }
  if (poll.status != runtime::RuntimeWatchPollStatus::Ok) {
    return "runtime event batch did not come from a successful poll";
  }
  if (!poll.source.valid()) {
    return "runtime event batch has no source namespace";
  }
  if (!poll.requested_cursor.valid() ||
      poll.requested_cursor.source != poll.source) {
    return "runtime event batch has an invalid requested cursor";
  }
  if (!poll.next_cursor.valid() || poll.next_cursor.source != poll.source) {
    return "runtime event batch has an invalid successor cursor";
  }
  if (poll.latest_epoch == std::numeric_limits<std::uint64_t>::max()) {
    return "runtime event batch epoch range is exhausted";
  }
  if (poll.oldest_retained_epoch == 0U ||
      poll.oldest_retained_epoch > poll.latest_epoch + 1U ||
      poll.requested_cursor.next_epoch < poll.oldest_retained_epoch ||
      poll.requested_cursor.next_epoch > poll.latest_epoch + 1U ||
      poll.next_cursor.next_epoch > poll.latest_epoch + 1U) {
    return "runtime event batch has inconsistent stream bounds";
  }
  if (poll.events.empty()) {
    if (poll.next_cursor.next_epoch != poll.requested_cursor.next_epoch ||
        poll.requested_cursor.next_epoch != poll.latest_epoch + 1U) {
      return "empty runtime event batch is not positioned at the stream tail";
    }
    return {};
  }

  std::uint64_t previous_epoch = 0U;
  for (const runtime::RuntimeWatchEvent &event : poll.events) {
    const runtime::RuntimeWatchStreamIdentity event_source{
        event.watch_world_id, event.watch_generation};
    if (event_source != poll.source) {
      return "runtime event batch mixes source namespaces";
    }
    if (event.watch_epoch < poll.oldest_retained_epoch ||
        event.watch_epoch > poll.latest_epoch ||
        (previous_epoch == 0U &&
         event.watch_epoch != poll.requested_cursor.next_epoch) ||
        (previous_epoch != 0U && event.watch_epoch != previous_epoch + 1U)) {
      return "runtime event batch epochs are not contiguous from its cursor";
    }
    previous_epoch = event.watch_epoch;
  }
  if (poll.next_cursor.next_epoch != previous_epoch + 1U) {
    return "runtime event batch successor does not follow its last event";
  }
  return {};
}

} // namespace

NotebookKernel::PreparedCellUpdate::PreparedCellUpdate(
    NotebookKernel *owner, std::unique_lock<std::mutex> execution_lock,
    std::vector<CellSource> sources, DependencyGraph graph,
    std::vector<EvaluationStep> plan, std::vector<BindingKey> stale_outputs,
    std::map<CellId, std::vector<BindingKey>> dynamic_dependencies,
    std::map<CellId, runtime::RuntimeDependencySet> runtime_dependencies,
    std::map<RuntimeDependencySourceKey, std::set<CellId>>
        runtime_dependency_consumers)
    : owner_(owner), execution_lock_(std::move(execution_lock)),
      sources_(std::move(sources)), graph_(std::move(graph)),
      plan_(std::move(plan)), stale_outputs_(std::move(stale_outputs)),
      dynamic_dependencies_(std::move(dynamic_dependencies)),
      runtime_dependencies_(std::move(runtime_dependencies)),
      runtime_dependency_consumers_(
          std::move(runtime_dependency_consumers)) {}

bool NotebookKernel::PreparedCellUpdate::active() const noexcept {
  return owner_ != nullptr && execution_lock_.owns_lock();
}

std::vector<EvaluationStep>
NotebookKernel::PreparedCellUpdate::take_plan() noexcept {
  return std::move(plan_);
}

void NotebookKernel::PreparedCellUpdate::cancel() noexcept {
  owner_ = nullptr;
  if (execution_lock_.owns_lock()) {
    execution_lock_.unlock();
  }
}

NotebookKernel::NotebookKernel(const std::vector<CellSource> &cells)
    : sources_(cells), graph_(cells) {}

NotebookKernel::NotebookKernel(const std::vector<CellSource> &cells,
                               std::set<std::string> ambient_names)
    : ambient_names_(std::move(ambient_names)), sources_(cells),
      graph_(cells, ambient_names_) {}

std::vector<EvaluationStep>
NotebookKernel::update_cells(const std::vector<CellSource> &cells,
                             const std::set<CellId> &changed, bool force_all) {
  PreparedCellUpdate update =
      prepare_update_cells(cells, changed, force_all);
  if (!commit_update_cells(update)) {
    throw std::runtime_error("prepared notebook cell update did not commit");
  }
  return update.take_plan();
}

NotebookKernel::PreparedCellUpdate NotebookKernel::prepare_update_cells(
    const std::vector<CellSource> &cells, const std::set<CellId> &changed,
    bool force_all) {
  std::unique_lock<std::mutex> execution_lock(execution_mutex_);
  std::lock_guard<std::mutex> state_lock(state_mutex_);
  DependencyGraph current(cells, ambient_names_);
  std::vector<CellSource> next_sources = cells;
  std::vector<EvaluationStep> plan;
  std::optional<DependencyGraph> previous;
  if (!sources_.empty()) {
    previous.emplace(sources_, ambient_names_);
  }
  if (!force_all && previous.has_value()) {
    plan = plan_transition_evaluation(current, *previous, cells, changed);
  } else {
    plan = plan_evaluation(current, cells, changed, force_all);
  }
  std::map<CellId, std::vector<BindingKey>> next_dynamic_dependencies =
      dynamic_dependencies_;
  for (auto it = next_dynamic_dependencies.begin();
       it != next_dynamic_dependencies.end();) {
    if (current.analysis(it->first) == nullptr) {
      it = next_dynamic_dependencies.erase(it);
    } else {
      ++it;
    }
  }
  std::map<CellId, runtime::RuntimeDependencySet> next_runtime_dependencies =
      runtime_dependencies_;
  for (auto it = next_runtime_dependencies.begin();
       it != next_runtime_dependencies.end();) {
    if (current.analysis(it->first) == nullptr) {
      it = next_runtime_dependencies.erase(it);
    } else {
      ++it;
    }
  }
  // Reconstruct from the surviving forward snapshots so removed consumers
  // cannot leave stale reverse edges behind.  Revisions are not part of the
  // source key, and successful run_cell() commits maintain the same invariant.
  std::map<RuntimeDependencySourceKey, std::set<CellId>> next_consumers;
  for (const auto &entry : next_runtime_dependencies) {
    for (const runtime::RuntimeDependency &dependency :
         entry.second.dependencies) {
      next_consumers[dependency.source_key()].insert(entry.first);
    }
  }
  std::vector<BindingKey> stale_outputs = plan_outputs_to_stale(
      previous.has_value() ? &*previous : nullptr, current, plan);

  return PreparedCellUpdate(
      this, std::move(execution_lock), std::move(next_sources),
      std::move(current), std::move(plan), std::move(stale_outputs),
      std::move(next_dynamic_dependencies),
      std::move(next_runtime_dependencies), std::move(next_consumers));
}

bool NotebookKernel::commit_update_cells(PreparedCellUpdate &update) noexcept {
  if (!update.active() || update.owner_ != this || update.committed_) {
    return false;
  }
  try {
    std::lock_guard<std::mutex> state_lock(state_mutex_);
    // Preparation has already built every allocating candidate. Publication
    // only marks existing slots and swaps complete generations while the
    // execution lock remains owned by the transaction/host.
    if (!slots_.mark_stale_batch(update.stale_outputs_)) {
      return false;
    }
    sources_.swap(update.sources_);
    graph_.swap(update.graph_);
    for (auto it = input_dependencies_.begin(); it != input_dependencies_.end();) {
      if (!graph_.analysis(it->first)) it = input_dependencies_.erase(it); else ++it;
    }
    for (auto it = input_retry_consumers_.begin(); it != input_retry_consumers_.end();) {
      if (!graph_.analysis(*it)) it = input_retry_consumers_.erase(it); else ++it;
    }
    dynamic_dependencies_.swap(update.dynamic_dependencies_);
    runtime_dependencies_.swap(update.runtime_dependencies_);
    runtime_dependency_consumers_.swap(
        update.runtime_dependency_consumers_);
    update.committed_ = true;
    return true;
  } catch (...) {
    return false;
  }
}

std::vector<EvaluationStep>
NotebookKernel::update_cells(const std::vector<CellSource> &cells,
                             CellId changed, bool force_all) {
  return update_cells(cells, std::set<CellId>{changed}, force_all);
}

NotebookKernel::PreparedCellUpdate NotebookKernel::prepare_update_cells(
    const std::vector<CellSource> &cells, CellId changed, bool force_all) {
  return prepare_update_cells(cells, std::set<CellId>{changed}, force_all);
}

CellRunResult NotebookKernel::run_cell(CellId id,
                                       const CellExecutor &executor) {
  std::lock_guard<std::mutex> execution_lock(execution_mutex_);
  CellRunResult result;
  if (!executor) {
    result.error = "notebook cell executor is not configured";
    return result;
  }
  std::vector<BindingKey> expected;
  {
    std::lock_guard<std::mutex> state_lock(state_mutex_);
    const CellAnalysis *analysis = graph_.analysis(id);
    if (analysis == nullptr) {
      result.error = "notebook cell is not present in the current graph";
      return result;
    }
    input_retry_consumers_.insert(id);
    if (!analysis->ok()) {
      result.error = "notebook cell has frontend diagnostics";
      return result;
    }

    for (const std::string &name : graph_.missing_names(id)) {
      result.error = "notebook input is unresolved: " + name;
      return result;
    }

    for (const BindingRead &read : graph_.reads_for(id)) {
      if (!read.provider.has_value()) {
        continue;
      }
      const std::optional<SlotState> slot = slots_.find(*read.provider);
      if (!slot.has_value() || !slot->initialized) {
        result.error = "notebook input is not initialized: " +
                       binding_label(*read.provider);
        return result;
      }
      if (slot->stale) {
        result.error =
            "notebook input is stale: " + binding_label(*read.provider);
        return result;
      }
      result.inputs.push_back(
          KernelInput{slot->key, slot->published, slot->revision});
    }
    expected = expected_writes(id);
  }

  CellExecutionResult execution;
  try {
    execution = executor(result.inputs);
  } catch (const std::exception &error) {
    result.error = std::string("notebook executor failed: ") + error.what();
    return result;
  } catch (...) {
    result.error = "notebook executor failed with an unknown exception";
    return result;
  }
  if (!execution.ok) {
    result.error = execution.error.empty() ? "notebook cell failed"
                                           : std::move(execution.error);
    return result;
  }

  std::map<BindingKey, const KernelWrite *> actual;
  for (const KernelWrite &write : execution.writes) {
    if (!actual.emplace(write.key, &write).second) {
      result.error = "duplicate notebook output: " + binding_label(write.key);
      return result;
    }
  }
  for (const BindingKey &key : expected) {
    if (actual.count(key) == 0U) {
      result.error = "notebook output was not produced: " + binding_label(key);
      return result;
    }
  }
  for (const auto &entry : actual) {
    if (!std::binary_search(expected.begin(), expected.end(), entry.first)) {
      result.error =
          "unexpected notebook output: " + binding_label(entry.first);
      return result;
    }
  }

  std::sort(execution.dynamic_dependencies.begin(),
            execution.dynamic_dependencies.end());
  execution.dynamic_dependencies.erase(
      std::unique(execution.dynamic_dependencies.begin(),
                  execution.dynamic_dependencies.end()),
      execution.dynamic_dependencies.end());
  if (execution.runtime_dependencies.has_value() &&
      (execution.runtime_dependencies->notebook_cell_id == 0U ||
       execution.runtime_dependencies->notebook_cell_id != id)) {
    result.error = "notebook runtime dependency capture belongs to cell " +
                   std::to_string(
                       execution.runtime_dependencies->notebook_cell_id) +
                   ", expected cell " + std::to_string(id);
    return result;
  }
  if (execution.runtime_dependencies.has_value()) {
    if (!execution.runtime_dependencies->source.valid()) {
      result.error =
          "notebook runtime dependency capture has no world namespace";
      return result;
    }
    for (const runtime::RuntimeDependency &dependency :
         execution.runtime_dependencies->dependencies) {
      if (!valid_runtime_dependency(dependency)) {
        result.error = "notebook runtime dependency capture has invalid "
                       "source identity";
        return result;
      }
    }
    execution.runtime_dependencies =
        normalize_runtime_dependencies(*execution.runtime_dependencies);
  }

  try {
    std::lock_guard<std::mutex> state_lock(state_mutex_);
    std::map<CellId, std::vector<BindingKey>> next_dependencies =
        dynamic_dependencies_;
    next_dependencies[id] = execution.dynamic_dependencies;
    auto next_inputs = input_dependencies_;
    if (execution.input_dependencies) next_inputs[id] = *execution.input_dependencies;
    std::optional<
        std::map<CellId, runtime::RuntimeDependencySet>> next_runtime_dependencies;
    std::optional<std::map<RuntimeDependencySourceKey, std::set<CellId>>>
        next_runtime_dependency_consumers;
    std::optional<runtime::RuntimeWatchStreamIdentity>
        next_runtime_watch_source = runtime_watch_source_;
    if (execution.runtime_dependencies.has_value()) {
      if (next_runtime_watch_source.has_value() &&
          *next_runtime_watch_source != execution.runtime_dependencies->source) {
        result.error =
            "notebook runtime dependency capture belongs to another world";
        return result;
      }
      next_runtime_watch_source = execution.runtime_dependencies->source;
      next_runtime_dependencies.emplace(runtime_dependencies_);
      (*next_runtime_dependencies)[id] = *execution.runtime_dependencies;

      // Rebuild this consumer's reverse edges in the same candidate state as
      // its forward snapshot.  Both maps are swapped only after the slot
      // transaction commits, so a failed commit preserves both directions.
      next_runtime_dependency_consumers.emplace(
          runtime_dependency_consumers_);
      for (auto reverse = next_runtime_dependency_consumers->begin();
           reverse != next_runtime_dependency_consumers->end();) {
        reverse->second.erase(id);
        if (reverse->second.empty()) {
          reverse = next_runtime_dependency_consumers->erase(reverse);
        } else {
          ++reverse;
        }
      }
      for (const runtime::RuntimeDependency &dependency :
           execution.runtime_dependencies->dependencies) {
        (*next_runtime_dependency_consumers)[dependency.source_key()].insert(id);
      }
    }
    NotebookSlotTable::Transaction transaction = slots_.begin_transaction();
    for (const KernelWrite &write : execution.writes) {
      if (write.provider.has_value()) {
        transaction.stage(write.key, write.value, *write.provider);
      } else {
        transaction.stage(write.key, write.value);
      }
    }
    if (runtime::runtime_run_cancel_requested()) {
      result.error = "CancelledError: execution cancelled before publication";
      return result;
    }
    result.publication = transaction.commit();
    if (result.publication.committed) {
      dynamic_dependencies_.swap(next_dependencies);
      input_dependencies_.swap(next_inputs);
      input_retry_consumers_.erase(id);
      if (next_runtime_dependencies.has_value()) {
        runtime_dependencies_.swap(*next_runtime_dependencies);
        runtime_dependency_consumers_.swap(*next_runtime_dependency_consumers);
        runtime_watch_source_ = *next_runtime_watch_source;
      }
    }
  } catch (const std::exception &error) {
    result.error =
        std::string("notebook output transaction failed: ") + error.what();
    return result;
  } catch (...) {
    result.error =
        "notebook output transaction failed with an unknown exception";
    return result;
  }
  if (!result.publication.committed) {
    result.error = "notebook output transaction did not commit";
    return result;
  }

  result.dynamic_dependencies = std::move(execution.dynamic_dependencies);
  result.runtime_dependencies = std::move(execution.runtime_dependencies);
  result.ok = true;
  return result;
}

std::optional<SlotState>
NotebookKernel::slot_state(const BindingKey &key) const {
  std::lock_guard<std::mutex> state_lock(state_mutex_);
  return slots_.find(key);
}

std::optional<runtime::Value>
NotebookKernel::read_slot(const BindingKey &key) const {
  std::lock_guard<std::mutex> state_lock(state_mutex_);
  return slots_.read(key);
}

std::size_t NotebookKernel::slot_count() const {
  std::lock_guard<std::mutex> state_lock(state_mutex_);
  return slots_.size();
}

std::vector<CellId> NotebookKernel::cell_order() const {
  std::lock_guard<std::mutex> state_lock(state_mutex_);
  return graph_.cell_order();
}

std::vector<BindingKey> NotebookKernel::dynamic_dependencies(CellId id) const {
  std::lock_guard<std::mutex> state_lock(state_mutex_);
  const auto found = dynamic_dependencies_.find(id);
  return found == dynamic_dependencies_.end() ? std::vector<BindingKey>{}
                                              : found->second;
}

std::optional<runtime::RuntimeDependencySet>
NotebookKernel::runtime_dependencies(CellId id) const {
  std::lock_guard<std::mutex> state_lock(state_mutex_);
  const auto found = runtime_dependencies_.find(id);
  return found == runtime_dependencies_.end() ? std::nullopt
                                              : std::optional(found->second);
}

std::vector<EvaluationStep> NotebookKernel::plan_runtime_events(
    const std::vector<runtime::RuntimeWatchEvent> &events) {
  std::lock_guard<std::mutex> execution_lock(execution_mutex_);
  std::lock_guard<std::mutex> state_lock(state_mutex_);
  return plan_runtime_events_locked(events);
}

std::vector<EvaluationStep> NotebookKernel::plan_runtime_events(
    const runtime::RuntimeWatchEvent &event) {
  std::lock_guard<std::mutex> execution_lock(execution_mutex_);
  std::lock_guard<std::mutex> state_lock(state_mutex_);
  return plan_runtime_events_locked(
      std::vector<runtime::RuntimeWatchEvent>{event});
}

RuntimeEventBatchPlanResult NotebookKernel::plan_runtime_event_batch(
    const runtime::RuntimeWatchPollResult &poll) {
  RuntimeEventBatchPlanResult result;
  result.error = validate_runtime_event_batch(poll);
  if (!result.error.empty()) {
    result.status = RuntimeEventBatchPlanStatus::InvalidBatch;
    return result;
  }

  std::lock_guard<std::mutex> execution_lock(execution_mutex_);
  std::lock_guard<std::mutex> state_lock(state_mutex_);
  if (!runtime_watch_source_.has_value() ||
      *runtime_watch_source_ != poll.source) {
    result.status = RuntimeEventBatchPlanStatus::SourceMismatch;
    result.error = "runtime event batch belongs to another world";
    return result;
  }
  result.plan = plan_runtime_events_locked(poll.events);
  result.acknowledgement_cursor = poll.next_cursor;
  result.status = RuntimeEventBatchPlanStatus::Accepted;
  return result;
}

bool NotebookKernel::bind_runtime_watch_source(
    const runtime::RuntimeWatchStreamIdentity &source) {
  if (!source.valid()) {
    return false;
  }
  std::lock_guard<std::mutex> execution_lock(execution_mutex_);
  std::lock_guard<std::mutex> state_lock(state_mutex_);
  if (runtime_watch_source_.has_value()) {
    return *runtime_watch_source_ == source;
  }
  // Rich dependency IDs are opaque within their originating world. Once an
  // unbound kernel has published any captures, attaching a namespace later
  // could make numerically equal IDs from another world alias those captures.
  if (!runtime_dependencies_.empty()) {
    return false;
  }
  runtime_watch_source_ = source;
  return true;
}

bool NotebookKernel::reset_runtime_watch_source(
    const runtime::RuntimeWatchStreamIdentity &source) {
  if (!source.valid()) {
    return false;
  }
  std::lock_guard<std::mutex> execution_lock(execution_mutex_);
  std::lock_guard<std::mutex> state_lock(state_mutex_);
  runtime_dependencies_.clear();
  runtime_dependency_consumers_.clear();
  // Published values may include watch cells or heap objects owned by the old
  // world. They cannot safely become inputs or external GC roots of the new
  // world, so a namespace change is a value-boundary reset, not a hot reload.
  // Source/image reloads within one RuntimeWorld retain the same identity and
  // never take this path.
  slots_.clear();
  runtime_watch_source_ = source;
  return true;
}

std::vector<EvaluationStep> NotebookKernel::plan_runtime_resync() {
  std::lock_guard<std::mutex> execution_lock(execution_mutex_);
  std::lock_guard<std::mutex> state_lock(state_mutex_);
  const std::vector<CellId> order = graph_.cell_order();
  const std::set<CellId> roots(order.begin(), order.end());
  std::vector<EvaluationStep> plan =
      plan_automatic_evaluation(graph_, sources_, roots);
  mark_plan_outputs_stale(nullptr, graph_, plan);
  return plan;
}

std::vector<CellId> NotebookKernel::runtime_dependency_consumers(
    const RuntimeDependencySourceKey &source) const {
  std::lock_guard<std::mutex> state_lock(state_mutex_);
  const auto found = runtime_dependency_consumers_.find(source);
  if (found == runtime_dependency_consumers_.end()) {
    return {};
  }
  return std::vector<CellId>(found->second.begin(), found->second.end());
}

std::vector<runtime::Value> NotebookKernel::gc_roots() const {
  std::lock_guard<std::mutex> state_lock(state_mutex_);
  return slots_.gc_roots();
}

void NotebookKernel::mark_failure_barrier(const std::set<CellId> &roots) {
  std::lock_guard<std::mutex> execution_lock(execution_mutex_);
  std::lock_guard<std::mutex> state_lock(state_mutex_);
  if (roots.empty()) {
    return;
  }
  const std::vector<CellId> affected = graph_.invalidation_plan(roots);
  for (const CellId id : affected) {
    const std::vector<BindingKey> writes = graph_.writes_for(id);
    for (const BindingKey &key : writes) {
      slots_.mark_stale(key);
    }
  }
}

void NotebookKernel::mark_failure_barrier(CellId root) {
  mark_failure_barrier(std::set<CellId>{root});
}

void NotebookKernel::reset() {
  std::lock_guard<std::mutex> execution_lock(execution_mutex_);
  std::lock_guard<std::mutex> state_lock(state_mutex_);
  sources_.clear();
  graph_.rebuild({}, ambient_names_);
  slots_.clear();
  dynamic_dependencies_.clear();
  input_dependencies_.clear();
  input_retry_consumers_.clear();
  runtime_dependencies_.clear();
  runtime_dependency_consumers_.clear();
  runtime_watch_source_.reset();
}

std::vector<EvaluationStep> NotebookKernel::plan_input_changes(const std::set<std::string> &keys) {
  std::lock_guard<std::mutex> execution_lock(execution_mutex_);
  std::lock_guard<std::mutex> state_lock(state_mutex_);
  if (keys.empty()) return {};
  std::set<CellId> roots;
  for (const auto id : graph_.cell_order()) {
    const auto found = input_dependencies_.find(id);
    if (found == input_dependencies_.end() || input_retry_consumers_.count(id) ||
        std::any_of(keys.begin(), keys.end(), [&](const auto &key) { return found->second.count(key); }))
      roots.insert(id);
  }
  auto plan = plan_automatic_evaluation(graph_, sources_, roots);
  mark_plan_outputs_stale(nullptr, graph_, plan);
  return plan;
}

std::vector<BindingKey> NotebookKernel::expected_writes(CellId id) const {
  std::vector<BindingKey> result = graph_.writes_for(id);
  std::sort(result.begin(), result.end());
  return result;
}

void NotebookKernel::mark_plan_outputs_stale(
    const DependencyGraph *previous, const DependencyGraph &current,
    const std::vector<EvaluationStep> &plan) {
  for (const BindingKey &key :
       plan_outputs_to_stale(previous, current, plan)) {
    slots_.mark_stale(key);
  }
}

std::vector<BindingKey> NotebookKernel::plan_outputs_to_stale(
    const DependencyGraph *previous, const DependencyGraph &current,
    const std::vector<EvaluationStep> &plan) {
  std::set<BindingKey> affected;
  for (const EvaluationStep &step : plan) {
    const std::vector<BindingKey> current_writes = current.writes_for(step.id);
    affected.insert(current_writes.begin(), current_writes.end());
    if (previous != nullptr) {
      const std::vector<BindingKey> previous_writes =
          previous->writes_for(step.id);
      affected.insert(previous_writes.begin(), previous_writes.end());
    }
  }
  if (previous != nullptr) {
    // A removed cell cannot appear in the current execution plan, but its
    // published bindings are still present in the slot table.  Keep those
    // bindings as historical values while making them unusable to consumers.
    for (const CellId id : previous->cell_order()) {
      if (current.analysis(id) != nullptr) {
        continue;
      }
      const std::vector<BindingKey> previous_writes = previous->writes_for(id);
      affected.insert(previous_writes.begin(), previous_writes.end());
    }
  }
  return std::vector<BindingKey>(affected.begin(), affected.end());
}

std::vector<EvaluationStep> NotebookKernel::plan_runtime_events_locked(
    const std::vector<runtime::RuntimeWatchEvent> &events) {
  std::set<CellId> roots;

  // The reverse index identifies only candidate consumers.  Revision checks
  // below use the forward capture so that a source key can remain stable
  // while each successful run observes a newer source revision.
  for (const runtime::RuntimeWatchEvent &event : events) {
    const runtime::RuntimeWatchStreamIdentity event_source{
        event.watch_world_id, event.watch_generation};
    if (!event_source.valid() || !runtime_watch_source_.has_value() ||
        event_source != *runtime_watch_source_) {
      continue;
    }
    const bool binding_mutation = is_binding_mutation(event);
    const bool ivar_mutation = is_ivar_mutation(event);
    const bool object_mutation = is_object_mutation(event);
    if (!binding_mutation && !ivar_mutation && !object_mutation) {
      // Registration events (watch.binding/watch.ivar) and malformed events
      // are deliberately quiet, even when their snapshot fields are copied
      // into old/new event fields by a host adapter.
      continue;
    }

    std::vector<RuntimeDependencySourceKey> sources;
    if (binding_mutation) {
      if (event.new_revision <= event.old_revision) {
        continue;
      }
      sources.push_back(binding_source_key(event.cell_id));
    }
    if (ivar_mutation) {
      if (event.new_revision < event.old_revision ||
          event.new_object_revision < event.old_object_revision ||
          (event.new_revision == event.old_revision &&
           event.new_object_revision == event.old_object_revision)) {
        continue;
      }
      sources.push_back(ivar_source_key(event.object_id, event.field_name));
      // An ivar mutation also changes the observed object version.  Object
      // dependencies intentionally match this event family.
      sources.push_back(object_source_key(event.object_id));
    } else if (object_mutation) {
      if (event.new_object_revision <= event.old_object_revision) {
        continue;
      }
      sources.push_back(object_source_key(event.object_id));
    }

    for (const RuntimeDependencySourceKey &source : sources) {
      const auto reverse = runtime_dependency_consumers_.find(source);
      if (reverse == runtime_dependency_consumers_.end()) {
        continue;
      }
      for (const CellId consumer : reverse->second) {
        const auto forward = runtime_dependencies_.find(consumer);
        if (forward == runtime_dependencies_.end()) {
          continue;
        }
        if (forward->second.source != event_source) {
          continue;
        }
        const runtime::RuntimeDependency *dependency =
            find_runtime_dependency(forward->second, source);
        if (dependency == nullptr) {
          continue;
        }

        bool changed = false;
        if (source.kind == runtime::RuntimeDependencyKind::Binding) {
          changed = event.new_revision > dependency->revision;
        } else if (source.kind == runtime::RuntimeDependencyKind::Ivar) {
          changed = event.new_revision > dependency->revision;
        } else {
          changed = event.new_object_revision > dependency->object_revision;
        }
        if (changed) {
          roots.insert(consumer);
        }
      }
    }
  }

  if (roots.empty()) {
    return {};
  }
  // The static graph remains the conservative source of downstream edges;
  // runtime captures never replace it.  Automatic roots go through the
  // scheduler hook so manual roots stay stale and watch descendants report a
  // static blocker.
  std::vector<EvaluationStep> plan =
      plan_automatic_evaluation(graph_, sources_, roots);

  // A runtime root can sit below a provider that was made stale by an earlier
  // edit but is not part of this event's closure.  Refine the graph-only plan
  // against the published slot snapshot so such a cell is reported as blocked
  // instead of being optimistically scheduled only to fail input validation.
  std::set<CellId> blocked;
  std::set<CellId> runnable_before;
  for (EvaluationStep &step : plan) {
    if (step.action != EvaluationAction::Run) {
      blocked.insert(step.id);
      continue;
    }
    std::optional<CellId> blocker;
    for (const BindingRead &read : graph_.reads_for(step.id)) {
      if (!read.provider.has_value()) {
        continue;
      }
      const CellId provider = read.provider->cell_id;
      if (blocked.count(provider) != 0U) {
        blocker = provider;
        break;
      }
      if (runnable_before.count(provider) != 0U) {
        continue;
      }
      const std::optional<SlotState> slot = slots_.find(*read.provider);
      if (!slot.has_value() || !slot->initialized || slot->stale) {
        blocker = provider;
        break;
      }
    }
    if (blocker.has_value()) {
      step.action = EvaluationAction::BlockedByStaleDependency;
      step.blocker = blocker;
      blocked.insert(step.id);
    } else {
      runnable_before.insert(step.id);
    }
  }
  mark_plan_outputs_stale(nullptr, graph_, plan);
  return plan;
}

} // namespace amber::notebook
