#include "tools/iamber/dispatch.h"
#include "tools/iamber/session.h"

#include <cstdlib>
#include <iostream>

namespace {
using namespace std::chrono_literals;

void expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "iamber dispatch test failed: " << message << "\n";
    std::exit(1);
  }
}

void test_idle_and_regular_completion() {
  RuntimePumpDispatch dispatch;
  auto now = RuntimePumpDispatch::Clock::time_point{};
  expect(!dispatch.pending() && !dispatch.due(now) &&
             !dispatch.wait_timeout(now),
         "idle dispatch should have no polling timer");
  dispatch.notify_runtime();
  expect(dispatch.pending() && dispatch.due(now) &&
             dispatch.wait_timeout(now) == 0ms,
         "an initial runtime hint should be immediately runnable");
  RuntimeEventPumpResult result;
  result.disposition = RuntimeEventPumpDisposition::Acknowledged;
  dispatch.complete(result, now);
  expect(!dispatch.pending() && !dispatch.wait_timeout(now),
         "a completed drain should return to timer-free idle");
  dispatch.notify_runtime();
  expect(dispatch.due(now), "a subsequent hint should not be suppressed");
  result.disposition = RuntimeEventPumpDisposition::NoWork;
  dispatch.complete(result, now);
  expect(!dispatch.pending(), "spurious hints should also return to idle");
  dispatch.notify_runtime();
  result.disposition = RuntimeEventPumpDisposition::DeferredByEdits;
  dispatch.complete(result, now);
  expect(!dispatch.pending() && !dispatch.wait_timeout(now),
         "uninstalled edits wait for explicit evaluation, not a retry timer");
}

void test_hot_sources_yield_to_input() {
  RuntimePumpDispatch dispatch;
  const auto now = RuntimePumpDispatch::Clock::time_point{};
  RuntimeEventPumpResult result;
  result.disposition = RuntimeEventPumpDisposition::Acknowledged;
  result.drain_limit_reached = true;
  dispatch.complete(result, now);
  for (int repeat = 0; repeat < 100; ++repeat) {
    dispatch.notify_runtime();
  }
  expect(dispatch.pending() && !dispatch.due(now) &&
             dispatch.wait_timeout(now) == 16ms && !dispatch.due(now + 15ms) &&
             dispatch.due(now + 16ms),
         "coalesced hot-source hints must not bypass the fairness cooldown");
  expect(dispatch.wait_timeout(now + 15500us) == 1ms,
         "sub-millisecond time remaining must round up, not spin");
  result.drain_limit_reached = false;
  result.disposition = RuntimeEventPumpDisposition::Busy;
  dispatch.complete(result, now + 16ms);
  expect(!dispatch.due(now + 16ms) && dispatch.due(now + 32ms),
         "Busy must coalesce into a later dispatch turn");
}

void test_recovery_backoff_is_bounded_and_resets() {
  RuntimePumpDispatch dispatch;
  auto now = RuntimePumpDispatch::Clock::time_point{};
  RuntimeEventPumpResult result;
  result.disposition = RuntimeEventPumpDisposition::RetryRequired;
  result.cursor_retained = true;
  for (const auto delay : {100ms, 200ms, 400ms, 800ms, 1000ms, 1000ms}) {
    dispatch.complete(result, now);
    dispatch.notify_runtime();
    expect(!dispatch.due(now) && dispatch.wait_timeout(now) == delay,
           "new hints must not bypass bounded recovery backoff");
    now += delay;
    expect(dispatch.due(now),
           "recovery should become runnable at its deadline");
  }
  result.cursor_retained = false;
  result.disposition = RuntimeEventPumpDisposition::Acknowledged;
  dispatch.complete(result, now);
  expect(!dispatch.pending(), "successful recovery should return to idle");
  result.cursor_retained = true;
  result.disposition = RuntimeEventPumpDisposition::Failed;
  dispatch.complete(result, now);
  expect(dispatch.wait_timeout(now) == 100ms,
         "recovery delay should reset after a successful drain");
}
} // namespace

int main() {
  test_idle_and_regular_completion();
  test_hot_sources_yield_to_input();
  test_recovery_backoff_is_bounded_and_resets();
  std::cout << "iamber_dispatch_tests ok\n";
}
