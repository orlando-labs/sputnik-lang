#include "notebook/kernel.h"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using amber::notebook::BindingKey;
using amber::notebook::CellExecutionResult;
using amber::notebook::CellId;
using amber::notebook::CellMode;
using amber::notebook::CellSource;
using amber::notebook::KernelWrite;
using amber::notebook::NotebookKernel;
using amber::notebook::EvaluationAction;
using amber::notebook::RuntimeEventBatchPlanStatus;
using amber::runtime::Value;
using amber::runtime::RuntimeDependency;
using amber::runtime::RuntimeDependencyKind;
using amber::runtime::RuntimeDependencySet;
using amber::runtime::RuntimeDependencySourceKey;
using amber::runtime::RuntimeWatchEvent;
using amber::runtime::RuntimeWatchPollResult;
using amber::runtime::RuntimeWatchPollStatus;
using amber::runtime::RuntimeWatchStreamIdentity;

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << "\n";
    std::exit(1);
  }
}

CellSource cell(CellId id, std::string source,
                CellMode mode = CellMode::Watch) {
  return CellSource{id, std::move(source), "<kernel-test>", mode};
}

void test_chain_uses_published_inputs_and_equal_write_is_quiet() {
  const std::vector<CellSource> cells{cell(1, "x = 2\n"),
                                      cell(2, "y = x + 1\n")};
  NotebookKernel kernel;
  const auto plan = kernel.update_cells(cells, 1);
  expect(plan.size() == 2U, "root and consumer should enter the plan");

  const auto first = kernel.run_cell(1, [](const auto &inputs) {
    expect(inputs.empty(), "root should have no slot inputs");
    CellExecutionResult result;
    result.ok = true;
    result.writes.push_back(KernelWrite{{1, "x"}, Value::integer(2), {}});
    return result;
  });
  expect(first.ok && first.publication.events.size() == 1U,
         "root output should publish once");

  const auto second = kernel.run_cell(2, [](const auto &inputs) {
    expect(inputs.size() == 1U && inputs[0].key == BindingKey{1, "x"} &&
               inputs[0].value.as_integer() == 2,
           "consumer should receive the preceding provider slot");
    CellExecutionResult result;
    result.ok = true;
    result.writes.push_back(KernelWrite{
        {2, "y"}, Value::integer(inputs[0].value.as_integer() + 1), {}});
    return result;
  });
  expect(second.ok && kernel.read_slot({2, "y"})->as_integer() == 3,
         "consumer output should persist in its own slot");

  kernel.update_cells(cells, 1);
  const auto equal = kernel.run_cell(1, [](const auto &) {
    CellExecutionResult result;
    result.ok = true;
    result.writes.push_back(KernelWrite{{1, "x"}, Value::integer(2), {}});
    return result;
  });
  expect(equal.ok && equal.publication.events.empty() &&
             kernel.slot_state({1, "x"})->revision == 1U,
         "equal rerun should clear stale without advancing revision");
}

void test_failure_and_invalid_output_publish_nothing() {
  NotebookKernel kernel({cell(1, "a = 1\nb = 2\n")});
  const auto failed = kernel.run_cell(1, [](const auto &) {
    CellExecutionResult result;
    result.error = "boom";
    result.writes.push_back(KernelWrite{{1, "a"}, Value::integer(1), {}});
    return result;
  });
  expect(!failed.ok && kernel.slots_empty(),
         "failed backend must not publish staged-looking writes");

  const auto partial = kernel.run_cell(1, [](const auto &) {
    CellExecutionResult result;
    result.ok = true;
    result.writes.push_back(KernelWrite{{1, "a"}, Value::integer(1), {}});
    return result;
  });
  expect(!partial.ok && kernel.slots_empty(),
         "missing declared output must reject the complete batch");

  const auto success = kernel.run_cell(1, [](const auto &) {
    CellExecutionResult result;
    result.ok = true;
    result.writes.push_back(KernelWrite{{1, "a"}, Value::integer(1), {}});
    result.writes.push_back(KernelWrite{{1, "b"}, Value::integer(2), {}});
    return result;
  });
  expect(success.ok && success.publication.events.size() == 2U,
         "complete output batch should commit atomically");
}

void test_executor_exception_publishes_nothing() {
  NotebookKernel kernel({cell(1, "value = 1\n")});
  const auto result =
      kernel.run_cell(1, [](const auto &) -> CellExecutionResult {
        throw std::runtime_error("backend exploded");
      });
  expect(!result.ok &&
             result.error.find("backend exploded") != std::string::npos &&
             kernel.slots_empty(),
         "executor exceptions must be converted to failure without writes");
}

void test_graph_transition_stales_removed_output_and_old_consumers() {
  NotebookKernel kernel;
  const std::vector<CellSource> initial{
      cell(1, "x = 1\n"), cell(2, "y = x + 1\n"), cell(3, "unrelated = 9\n")};
  kernel.update_cells(initial, 1);
  kernel.run_cell(1, [](const auto &) {
    CellExecutionResult result;
    result.ok = true;
    result.writes.push_back(KernelWrite{{1, "x"}, Value::integer(1), {}});
    return result;
  });

  const std::vector<CellSource> edited{cell(1, "renamed = 1\n"),
                                       cell(2, "y = x + 1\n"),
                                       cell(3, "unrelated = 9\n")};
  const auto plan = kernel.update_cells(edited, 1);
  expect(plan.size() == 2U && plan[0].id == 1 && plan[1].id == 2,
         "former consumer should remain invalidated after output removal");
  expect(kernel.slot_state({1, "x"})->stale,
         "removed published output should remain explicitly stale");
}

void test_removed_cell_stales_its_published_provider() {
  NotebookKernel kernel;
  const std::vector<CellSource> initial{cell(1, "x = 1\n"),
                                        cell(2, "y = x + 1\n")};
  kernel.update_cells(initial, 1);
  expect(kernel
             .run_cell(1,
                       [](const auto &) {
                         CellExecutionResult result;
                         result.ok = true;
                         result.writes.push_back(
                             KernelWrite{{1, "x"}, Value::integer(1), {}});
                         return result;
                       })
             .ok,
         "provider should publish before the cell is removed");

  kernel.update_cells({cell(2, "y = x + 1\n")}, 1);
  const auto removed = kernel.slot_state({1, "x"});
  expect(removed.has_value() && removed->stale,
         "a removed cell's historical provider must become stale");
}

