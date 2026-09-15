#include "tools/iamber/session.h"

#include <cstdlib>
#include <chrono>
#include <iostream>
#include <string>
#include <vector>

namespace {

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << "\n";
    std::exit(1);
  }
}

std::string
output_text(const std::vector<amber::runtime::RuntimeTextOutputEvent> &events) {
  std::string out;
  for (const amber::runtime::RuntimeTextOutputEvent &event : events) {
    out += event.text;
  }
  return out;
}

Cell make_cell(std::string source) {
  Cell cell;
  cell.id = amber::notebook::allocate_cell_id();
  cell.source = std::move(source);
  return cell;
}

void test_string_tags_and_system() {
  const std::vector<std::string> sources = {
      "string_tag macro def quote_kind(t as Ast.StringTemplate):\n"
      "  return Ast.lift(t.quote_kind)\n"
      "quote_kind'--eval \"quoted\"'\n",
      "from system import cmd\ncmd'printf \"%s\" #{\"single\"}'.output()\n",
      "import system\nsystem.command(\"/usr/bin/printf\", \"single\").output()\n"};
  for (const auto &source : sources) {
    std::vector<Cell> cells{make_cell(source)};
    const auto view = evaluate_prefix(cells, 0);
    expect(view.ok, "string tags and system should work in notebook cells");
    expect(view.result == "\"single\"", "tag result should preserve string contents");
  }
}

void test_system_in_persistent_notebook() {
  const std::vector<std::string> sources = {
      "import system\nsystem.cmd\"/usr/bin/printf single\".output\n",
      "from system import cmd\ncmd'printf \"%s\" #{\"single\"}'.output\n",
      "import system as sys\nsys.cmd'printf single'.output\n",
      "import system as sys\nsys.command(\"/usr/bin/printf\", \"single\").output\n"};
  for (const auto &source : sources) {
    Session session;
    session.runtime_capability_grants = {
        amber::capability::make_capability("process.spawn", "/usr/bin/printf")};
    session.cells.push_back(make_cell(source));
    evaluate_from(&session, 0, true, false, false);
    expect(session.cells[0].ok,
           "system should work in persistent notebook: " + session.cells[0].error);
    expect(session.cells[0].result == "\"single\"",
           "persistent system tag should return process stdout");
    session.cells[0].source += "# edit and rebuild notebook image\n";
    session.cells[0].dirty = true;
    evaluate_from(&session, 0, false, false, false);
    expect(session.cells[0].ok && session.cells[0].result == "\"single\"",
           "system should keep working after a notebook image rebuild: " +
               session.cells[0].error);
  }
  Session denied;
  denied.cells.push_back(make_cell(sources.front()));
  evaluate_from(&denied, 0, true, false, false);
  expect(!denied.cells[0].ok && denied.cells[0].error.find("CapabilityError") != std::string::npos,
         "notebook process creation must still require an explicit grant");

  Session watch;
  watch.runtime_capability_grants = {
      amber::capability::make_capability("process.spawn", "/usr/bin/printf")};
  watch.cells.push_back(make_cell("payload = \"before\"\n"));
  watch.cells.push_back(make_cell(
      "import system\nsystem.cmd'printf \"%s\" #{payload}'.output\n"));
  evaluate_from(&watch, 0, true, false, false);
  expect(watch.cells[1].ok && watch.cells[1].result == "\"before\"",
         "a command interpolant should read the preceding notebook cell");
  watch.cells[0].source = "payload = \"after\"\n";
  watch.cells[0].dirty = true;
  evaluate_from(&watch, 0, false, false, false);
  expect(watch.cells[1].ok && watch.cells[1].result == "\"after\"",
         "a changed interpolant should reevaluate the dependent command");
}

