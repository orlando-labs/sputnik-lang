#include "tools/iamber/activity.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <future>
#include <iostream>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

void expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "iamber activity test failed: " << message << "\n";
    std::exit(1);
  }
}

struct StartGate {
  void announce() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      ++started;
    }
    condition.notify_all();
  }

  void wait_for(std::size_t count) {
    std::unique_lock<std::mutex> lock(mutex);
    expect(condition.wait_for(lock, 1s,
                              [this, count] { return started >= count; }),
           "waiter thread should reach its start gate");
  }

  std::mutex mutex;
  std::condition_variable condition;
  std::size_t started = 0;
};

void test_handle_traits_and_default_handles() {
  static_assert(!std::is_copy_constructible_v<SessionActivityChannel>);
  static_assert(!std::is_copy_assignable_v<SessionActivityChannel>);
  static_assert(std::is_nothrow_move_constructible_v<SessionActivityChannel>);
  static_assert(std::is_nothrow_move_assignable_v<SessionActivityChannel>);
  static_assert(std::is_copy_constructible_v<SessionActivityNotifier>);
  static_assert(std::is_copy_assignable_v<SessionActivityNotifier>);
  static_assert(std::is_copy_constructible_v<SessionActivityWaiter>);
  static_assert(std::is_copy_assignable_v<SessionActivityWaiter>);

  const SessionActivityNotifier notifier;
  expect(!notifier.notify_runtime() && !notifier.notify_input(),
         "default notifier should report an expired channel");

  const SessionActivityWaiter waiter;
  const SessionActivityResult result = waiter.wait(0ms);
  expect(result.shutdown && !result.timed_out && !result.runtime_ready &&
             !result.input_ready,
         "default waiter should return immediate shutdown");
}

void test_pre_notify_coalesces_and_is_consumed() {
  SessionActivityChannel channel;
  const SessionActivityNotifier notifier = channel.notifier();
  const SessionActivityWaiter waiter = channel.waiter();

  expect(notifier.notify_runtime() && notifier.notify_runtime() &&
             notifier.notify_input() && notifier.notify_input(),
         "live notifier should accept pre-wait hints");

  const SessionActivityResult ready = waiter.wait(0ms);
  expect(ready.runtime_ready && ready.input_ready && !ready.shutdown &&
             !ready.timed_out,
         "pre-wait runtime and input hints should be coalesced");

  const SessionActivityResult consumed = waiter.wait(0ms);
  expect(consumed.timed_out && !consumed.runtime_ready &&
             !consumed.input_ready && !consumed.shutdown,
         "wait should consume both coalesced hints exactly once");

  const SessionActivityResult timed_out = waiter.wait(20ms);
  expect(timed_out.timed_out && !timed_out.shutdown,
         "an idle waiter should report a bounded timeout");
}

void test_notification_before_and_after_wait_start_is_not_lost() {
  SessionActivityChannel channel;
  const SessionActivityNotifier notifier = channel.notifier();
  const SessionActivityWaiter waiter = channel.waiter();

  // The producer can run before the consumer even begins wait(). The state
  // predicate, rather than the condition-variable edge, must retain it.
  expect(notifier.notify_input(), "pre-start input notification should work");
  const SessionActivityResult before = waiter.wait(0ms);
  expect(before.input_ready && !before.runtime_ready && !before.timed_out,
         "notification before wait start should be observed");

  StartGate gate;
  SessionActivityResult after;
  std::thread waiting([&] {
    gate.announce();
    after = waiter.wait(500ms);
  });
  gate.wait_for(1);
  // This intentionally races only the transition into wait(). A correct
  // predicate-based wait handles either ordering without losing the wake.
  expect(notifier.notify_runtime(),
         "post-start runtime notification should work");
  waiting.join();

  expect(after.runtime_ready && !after.input_ready && !after.shutdown &&
             !after.timed_out,
         "notification after wait start should wake the waiter");
}