void test_removed_cell_drops_dynamic_dependencies() {
  NotebookKernel kernel({cell(1, "x = 1\n"), cell(2, "y = x + 1\n")});
  const auto run = kernel.run_cell(1, [](const auto &) {
    CellExecutionResult result;
    result.ok = true;
    result.writes.push_back(KernelWrite{{1, "x"}, Value::integer(1), {}});
    result.dynamic_dependencies.push_back({99, "captured"});
    return result;
  });
  expect(run.ok && !kernel.dynamic_dependencies(1).empty(),
         "dynamic dependencies should be retained for a live cell");

  kernel.update_cells({cell(2, "y = x + 1\n")}, 1);
  expect(kernel.dynamic_dependencies(1).empty(),
         "removed cells must not leave dynamic dependencies behind");
}

void test_unresolved_input_is_rejected_before_executor() {
  NotebookKernel kernel({cell(2, "y = missing + 1\n")});
  bool called = false;
  const auto result = kernel.run_cell(2, [&called](const auto &) {
    called = true;
    CellExecutionResult execution;
    execution.ok = true;
    execution.writes.push_back(KernelWrite{{2, "y"}, Value::integer(1), {}});
    return execution;
  });
  expect(!result.ok && !called && kernel.slots_empty(),
         "unresolved notebook inputs must block execution and publication");
}

void test_ambient_input_runs_without_a_notebook_slot() {
  NotebookKernel kernel({cell(1, "value = Ambient + 1\n")},
                        std::set<std::string>{"Ambient"});
  bool called = false;
  const auto result = kernel.run_cell(1, [&called](const auto &inputs) {
    called = true;
    expect(inputs.empty(), "ambient reads must not become kernel inputs");
    CellExecutionResult execution;
    execution.ok = true;
    execution.writes.push_back(
        KernelWrite{{1, "value"}, Value::integer(2), {}});
    return execution;
  });
  expect(result.ok && called,
         "a configured ambient read should allow cell execution");
}

void test_notebook_provider_shadows_ambient_and_stale_blocks() {
  NotebookKernel kernel(
      {cell(1, "Ambient = 1\n"), cell(2, "value = Ambient + 1\n")},
      std::set<std::string>{"Ambient"});
  expect(kernel.run_cell(1, [](const auto &) {
           CellExecutionResult execution;
           execution.ok = true;
           execution.writes.push_back(
               KernelWrite{{1, "Ambient"}, Value::integer(1), {}});
           return execution;
         }).ok,
         "the notebook provider should publish before its consumer");

  kernel.update_cells(
      {cell(1, "Ambient = 2\n"), cell(2, "value = Ambient + 1\n")}, 1);
  const auto result = kernel.run_cell(2, [](const auto &) {
    CellExecutionResult execution;
    execution.ok = true;
    execution.writes.push_back(
        KernelWrite{{2, "value"}, Value::integer(3), {}});
    return execution;
  });
  expect(!result.ok && result.error.find("stale") != std::string::npos,
         "a stale notebook provider must still block an ambient-shadowed read");
}

void test_provider_removal_falls_back_to_ambient() {
  NotebookKernel kernel(
      {cell(1, "Ambient = 1\n"), cell(2, "value = Ambient + 1\n")},
      std::set<std::string>{"Ambient"});
  expect(kernel.run_cell(1, [](const auto &) {
           CellExecutionResult execution;
           execution.ok = true;
           execution.writes.push_back(
               KernelWrite{{1, "Ambient"}, Value::integer(1), {}});
           return execution;
         }).ok,
         "the initial notebook provider should publish");

  kernel.update_cells(
      {cell(1, "renamed = 1\n"), cell(2, "value = Ambient + 1\n")}, 1);
  const auto result = kernel.run_cell(2, [](const auto &inputs) {
    expect(inputs.empty(), "ambient fallback should have no slot input");
    CellExecutionResult execution;
    execution.ok = true;
    execution.writes.push_back(
        KernelWrite{{2, "value"}, Value::integer(3), {}});
    return execution;
  });
  expect(result.ok,
         "removing a notebook provider should restore ambient resolution");
}

void test_reset_preserves_ambient_names() {
  NotebookKernel kernel({cell(1, "value = Ambient + 1\n")},
                        std::set<std::string>{"Ambient"});
  kernel.reset();
  kernel.update_cells({cell(1, "value = Ambient + 1\n")}, 1);
  const auto result = kernel.run_cell(1, [](const auto &) {
    CellExecutionResult execution;
    execution.ok = true;
    execution.writes.push_back(
        KernelWrite{{1, "value"}, Value::integer(2), {}});
    return execution;
  });
  expect(result.ok, "reset should preserve the kernel ambient environment");
}

void test_dynamic_dependencies_replace_only_after_success() {
  NotebookKernel kernel({cell(1, "value = 1\n")});
  auto run_with = [&kernel](BindingKey dependency, bool ok) {
    return kernel.run_cell(1, [dependency, ok](const auto &) {
      CellExecutionResult result;
      result.ok = ok;
      result.error = ok ? "" : "failed";
      result.writes.push_back(KernelWrite{{1, "value"}, Value::integer(1), {}});
      result.dynamic_dependencies.push_back(dependency);
      return result;
    });
  };

  expect(run_with({10, "left"}, true).ok,
         "first dynamic capture should publish");
  expect(kernel.dynamic_dependencies(1) ==
             std::vector<BindingKey>({{10, "left"}}),
         "first capture should be retained");
  expect(!run_with({20, "failed"}, false).ok &&
             kernel.dynamic_dependencies(1) ==
                 std::vector<BindingKey>({{10, "left"}}),
         "failed run should preserve the previous capture");
  expect(run_with({30, "right"}, true).ok &&
             kernel.dynamic_dependencies(1) ==
                 std::vector<BindingKey>({{30, "right"}}),
         "successful run should replace rather than accumulate dependencies");

  kernel.reset();
  expect(kernel.slots_empty() && kernel.gc_roots().empty() &&
             kernel.dynamic_dependencies(1).empty(),
         "kernel reset should release slots, roots, and dynamic edges");
}

RuntimeWatchStreamIdentity test_runtime_source() { return {17, 3}; }