void test_threaded_system_in_persistent_notebook() {
  Session session;
  session.runtime_capability_grants = {
      amber::capability::make_capability("process.spawn", "/usr/bin/printf")};
  session.cells.push_back(make_cell(
      "import system\n"
      "cmd = system.cmd\"/usr/bin/printf output\"\n"
      "24.times.threaded(4).map: cmd.output .group: $it .transform_values: $it.size\n"
      "print $_\n"));
  evaluate_from(&session, 0, true, false, false);
  expect(session.cells[0].ok,
         "threaded system should run in a persistent cell: " + session.cells[0].error);
  expect(output_text(session.cells[0].output_events).find("24") != std::string::npos,
         "threaded command outputs should be grouped and counted");

  Session separate;
  separate.runtime_capability_grants = session.runtime_capability_grants;
  separate.cells.push_back(make_cell("from system import cmd\ncommand = cmd'printf captured'\n"));
  separate.cells.push_back(make_cell(
      "[1, 2, 3].threaded(2).map |index|:\n"
      "  chunks = [index].map |inner|: command.output\n"
      "  chunks.join(\"\")\n"));
  evaluate_from(&separate, 0, true, false, false);
  expect(separate.cells[1].ok && separate.cells[1].result ==
             "[\"captured\", \"captured\", \"captured\"]",
         "nested threaded blocks should capture preceding notebook inputs: " +
             separate.cells[1].error);
  separate.cells[0].source = "from system import cmd\ncommand = cmd'printf updated'\n";
  separate.cells[0].dirty = true;
  evaluate_from(&separate, 0, false, false, false);
  expect(separate.cells[0].ok,
         "command provider should update on notebook rebuild: " + separate.cells[0].error);
  expect(separate.cells[1].ok && separate.cells[1].result ==
             "[\"updated\", \"updated\", \"updated\"]",
         "threaded block captures should update on notebook rebuild: " + separate.cells[1].error);
}

void test_prefix_eval_filters_prior_cell_output() {
  std::vector<Cell> cells;
  cells.push_back(make_cell("logger = io.Logger.new\n"
                            "logger.info(\"first-cell\")\n"
                            "1\n"));
  cells.push_back(make_cell("logger\n"));

  const EvalView first = evaluate_prefix(cells, 0);
  expect(first.ok, "first cell should evaluate");
  expect(output_text(first.output_events).find("first-cell") !=
             std::string::npos,
         "first cell should keep its own logger output");

  const EvalView second = evaluate_prefix(cells, 1);
  expect(second.ok, "second cell should evaluate");
  expect(output_text(second.output_events).find("first-cell") ==
             std::string::npos,
         "second cell should not inherit prior cell logger output");
}

void test_prefix_eval_keeps_current_cell_output() {
  std::vector<Cell> cells;
  cells.push_back(make_cell("x = 1\n"));
  cells.push_back(make_cell("logger = io.Logger.new\n"
                            "logger.warn(\"second-cell\")\n"
                            "Array\n"));

  const EvalView second = evaluate_prefix(cells, 1);
  expect(second.ok, "second cell with logger should evaluate");
  expect(output_text(second.output_events).find("second-cell") !=
             std::string::npos,
         "second cell should keep its own logger output");
}

void test_independent_cell_uses_isolated_eval() {
  std::vector<Cell> cells;
  cells.push_back(make_cell("1 / 0\n"));
  cells.push_back(make_cell("Array\n"));

  const EvalView second = evaluate_prefix(cells, 1);
  expect(second.ok, "independent second cell should not execute first cell");
}

void test_compound_assignment_eval() {
  std::vector<Cell> cells;
  cells.push_back(make_cell("x = 2\n"
                            "x += 1\n"));

  const EvalView view = evaluate_prefix(cells, 0);
  expect(view.ok, "compound assignment cell should evaluate");
  expect(view.result == "3", "compound assignment should return updated value");
}

void test_range_literal_uses_native_prelude() {
  std::vector<Cell> cells;
  cells.push_back(make_cell("(0..5)\n"));

  EvalView view = evaluate_prefix(cells, 0);
  expect(view.ok, "range literal should evaluate without a local Range class");
  expect(view.result == "<instance Range>",
         "range literal should use the native Range prelude");

  cells[0] = make_cell("(0..5).array\n");
  view = evaluate_prefix(cells, 0);
  expect(view.ok, "native Range literal should materialize");
  expect(view.result == "[0, 1, 2, 3, 4, 5]",
         "range literal should materialize through the native prelude");
}

void test_cell_can_read_binding_initialized_earlier_in_cell() {
  std::vector<Cell> cells;
  cells.push_back(make_cell("h = {}\n"
                            "h[?:key]\n"));

  expect(!cyclic_watch_error_for_cell(cells, 0).has_value(),
         "same-cell read after initialization should not be cyclic watch");

  const EvalView view = evaluate_prefix(cells, 0);
  expect(view.ok, "same-cell optional map lookup should evaluate");
  expect(view.result == "null", "missing optional map key should return null");
}

