#include "notebook/dependency_graph.h"
#include "notebook/model.h"
#include "notebook/scheduler.h"

#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace {

using amber::notebook::BindingKey;
using amber::notebook::CellId;
using amber::notebook::CellMode;
using amber::notebook::CellSource;
using amber::notebook::DependencyGraph;
using amber::notebook::EvaluationAction;

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << "\n";
    std::exit(1);
  }
}

CellSource cell(CellId id, std::string source,
                CellMode mode = CellMode::Watch) {
  CellSource result;
  result.id = id;
  result.source = std::move(source);
  result.file = "<notebook-test>";
  result.mode = mode;
  return result;
}

void test_analysis_uses_ast_and_tracks_ordered_assignment_reads() {
  const auto analysis = amber::notebook::analyze_cell(
      cell(1, "x = 1\ny = x + 1\nx = x + 1\nz = y + x\n"));
  expect(analysis.ok(), "valid cell should parse");
  expect(analysis.writes == std::set<std::string>({"x", "y", "z"}),
         "analysis should expose top-level writes");
  // x/y are produced in this cell before their later reads, so those reads
  // resolve locally and do not create cross-cell dependencies.
  expect(analysis.reads.empty(),
         "same-cell reads after assignment should be local");
  expect(analysis.local_reads == std::set<std::string>({"x", "y"}),
         "local reads should be retained for diagnostics");

  const auto increment = amber::notebook::analyze_cell(cell(2, "x = x + 1\n"));
  expect(increment.ok(), "increment cell should parse");
  expect(increment.reads == std::set<std::string>({"x"}),
         "x = x + 1 should read the previous x");
  expect(increment.writes == std::set<std::string>({"x"}),
         "x = x + 1 should write the current-cell x version");
}

void test_kernel_watch_intrinsic_does_not_require_kernel_slot() {
  const auto watched = amber::notebook::analyze_cell(
      cell(3, "Kernel.watch(x)\ny = x\n"));
  expect(watched.ok(), "Kernel.watch notebook cell should parse");
  expect(watched.reads == std::set<std::string>({"x"}),
         "Kernel.watch should read its target without a Kernel input slot");
  expect(watched.writes == std::set<std::string>({"y"}),
         "Kernel.watch should not publish a synthetic binding");

  const auto shadowed = amber::notebook::analyze_cell(
      cell(4, "Kernel = helper\nx = 1\nKernel.watch(x)\n"));
  expect(shadowed.reads == std::set<std::string>({"helper"}) &&
             shadowed.local_reads ==
                 std::set<std::string>({"Kernel", "x"}),
         "a same-cell Kernel binding should remain an ordinary local send");
}

void test_graph_resolves_nearest_preceding_version() {
  const std::vector<CellSource> cells{
      cell(101, "x = 10\n"),
      cell(205, "y = x + 1\n"),
      cell(309, "x = x + 1\n"),
      cell(411, "z = x + y\n"),
  };
  DependencyGraph graph(cells);
  expect(graph.cell_order() == std::vector<CellId>({101, 205, 309, 411}),
         "graph must retain document order independently of IDs");
  const auto second_provider = graph.provider_for(205, "x");
  expect(second_provider.has_value() &&
             *second_provider == BindingKey{101, "x"},
         "external x should resolve to the preceding provider");
  const auto increment_provider = graph.provider_for(309, "x");
  expect(increment_provider.has_value() &&
             *increment_provider == BindingKey{101, "x"},
         "x = x + 1 must not resolve x to its own new version");
  const auto z_provider = graph.provider_for(411, "x");
  expect(z_provider.has_value() && *z_provider == BindingKey{309, "x"},
         "a later consumer should resolve the latest preceding x version");
  const auto y_provider = graph.provider_for(411, "y");
  expect(y_provider.has_value() && *y_provider == BindingKey{205, "y"},
         "later consumers should resolve preceding y versions");
  expect(!graph.has_cycle(),
         "versioned preceding providers must not create a false cycle");
  expect(graph.execution_order() == std::vector<CellId>({101, 205, 309, 411}),
         "execution order should be a stable topological order");
}

void test_same_cell_local_read_has_no_external_edge() {
  DependencyGraph graph(
      {cell(1, "x = 10\n"), cell(2, "x = 20\ny = x\n"), cell(3, "z = y\n")});
  expect(!graph.provider_for(2, "x").has_value(),
         "a read after a same-cell assignment is local");
  expect(graph.edges().size() == 1U,
         "only the external y dependency should produce an edge");
  expect(graph.provider_for(3, "y") ==
             std::optional<BindingKey>(BindingKey{2, "y"}),
         "later cells should consume current-cell outputs");
}