RuntimeDependencySet rich_capture(
    CellId consumer, CellId provider, std::string target,
    RuntimeWatchStreamIdentity source = test_runtime_source()) {
  RuntimeDependencySet capture;
  capture.notebook_cell_id = consumer;
  capture.source = source;
  RuntimeDependency dependency;
  dependency.kind = RuntimeDependencyKind::Binding;
  dependency.cell_id = provider;
  dependency.target_name = std::move(target);
  dependency.revision = 4;
  capture.dependencies.push_back(std::move(dependency));
  return capture;
}

RuntimeWatchEvent runtime_event(std::string kind) {
  RuntimeWatchEvent event;
  event.kind = std::move(kind);
  event.watch_world_id = test_runtime_source().world_id;
  event.watch_generation = test_runtime_source().generation;
  return event;
}

RuntimeWatchPollResult runtime_batch(
    std::vector<RuntimeWatchEvent> events,
    RuntimeWatchStreamIdentity source = test_runtime_source()) {
  RuntimeWatchPollResult batch;
  batch.source = source;
  batch.requested_cursor = {source, 1U};
  batch.oldest_retained_epoch = 1U;
  for (std::size_t index = 0; index < events.size(); ++index) {
    RuntimeWatchEvent &event = events[index];
    event.watch_world_id = source.world_id;
    event.watch_generation = source.generation;
    if (event.watch_epoch == 0U) {
      event.watch_epoch = static_cast<std::uint64_t>(index) + 1U;
    }
  }
  batch.events = std::move(events);
  batch.latest_epoch = batch.events.empty()
                           ? 0U
                           : batch.events.back().watch_epoch;
  batch.next_cursor = {source, batch.latest_epoch + 1U};
  return batch;
}

void test_runtime_event_planner_normalizes_captures_and_matches_revisions() {
  NotebookKernel kernel({cell(1, "value = 1\n")});
  RuntimeDependencySet capture;
  capture.notebook_cell_id = 1;
  capture.source = test_runtime_source();

  RuntimeDependency binding;
  binding.kind = RuntimeDependencyKind::Binding;
  binding.cell_id = 901;
  binding.target_name = "z";
  binding.revision = 3;
  capture.dependencies.push_back(binding);
  binding.target_name = "a";
  binding.revision = 5;
  binding.object_id = 88; // must not split Binding identity
  capture.dependencies.push_back(binding);

  RuntimeDependency ivar;
  ivar.kind = RuntimeDependencyKind::Ivar;
  ivar.object_id = 77;
  ivar.field_name = "left";
  ivar.revision = 3;
  ivar.object_revision = 4;
  capture.dependencies.push_back(ivar);
  ivar.revision = 7;
  ivar.object_revision = 2;
  ivar.target_name = "@other-spelling";
  capture.dependencies.push_back(ivar);

  RuntimeDependency object;
  object.kind = RuntimeDependencyKind::Object;
  object.object_id = 77;
  object.object_revision = 6;
  capture.dependencies.push_back(object);
  object.object_revision = 5;
  capture.dependencies.push_back(object);

  const auto initial = kernel.run_cell(1, [capture](const auto &) {
    CellExecutionResult result;
    result.ok = true;
    result.writes.push_back(KernelWrite{{1, "value"}, Value::integer(1), {}});
    result.runtime_dependencies = capture;
    return result;
  });
  expect(initial.ok && initial.runtime_dependencies.has_value(),
         "normalized runtime capture should publish with the slot");
  const auto stored = kernel.runtime_dependencies(1);
  expect(stored.has_value() && stored->dependencies.size() == 3U,
         "duplicate source identities should collapse to three sources");
  const RuntimeDependency *stored_binding = nullptr;
  const RuntimeDependency *stored_ivar = nullptr;
  const RuntimeDependency *stored_object = nullptr;
  for (const RuntimeDependency &dependency : stored->dependencies) {
    if (dependency.kind == RuntimeDependencyKind::Binding) {
      stored_binding = &dependency;
    } else if (dependency.kind == RuntimeDependencyKind::Ivar) {
      stored_ivar = &dependency;
    } else {
      stored_object = &dependency;
    }
  }
  expect(stored_binding != nullptr && stored_binding->revision == 5U &&
             stored_binding->object_id == 0U &&
             stored_ivar != nullptr && stored_ivar->revision == 7U &&
             stored_ivar->object_revision == 4U && stored_object != nullptr &&
             stored_object->object_revision == 6U,
         "normalization should retain max source revisions and canonical IDs");

  const RuntimeDependencySourceKey binding_key =
      stored_binding->source_key();
  expect(kernel.runtime_dependency_consumers(binding_key) ==
             std::vector<CellId>({1}),
         "forward publication should install the reverse consumer edge");

  RuntimeWatchEvent registration = runtime_event("watch.binding");
  registration.cell_id = 901;
  registration.old_revision = 5;
  registration.new_revision = 99;
  expect(kernel.plan_runtime_events(registration).empty(),
         "registration events must remain quiet regardless of copied revisions");

  RuntimeWatchEvent equal = runtime_event("watch.write");
  equal.cell_id = 901;
  equal.old_revision = 5;
  equal.new_revision = 5;
  expect(kernel.plan_runtime_events(equal).empty(),
         "equal binding revision must remain quiet");

  RuntimeWatchEvent lower = equal;
  lower.old_revision = 6;
  lower.new_revision = 4;
  expect(kernel.plan_runtime_events(lower).empty(),
         "lower binding revision must remain quiet");

  RuntimeWatchEvent wrong_field = runtime_event("watch.ivar.write");
  wrong_field.object_id = 77;
  wrong_field.field_name = "right";
  wrong_field.old_revision = 0;
  wrong_field.new_revision = 99;
  wrong_field.old_object_revision = 6;
  wrong_field.new_object_revision = 6;
  expect(kernel.plan_runtime_events(wrong_field).empty(),
         "an ivar mutation for another field must remain quiet");

  RuntimeWatchEvent regressed_field = runtime_event("watch.ivar.write");
  regressed_field.object_id = 77;
  regressed_field.field_name = "left";
  regressed_field.old_revision = 8;
  regressed_field.new_revision = 7;
  regressed_field.old_object_revision = 6;
  regressed_field.new_object_revision = 7;
  expect(kernel.plan_runtime_events(regressed_field).empty(),
         "a malformed mixed-revision ivar event must remain quiet");

  RuntimeWatchEvent newer_binding = runtime_event("watch.write");
  newer_binding.cell_id = 901;
  newer_binding.old_revision = 5;
  newer_binding.new_revision = 6;
  // This kernel has one cell, so the returned runtime root is the only step.
  const auto binding_plan = kernel.plan_runtime_events(newer_binding);
  expect(binding_plan.size() == 1U && binding_plan[0].id == 1U &&
             binding_plan[0].action == EvaluationAction::Run,
         "newer binding mutation should return its automatic root");
  expect(kernel.slot_state({1, "value"})->stale,
         "runtime planner should mark returned outputs stale");

  // Re-publish a fresh capture after the automatic invalidation and verify
  // both ivar and object matching against their independent revisions.
  const auto refreshed = kernel.run_cell(1, [capture](const auto &) {
    CellExecutionResult result;
    result.ok = true;
    result.writes.push_back(KernelWrite{{1, "value"}, Value::integer(2), {}});
    result.runtime_dependencies = capture;
    return result;
  });
  expect(refreshed.ok, "fresh capture should clear stale output");

  RuntimeWatchEvent newer_ivar = runtime_event("watch.ivar.write");
  newer_ivar.object_id = 77;
  newer_ivar.field_name = "left";
  newer_ivar.old_revision = 7;
  newer_ivar.new_revision = 8;
  newer_ivar.old_object_revision = 6;
  newer_ivar.new_object_revision = 7;
  expect(kernel.plan_runtime_events(newer_ivar).size() == 1U,
         "newer ivar field revision should match the ivar dependency");

  const auto refreshed_again = kernel.run_cell(1, [capture](const auto &) {
    CellExecutionResult result;
    result.ok = true;
    result.writes.push_back(KernelWrite{{1, "value"}, Value::integer(3), {}});
    result.runtime_dependencies = capture;
    return result;
  });
  expect(refreshed_again.ok, "second fresh capture should publish");

  RuntimeWatchEvent newer_object = runtime_event("watch.ivar.write");
  newer_object.object_id = 77;
  newer_object.field_name = "left";
  newer_object.old_revision = 7;
  newer_object.new_revision = 7;
  newer_object.old_object_revision = 6;
  newer_object.new_object_revision = 8;
  expect(kernel.plan_runtime_events(newer_object).size() == 1U,
         "ivar object revision should match Object dependencies");
}