void test_versioned_write_reads_previous_provider() {
  std::vector<Cell> cells;
  cells.push_back(make_cell("x = 2\n"));
  cells.push_back(make_cell("x = 6 + x\n"));
  cells.push_back(make_cell("x += 1\n"));

  expect(!cyclic_watch_error_for_cell(cells, 1).has_value(),
         "x = x + 1 should consume the preceding x version");
  expect(!cyclic_watch_error_for_cell(cells, 2).has_value(),
         "compound assignment should consume the preceding x version");
}

void test_evaluate_from_runs_only_dependent_watch_cells() {
  Session session;
  session.cells.push_back(make_cell("x = 2\n"));
  session.cells.push_back(make_cell("x = 6 + x\n"));
  session.cells.push_back(make_cell("x += 1\n"));
  session.cells.push_back(make_cell("99\n"));

  evaluate_from(&session, 0, false, false, false);
  expect(session.cells[0].ok, "changed source cell should evaluate");
  expect(session.cells[1].ok && session.cells[1].result == "8",
         "first dependent should evaluate against the preceding x");
  expect(session.cells[2].ok && session.cells[2].result == "9",
         "transitive dependent should evaluate exactly once in graph order");
  expect(!session.cells[3].ok && session.cells[3].dirty,
         "unrelated later cell should not be evaluated");
}

void test_watch_cell_waits_for_stale_manual_dependency() {
  Session session;
  session.cells.push_back(make_cell("x = 2\n"));
  session.cells.push_back(make_cell("y = x + 1\n"));
  session.cells.back().watch = false;
  session.cells.push_back(make_cell("y * 2\n"));

  evaluate_from(&session, 0, false, false, false);
  expect(session.cells[0].ok, "changed source cell should evaluate");
  expect(session.cells[1].dirty && !session.cells[1].ok,
         "manual dependent should remain stale");
  expect(session.cells[2].dirty && !session.cells[2].ok &&
             session.cells[2].error.find("stale manual dependency") !=
                 std::string::npos,
         "watch dependent should be blocked behind a stale manual cell");
}

void test_removed_output_invalidates_old_consumers_and_blocks_downstream() {
  Session session;
  session.cells.push_back(make_cell("x = 2\n"));
  session.cells.push_back(make_cell("middle = x + 1\n"));
  session.cells.push_back(make_cell("answer = middle * 2\n"));
  session.cells.push_back(make_cell("99\n"));

  evaluate_from(&session, 0, true, false, false);
  expect(session.cells[2].ok && session.cells[2].result == "6",
         "initial dependent chain should evaluate");
  expect(session.cells[3].ok && session.cells[3].result == "99",
         "initial unrelated cell should evaluate during force-all");

  session.cells[0].source = "renamed = 2\n";
  session.cells[0].dirty = true;
  evaluate_from(&session, 0, false, false, false);

  expect(!session.cells[1].ok,
         "a former consumer should rerun after its provider disappears");
  expect(session.cells[2].dirty && !session.cells[2].ok &&
             session.cells[2].error.find("failed dependency") !=
                 std::string::npos,
         "downstream watch cell should wait behind the failed former consumer");
  expect(session.cells[3].ok && session.cells[3].result == "99",
         "unrelated cell should retain its previous result");
}

void test_persistent_backend_rebuilds_image_without_losing_heap_values() {
  Session session;
  session.cells.push_back(make_cell("x = [1, 2]\n"));
  session.cells.push_back(make_cell("y = x\n"));

  evaluate_from(&session, 0, true, false, false);
  expect(session.cells[0].ok && session.cells[1].ok,
         "persistent backend should evaluate the initial image");
  expect(session.cells[1].result == "[1, 2]",
         "initial notebook output should be visible");

  // Changing a consumer forces a new immutable image, while the existing
  // RuntimeWorld keeps its heap and the kernel keeps x as a root. This edit
  // must not copy a Value into a newly-created world.
  session.cells[1].source = "y = x\n# image generation two\n";
  session.cells[1].dirty = true;
  evaluate_from(&session, 1, false, false, false);
  expect(session.cells[1].ok && session.cells[1].result == "[1, 2]",
         "image install should preserve a heap provider across edits");
}