void test_missing_names_and_transitive_invalidation() {
  DependencyGraph graph(
      {cell(1, "source = 3\n"), cell(2, "middle = source + 1\n"),
       cell(3, "answer = middle * 2\n"), cell(4, "unknown + 1\n")});
  expect(graph.missing_names(4) == std::set<std::string>({"unknown"}),
         "unresolved external names should be reported per cell");
  expect(graph.direct_dependents_of(1) == std::vector<CellId>({2}),
         "direct dependents should be exposed");
  expect(graph.dependents_of(1) == std::vector<CellId>({2, 3}),
         "dependents should include the transitive closure");
  expect(graph.invalidation_plan(1) == std::vector<CellId>({1, 2, 3}),
         "invalidation plan should include roots and dependents in order");
  expect(graph.invalidation_plan(std::set<CellId>{2, 4}) ==
             std::vector<CellId>({2, 3, 4}),
         "multiple invalidation roots should be coalesced");
}

void test_ambient_names_are_not_missing_and_never_create_providers() {
  DependencyGraph graph(
      {cell(1, "value = Ambient + 1\n"),
       cell(2, "value = Ambient + 2\n")},
      std::set<std::string>{"Ambient"});
  expect(graph.missing_names(1).empty() && graph.missing_names(2).empty(),
         "ambient reads should not be reported as missing");
  expect(!graph.provider_for(1, "Ambient").has_value() &&
             !graph.provider_for(2, "Ambient").has_value(),
         "ambient reads must not fabricate notebook providers");

  DependencyGraph shadowed(
      {cell(1, "Ambient = 3\n"), cell(2, "value = Ambient + 1\n")},
      std::set<std::string>{"Ambient"});
  expect(shadowed.provider_for(2, "Ambient") ==
             std::optional<BindingKey>(BindingKey{1, "Ambient"}),
         "a preceding notebook provider must shadow an ambient name");
  expect(shadowed.missing_names(2).empty(),
         "a notebook provider should resolve a shadowed ambient name");
}

void test_imports_are_cell_local_bindings() {
  const auto analysis = amber::notebook::analyze_cell(cell(
      1, "import system as sys\nx = sys.command(\"/bin/cat\")\n"));
  expect(analysis.ok() && analysis.reads.empty() &&
             analysis.writes == std::set<std::string>{"x"} &&
             analysis.local_reads == std::set<std::string>{"sys"},
         "an import alias belongs to this cell without publishing a notebook slot");
  const DependencyGraph graph({
      cell(1, "from system import cmd\ncmd'printf hello'.output\n")});
  expect(graph.missing_names(1).empty(),
         "a native prelude introduced by a string tag must not be missing");
}

void test_scheduler_keeps_manual_cells_stale_and_blocks_consumers() {
  const std::vector<CellSource> cells{
      cell(1, "source = 3\n"),
      cell(2, "manual = source + 1\n", CellMode::Manual),
      cell(3, "answer = manual * 2\n"),
      cell(4, "unrelated = 99\n"),
  };
  DependencyGraph graph(cells);
  const auto plan = amber::notebook::plan_evaluation(graph, cells, 1);
  expect(plan.size() == 3U, "unrelated cells should not enter the plan");
  expect(plan[0].id == 1 && plan[0].action == EvaluationAction::Run,
         "the changed root should run");
  expect(plan[1].id == 2 && plan[1].action == EvaluationAction::KeepManualStale,
         "a dependent manual cell should remain stale");
  expect(plan[2].id == 3 &&
             plan[2].action == EvaluationAction::BlockedByStaleDependency &&
             plan[2].blocker == std::optional<CellId>(2),
         "a watch consumer should report its stale blocker");

  const auto forced = amber::notebook::plan_evaluation(graph, cells, 1, true);
  expect(forced.size() == cells.size(), "force-all should include every cell");
  for (const auto &step : forced) {
    expect(step.action == EvaluationAction::Run,
           "force-all should run manual and watch cells");
  }
}