void test_runtime_event_reverse_index_replaces_empty_and_removes_consumers() {
  NotebookKernel kernel({cell(1, "value = 1\n")});
  auto capture_run = [&kernel](std::optional<RuntimeDependencySet> capture,
                               bool ok, int value) {
    return kernel.run_cell(1, [capture = std::move(capture), ok,
                               value](const auto &) {
      CellExecutionResult result;
      result.ok = ok;
      result.error = ok ? "" : "failed";
      result.writes.push_back(
          KernelWrite{{1, "value"}, Value::integer(value), {}});
      result.runtime_dependencies = capture;
      return result;
    });
  };

  RuntimeDependencySet first;
  first.notebook_cell_id = 1;
  first.source = test_runtime_source();
  RuntimeDependency dependency;
  dependency.kind = RuntimeDependencyKind::Binding;
  dependency.cell_id = 902;
  dependency.revision = 1;
  first.dependencies.push_back(dependency);
  expect(capture_run(first, true, 1).ok, "first source capture should publish");
  expect(kernel.runtime_dependency_consumers(dependency.source_key()) ==
             std::vector<CellId>({1}),
         "first source should be reverse indexed");

  RuntimeDependencySet replacement = first;
  replacement.dependencies.front().cell_id = 903;
  replacement.dependencies.front().revision = 2;
  expect(!capture_run(replacement, false, 2).ok &&
             kernel.runtime_dependency_consumers(dependency.source_key()) ==
                 std::vector<CellId>({1}),
         "failed replacement must preserve forward and reverse captures");
  expect(capture_run(replacement, true, 2).ok &&
             kernel.runtime_dependency_consumers(dependency.source_key()).empty() &&
             kernel.runtime_dependency_consumers(
                 replacement.dependencies.front().source_key()) ==
                 std::vector<CellId>({1}),
         "successful replacement must remove the old reverse edge");

  RuntimeDependencySet empty;
  empty.notebook_cell_id = 1;
  empty.source = test_runtime_source();
  expect(capture_run(empty, true, 3).ok &&
             kernel.runtime_dependency_consumers(
                 replacement.dependencies.front().source_key()).empty() &&
             kernel.runtime_dependencies(1).has_value() &&
             kernel.runtime_dependencies(1)->dependencies.empty(),
         "successful empty capture must clear reverse and forward edges");

  expect(capture_run(first, true, 4).ok, "capture should be restorable");
  kernel.update_cells({}, 1);
  expect(kernel.runtime_dependency_consumers(dependency.source_key()).empty(),
         "removing a consumer cell must remove its reverse edge");
  kernel.reset();
  expect(kernel.runtime_dependency_consumers(dependency.source_key()).empty(),
         "reset must clear all reverse edges");
}