void test_verified_initial_image_rejects_malformed_notebook_metadata() {
  const CompileResult ordinary = compile_source_text("1\n", "<iamber-test>");
  expect(ordinary.ok, "ordinary source should compile for image-boundary test");

  // A notebook-only header without its NBMD sidecar is malformed.  Exercise
  // the same public seam used by build_backend before RuntimeWorld creation.
  amber::bytecode::BcModule malformed = ordinary.module;
  malformed.file_flags |= amber::bytecode::kFileFlagNotebookOnly;
  std::string error;
  const std::shared_ptr<const amber::bytecode::BcModule> verified =
      verified_notebook_image(malformed, &error);
  expect(verified == nullptr,
         "initial image boundary should reject missing notebook metadata");
  expect(error.find("BC1421") != std::string::npos,
         "initial image rejection should preserve verifier diagnostic");
}

void test_persistent_backend_keeps_older_literal_name_ids() {
  Session session;
  session.cells.push_back(make_cell("x = \"old\"\n"));
  session.cells.push_back(make_cell("y = \"keep\"\n"));
  session.cells.push_back(make_cell("z = y\n"));
  evaluate_from(&session, 0, true, false, false);
  expect(session.cells[2].ok && session.cells[2].result == "\"keep\"",
         "literal provider should initialize before image edit");

  // Add names to an earlier cell. The seeded replacement image must append
  // them after the active table, preserving y's old StringValue id.
  session.cells[0].source = "x = \"new\"\nq = \"another\"\n";
  session.cells[0].dirty = true;
  evaluate_from(&session, 0, false, false, false);

  session.cells[2].source = "z = y + \"!\"\n";
  session.cells[2].dirty = true;
  evaluate_from(&session, 2, false, false, false);
  expect(session.cells[2].ok && session.cells[2].result == "\"keep!\"",
         "older literal slot should retain its name after image install");
}

void test_failed_cell_does_not_publish_partial_writes() {
  Session session;
  session.cells.push_back(make_cell("x = 1\n"));
  session.cells.push_back(make_cell("y = x + 1\n"));

  evaluate_from(&session, 0, true, false, false);
  expect(session.cells[1].ok && session.cells[1].result == "2",
         "atomicity test should initialize the consumer");

  // The VM stores x before the later fault. NotebookVmCellExecutor and
  // NotebookKernel must discard that batch, leaving the old x available to
  // the consumer only after the provider is repaired.
  session.cells[0].source = "x = 9\n1 / 0\n";
  session.cells[0].dirty = true;
  evaluate_from(&session, 0, false, false, false);
  expect(!session.cells[0].ok && session.cells[0].error.find("ZeroDivision") !=
                              std::string::npos,
         "failed provider should expose its runtime error");
  expect(session.cells[1].dirty && !session.cells[1].ok,
         "consumer should be blocked behind the failed provider");

  session.cells[0].source = "x = 9\n";
  session.cells[0].dirty = true;
  evaluate_from(&session, 0, false, false, false);
  expect(session.cells[0].ok && session.cells[1].ok &&
             session.cells[1].result == "10",
         "repair should publish the complete provider batch and rerun consumer");
}

void test_compile_failure_blocks_old_transitive_dependents() {
  Session session;
  session.cells.push_back(make_cell("x = 1\n"));
  session.cells.push_back(make_cell("y = x + 1\n"));
  session.cells.push_back(make_cell("z = y + 1\n"));

  evaluate_from(&session, 0, true, false, false);
  expect(session.cells[0].ok && session.cells[1].ok && session.cells[2].ok,
         "transitive failure barrier test should initialize the chain");
  expect(session.cells[2].result == "3",
         "transitive failure barrier test should initialize z");

  // A frontend error prevents the candidate image from reaching
  // NotebookKernel::update_cells(). The old graph must nevertheless make
  // every historical consumer unusable until x is repaired.
  session.cells[0].source = "x =\n";
  session.cells[0].dirty = true;
  evaluate_from(&session, 0, false, false, false);

  expect(!session.cells[0].ok && !session.cells[1].ok &&
             !session.cells[2].ok,
         "compile failure should invalidate provider and all old consumers");
  expect(session.cells[1].dirty && session.cells[2].dirty,
         "old transitive consumers should remain dirty behind the barrier");
  expect(session.cells[1].error.find("failed dependency") !=
             std::string::npos,
         "direct old consumer should expose failed dependency barrier");
  expect(session.cells[2].error.find("failed dependency") !=
             std::string::npos,
         "transitive old consumer should expose failed dependency barrier");

  session.cells[0].source = "x = 4\n";
  session.cells[0].dirty = true;
  evaluate_from(&session, 0, false, false, false);
  expect(session.cells[0].ok && session.cells[1].ok && session.cells[2].ok &&
             session.cells[2].result == "6",
         "repair should clear the failure barrier and recompute the chain");
}

