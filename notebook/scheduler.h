#pragma once

#include "notebook/dependency_graph.h"

#include <optional>
#include <set>
#include <vector>

namespace amber::notebook {

enum class EvaluationAction {
  Run,
  KeepManualStale,
  BlockedByStaleDependency,
};

struct EvaluationStep {
  CellId id = 0;
  EvaluationAction action = EvaluationAction::Run;
  std::optional<CellId> blocker;
};

// Produces a deterministic execution plan without evaluating Amber code.  An
// explicitly changed manual cell is runnable, while downstream manual cells
// remain stale and block watch cells that consume their outputs.
std::vector<EvaluationStep>
plan_evaluation(const DependencyGraph &graph,
                const std::vector<CellSource> &cells,
                const std::set<CellId> &changed, bool force_all = false);

std::vector<EvaluationStep>
plan_evaluation(const DependencyGraph &graph,
               const std::vector<CellSource> &cells, CellId changed,
               bool force_all = false);

// Plans roots raised by an external runtime event.  These roots are
// automatic, rather than explicit document edits: a manual root is kept
// stale and a watch consumer is blocked through the static graph.  Keeping
// this distinction out of `changed` prevents automatic runtime events from
// accidentally executing manual cells.
std::vector<EvaluationStep> plan_automatic_evaluation(
    const DependencyGraph &graph, const std::vector<CellSource> &cells,
    const std::set<CellId> &automatic_roots, bool force_all = false);

// Plans an edit against both graph versions so removed dependency edges still
// invalidate their former consumers.
std::vector<EvaluationStep> plan_transition_evaluation(
    const DependencyGraph &graph, const DependencyGraph &previous,
    const std::vector<CellSource> &cells, const std::set<CellId> &changed,
    bool force_all = false);

std::vector<EvaluationStep>
plan_transition_evaluation(const DependencyGraph &graph,
                           const DependencyGraph &previous,
                           const std::vector<CellSource> &cells, CellId changed,
                           bool force_all = false);

} // namespace amber::notebook