void test_runtime_event_batch_uses_static_closure_and_manual_policy() {
  const std::vector<CellSource> cells{
      cell(1, "watch_root = 1\n"),
      cell(2, "middle = watch_root + 1\n"),
      cell(3, "tail = middle + 1\n"),
      cell(4, "manual_root = 1\n", CellMode::Manual),
      cell(5, "manual_tail = manual_root + 1\n"),
  };
  NotebookKernel kernel(cells);
  auto publish_root = [&kernel](CellId id, const std::string &name,
                                std::uint64_t runtime_cell_id,
                                std::uint64_t revision) {
    return kernel.run_cell(id, [=](const auto &) {
      CellExecutionResult result;
      result.ok = true;
      result.writes.push_back(KernelWrite{{id, name}, Value::integer(1), {}});
      result.runtime_dependencies =
          rich_capture(id, runtime_cell_id, name + "-runtime");
      result.runtime_dependencies->dependencies.front().revision = revision;
      return result;
    });
  };
  expect(publish_root(1, "watch_root", 1001, 2).ok &&
             publish_root(4, "manual_root", 1004, 3).ok,
         "runtime roots should publish their captures");

  RuntimeWatchEvent watch_event = runtime_event("watch.write");
  watch_event.cell_id = 1001;
  watch_event.old_revision = 2;
  watch_event.new_revision = 3;
  watch_event.old_value = Value::integer(7);
  watch_event.new_value = Value::integer(7);
  RuntimeWatchEvent manual_event = runtime_event("watch.write");
  manual_event.cell_id = 1004;
  manual_event.old_revision = 3;
  manual_event.new_revision = 4;

  const auto plan =
      kernel.plan_runtime_events({manual_event, watch_event});
  expect(plan.size() == 5U && plan[0].id == 1U && plan[1].id == 2U &&
             plan[2].id == 3U && plan[3].id == 4U && plan[4].id == 5U,
         "runtime roots should be coalesced into one stable static closure");
  expect(plan[0].action == EvaluationAction::Run &&
             plan[1].action == EvaluationAction::Run &&
             plan[2].action == EvaluationAction::Run,
         "watch roots and their watch descendants should run");
  expect(plan[3].action == EvaluationAction::KeepManualStale &&
             plan[4].action == EvaluationAction::BlockedByStaleDependency &&
             plan[4].blocker == std::optional<CellId>(4),
         "an automatic event must not execute a manual root");
  const auto reversed =
      kernel.plan_runtime_events({watch_event, manual_event});
  bool same_plan = reversed.size() == plan.size();
  for (std::size_t index = 0; same_plan && index < plan.size(); ++index) {
    same_plan = reversed[index].id == plan[index].id &&
                reversed[index].action == plan[index].action &&
                reversed[index].blocker == plan[index].blocker;
  }
  expect(same_plan, "batch planning should be independent of event order");

  RuntimeWatchEvent unknown_alias = watch_event;
  unknown_alias.kind = "binding.write";
  expect(kernel.plan_runtime_events(unknown_alias).empty(),
         "the planner should accept only the runtime's canonical event kinds");
}

void test_runtime_event_root_respects_preexisting_stale_input() {
  const std::vector<CellSource> cells{
      cell(1, "source = 1\n"), cell(2, "value = source + 1\n")};
  NotebookKernel kernel(cells);
  expect(kernel.run_cell(1, [](const auto &) {
           CellExecutionResult result;
           result.ok = true;
           result.writes.push_back(
               KernelWrite{{1, "source"}, Value::integer(1), {}});
           return result;
         }).ok,
         "static provider should publish before its consumer");
  expect(kernel.run_cell(2, [](const auto &) {
           CellExecutionResult result;
           result.ok = true;
           result.writes.push_back(
               KernelWrite{{2, "value"}, Value::integer(2), {}});
           result.runtime_dependencies = rich_capture(2, 2002, "external");
           return result;
         }).ok,
         "runtime consumer should publish its initial capture");

  kernel.update_cells(cells, 1);
  expect(kernel.slot_state({1, "source"})->stale,
         "an unexecuted edit plan should leave the provider stale");
  RuntimeWatchEvent event = runtime_event("watch.write");
  event.cell_id = 2002;
  event.old_revision = 4;
  event.new_revision = 5;
  const auto plan = kernel.plan_runtime_events(event);
  expect(plan.size() == 1U && plan[0].id == 2U &&
             plan[0].action == EvaluationAction::BlockedByStaleDependency &&
             plan[0].blocker == std::optional<CellId>(1),
         "runtime root must be blocked by a stale provider outside its closure");
}

void test_runtime_stream_resync_preserves_automatic_manual_policy() {
  const std::vector<CellSource> cells{
      cell(1, "watch_root = 1\n"),
      cell(2, "watch_tail = watch_root + 1\n"),
      cell(3, "manual_root = 1\n", CellMode::Manual),
      cell(4, "manual_tail = manual_root + 1\n"),
  };
  NotebookKernel kernel(cells);
  expect(kernel.run_cell(1, [](const auto &) {
           CellExecutionResult result;
           result.ok = true;
           result.writes.push_back(
               KernelWrite{{1, "watch_root"}, Value::integer(1), {}});
           return result;
         }).ok &&
             kernel.run_cell(3, [](const auto &) {
               CellExecutionResult result;
               result.ok = true;
               result.writes.push_back(
                   KernelWrite{{3, "manual_root"}, Value::integer(1), {}});
               return result;
             }).ok,
         "resync roots should have published values to invalidate");

  const auto plan = kernel.plan_runtime_resync();
  expect(plan.size() == 4U && plan[0].id == 1U && plan[1].id == 2U &&
             plan[2].id == 3U && plan[3].id == 4U,
         "stream resync should cover every cell in stable graph order");
  expect(plan[0].action == EvaluationAction::Run &&
             plan[1].action == EvaluationAction::Run &&
             plan[2].action == EvaluationAction::KeepManualStale &&
             plan[3].action ==
                 EvaluationAction::BlockedByStaleDependency &&
             plan[3].blocker == std::optional<CellId>(3),
         "stream resync must remain an automatic evaluation");
  expect(kernel.slot_state({1, "watch_root"})->stale &&
             kernel.slot_state({3, "manual_root"})->stale,
         "resync marks both runnable and manual root outputs stale");
}