void test_failed_rebuild_preserves_published_dependency_snapshot() {
  Session session;
  session.cells.push_back(make_cell("x = 1\n"));
  evaluate_from(&session, 0, true, false, false);
  expect(session.cells[0].ok && session.dependency_snapshot.size() == 1U &&
             session.dependency_snapshot[0].source == "x = 1\n",
         "successful rebuild should publish its dependency snapshot");

  session.cells[0].source = "x =\n";
  session.cells[0].dirty = true;
  evaluate_from(&session, 0, false, false, false);
  expect(!session.cells[0].ok && session.dependency_snapshot.size() == 1U &&
             session.dependency_snapshot[0].source == "x = 1\n",
         "failed candidate must preserve the active backend snapshot");
}

void test_runtime_event_pump_acknowledges_registration_batch_once() {
  Session session;
  session.cells.push_back(make_cell("x = 1\nKernel.watch(x)\nx\n"));
  evaluate_from(&session, 0, true, false, false);
  expect(session.cells[0].ok,
         "runtime event pump test should initialize its persistent backend: " +
             session.cells[0].error);
  const RuntimeEventPumpResult first =
      pump_runtime_events_detailed(&session, false);
  expect(first.disposition == RuntimeEventPumpDisposition::Acknowledged &&
             first.handled() && first.acknowledged_batches >= 1U &&
             first.observed_events >= 1U,
         "the first typed pump should acknowledge evaluation events");
  const RuntimeEventPumpResult repeated =
      pump_runtime_events_detailed(&session, false);
  expect(repeated.disposition == RuntimeEventPumpDisposition::NoWork &&
             !repeated.handled() && repeated.acknowledged_batches == 0U,
         "an acknowledged typed batch must not be replayed");
  expect(!pump_runtime_events(nullptr, false) &&
             pump_runtime_events_detailed(nullptr, false).disposition ==
                 RuntimeEventPumpDisposition::Unavailable,
         "the compatibility and typed pumps should reject no session");
}

void test_runtime_event_pump_defers_edits_and_rearms_after_repair() {
  using namespace std::chrono_literals;
  Session session;
  session.runtime_watch_event_capacity = 1U;
  session.cells.push_back(
      make_cell("x = 1\nKernel.watch(x)\nx = 2\nx = 3\nx\n"));
  const SessionActivityWaiter waiter = session.activity_waiter();

  evaluate_from(&session, 0, true, false, false);
  expect(session.cells[0].ok && session.cells[0].result == "3" &&
             !session.cells[0].dirty,
         "deferred-edit fixture should evaluate its hot watched write");

  session.cells[0].source = "x = 42\nx\n";
  session.cells[0].dirty = true;
  session.status = "edit pending sentinel";
  const std::string edited_source = session.cells[0].source;
  const std::string prior_result = session.cells[0].result;
  const std::string prior_status = session.status;

  const RuntimeEventPumpResult deferred =
      pump_runtime_events_detailed(&session, false);
  expect(deferred.disposition == RuntimeEventPumpDisposition::DeferredByEdits &&
             !deferred.handled() && deferred.acknowledged_batches == 0U &&
             session.cells[0].dirty &&
             session.cells[0].source == edited_source &&
             session.cells[0].result == prior_result &&
             session.status == prior_status,
         "runtime pump should defer edited sources without mutating UI state");

  // Consume the pre-edit activity hint before evaluation. The repaired source
  // emits no watch events, so evaluate_from's owner-scope rearm is observable
  // only if this older hint has been consumed first.
  const SessionActivityResult prior_hint = waiter.wait(0ms);
  expect(prior_hint.runtime_ready && !prior_hint.shutdown &&
             !prior_hint.timed_out,
         "deferred fixture should retain its pre-edit activity hint");

  evaluate_from(&session, 0, false, false, false);
  expect(session.cells[0].ok && session.cells[0].result == "42" &&
             !session.cells[0].dirty,
         "explicit repaired source should evaluate cleanly");
  const SessionActivityResult rearmed = waiter.wait(0ms);
  expect(rearmed.runtime_ready && !rearmed.shutdown && !rearmed.timed_out,
         "an event-free explicit evaluation should rearm activity");

  const RuntimeEventPumpResult resumed =
      pump_runtime_events_detailed(&session, false);
  expect(resumed.disposition == RuntimeEventPumpDisposition::Acknowledged &&
             resumed.handled() && resumed.resynchronized_batches >= 1U &&
             !resumed.cursor_retained && session.cells[0].ok &&
             session.cells[0].result == "42" && !session.cells[0].dirty,
         "next pump should resynchronize the retained old cursor on the new "
         "image");
}

