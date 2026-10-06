#include "tools/iamber/session.h"
#include "runtime/context.h"
#include "runtime/reactor.h"
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace amber::runtime;
using namespace std::chrono_literals;
namespace {
RuntimeTaskModule &native_task_runtime() {
  static RuntimeTaskModule tasks(1);
  return tasks;
}
// Exercise the actual generated-native task driver, not a duplicate fixture.
#include "runtime/system_native_task.inc"
}
static void require(bool ok, const std::string &message) {
  if (!ok) { std::cerr << "run tasks: " << message << '\n'; std::exit(1); }
}
template<class F> static void until(F predicate, const std::string &message) {
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (!predicate() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
  require(predicate(), message);
}
static void amber_sleep_cleanup() {
  auto compiled = compile_source_text(
      "import task\ntask.spawn:\n  try:\n    task.sleep(60000)\n"
      "  ensure:\n    print(\"before cleanup sleep\")\n    task.sleep(10)\n"
      "    print(\"after cleanup sleep\")\n", "<run-sleep>");
  require(compiled.ok, compiled.error);
  RuntimeWorld world(compiled.module);
  auto run = std::make_shared<RuntimeRunState>();
  auto output = RuntimeTextWriter::buffer();
  std::shared_ptr<RuntimeTaskHandle> child;
  {
    RuntimeRunCancellationScope scope(run);
    RuntimeOutputScope output_scope(output, output);
    const auto result = world.execute(compiled.module.init.entry_code_id);
    require(result.ok() && result.value.is_task_handle(), "spawn sleeping Amber child");
    child = result.value.as_task_handle();
  }
  until([&] { return child->state() == RuntimeTaskHandleState::Sleeping; }, "child reaches timer park");
  run->close_root();
  require(run->active_tasks() == 1 && !run->wait_for_idle(5ms), "root return is not completion");
  run->request_cancel();
  const bool drained = run->wait_for_idle(2s);
  require(drained, "Stop wakes and drains the sleeping VM; state=" +
          std::to_string(static_cast<int>(child->state())) + "; active=" +
          std::to_string(run->active_tasks()) + "; output=" + output->to_string() + "; error=" + child->failure().error_name + ":" + child->failure().message);
  require(child->wait(1s).cancelled, "cancelled task state");
  require(output->to_string() == "before cleanup sleep\nafter cleanup sleep\n", "ensure may itself suspend exactly once: " + output->to_string());
  run->request_cancel(); // Idempotent, even after task closure retirement.
  bool rejected = false;
  try { run->register_task(); } catch (const RuntimeTaskFailure &) { rejected = true; }
  require(rejected, "closed run rejects late detached work");
}
static void before_park_race() {
  RuntimeTaskModule tasks(1);
  auto run = std::make_shared<RuntimeRunState>();
  std::atomic<bool> entered{false}, release{false};
  RuntimeTaskHandle child;
  {
    RuntimeRunCancellationScope scope(run);
    child = tasks.spawn_resumable([&, first = true]() mutable {
      if (first) {
        first = false; entered.store(true);
        while (!release.load()) std::this_thread::yield();
        require(tasks.scheduler().park_current(60s), "publish a park after Stop");
        runtime_mark_task_parked();
        return Value::null();
      }
      throw_if_runtime_task_cancelled();
      return Value::null();
    });
  }
  until([&] { return entered.load(); }, "first dispatch");
  run->close_root(); run->request_cancel(); release.store(true);
  require(run->wait_for_idle(2s) && child.wait(1s).cancelled, "pre-park cancellation wake is not lost");
}
static void descendant_and_queue_ownership() {
  RuntimeTaskModule first(1), second(1);
  auto run = std::make_shared<RuntimeRunState>();
  std::atomic<bool> entered{false}, spawn_child{false}, child_started{false}, child_cleaned{false};
  {
    RuntimeRunCancellationScope scope(run);
    // Drop both handles. The run, not UI handles, owns completion accounting.
    first.spawn([&] {
      entered.store(true);
      while (!spawn_child.load()) std::this_thread::yield();
      second.spawn([&] {
        child_started.store(true);
        try { second.sleep(60s); }
        catch (const RuntimeTaskCancelled &) { child_cleaned.store(true); throw; }
        return Value::null();
      });
      return Value::null();
    });
  }
  until([&] { return entered.load(); }, "parent starts");
  run->close_root(); spawn_child.store(true);
  until([&] { return child_started.load(); }, "descendant may start while draining");
  require(!run->wait_for_idle(5ms), "unretained cross-scheduler descendant keeps run alive");
  run->request_cancel();
  require(run->wait_for_idle(2s) && child_cleaned.load(), "cross-scheduler descendant drained");

  auto queued_run = std::make_shared<RuntimeRunState>();
  std::atomic<bool> blocking{false}, finish{false}, queued_ran{false};
  {
    RuntimeRunCancellationScope scope(queued_run);
    first.spawn([&] { blocking.store(true); while (!finish.load()) std::this_thread::yield(); return Value::null(); });
    until([&] { return blocking.load(); }, "occupy only worker");
    first.spawn([&] { queued_ran.store(true); return Value::null(); });
  }
  queued_run->close_root(); queued_run->request_cancel();
  require(!queued_run->wait_for_idle(10ms), "uncooperative child is not falsely reported stopped");
  require(queued_run->active_tasks() == 1 && !queued_ran.load(), "queued closure retired without executing");
  finish.store(true);
  require(queued_run->wait_for_idle(2s), "native completion eventually drains run");
}
static void reactor_cancellation() {
  int pair[2]; require(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0, "socketpair");
  auto run = std::make_shared<RuntimeRunState>();
  std::atomic<bool> entered{false};
  ReactorOutcome outcome = ReactorOutcome::Ready;
  std::thread root([&] {
    RuntimeRunCancellationScope scope(run);
    entered.store(true);
    outcome = RuntimeReactor::instance().wait(pair[0], ReactorInterest::Read, std::nullopt, runtime_wait_cancel_flag());
  });
  until([&] { return entered.load(); }, "root IO enters wait");
  run->request_cancel(); root.join();
  require(outcome == ReactorOutcome::Cancelled, "host-thread IO has a run cancel flag and reactor kick");
  ::close(pair[0]); ::close(pair[1]);
}
static void amber_socket_cleanup() {
  auto compiled = compile_source_text(
      "import task\nimport net\nfrom io import ByteBuffer\n"
      "listener = net.tcp.listen(\"127.0.0.1\", 0)\n"
      "port = listener.local_endpoint().port()\n"
      "reader = task.spawn:\n"
      "  client = net.tcp.connect(\"127.0.0.1\", port)\n"
      "  try:\n    buf = ByteBuffer.new(4)\n    print(\"READING\")\n    client.read!(buf)\n"
      "  ensure:\n    client.close!()\n    task.sleep(5)\n    print(\"IO CLEANED\")\n"
      "server = listener.accept!()\nreader\n", "<run-io>");
  require(compiled.ok, compiled.error);
  RuntimeWorldOptions options;
  options.capability_grants = {amber::capability::make_capability("net.listen", "127.0.0.1:0"),
                               amber::capability::make_capability("net.connect", "*")};
  compiled.module.capabilities = options.capability_grants;
  RuntimeWorld world(compiled.module, options);
  auto run = std::make_shared<RuntimeRunState>();
  auto output = RuntimeTextWriter::buffer();
  std::shared_ptr<RuntimeTaskHandle> child;
  {
    RuntimeRunCancellationScope scope(run);
    RuntimeOutputScope output_scope(output, output);
    const auto result = world.execute(compiled.module.init.entry_code_id);
    require(result.ok() && result.value.is_task_handle(), "start local IO fixture" +
            (result.fault ? ": " + result.fault->message : ""));
    child = result.value.as_task_handle();
  }
  until([&] { return output->to_string().find("READING") != std::string::npos &&
      child->state() == RuntimeTaskHandleState::Sleeping; }, "Amber reader parks on reactor");
  run->close_root(); run->request_cancel();
  require(run->wait_for_idle(2s) && child->wait(1s).cancelled, "Stop wakes an IO-parked VM");
  require(output->to_string().find("IO CLEANED") != std::string::npos, "IO cleanup completes");
  child.reset();
  until([] { return RuntimeReactor::instance().stats().current_waiters == 0; }, "cancelled async wait retires without dangling flags");
}
static void register_cancel_race() {
  RuntimeTaskModule tasks(2);
  for (int pass = 0; pass < 50; ++pass) {
    auto run = std::make_shared<RuntimeRunState>();
    std::thread producer([&] {
      RuntimeRunCancellationScope scope(run);
      for (int i = 0; i < 20; ++i) tasks.spawn([] { throw_if_runtime_task_cancelled(); return Value::null(); });
    });
    run->request_cancel(); producer.join(); run->close_root();
    require(run->wait_for_idle(2s) && run->active_tasks() == 0, "concurrent registration/Stop does not lose members");
  }
}
static void amber_subprocess_cleanup(bool child_task) {
  const std::string body =
      "try:\n"
      "  system.command(\"/bin/sleep\", \"60\").capture(timeout: 5.0, kill_after: 0.05) with:\n"
      "    stdout |stream|:\n"
      "      try:\n        print(\"CALLBACK\")\n        while true:\n          x = 1\n"
      "      ensure:\n        task.sleep(5)\n        print(\"CALLBACK CLEANED\")\n"
      "ensure:\n"
      "  system.command(\"/usr/bin/printf\", \"PROCESS CLEANED\").capture(timeout: 2.0) with:\n"
      "    stdout |stream|:\n      print(stream.read_all!().to_str())\n";
  std::string source = "import system\nimport task\n";
  if (child_task) {
    source += "task.spawn:\n  ";
    for (std::size_t i = 0; i < body.size(); ++i) {
      source += body[i];
      if (body[i] == '\n' && i + 1 < body.size()) source += "  ";
    }
  } else source += body;
  auto compiled = compile_source_text(source, "<run-process>");
  require(compiled.ok, compiled.error);
  RuntimeWorldOptions options;
  options.capability_grants = {
      amber::capability::make_capability("process.spawn", "/bin/sleep"),
      amber::capability::make_capability("process.spawn", "/usr/bin/printf")};
  compiled.module.capabilities = options.capability_grants;
  RuntimeWorld world(compiled.module, options);
  auto run = std::make_shared<RuntimeRunState>();
  auto output = RuntimeTextWriter::buffer();
  ExecutionResult result;
  std::thread root([&] {
    RuntimeRunCancellationScope scope(run);
    RuntimeOutputScope output_scope(output, output);
    result = world.execute(compiled.module.init.entry_code_id);
    run->close_root();
  });
  until([&] { return output->to_string().find("CALLBACK\n") != std::string::npos; },
        "subprocess callback enters nested VM");
  require(!run->wait_for_idle(5ms), "foreign callback keeps execution active");
  run->request_cancel();
  root.join();
  require(run->wait_for_idle(2s), "process and callback cleanup drain before completion");
  if (child_task) {
    require(result.ok() && result.value.is_task_handle(), "spawn process task");
    require(result.value.as_task_handle()->wait(1s).cancelled, "process task cancelled");
  } else {
    require(result.fault && result.fault->error_name == "CancelledError", "root process cancelled");
  }
  require(output->to_string() == "CALLBACK\nCALLBACK CLEANED\nPROCESS CLEANED\n",
          "subprocess cleanup may run a new process/callback after Stop: " + output->to_string());
}
static void native_host_stack_ownership() {
  auto run = std::make_shared<RuntimeRunState>();
  auto &tasks = native_task_runtime();
  std::atomic<bool> entered{false}, release{false};
  RuntimeTaskHandle child;
  {
    RuntimeRunCancellationScope scope(run);
    child = tasks.spawn_resumable(native_system_task_driver([&] {
      require(current_runtime_run_cancellation() == run, "native thread inherits run");
      require(current_runtime_task_cancel_owner() &&
              current_runtime_task_cancel_owner().get() == tls_runtime_task_cancel_flag,
              "native thread retains its cancellation flag owner");
      entered.store(true);
      // Model a native stack that has received Stop but cannot return yet.
      while (!release.load()) std::this_thread::yield();
      throw_if_runtime_task_cancelled();
      return Value::null();
    }));
  }
  until([&] { return entered.load() && child.state() == RuntimeTaskHandleState::Sleeping; },
        "native task parks while its host stack is active");
  run->close_root();
  const auto before = tasks.scheduler().stats().worker_dequeues;
  run->request_cancel();
  require(!run->wait_for_idle(20ms), "Stop cannot retire an active native stack");
  require(tasks.scheduler().stats().worker_dequeues <= before + 1,
          "cancelled native driver reparks without a busy wake loop");
  release.store(true);
  require(run->wait_for_idle(2s) && child.wait(1s).cancelled,
          "native task drains after host-stack return");
}
static void task_failure_observation() {
  RuntimeTaskModule tasks(2), other(1);
  auto run = std::make_shared<RuntimeRunState>();
  RuntimeTaskHandle waited, queried, inspected, delayed;
  std::atomic<bool> release{false};
  {
    RuntimeRunCancellationScope scope(run);
    RuntimeTextSourceLocationScope source({true, 7, 8, "<spawn>", 12, 3});
    const auto fail = []() -> Value { throw RuntimeTaskFailure("TypeError", "child failed"); };
    waited = tasks.spawn(fail);
    queried = tasks.spawn(fail);
    inspected = tasks.spawn(fail);
    delayed = other.spawn([&, fail]() -> Value {
      while (!release.load()) std::this_thread::yield();
      return fail();
    });
    // Neither a timeout nor an early result/failure poll acknowledges a future failure.
    require(delayed.wait(1ms).timed_out && !delayed.result().ready && !delayed.failure().ready,
            "not-ready retrieval does not consume a later failure");
  }
  run->close_root(); release.store(true);
  require(run->wait_for_idle(2s), "failed children drain with retained handles");
  const auto failures = run->unobserved_failures();
  require(failures.size() == 4, "cross-scheduler failures are retained independently");
  for (std::size_t i = 0; i < failures.size(); ++i) {
    require(failures[i].task_id == i + 1 && failures[i].error_name == "TypeError" &&
            failures[i].spawn_source.file == "<spawn>" && failures[i].spawn_source.line == 12,
            "run identity/order and spawn location survive task retirement");
  }
  require(waited.failed() && waited.snapshot().failed && !waited.running(), "status remains available");
  require(run->unobserved_failures().size() == 4, "status/failed predicate does not consume errors");
  require(waited.wait(1s).failed && run->unobserved_failures().size() == 3, "wait consumes delivered failure");
  require(queried.result().failed && run->unobserved_failures().size() == 2, "result consumes delivered failure");
  require(inspected.failure().failed && run->unobserved_failures().size() == 1, "explicit failure retrieval consumes it");
  require(waited.wait(1s).failed && run->unobserved_failures().size() == 1, "observation is idempotent");
  require(failures.size() == 4, "an already published host snapshot is immutable");
}
static void structured_failure_receipts() {
  RuntimeTaskModule tasks(2);
  auto run = std::make_shared<RuntimeRunState>();
  RuntimeTaskHandle parent;
  std::atomic<bool> release{false};
  {
    RuntimeRunCancellationScope scope(run);
    parent = tasks.spawn([&] {
      tasks.spawn([&]() -> Value {
        while (!release.load()) std::this_thread::yield();
        throw RuntimeTaskFailure("TypeError", "grandchild failed");
      }); // Dropped child handle must not lose the error.
      return Value::null();
    });
  }
  until([&] { return parent.state() == RuntimeTaskHandleState::Waiting; }, "parent waiting after body return");
  run->close_root(); release.store(true);
  require(run->wait_for_idle(2s) && parent.failed(), "structured failure reaches parent");
  const auto failures = run->unobserved_failures();
  require(failures.size() == 1 && failures[0].task_id == 2, "propagated cause is not duplicated at parent");
  require(parent.wait(1s).failed && run->unobserved_failures().empty(), "retrieving propagated error observes original cause");

  auto multiple = std::make_shared<RuntimeRunState>();
  std::atomic<unsigned> entered{0};
  release.store(false);
  {
    RuntimeRunCancellationScope scope(multiple);
    parent = tasks.spawn([&] {
      for (int i = 0; i < 2; ++i) tasks.spawn([&]() -> Value {
        entered.fetch_add(1);
        while (!release.load()) std::this_thread::yield();
        // Both foreign operations fail independently, even if first-failure
        // supervision has already requested cancellation of the other.
        throw RuntimeTaskFailure("IOError", "independent failure");
      });
      return Value::null();
    });
  }
  until([&] { return entered.load() == 2 && parent.state() == RuntimeTaskHandleState::Waiting; },
        "two children active while parent waits");
  multiple->close_root(); release.store(true);
  require(multiple->wait_for_idle(2s) && multiple->unobserved_failures().size() == 2,
          "two independent sibling failures remain two causes");
  require(parent.wait(1s).failed && multiple->unobserved_failures().size() == 1,
          "retrieving first propagated failure does not consume unrelated sibling failure");

  auto raw_run = std::make_shared<RuntimeRunState>();
  {
    RuntimeRunCancellationScope scope(raw_run);
    tasks.scheduler().spawn_task([] { throw std::runtime_error("raw failure"); });
    tasks.spawn([]() -> Value { throw RuntimeTaskCancelled(); });
  }
  raw_run->close_root();
  require(raw_run->wait_for_idle(2s), "raw and cancelled tasks retire");
  const auto raw = raw_run->unobserved_failures();
  require(raw.size() == 1 && raw[0].error_name == "RuntimeError" && raw[0].message == "raw failure",
          "handle-less scheduler error captured; cancellation is not a failure");
}
static void amber_rescued_failure() {
  auto compiled = compile_source_text(
      "import task\nchild = task.spawn:\n  1 / 0\n"
      "try:\n  child.wait()\nrescue:\n  42\n", "<rescued-child>");
  require(compiled.ok, compiled.error);
  RuntimeWorld world(compiled.module);
  auto run = std::make_shared<RuntimeRunState>();
  {
    RuntimeRunCancellationScope scope(run);
    const auto result = world.execute(compiled.module.init.entry_code_id);
    require(result.ok() && result.value.is_integer() && result.value.as_integer() == 42,
            "Amber code rescues delivered child failure");
  }
  run->close_root();
  require(run->wait_for_idle(2s) && run->unobserved_failures().empty(), "rescued child does not fail host run");
}
static void native_failure_during_stop() {
  auto run = std::make_shared<RuntimeRunState>();
  std::atomic<bool> entered{false}, release{false};
  {
    RuntimeRunCancellationScope scope(run);
    native_task_runtime().spawn_resumable(native_system_task_driver([&]() -> Value {
      entered.store(true);
      while (!release.load()) std::this_thread::yield();
      throw RuntimeTaskFailure("IOError", "cleanup failed");
    }));
  }
  until([&] { return entered.load(); }, "native work entered");
  run->close_root(); run->request_cancel(); release.store(true);
  require(run->wait_for_idle(2s) && run->cancelled(), "failed native work drains after Stop");
  const auto failures = run->unobserved_failures();
  require(failures.size() == 1 && failures[0].error_name == "IOError",
          "native stack failure delivered once, not masked by run cancellation");
}
int main() {
  alarm(30);
  amber_sleep_cleanup();
  before_park_race();
  descendant_and_queue_ownership();
  reactor_cancellation();
  amber_socket_cleanup();
  amber_subprocess_cleanup(false);
  amber_subprocess_cleanup(true);
  native_host_stack_ownership();
  task_failure_observation();
  structured_failure_receipts();
  amber_rescued_failure();
  native_failure_during_stop();
  register_cancel_race();
  alarm(0);
  std::cout << "notebook_run_tasks_tests: ok\n";
}