void test_scheduler_automatic_roots_keep_manual_cells_stale() {
  const std::vector<CellSource> cells{
      cell(1, "source = 3\n"),
      cell(2, "manual = source + 1\n", CellMode::Manual),
      cell(3, "answer = manual * 2\n"),
      cell(4, "tail = answer + 1\n"),
  };
  DependencyGraph graph(cells);
  const auto plan = amber::notebook::plan_automatic_evaluation(
      graph, cells, std::set<CellId>{2});
  expect(plan.size() == 3U && plan[0].id == 2 && plan[1].id == 3 &&
             plan[2].id == 4,
         "automatic root should use the static transitive invalidation plan");
  expect(plan[0].action == EvaluationAction::KeepManualStale,
         "automatic manual root should remain stale");
  expect(plan[1].action == EvaluationAction::BlockedByStaleDependency &&
             plan[1].blocker == std::optional<CellId>(2),
         "watch downstream of automatic manual root should be blocked");
  expect(plan[2].action == EvaluationAction::BlockedByStaleDependency &&
             plan[2].blocker == std::optional<CellId>(3),
         "transitive watch downstream should retain the nearest blocker");
}

void test_multiple_bindings_from_one_provider_form_one_cell_dependency() {
  DependencyGraph graph({cell(1, "x = 1\ny = 2\n"), cell(2, "sum = x + y\n"),
                         cell(3, "answer = sum * 2\n")});
  expect(graph.edges().size() == 3U,
         "binding-level edges should retain both x and y reads");
  expect(graph.direct_dependents_of(1) == std::vector<CellId>({2}),
         "cell-level dependents should deduplicate one provider/consumer");
  expect(graph.execution_order() == std::vector<CellId>({1, 2, 3}),
         "deduplicated cell edges should produce a complete topo order");
}

void test_transition_invalidation_retains_consumers_of_removed_output() {
  const std::vector<CellSource> previous_cells{
      cell(1, "x = 1\n"), cell(2, "middle = x + 1\n"),
      cell(3, "answer = middle * 2\n"), cell(4, "unrelated = 99\n")};
  const std::vector<CellSource> current_cells{
      cell(1, "renamed = 1\n"), cell(2, "middle = x + 1\n"),
      cell(3, "answer = middle * 2\n"), cell(4, "unrelated = 99\n")};
  DependencyGraph previous(previous_cells);
  DependencyGraph current(current_cells);

  expect(current.invalidation_plan(1) == std::vector<CellId>({1}),
         "the rebuilt graph alone no longer reaches an old x consumer");
  expect(current.transition_invalidation_plan(previous, 1) ==
             std::vector<CellId>({1, 2, 3}),
         "graph transition should retain former transitive consumers");

  const auto plan = amber::notebook::plan_transition_evaluation(
      current, previous, current_cells, 1);
  expect(plan.size() == 3U && plan[0].id == 1 && plan[1].id == 2 &&
             plan[2].id == 3,
         "transition scheduler should exclude unrelated cells");
}

void test_ids_are_stable_and_structural_errors_are_reported() {
  const CellId first = amber::notebook::allocate_cell_id();
  const CellId second = amber::notebook::allocate_cell_id();
  expect(second > first, "generated cell IDs should be monotonic");

  DependencyGraph graph(
      {cell(9, "x = 1\n"), cell(9, "y = 2\n"), cell(0, "z = 3\n")});
  expect(graph.cell_order() == std::vector<CellId>({9}),
         "invalid IDs must not silently alias graph nodes");
  expect(graph.diagnostics().size() == 2U,
         "zero and duplicate IDs should be structural diagnostics");
}

void test_frontend_diagnostics_remain_attached_to_cell() {
  const auto analysis = amber::notebook::analyze_cell(cell(8, "x = (1 +\n"));
  expect(!analysis.ok() && !analysis.diagnostics.empty(),
         "parser diagnostics should be returned by cell analysis");
  expect(analysis.id == 8, "analysis must preserve the stable CellId");
}

} // namespace

int main() {
  test_analysis_uses_ast_and_tracks_ordered_assignment_reads();
  test_kernel_watch_intrinsic_does_not_require_kernel_slot();
  test_graph_resolves_nearest_preceding_version();
  test_same_cell_local_read_has_no_external_edge();
  test_missing_names_and_transitive_invalidation();
  test_ambient_names_are_not_missing_and_never_create_providers();
  test_imports_are_cell_local_bindings();
  test_scheduler_keeps_manual_cells_stale_and_blocks_consumers();
  test_scheduler_automatic_roots_keep_manual_cells_stale();
  test_multiple_bindings_from_one_provider_form_one_cell_dependency();
  test_transition_invalidation_retains_consumers_of_removed_output();
  test_ids_are_stable_and_structural_errors_are_reported();
  test_frontend_diagnostics_remain_attached_to_cell();
  std::cout << "notebook_core_tests ok\n";
  return 0;
}