void test_runtime_event_source_namespace_rejects_foreign_world() {
  NotebookKernel kernel({cell(1, "value = 1\n")});
  const amber::runtime::RuntimeWatchStreamIdentity source{41, 2};
  const amber::runtime::RuntimeWatchStreamIdentity foreign{42, 2};
  expect(kernel.bind_runtime_watch_source(source),
         "kernel should bind its first valid runtime source");
  expect(kernel.run_cell(1, [source](const auto &) {
           CellExecutionResult result;
           result.ok = true;
           result.writes.push_back(
               KernelWrite{{1, "value"}, Value::integer(1), {}});
           result.runtime_dependencies =
               rich_capture(1, 7001, "external", source);
           return result;
         }).ok,
         "source namespace test should publish a runtime capture");

  RuntimeWatchEvent event = runtime_event("watch.write");
  event.cell_id = 7001;
  event.old_revision = 4;
  event.new_revision = 5;
  event.watch_world_id = foreign.world_id;
  event.watch_generation = foreign.generation;
  expect(kernel.plan_runtime_events(event).empty(),
         "matching numeric IDs from another world must remain quiet");

  const auto foreign_batch =
      kernel.plan_runtime_event_batch(runtime_batch({event}, foreign));
  expect(foreign_batch.status ==
             RuntimeEventBatchPlanStatus::SourceMismatch &&
             foreign_batch.plan.empty(),
         "a valid foreign batch must be rejected before acknowledgement");

  RuntimeWatchPollResult mixed = runtime_batch({event}, source);
  mixed.events.front().watch_world_id = foreign.world_id;
  const auto invalid_batch = kernel.plan_runtime_event_batch(mixed);
  expect(invalid_batch.status == RuntimeEventBatchPlanStatus::InvalidBatch,
         "a batch that mixes event namespaces must be structurally invalid");

  event.watch_world_id = source.world_id;
  event.watch_generation = source.generation;
  const auto accepted =
      kernel.plan_runtime_event_batch(runtime_batch({event}, source));
  expect(accepted.accepted() && accepted.plan.size() == 1U &&
             accepted.plan[0].id == 1U &&
             accepted.plan[0].action == EvaluationAction::Run,
         "the bound world source should invalidate its runtime consumer");
  expect(!kernel.bind_runtime_watch_source(foreign),
         "a live kernel must reject rebinding to a different world source");
  expect(kernel.reset_runtime_watch_source(foreign) &&
             !kernel.runtime_dependencies(1).has_value() &&
             !kernel.slot_state({1, "value"}).has_value(),
         "source recovery must clear runtime edges and foreign slot values");
  expect(!kernel.bind_runtime_watch_source(source),
         "source recovery must replace the old namespace");
  kernel.reset();
  expect(kernel.bind_runtime_watch_source(source),
         "reset should release the previous runtime source namespace");

  NotebookKernel late_bound({cell(1, "value = 1\n")});
  expect(late_bound.run_cell(1, [](const auto &) {
           CellExecutionResult result;
           result.ok = true;
           result.writes.push_back(
               KernelWrite{{1, "value"}, Value::integer(1), {}});
           result.runtime_dependencies = rich_capture(1, 7001, "external");
           return result;
         }).ok,
         "first namespaced capture should bind an unbound kernel");
  expect(!late_bound.bind_runtime_watch_source(source),
         "kernel must reject rebinding an auto-bound capture namespace");
}

void test_runtime_event_batch_rejects_malformed_ack_boundaries() {
  NotebookKernel kernel({cell(1, "value = 1\n")});
  const RuntimeWatchStreamIdentity source = test_runtime_source();
  expect(kernel.run_cell(1, [](const auto &) {
           CellExecutionResult result;
           result.ok = true;
           result.writes.push_back(
               KernelWrite{{1, "value"}, Value::integer(1), {}});
           return result;
         }).ok,
         "malformed batch test should publish a non-stale sentinel slot");
  expect(kernel.bind_runtime_watch_source(source),
         "typed batch validation requires an explicit source namespace");

  const auto registration =
      kernel.plan_runtime_event_batch(runtime_batch({}, source));
  expect(registration.accepted() && registration.plan.empty() &&
             registration.acknowledgement_cursor.has_value() &&
             registration.acknowledgement_cursor->source == source &&
             registration.acknowledgement_cursor->next_epoch == 1U,
         "an empty tail batch is a valid acknowledgement unit");

  RuntimeWatchPollResult partial = runtime_batch(
      {runtime_event("watch.binding"), runtime_event("watch.binding")},
      source);
  partial.latest_epoch = 3U;
  const auto accepted_partial = kernel.plan_runtime_event_batch(partial);
  expect(accepted_partial.accepted() && accepted_partial.plan.empty() &&
             accepted_partial.acknowledgement_cursor->next_epoch == 3U,
         "a bounded contiguous prefix may acknowledge before the stream tail");

  RuntimeWatchEvent first = runtime_event("watch.write");
  first.watch_epoch = 1U;
  RuntimeWatchEvent duplicate = runtime_event("watch.write");
  duplicate.watch_epoch = 1U;
  RuntimeWatchPollResult unordered =
      runtime_batch({first, duplicate}, source);
  expect(kernel.plan_runtime_event_batch(unordered).status ==
             RuntimeEventBatchPlanStatus::InvalidBatch,
         "duplicate or unordered event epochs must reject the entire batch");

  RuntimeWatchEvent skipped = runtime_event("watch.write");
  skipped.watch_epoch = 2U;
  RuntimeWatchPollResult gap_at_front = runtime_batch({skipped}, source);
  expect(kernel.plan_runtime_event_batch(gap_at_front).status ==
             RuntimeEventBatchPlanStatus::InvalidBatch,
         "a successful batch must begin at its exact requested cursor");

  RuntimeWatchPollResult wrong_successor = runtime_batch({first}, source);
  wrong_successor.next_cursor.next_epoch = 1U;
  expect(kernel.plan_runtime_event_batch(wrong_successor).status ==
             RuntimeEventBatchPlanStatus::InvalidBatch,
         "a cursor that does not follow the last event must not be published");

  RuntimeWatchPollResult outside_retention = runtime_batch({first}, source);
  outside_retention.oldest_retained_epoch = 2U;
  expect(kernel.plan_runtime_event_batch(outside_retention).status ==
             RuntimeEventBatchPlanStatus::InvalidBatch,
         "events outside the retained stream bounds must be rejected");

  RuntimeWatchPollResult gap = runtime_batch({first}, source);
  gap.status = RuntimeWatchPollStatus::Overflow;
  expect(kernel.plan_runtime_event_batch(gap).status ==
             RuntimeEventBatchPlanStatus::InvalidBatch,
         "overflow recovery must use conservative resync, not typed events");

  RuntimeWatchPollResult empty_bad_tail = runtime_batch({}, source);
  empty_bad_tail.latest_epoch = 4U;
  expect(kernel.plan_runtime_event_batch(empty_bad_tail).status ==
             RuntimeEventBatchPlanStatus::InvalidBatch,
         "an empty batch must acknowledge exactly the reported stream tail");

  RuntimeWatchPollResult timeout = runtime_batch({}, source);
  timeout.timed_out = true;
  const auto timed_out_batch = kernel.plan_runtime_event_batch(timeout);
  expect(timed_out_batch.status ==
             RuntimeEventBatchPlanStatus::InvalidBatch &&
             !timed_out_batch.acknowledgement_cursor.has_value(),
         "a wait timeout must not become an acknowledgement batch");
  expect(kernel.slot_state({1, "value"}).has_value() &&
             !kernel.slot_state({1, "value"})->stale,
         "rejected batches must not mutate published slot staleness");
}