void test_concurrent_notifier_copies_wake_and_coalesce() {
  SessionActivityChannel channel;
  const SessionActivityNotifier notifier = channel.notifier();
  const SessionActivityWaiter waiter = channel.waiter();
  const SessionActivityNotifier runtime_notifier = notifier;
  const SessionActivityNotifier input_notifier = notifier;

  std::atomic<bool> go{false};
  std::thread runtime_thread([&] {
    while (!go.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    expect(runtime_notifier.notify_runtime(),
           "concurrent runtime notifier should remain live");
  });
  std::thread input_thread([&] {
    while (!go.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    expect(input_notifier.notify_input(),
           "concurrent input notifier should remain live");
  });
  go.store(true, std::memory_order_release);
  runtime_thread.join();
  input_thread.join();

  const SessionActivityResult result = waiter.wait(0ms);
  expect(result.runtime_ready && result.input_ready && !result.shutdown &&
             !result.timed_out,
         "concurrent notifier copies should coalesce both activity kinds");
}

void test_with_wakeup_forwards_independently_of_local_coalescing() {
  SessionActivityChannel local_channel;
  SessionActivityChannel host_channel;
  const SessionActivityNotifier host_notifier = host_channel.notifier();
  const SessionActivityNotifier notifier =
      local_channel.notifier().with_wakeup(host_notifier);
  const SessionActivityWaiter local_waiter = local_channel.waiter();
  const SessionActivityWaiter host_waiter = host_channel.waiter();

  expect(notifier.notify_runtime(),
         "with_wakeup notifier should accept its first runtime hint");
  const SessionActivityResult local_first = local_waiter.wait(0ms);
  const SessionActivityResult host_first = host_waiter.wait(0ms);
  expect(local_first.runtime_ready && host_first.runtime_ready,
         "one hint should be delivered to both local and host waiters");

  // Consume the host hint while leaving the local hint coalesced. The second
  // call must still enqueue a fresh host hint instead of returning early.
  expect(notifier.notify_runtime(),
         "with_wakeup notifier should accept a second runtime hint");
  expect(host_waiter.wait(0ms).runtime_ready,
         "host should receive the second runtime hint");
  expect(notifier.notify_runtime(),
         "a coalesced local hint should still wake the host");
  const SessionActivityResult host_after_local_coalesce = host_waiter.wait(0ms);
  expect(host_after_local_coalesce.runtime_ready &&
             !host_after_local_coalesce.timed_out,
         "host hint must not be coalesced with the local mailbox");
  expect(local_waiter.wait(0ms).runtime_ready,
         "local hint should remain coalesced until its own waiter consumes it");
}

void test_with_wakeup_lifetime_move_and_self_target() {
  SessionActivityChannel host_channel;
  const SessionActivityNotifier host_notifier = host_channel.notifier();

  SessionActivityNotifier moved_notifier;
  SessionActivityWaiter moved_waiter;
  {
    SessionActivityChannel source;
    moved_notifier = source.notifier().with_wakeup(host_notifier);
    moved_waiter = source.waiter();
    SessionActivityChannel moved(std::move(source));
    expect(moved_notifier.notify_input(),
           "with_wakeup notifier should follow a moved local channel");
    expect(moved_waiter.wait(0ms).input_ready,
           "moved local waiter should consume its input hint");
    expect(host_channel.waiter().wait(0ms).input_ready,
           "moved local notifier should wake the host");
  }
  expect(moved_waiter.wait(0ms).shutdown,
         "destroying moved owner should close retained local state");
  expect(!moved_notifier.notify_input(),
         "expired local notifier must reject and not wake its host");
  expect(host_channel.waiter().wait(0ms).timed_out,
         "expired local notifier must not leave a host hint");

  SessionActivityChannel local_channel;
  const SessionActivityNotifier self_notifier =
      local_channel.notifier().with_wakeup(local_channel.notifier());
  expect(self_notifier.notify_runtime(),
         "self wakeup target should be harmless and accepted");
  const SessionActivityResult self_result = local_channel.waiter().wait(0ms);
  expect(self_result.runtime_ready && !self_result.shutdown,
         "self wakeup target should produce one local hint");
}

void test_with_wakeup_host_lifetime_and_shutdown() {
  SessionActivityChannel local_channel;
  const SessionActivityWaiter local_waiter = local_channel.waiter();
  SessionActivityNotifier notifier;
  SessionActivityWaiter expired_host_waiter;
  {
    SessionActivityChannel host_channel;
    notifier = local_channel.notifier().with_wakeup(host_channel.notifier());
    expired_host_waiter = host_channel.waiter();
  }

  expect(notifier.notify_runtime(),
         "expired host should not make local notification fail");
  expect(local_waiter.wait(0ms).runtime_ready,
         "local mailbox should receive a hint after host expiry");
  expect(expired_host_waiter.wait(0ms).shutdown,
         "retained waiter should observe expired host shutdown");

  SessionActivityChannel open_host;
  const SessionActivityWaiter open_host_waiter = open_host.waiter();
  const SessionActivityNotifier open_local =
      local_channel.notifier().with_wakeup(open_host.notifier());
  local_channel.close();
  expect(!open_local.notify_input(),
         "closed local notifier should reject the forwarded hint");
  expect(local_waiter.wait(0ms).shutdown,
         "closed local mailbox should report sticky shutdown");
  expect(open_host_waiter.wait(0ms).timed_out,
         "closed local notifier should not wake an open host");

  SessionActivityChannel shutdown_host;
  const SessionActivityNotifier shutdown_notifier =
      local_channel.notifier().with_wakeup(shutdown_host.notifier());
  shutdown_host.close();
  expect(!local_channel.notifier().notify_input(),
         "closed local channel should remain closed after host shutdown");
  expect(shutdown_notifier.notify_runtime() == false,
         "local shutdown should take priority over host shutdown");
}

void test_with_wakeup_cross_calls_do_not_deadlock() {
  SessionActivityChannel first;
  SessionActivityChannel second;
  const SessionActivityWaiter first_waiter = first.waiter();
  const SessionActivityWaiter second_waiter = second.waiter();
  const SessionActivityNotifier first_notifier =
      first.notifier().with_wakeup(second.notifier());
  const SessionActivityNotifier second_notifier =
      second.notifier().with_wakeup(first.notifier());
  std::atomic<bool> go{false};
  std::thread first_thread([&] {
    while (!go.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    expect(first_notifier.notify_runtime(),
           "cross-wakeup first notifier should remain live");
  });
  std::thread second_thread([&] {
    while (!go.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    expect(second_notifier.notify_input(),
           "cross-wakeup second notifier should remain live");
  });
  go.store(true, std::memory_order_release);
  first_thread.join();
  second_thread.join();

  const SessionActivityResult first_result = first_waiter.wait(0ms);
  const SessionActivityResult second_result = second_waiter.wait(0ms);
  expect(first_result.runtime_ready && first_result.input_ready &&
             second_result.runtime_ready && second_result.input_ready,
         "cross-wakeup calls should deliver both hints without deadlock");
}

void test_close_discards_hints_and_is_sticky() {
  SessionActivityChannel channel;
  const SessionActivityNotifier notifier = channel.notifier();
  const SessionActivityWaiter waiter = channel.waiter();
  expect(notifier.notify_runtime() && notifier.notify_input(),
         "close-priority setup should accept hints");

  channel.close();
  const SessionActivityResult first = waiter.wait(0ms);
  const SessionActivityResult second = waiter.wait(200ms);
  expect(first.shutdown && !first.runtime_ready && !first.input_ready &&
             !first.timed_out,
         "close should prioritize shutdown over pending hints");
  expect(second.shutdown && !second.runtime_ready && !second.input_ready &&
             !second.timed_out,
         "shutdown should remain sticky across waits");
  expect(!notifier.notify_runtime() && !notifier.notify_input(),
         "notifier should reject hints after close");
}

void test_close_wakes_multiple_waiters() {
  SessionActivityChannel channel;
  const SessionActivityNotifier notifier = channel.notifier();
  const SessionActivityWaiter first_waiter = channel.waiter();
  const SessionActivityWaiter second_waiter = first_waiter;
  StartGate gate;
  SessionActivityResult first;
  SessionActivityResult second;

  std::thread first_thread([&] {
    gate.announce();
    first = first_waiter.wait(1s);
  });
  std::thread second_thread([&] {
    gate.announce();
    second = second_waiter.wait(1s);
  });
  gate.wait_for(2);

  channel.close();
  first_thread.join();
  second_thread.join();

  expect(first.shutdown && !first.timed_out && !first.runtime_ready &&
             !first.input_ready && second.shutdown && !second.timed_out &&
             !second.runtime_ready && !second.input_ready,
         "close should wake every copied waiter with shutdown");
  expect(!notifier.notify_runtime(),
         "close should reject notifiers after waking waiters");
}

void test_close_and_destruction_race_with_weak_notifiers() {
  auto wait_for_counter = [](const std::atomic<std::size_t> &counter,
                             std::size_t target, const char *message) {
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (counter.load(std::memory_order_acquire) < target &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::yield();
    }
    expect(counter.load(std::memory_order_acquire) >= target, message);
  };

  auto run_producers = [](const SessionActivityNotifier &runtime_notifier,
                          const SessionActivityNotifier &input_notifier,
                          std::atomic<bool> *go, std::atomic<bool> *stop,
                          std::atomic<std::size_t> *ready,
                          std::atomic<std::size_t> *attempts) {
    auto producer = [go, stop, ready,
                     attempts](SessionActivityNotifier notifier, bool runtime) {
      ready->fetch_add(1, std::memory_order_release);
      while (!go->load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      while (!stop->load(std::memory_order_acquire)) {
        attempts->fetch_add(1, std::memory_order_relaxed);
        if (runtime) {
          (void)notifier.notify_runtime();
        } else {
          (void)notifier.notify_input();
        }
      }
    };

    std::vector<std::thread> producers;
    producers.emplace_back(producer, runtime_notifier, true);
    producers.emplace_back(producer, input_notifier, false);
    return producers;
  };

  {
    SessionActivityChannel channel;
    const SessionActivityNotifier notifier = channel.notifier();
    const SessionActivityNotifier runtime_notifier = notifier;
    const SessionActivityNotifier input_notifier = notifier;
    const SessionActivityWaiter waiter = channel.waiter();
    std::atomic<bool> go{false};
    std::atomic<bool> stop{false};
    std::atomic<std::size_t> ready{0};
    std::atomic<std::size_t> attempts{0};
    std::vector<std::thread> producers = run_producers(
        runtime_notifier, input_notifier, &go, &stop, &ready, &attempts);
    wait_for_counter(ready, 2,
                     "close race producers should reach their start gate");
    go.store(true, std::memory_order_release);
    wait_for_counter(attempts, 32,
                     "close race producers should attempt notifications");

    // Producers continue using only weak copied notifiers while the owner
    // closes the shared state. Calls racing with close may finish either way;
    // all calls after the close must fail and may not resurrect the mailbox.
    channel.close();
    stop.store(true, std::memory_order_release);
    for (std::thread &producer : producers) {
      producer.join();
    }

    const SessionActivityResult first = waiter.wait(0ms);
    const SessionActivityResult second = waiter.wait(0ms);
    expect(first.shutdown && !first.timed_out && second.shutdown &&
               !second.timed_out,
           "close racing weak notifiers should produce sticky shutdown");
    expect(!notifier.notify_runtime() && !runtime_notifier.notify_runtime() &&
               !input_notifier.notify_input(),
           "all copied notifiers should reject calls after close");
  }

  SessionActivityNotifier late_notifier;
  SessionActivityWaiter retained_waiter;
  std::atomic<bool> go{false};
  std::atomic<bool> stop{false};
  std::atomic<std::size_t> ready{0};
  std::atomic<std::size_t> attempts{0};
  std::vector<std::thread> producers;
  {
    SessionActivityChannel channel;
    late_notifier = channel.notifier();
    retained_waiter = channel.waiter();
    const SessionActivityNotifier runtime_notifier = late_notifier;
    const SessionActivityNotifier input_notifier = late_notifier;
    producers = run_producers(runtime_notifier, input_notifier, &go, &stop,
                              &ready, &attempts);
    wait_for_counter(
        ready, 2, "destruction race producers should reach their start gate");
    go.store(true, std::memory_order_release);
    wait_for_counter(attempts, 32,
                     "destruction race producers should attempt notifications");
  }

  // No producer has an owner reference. Destroying the owner while they are
  // still calling weak notifiers must close the retained state safely.
  stop.store(true, std::memory_order_release);
  for (std::thread &producer : producers) {
    producer.join();
  }
  const SessionActivityResult destroyed = retained_waiter.wait(0ms);
  expect(destroyed.shutdown && !destroyed.timed_out,
         "owner destruction racing weak notifiers should shut down waiters");
  expect(!late_notifier.notify_runtime() && !late_notifier.notify_input(),
         "late copied notifier should fail after owner destruction");
  const SessionActivityResult sticky = retained_waiter.wait(0ms);
  expect(sticky.shutdown && !sticky.timed_out,
         "destruction shutdown should remain sticky after the race");
}

void test_maximum_wait_durations_block_until_close() {
  const std::chrono::milliseconds maximum = std::chrono::milliseconds::max();
  const std::chrono::milliseconds durations[] = {maximum, maximum / 2};

  for (const std::chrono::milliseconds timeout : durations) {
    SessionActivityChannel channel;
    const SessionActivityWaiter waiter = channel.waiter();
    std::promise<void> started;
    std::future<void> start = started.get_future();
    std::promise<SessionActivityResult> completed;
    std::future<SessionActivityResult> completion = completed.get_future();
    std::thread waiting([&] {
      started.set_value();
      completed.set_value(waiter.wait(timeout));
    });
    expect(start.wait_for(1s) == std::future_status::ready,
           "maximum-timeout waiter should start");
    const bool blocked_for_probe =
        completion.wait_for(20ms) == std::future_status::timeout;

    channel.close();
    expect(completion.wait_for(500ms) == std::future_status::ready,
           "maximum wait should complete within a bound after close");
    const SessionActivityResult result = completion.get();
    waiting.join();
    expect(blocked_for_probe && result.shutdown && !result.timed_out &&
               !result.runtime_ready && !result.input_ready,
           "maximum wait should return sticky shutdown after close");
  }
}

void test_owner_destruction_wakes_waiter_and_late_notifier_is_weak() {
  SessionActivityNotifier late_notifier;
  SessionActivityWaiter retained_waiter;
  StartGate gate;
  SessionActivityResult result;
  std::thread waiting;

  {
    SessionActivityChannel channel;
    late_notifier = channel.notifier();
    retained_waiter = channel.waiter();
    waiting = std::thread([&] {
      gate.announce();
      result = retained_waiter.wait(1s);
    });
    gate.wait_for(1);
  }

  // The owner destructor must close the shared state even though a notifier
  // is retained; the notifier is weak and cannot keep the owner/state alive.
  waiting.join();
  expect(result.shutdown && !result.timed_out,
         "owner destruction should wake a retained waiter");
  expect(!late_notifier.notify_runtime() && !late_notifier.notify_input(),
         "late weak notifier should fail harmlessly after destruction");
  const SessionActivityResult after = retained_waiter.wait(0ms);
  expect(after.shutdown && !after.timed_out,
         "destruction shutdown should remain sticky for retained waiters");
}

void test_move_constructor_keeps_state_and_moved_from_handles_shutdown() {
  SessionActivityChannel source;
  const SessionActivityNotifier notifier = source.notifier();
  const SessionActivityWaiter waiter = source.waiter();
  SessionActivityChannel moved(std::move(source));

  expect(source.notifier().notify_runtime() == false,
         "moved-from owner should produce an expired notifier");
  const SessionActivityResult moved_from_wait = source.waiter().wait(0ms);
  expect(moved_from_wait.shutdown && !moved_from_wait.timed_out,
         "moved-from owner should produce a shutdown waiter");

  expect(notifier.notify_runtime(),
         "notifier obtained before move should follow moved state");
  const SessionActivityResult retained = waiter.wait(0ms);
  expect(retained.runtime_ready && !retained.shutdown,
         "waiter obtained before move should follow moved state");

  moved.close();
  const SessionActivityResult closed = waiter.wait(0ms);
  expect(closed.shutdown && !closed.timed_out,
         "moved-to owner should close the transferred state");
}

void test_move_assignment_closes_old_state_and_transfers_new_state() {
  SessionActivityChannel source;
  const SessionActivityNotifier source_notifier = source.notifier();
  const SessionActivityWaiter source_waiter = source.waiter();
  SessionActivityChannel destination;
  const SessionActivityNotifier old_notifier = destination.notifier();
  const SessionActivityWaiter old_waiter = destination.waiter();

  destination = std::move(source);

  expect(!old_notifier.notify_input(),
         "move assignment should close the destination's old state");
  const SessionActivityResult old_state = old_waiter.wait(0ms);
  expect(old_state.shutdown && !old_state.timed_out,
         "waiters of replaced state should observe sticky shutdown");
  expect(source.waiter().wait(0ms).shutdown,
         "move-assigned-from owner should produce shutdown handles");

  expect(source_notifier.notify_input(),
         "pre-move notifier should follow move-assigned state");
  const SessionActivityResult transferred = source_waiter.wait(0ms);
  expect(transferred.input_ready && !transferred.shutdown,
         "pre-move waiter should follow move-assigned state");

  destination.close();
}

} // namespace

int main() {
  test_handle_traits_and_default_handles();
  test_pre_notify_coalesces_and_is_consumed();
  test_notification_before_and_after_wait_start_is_not_lost();
  test_concurrent_notifier_copies_wake_and_coalesce();
  test_with_wakeup_forwards_independently_of_local_coalescing();
  test_with_wakeup_lifetime_move_and_self_target();
  test_with_wakeup_host_lifetime_and_shutdown();
  test_with_wakeup_cross_calls_do_not_deadlock();
  test_close_discards_hints_and_is_sticky();
  test_close_wakes_multiple_waiters();
  test_close_and_destruction_race_with_weak_notifiers();
  test_maximum_wait_durations_block_until_close();
  test_owner_destruction_wakes_waiter_and_late_notifier_is_weak();
  test_move_constructor_keeps_state_and_moved_from_handles_shutdown();
  test_move_assignment_closes_old_state_and_transfers_new_state();
  std::cout << "iamber_activity_tests ok\n";
  return 0;
}
