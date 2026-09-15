#include "notebook/scheduler.h"

#include <unordered_map>
#include <unordered_set>

namespace amber::notebook {

namespace {

std::vector<EvaluationStep> plan_order(const DependencyGraph &graph,
                                       const std::vector<CellSource> &cells,
                                       const std::set<CellId> &changed,
                                       const std::vector<CellId> &order,
                                       bool force_all) {
  std::unordered_map<CellId, CellMode> modes;
  modes.reserve(cells.size());
  for (const CellSource &cell : cells) {
    if (cell.id != 0) {
      modes.emplace(cell.id, cell.mode);
    }
  }

  std::unordered_set<CellId> blocked;
  std::vector<EvaluationStep> result;
  result.reserve(order.size());

  for (const CellId id : order) {
    const auto mode_it = modes.find(id);
    const CellMode mode =
        mode_it == modes.end() ? CellMode::Manual : mode_it->second;
    if (!force_all && changed.count(id) == 0U && mode == CellMode::Manual) {
      result.push_back(
          EvaluationStep{id, EvaluationAction::KeepManualStale, std::nullopt});
      blocked.insert(id);
      continue;
    }

    std::optional<CellId> blocker;
    if (!force_all) {
      for (const BindingRead &read : graph.reads_for(id)) {
        if (read.provider.has_value() &&
            blocked.count(read.provider->cell_id) != 0U) {
          blocker = read.provider->cell_id;
          break;
        }
      }
    }
    if (blocker.has_value()) {
      result.push_back(EvaluationStep{
          id, EvaluationAction::BlockedByStaleDependency, blocker});
      blocked.insert(id);
      continue;
    }
    result.push_back(EvaluationStep{id, EvaluationAction::Run, std::nullopt});
  }
  return result;
}

} // namespace

std::vector<EvaluationStep>
plan_evaluation(const DependencyGraph &graph,
                const std::vector<CellSource> &cells,
                const std::set<CellId> &changed, bool force_all) {
  const std::vector<CellId> order =
      force_all ? graph.execution_order() : graph.invalidation_plan(changed);
  return plan_order(graph, cells, changed, order, force_all);
}

std::vector<EvaluationStep>
plan_evaluation(const DependencyGraph &graph,
               const std::vector<CellSource> &cells, CellId changed,
               bool force_all) {
  return plan_evaluation(graph, cells, std::set<CellId>{changed}, force_all);
}

std::vector<EvaluationStep> plan_automatic_evaluation(
    const DependencyGraph &graph, const std::vector<CellSource> &cells,
    const std::set<CellId> &automatic_roots, bool force_all) {
  // Do not pass runtime roots as `changed`: changed roots are explicit edits
  // and are therefore allowed to run manual cells.  Runtime roots are
  // automatic and must retain the stale/manual blocking contract.
  const std::vector<CellId> order =
      force_all ? graph.execution_order()
                : graph.invalidation_plan(automatic_roots);
  return plan_order(graph, cells, /*changed=*/{}, order, force_all);
}

std::vector<EvaluationStep>
plan_transition_evaluation(const DependencyGraph &graph,
                           const DependencyGraph &previous,
                           const std::vector<CellSource> &cells,
                           const std::set<CellId> &changed, bool force_all) {
  const std::vector<CellId> order =
      force_all ? graph.execution_order()
                : graph.transition_invalidation_plan(previous, changed);
  return plan_order(graph, cells, changed, order, force_all);
}

std::vector<EvaluationStep> plan_transition_evaluation(
    const DependencyGraph &graph, const DependencyGraph &previous,
    const std::vector<CellSource> &cells, CellId changed, bool force_all) {
  return plan_transition_evaluation(graph, previous, cells,
                                    std::set<CellId>{changed}, force_all);
}

} // namespace amber::notebook