void test_runtime_source_reset_drops_old_watch_values_until_resync() {
  NotebookKernel kernel(
      {cell(1, "value = 1\n"), cell(2, "copy = value\n")});
  const RuntimeWatchStreamIdentity old_source{51U, 1U};
  const RuntimeWatchStreamIdentity new_source{52U, 1U};
  expect(kernel.bind_runtime_watch_source(old_source),
         "old-world slot test should bind its initial source");
  auto old_watch_cell = std::make_shared<amber::runtime::RuntimeWatchCell>(
      Value::integer(1), 91U, "value");
  expect(kernel.run_cell(1, [old_watch_cell](const auto &) {
           CellExecutionResult result;
           result.ok = true;
           result.writes.push_back(KernelWrite{
               {1, "value"}, Value::watch_cell(old_watch_cell), {}});
           return result;
         }).ok,
         "provider should publish an old-world watch wrapper");

  expect(kernel.reset_runtime_watch_source(new_source),
         "source reset should accept the replacement namespace");
  bool consumer_invoked = false;
  const auto quarantined = kernel.run_cell(
      2, [&consumer_invoked](const auto &) {
        consumer_invoked = true;
        return CellExecutionResult{};
      });
  expect(!quarantined.ok && !consumer_invoked &&
             quarantined.error.find("not initialized") != std::string::npos,
         "old-world slot values must be rejected before entering an executor");

  const auto resync = kernel.plan_runtime_resync();
  expect(resync.size() == 2U && resync[0].id == 1U &&
             resync[1].id == 2U,
         "source recovery should schedule providers before their consumers");
  expect(kernel.run_cell(1, [](const auto &) {
           CellExecutionResult result;
           result.ok = true;
           result.writes.push_back(
               KernelWrite{{1, "value"}, Value::integer(7), {}});
           return result;
         }).ok,
         "new-world provider should replace the discarded wrapper");
  expect(kernel.run_cell(2, [](const auto &inputs) {
           CellExecutionResult result;
           result.ok = inputs.size() == 1U && inputs[0].value.is_integer() &&
                       inputs[0].value.as_integer() == 7;
           result.writes.push_back(
               KernelWrite{{2, "copy"}, Value::integer(7), {}});
           return result;
         }).ok,
         "consumer should run after its provider republishes in the new world");
}

void test_prepared_cell_update_cancels_or_commits_as_one_generation() {
  const std::vector<CellSource> initial_cells{cell(1, "value = 1\n")};
  const std::vector<CellSource> renamed_cells{cell(1, "renamed = 2\n")};
  NotebookKernel kernel(initial_cells);
  expect(kernel.run_cell(1, [](const auto &) {
           CellExecutionResult result;
           result.ok = true;
           result.writes.push_back(
               KernelWrite{{1, "value"}, Value::integer(1), {}});
           return result;
         }).ok,
         "initial graph generation should publish a value");

  auto cancelled = kernel.prepare_update_cells(renamed_cells, 1);
  expect(cancelled.active() && !cancelled.committed() &&
             !kernel.slot_state({1, "value"})->stale,
         "preparation must not mutate the published graph or slots");
  cancelled.cancel();
  expect(!cancelled.active() && !kernel.slot_state({1, "value"})->stale,
         "destroying/cancelling a candidate must leave publication unchanged");

  std::vector<amber::notebook::EvaluationStep> plan;
  {
    auto prepared = kernel.prepare_update_cells(renamed_cells, 1);
    expect(kernel.commit_update_cells(prepared) && prepared.committed(),
           "prepared generation should commit once");
    plan = prepared.take_plan();
  }
  expect(plan.size() == 1U && plan.front().id == 1U &&
             kernel.slot_state({1, "value"})->stale,
         "committed rename should stale the historical output");
  expect(kernel.run_cell(1, [](const auto &) {
           CellExecutionResult result;
           result.ok = true;
           result.writes.push_back(
               KernelWrite{{1, "renamed"}, Value::integer(2), {}});
           return result;
         }).ok,
         "run_cell should observe the committed candidate graph generation");
}