void test_session_activity_tracks_runtime_without_acknowledging() {
  using namespace std::chrono_literals;
  Session session;
  const auto waiter = session.activity_waiter();
  expect(waiter.wait(0ms).timed_out,
         "a new session activity mailbox should be idle");
  session.cells.push_back(make_cell("x = 1\nKernel.watch(x)\nx\n"));
  evaluate_from(&session, 0, true, false, false);
  expect(session.cells[0].ok, "activity fixture should evaluate");
  const auto hint = waiter.wait(0ms);
  expect(hint.runtime_ready && !hint.shutdown && !hint.timed_out,
         "real VM watch publication should signal the session mailbox");
  expect(waiter.wait(0ms).timed_out,
         "one wait should consume the coalesced hint only");
  expect(pump_runtime_events(&session, false),
         "consuming an activity hint must not acknowledge the watch cursor");
  expect(!pump_runtime_events(&session, false),
         "the owner pump should acknowledge registration exactly once");

  session.cells[0].source += "# replacement image\n";
  session.cells[0].dirty = true;
  evaluate_from(&session, 0, false, false, false);
  expect(session.cells[0].ok && waiter.wait(0ms).runtime_ready,
         "notebook image replacement must retain the activity connection");
  expect(pump_runtime_events(&session, false),
         "replacement image events still require owner acknowledgement");
}

void test_session_activity_lifetime_and_moves() {
  using namespace std::chrono_literals;
  SessionActivityWaiter retained;
  SessionActivityNotifier late;
  {
    Session source;
    retained = source.activity_waiter();
    late = source.activity_notifier();
    source.cells.push_back(make_cell("x = 1\nKernel.watch(x)\nx\n"));
    evaluate_from(&source, 0, true, false, false);
    (void)retained.wait(0ms);
    Session destination(std::move(source));
    expect(source.activity_waiter().wait(0ms).shutdown,
           "moved-from session must not retain the mailbox owner");
    evaluate_from(&destination, 0, true, false, false);
    expect(retained.wait(0ms).runtime_ready,
           "runtime hook and old waiter should follow a moved Session");

    Session replacement;
    const auto replaced_waiter = replacement.activity_waiter();
    const auto replaced_notifier = replacement.activity_notifier();
    replacement = std::move(destination);
    expect(replaced_waiter.wait(0ms).shutdown &&
               !replaced_notifier.notify_runtime(),
           "Session move assignment must close the discarded mailbox");
    expect(late.notify_input() && retained.wait(0ms).input_ready,
           "source handles should follow Session move assignment");
  }
  expect(retained.wait(0ms).shutdown && !late.notify_runtime(),
         "Session destruction must close retained handles before teardown");
}

