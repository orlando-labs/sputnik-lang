#include "tools/iamber/session.h"
#include "runtime/context.h"
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <unistd.h>

using namespace amber::runtime;
static void require(bool ok, const std::string &message) {
  if (!ok) { std::cerr << "cancellation: " << message << '\n'; std::exit(1); }
}
static std::string output(const Cell &cell) {
  std::string text;
  for (const auto &event : cell.output_events) text += event.text;
  return text;
}
static void cancel(const std::string &body, const std::string &cleanup) {
  Session session;
  Cell cell; cell.id = amber::notebook::allocate_cell_id();
  cell.source = "import task\ntry:\n  try:\n" + body +
      "  ensure:\n    " + cleanup + "\nrescue:\n  print(\"RESCUED\")\n"
      "ensure:\n  print(\"OUTER\")\n";
  session.cells.push_back(cell);
  Cell later; later.id = amber::notebook::allocate_cell_id();
  later.source = "print(\"SHOULD NOT RUN\")\n";
  session.cells.push_back(later);
  auto token = std::make_shared<RuntimeRunState>();
  std::thread runner([&] {
    RuntimeRunCancellationScope scope(token);
    evaluate_from(&session, 0, true, false, false);
  });
  // The process-wide alarm below bounds a regression even for a no-yield loop.
  std::this_thread::sleep_for(std::chrono::milliseconds(250));
  token->request_cancel();
  runner.join();
  require(!session.cells[0].ok, "cancelled cell must fail");
  require(session.cells[0].error.find("CancelledError") != std::string::npos,
          session.cells[0].error + " / " + session.status);
  const auto text = output(session.cells[0]);
  require(text.find("INNER") != std::string::npos && text.find("OUTER") != std::string::npos,
          "nested ensure cleanup: " + text + " source: " + cell.source + " error: " + session.cells[0].error);
  require(text.find("RESCUED") == std::string::npos, "Stop cannot be swallowed by rescue");
  require(output(session.cells[1]).empty(), "no independent later cell after Stop");
  require(!runtime_run_cancel_requested(), "scope restores host thread state");
}
static void cancelled_publication() {
  using namespace amber::notebook;
  NotebookKernel kernel({CellSource{1, "x = 1\n", "<cancel-test>", CellMode::Watch}});
  const BindingKey key{1, "x"};
  auto token = std::make_shared<RuntimeRunState>();
  auto executor = [&](const std::vector<KernelInput> &) {
    CellExecutionResult result;
    result.ok = true;
    result.writes.push_back({key, Value::integer(42), std::nullopt});
    result.dynamic_dependencies.push_back({9, "observed"});
    token->request_cancel(); // Stop arrives after backend success, before commit.
    return result;
  };
  {
    RuntimeRunCancellationScope scope(token);
    const auto result = kernel.run_cell(1, executor);
    require(!result.ok && result.error.find("CancelledError") != std::string::npos,
            "late cancellation must reject successful executor output");
  }
  const auto slot = kernel.slot_state(key);
  require(!slot || !slot->initialized, "cancelled output remains unpublished");
  require(kernel.dynamic_dependencies(1).empty(), "cancelled dependencies remain unpublished");
}
static void amber_child_cancellation() {
  const auto compiled = compile_source_text(
      "import task\ntask.spawn:\n  try:\n    print(\"CHILD STARTED\")\n"
      "    while true:\n      x = 1 + 2\n  ensure:\n    print(\"CHILD CLEANED\")\n",
      "<child-cancel-test>");
  require(compiled.ok, compiled.error);
  RuntimeWorld world(compiled.module);
  auto token = std::make_shared<RuntimeRunState>();
  auto output = RuntimeTextWriter::buffer();
  std::shared_ptr<RuntimeTaskHandle> child;
  {
    RuntimeRunCancellationScope scope(token);
    RuntimeOutputScope output_scope(output, output);
    const auto result = world.execute(compiled.module.init.entry_code_id);
    require(result.ok() && result.value.is_task_handle(), "spawn an Amber CPU task");
    child = result.value.as_task_handle();
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (output->to_string().find("CHILD STARTED") == std::string::npos &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  require(output->to_string().find("CHILD STARTED") != std::string::npos, "child started");
  token->request_cancel();
  const auto result = child->wait(std::chrono::seconds(2));
  require(result.cancelled, "resumable Amber VM receives host run cancellation");
  require(output->to_string().find("CHILD CLEANED") != std::string::npos, "child VM ensure runs");
}
static void module_initialization_cancellation() {
  Session session;
  Cell cell; cell.id = amber::notebook::allocate_cell_id(); cell.source = "answer()\n";
  session.cells.push_back(cell);
  const std::vector<BundledModuleSource> modules{{"training", "modules/training.am",
      "package training\nexport answer\ndef answer(): 42\n"
      "try:\n  while true:\n    x = 1 + 2\nensure:\n  print(\"MODULE CLEANED\")\n", true}};
  auto token = std::make_shared<RuntimeRunState>();
  bool applied = true;
  std::thread runner([&] {
    RuntimeRunCancellationScope scope(token);
    applied = apply_bundled_environment(&session, modules, true, false);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(250));
  token->request_cancel();
  runner.join();
  require(!applied && !session.backend && session.environment_error.find("CancelledError") != std::string::npos,
          "cancelled module initialization must not install a partial environment: " + session.environment_error);
  std::string text;
  for (const auto &event : session.environment_output) text += event.text;
  require(text.find("MODULE CLEANED") != std::string::npos, "module initialization ensure runs");
}
int main() {
  alarm(30);
  cancel("    while true:\n      x = 1 + 2\n", "print(\"INNER\")");
  cancel("    task.sleep(60000)\n", "print(\"INNER\")");
  // A nested VM invoked from an ensure must not re-inject Stop during cleanup.
  cancel("    while true:\n      x = 1\n", "[1].each |n|: print(\"INNER\")");
  auto token = std::make_shared<RuntimeRunState>();
  RuntimeTaskModule tasks(1);
  RuntimeTaskHandle child;
  {
    RuntimeRunCancellationScope scope(token);
    child = tasks.spawn([] {
      while (!current_runtime_task_cancel_requested()) std::this_thread::yield();
      throw_if_runtime_task_cancelled();
      return Value::null();
    });
  }
  token->request_cancel();
  const auto result = child.wait(std::chrono::seconds(2));
  require(result.cancelled, "migrated child inherits the shared token");
  cancelled_publication();
  amber_child_cancellation();
  module_initialization_cancellation();
  alarm(0);
  std::cout << "notebook_cancellation_tests: ok\n";
}