void test_rich_dependencies_are_transactional_and_separate() {
  NotebookKernel kernel({cell(1, "value = 1\n")});

  const auto initial = kernel.run_cell(1, [](const auto &) {
    CellExecutionResult result;
    result.ok = true;
    result.writes.push_back(
        KernelWrite{{1, "value"}, Value::integer(1), {}});
    result.dynamic_dependencies.push_back({77, "legacy"});
    result.runtime_dependencies = rich_capture(1, 901, "watched");
    return result;
  });
  expect(initial.ok && initial.runtime_dependencies.has_value(),
         "initial rich dependency capture should publish");
  const auto published = kernel.runtime_dependencies(1);
  expect(published.has_value() &&
             published->notebook_cell_id == 1U &&
             published->source == test_runtime_source() &&
             published->dependencies.size() == 1U &&
             published->dependencies.front().cell_id == 901U,
         "kernel should retain the rich capture with runtime provider identity");
  expect(kernel.dynamic_dependencies(1) ==
             std::vector<BindingKey>{{77, "legacy"}},
         "legacy dynamic dependencies must remain a separate snapshot");

  const auto foreign = kernel.run_cell(1, [](const auto &) {
    CellExecutionResult result;
    result.ok = true;
    result.writes.push_back(
        KernelWrite{{1, "value"}, Value::integer(9), {}});
    result.runtime_dependencies = rich_capture(
        1, 999, "foreign", RuntimeWatchStreamIdentity{18, 3});
    return result;
  });
  expect(!foreign.ok &&
             foreign.error.find("another world") != std::string::npos &&
             kernel.runtime_dependencies(1)->source == test_runtime_source() &&
             kernel.runtime_dependencies(1)->dependencies.front().cell_id ==
                 901U &&
             kernel.read_slot({1, "value"})->as_integer() == 1,
         "foreign capture must preserve both slot and dependency namespace");

  const auto absent = kernel.run_cell(1, [](const auto &) {
    CellExecutionResult result;
    result.ok = true;
    result.writes.push_back(
        KernelWrite{{1, "value"}, Value::integer(2), {}});
    result.dynamic_dependencies.push_back({88, "next"});
    return result;
  });
  expect(absent.ok && !absent.runtime_dependencies.has_value() &&
             kernel.runtime_dependencies(1).has_value() &&
             kernel.runtime_dependencies(1)->dependencies.front().cell_id == 901U,
         "an executor without capture must preserve the rich snapshot");

  const auto failed = kernel.run_cell(1, [](const auto &) {
    CellExecutionResult result;
    result.ok = false;
    result.error = "failed";
    result.runtime_dependencies = rich_capture(1, 902, "replacement");
    result.writes.push_back(
        KernelWrite{{1, "value"}, Value::integer(3), {}});
    return result;
  });
  expect(!failed.ok && kernel.runtime_dependencies(1)->dependencies.front().cell_id ==
                              901U,
         "a fault must preserve the previous rich snapshot");

  const auto thrown = kernel.run_cell(1, [](const auto &) -> CellExecutionResult {
    throw std::runtime_error("executor threw");
  });
  expect(!thrown.ok &&
             kernel.runtime_dependencies(1)->dependencies.front().cell_id == 901U,
         "an executor exception must preserve the previous rich snapshot");

  const auto invalid = kernel.run_cell(1, [](const auto &) {
    CellExecutionResult result;
    result.ok = true;
    result.runtime_dependencies = rich_capture(1, 902, "replacement");
    return result;
  });
  expect(!invalid.ok &&
             kernel.runtime_dependencies(1)->dependencies.front().cell_id == 901U,
         "incomplete output must preserve the previous rich snapshot");

  const auto mismatched = kernel.run_cell(1, [](const auto &) {
    CellExecutionResult result;
    result.ok = true;
    result.writes.push_back(
        KernelWrite{{1, "value"}, Value::integer(4), {}});
    result.runtime_dependencies = rich_capture(2, 903, "wrong-consumer");
    return result;
  });
  expect(!mismatched.ok &&
             mismatched.error.find("dependency capture") != std::string::npos &&
             kernel.runtime_dependencies(1)->dependencies.front().cell_id == 901U,
         "capture for another notebook cell must be rejected atomically");

  RuntimeDependencySet invalid_source;
  invalid_source.notebook_cell_id = 1;
  invalid_source.source = test_runtime_source();
  RuntimeDependency invalid_ivar;
  invalid_ivar.kind = RuntimeDependencyKind::Ivar;
  invalid_ivar.object_id = 77;
  invalid_source.dependencies.push_back(invalid_ivar);
  const auto invalid_identity = kernel.run_cell(1, [invalid_source](const auto &) {
    CellExecutionResult result;
    result.ok = true;
    result.writes.push_back(
        KernelWrite{{1, "value"}, Value::integer(4), {}});
    result.runtime_dependencies = invalid_source;
    return result;
  });
  expect(!invalid_identity.ok &&
             invalid_identity.error.find("invalid source identity") !=
                 std::string::npos &&
             kernel.runtime_dependencies(1)->dependencies.front().cell_id ==
                 901U,
         "malformed runtime source identity must not replace the snapshot");

  RuntimeDependencySet malformed;
  const auto zero_id = kernel.run_cell(1, [malformed](const auto &) {
    CellExecutionResult result;
    result.ok = true;
    result.writes.push_back(
        KernelWrite{{1, "value"}, Value::integer(4), {}});
    result.runtime_dependencies = malformed;
    return result;
  });
  expect(!zero_id.ok &&
             zero_id.error.find("dependency capture") != std::string::npos &&
             kernel.runtime_dependencies(1)->dependencies.front().cell_id == 901U,
         "an engaged capture must identify its notebook consumer cell");

  RuntimeDependencySet empty;
  empty.notebook_cell_id = 1;
  empty.source = test_runtime_source();
  const auto cleared = kernel.run_cell(1, [empty](const auto &) {
    CellExecutionResult result;
    result.ok = true;
    result.writes.push_back(
        KernelWrite{{1, "value"}, Value::integer(5), {}});
    result.runtime_dependencies = empty;
    return result;
  });
  expect(cleared.ok && kernel.runtime_dependencies(1).has_value() &&
             kernel.runtime_dependencies(1)->notebook_cell_id == 1U &&
             kernel.runtime_dependencies(1)->dependencies.empty(),
         "successful empty capture must clear the rich snapshot");

  kernel.update_cells({}, 1);
  expect(!kernel.runtime_dependencies(1).has_value(),
         "removed cells must not leave rich captures behind");
}

void test_executor_may_snapshot_kernel_gc_roots() {
  NotebookKernel kernel({cell(1, "value = 1\n")});
  const auto result = kernel.run_cell(1, [&kernel](const auto &) {
    expect(kernel.gc_roots().empty(),
           "executor should re-enter root snapshot without deadlocking");
    CellExecutionResult execution;
    execution.ok = true;
    execution.writes.push_back(
        KernelWrite{{1, "value"}, Value::integer(1), {}});
    return execution;
  });
  expect(result.ok && kernel.gc_roots().size() == 1U,
         "successful re-entrant executor should publish normally");
}

} // namespace

int main() {
  test_chain_uses_published_inputs_and_equal_write_is_quiet();
  test_failure_and_invalid_output_publish_nothing();
  test_executor_exception_publishes_nothing();
  test_graph_transition_stales_removed_output_and_old_consumers();
  test_removed_cell_stales_its_published_provider();
  test_removed_cell_drops_dynamic_dependencies();
  test_unresolved_input_is_rejected_before_executor();
  test_ambient_input_runs_without_a_notebook_slot();
  test_notebook_provider_shadows_ambient_and_stale_blocks();
  test_provider_removal_falls_back_to_ambient();
  test_reset_preserves_ambient_names();
  test_dynamic_dependencies_replace_only_after_success();
  test_rich_dependencies_are_transactional_and_separate();
  test_runtime_event_planner_normalizes_captures_and_matches_revisions();
  test_runtime_event_reverse_index_replaces_empty_and_removes_consumers();
  test_runtime_event_batch_uses_static_closure_and_manual_policy();
  test_runtime_event_root_respects_preexisting_stale_input();
  test_runtime_stream_resync_preserves_automatic_manual_policy();
  test_runtime_event_source_namespace_rejects_foreign_world();
  test_runtime_event_batch_rejects_malformed_ack_boundaries();
  test_runtime_source_reset_drops_old_watch_values_until_resync();
  test_prepared_cell_update_cancels_or_commits_as_one_generation();
  test_executor_may_snapshot_kernel_gc_roots();
  std::cout << "notebook_kernel_tests ok\n";
  return 0;
}