void test_runtime_event_pump_retries_incomplete_overflow_resync() {
  Session session;
  session.runtime_watch_event_capacity = 2U;
  session.cells.push_back(
      make_cell("x = 1\nKernel.watch(x)\nx = 2\nx = 3\nx\n"));
  session.cells.push_back(make_cell("y = 1 / 0\n"));
  evaluate_from(&session, 0, true, false, false);
  expect(session.cells[0].ok && !session.cells[1].ok,
         "overflow retry test should retain a runnable backend with one "
         "failing watch cell");

  const RuntimeEventPumpResult incomplete =
      pump_runtime_events_detailed(&session, false);
  expect(incomplete.disposition ==
                 RuntimeEventPumpDisposition::RetryRequired &&
             incomplete.retry_required() && !incomplete.handled() &&
             incomplete.acknowledged_batches == 0U &&
             incomplete.resynchronized_batches == 0U &&
             incomplete.needs_recovery() && !incomplete.execution_complete &&
             session.status.find("resync is incomplete") != std::string::npos,
         "a failed conservative resync must retain its unacknowledged cursor");

  session.cells[1].source = "y = 2\n";
  session.cells[1].dirty = true;
  evaluate_from(&session, 1, false, false, false);
  expect(session.cells[1].ok,
         "repair should make the previously failing resync root runnable");
  const RuntimeEventPumpResult recovered =
      pump_runtime_events_detailed(&session, false);
  expect(recovered.disposition == RuntimeEventPumpDisposition::Acknowledged &&
             recovered.handled() && recovered.acknowledged_batches >= 1U &&
             recovered.resynchronized_batches >= 1U &&
             recovered.execution_complete && !recovered.cursor_retained,
         "the retained overflow cursor should be recoverable after repair");
}

void test_runtime_event_pump_coalesces_reentry_and_reports_drain_cap() {
  Session session;
  // The first resync reruns the same watched write and capacity one overflows
  // again. This deliberately creates a hot source: the nested progress pump
  // must coalesce, while the outer owner remains bounded to eight rounds and
  // tells a native loop to schedule another drain.
  session.runtime_watch_event_capacity = 1U;
  session.cells.push_back(
      make_cell("x = 1\nKernel.watch(x)\nx = 2\nx\n"));
  evaluate_from(&session, 0, true, false, false);
  expect(session.cells[0].ok,
         "reentrant pump test should initialize its watched cell");

  bool progress_called = false;
  RuntimeEventPumpDisposition nested_disposition =
      RuntimeEventPumpDisposition::Unavailable;
  const RuntimeEventPumpResult outer = pump_runtime_events_detailed(
      &session, true,
      [&progress_called, &nested_disposition](Session *running,
                                               std::size_t) {
        if (progress_called) {
          return;
        }
        progress_called = true;
        nested_disposition =
            pump_runtime_events_detailed(running, false).disposition;
      });
  expect(progress_called &&
             nested_disposition == RuntimeEventPumpDisposition::Busy &&
             outer.disposition == RuntimeEventPumpDisposition::Acknowledged &&
             outer.handled() && outer.rounds == 8U &&
             outer.drain_limit_reached,
         "a recursive owner pump should coalesce as Busy and report its "
         "bounded hot-source drain cap");
  expect(session.activity_waiter().wait(std::chrono::milliseconds(0))
             .runtime_ready,
         "a fairness-capped pump must leave a continuation activity hint");
}

} // namespace

int main() {
  test_string_tags_and_system();
  test_system_in_persistent_notebook();
  test_threaded_system_in_persistent_notebook();
  test_prefix_eval_filters_prior_cell_output();
  test_prefix_eval_keeps_current_cell_output();
  test_independent_cell_uses_isolated_eval();
  test_compound_assignment_eval();
  test_range_literal_uses_native_prelude();
  test_cell_can_read_binding_initialized_earlier_in_cell();
  test_versioned_write_reads_previous_provider();
  test_evaluate_from_runs_only_dependent_watch_cells();
  test_watch_cell_waits_for_stale_manual_dependency();
  test_removed_output_invalidates_old_consumers_and_blocks_downstream();
  test_persistent_backend_rebuilds_image_without_losing_heap_values();
  test_verified_initial_image_rejects_malformed_notebook_metadata();
  test_persistent_backend_keeps_older_literal_name_ids();
  test_failed_cell_does_not_publish_partial_writes();
  test_compile_failure_blocks_old_transitive_dependents();
  test_failed_rebuild_preserves_published_dependency_snapshot();
  test_runtime_event_pump_acknowledges_registration_batch_once();
  test_runtime_event_pump_defers_edits_and_rearms_after_repair();
  test_session_activity_tracks_runtime_without_acknowledging();
  test_session_activity_lifetime_and_moves();
  test_runtime_event_pump_retries_incomplete_overflow_resync();
  test_runtime_event_pump_coalesces_reentry_and_reports_drain_cap();
  std::cout << "iamber_tests ok\n";
  return 0;
}
