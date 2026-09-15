#include "tools/iamber/activity.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <thread>
#include <utility>

namespace {

using namespace std::chrono_literals;

void expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "iamber poll test failed: " << message << "\n";
    std::exit(1);
  }
}

void expect_descriptor_flags(int descriptor) {
  expect(descriptor >= 0, "poll descriptor should be valid");
  const int descriptor_flags = fcntl(descriptor, F_GETFD);
  expect(descriptor_flags >= 0 && (descriptor_flags & FD_CLOEXEC) != 0,
         "poll descriptor should have close-on-exec set");
  const int status_flags = fcntl(descriptor, F_GETFL);
  expect(status_flags >= 0 && (status_flags & O_NONBLOCK) != 0,
         "poll descriptor should be nonblocking");
}

void expect_readable(int descriptor, int timeout_ms, const char *message) {
  struct pollfd polled{descriptor, POLLIN, 0};
  const int result = poll(&polled, 1, timeout_ms);
  expect(result == 1 && (polled.revents & POLLIN) != 0, message);
}

void expect_not_readable(int descriptor, const char *message) {
  struct pollfd polled{descriptor, POLLIN, 0};
  const int result = poll(&polled, 1, 0);
  expect(result == 0 && polled.revents == 0, message);
}

void test_default_and_stable_descriptors() {
  const SessionActivityWaiter empty_waiter;
  expect(empty_waiter.poll_descriptor() == -1,
         "default waiter should have no poll descriptor");

  SessionActivityChannel channel;
  const SessionActivityWaiter waiter = channel.waiter();
  const SessionActivityWaiter copy = waiter;
  const int descriptor = waiter.poll_descriptor();
  expect_descriptor_flags(descriptor);
  expect(copy.poll_descriptor() == descriptor &&
             waiter.poll_descriptor() == descriptor,
         "copied waiter should retain one stable borrowed descriptor");
  expect_not_readable(descriptor,
                      "an idle attached waiter should not be readable");
}

void test_hints_before_and_after_descriptor_attachment() {
  {
    SessionActivityChannel channel;
    const SessionActivityNotifier notifier = channel.notifier();
    const SessionActivityWaiter waiter = channel.waiter();
    expect(notifier.notify_runtime() && notifier.notify_input(),
           "pre-attachment hints should be accepted");

    const int descriptor = waiter.poll_descriptor();
    expect_readable(
        descriptor, 50,
        "pending hints should make a newly attached descriptor readable");
    const SessionActivityResult result = waiter.wait(0ms);
    expect(result.runtime_ready && result.input_ready && !result.shutdown &&
               !result.timed_out,
           "pre-attachment hints should be consumed together");
    expect_not_readable(
        descriptor, "consuming pre-attachment hints should drain readiness");
  }

  {
    SessionActivityChannel channel;
    const SessionActivityNotifier notifier = channel.notifier();
    const SessionActivityWaiter waiter = channel.waiter();
    const int descriptor = waiter.poll_descriptor();
    expect_not_readable(descriptor,
                        "descriptor should begin idle after attachment");

    expect(notifier.notify_input(), "post-attachment input hint should work");
    expect_readable(descriptor, 50,
                    "post-attachment hint should make descriptor readable");
    const SessionActivityResult result = waiter.wait(0ms);
    expect(result.input_ready && !result.runtime_ready && !result.shutdown &&
               !result.timed_out,
           "post-attachment input hint should be reported");
    expect_not_readable(
        descriptor, "consuming post-attachment hint should drain readiness");
  }
}

void test_close_before_descriptor_attachment_is_readable() {
  SessionActivityChannel channel;
  const SessionActivityWaiter waiter = channel.waiter();
  channel.close();

  const int descriptor = waiter.poll_descriptor();
  expect_descriptor_flags(descriptor);
  expect_readable(descriptor, 50,
                  "late descriptor attachment should expose sticky close");
  const SessionActivityResult result = waiter.wait(0ms);
  expect(result.shutdown && !result.timed_out && !result.runtime_ready &&
             !result.input_ready,
         "late descriptor attachment should preserve shutdown");
  expect_readable(descriptor, 0,
                  "late-attached close descriptor should remain readable");
}

void test_positive_wait_consumes_descriptor_readiness() {
  SessionActivityChannel channel;
  const SessionActivityNotifier notifier = channel.notifier();
  const SessionActivityWaiter waiter = channel.waiter();
  const int descriptor = waiter.poll_descriptor();
  expect(notifier.notify_runtime(),
         "positive-wait readiness setup should accept runtime hint");
  expect_readable(descriptor, 50,
                  "positive-wait readiness setup should be readable");

  const SessionActivityResult result = waiter.wait(100ms);
  expect(result.runtime_ready && !result.input_ready && !result.shutdown &&
             !result.timed_out,
         "positive wait should consume the runtime hint");
  expect_not_readable(descriptor,
                      "positive wait should drain consumed readiness");
}

void test_repeated_hints_coalesce_without_stale_readiness() {
  SessionActivityChannel channel;
  const SessionActivityNotifier notifier = channel.notifier();
  const SessionActivityWaiter waiter = channel.waiter();
  const int descriptor = waiter.poll_descriptor();

  for (int index = 0; index < 8; ++index) {
    expect(notifier.notify_runtime() && notifier.notify_input(),
           "repeated coalesced hints should be accepted");
  }
  expect_readable(descriptor, 50,
                  "coalesced repeated hints should signal readability");
  const SessionActivityResult result = waiter.wait(0ms);
  expect(result.runtime_ready && result.input_ready && !result.shutdown &&
             !result.timed_out,
         "repeated hints should consume as one runtime/input pair");
  const SessionActivityResult empty = waiter.wait(0ms);
  expect(empty.timed_out && !empty.runtime_ready && !empty.input_ready &&
             !empty.shutdown,
         "coalesced hints should be consumed exactly once");
  expect_not_readable(
      descriptor, "consumed coalesced hints should leave no stale readiness");
}

void test_close_is_sticky_and_keeps_descriptor_readable() {
  SessionActivityChannel channel;
  const SessionActivityNotifier notifier = channel.notifier();
  const SessionActivityWaiter waiter = channel.waiter();
  const int descriptor = waiter.poll_descriptor();
  expect(notifier.notify_runtime(), "close-priority hint should be accepted");

  channel.close();
  expect_readable(descriptor, 50,
                  "close should leave the descriptor persistently readable");
  const SessionActivityResult first = waiter.wait(0ms);
  expect(first.shutdown && !first.timed_out && !first.runtime_ready &&
             !first.input_ready,
         "close should discard hints and report shutdown");
  expect_readable(descriptor, 0,
                  "wait shutdown should not drain the close readiness token");
  const SessionActivityResult second = waiter.wait(0ms);
  expect(second.shutdown && !second.timed_out,
         "close shutdown should remain sticky");
  expect_readable(descriptor, 0, "sticky shutdown should remain poll-readable");
  expect(!notifier.notify_runtime() && !notifier.notify_input(),
         "late notifier should fail after close");
}

void test_notification_race_with_consumption_drains_exactly() {
  SessionActivityChannel channel;
  const SessionActivityNotifier notifier = channel.notifier();
  const SessionActivityWaiter waiter = channel.waiter();
  const int descriptor = waiter.poll_descriptor();
  expect(notifier.notify_input(), "race setup input hint should be accepted");
  expect_readable(descriptor, 50, "race setup should be poll-readable");

  const SessionActivityNotifier producer_notifier = notifier;
  std::atomic<bool> go{false};
  std::atomic<bool> producer_done{false};
  bool producer_accepted = false;
  std::thread producer([&] {
    while (!go.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    producer_accepted = producer_notifier.notify_runtime();
    producer_done.store(true, std::memory_order_release);
  });
  go.store(true, std::memory_order_release);
  const SessionActivityResult first = waiter.wait(0ms);
  producer.join();
  expect(producer_done.load(std::memory_order_acquire) && producer_accepted,
         "concurrent producer should finish and accept its hint");

  const SessionActivityResult second = waiter.wait(0ms);
  expect(first.runtime_ready || second.runtime_ready,
         "runtime hint racing consumption should not be lost");
  expect_not_readable(descriptor,
                      "race consumption should drain all coalesced readiness");
}

void test_independent_channels_have_independent_descriptors() {
  SessionActivityChannel first_channel;
  SessionActivityChannel second_channel;
  const SessionActivityNotifier first_notifier = first_channel.notifier();
  const SessionActivityNotifier second_notifier = second_channel.notifier();
  const SessionActivityWaiter first_waiter = first_channel.waiter();
  const SessionActivityWaiter second_waiter = second_channel.waiter();
  const int first_descriptor = first_waiter.poll_descriptor();
  const int second_descriptor = second_waiter.poll_descriptor();
  expect_descriptor_flags(first_descriptor);
  expect_descriptor_flags(second_descriptor);
  expect(first_descriptor != second_descriptor,
         "independent channels should not share descriptors");

  expect(first_notifier.notify_runtime(),
         "first independent notifier should accept its hint");
  expect_readable(first_descriptor, 50,
                  "first channel should signal its own descriptor");
  expect_not_readable(second_descriptor,
                      "first channel should not signal second descriptor");
  const SessionActivityResult first_result = first_waiter.wait(0ms);
  expect(first_result.runtime_ready && !first_result.shutdown,
         "first channel should consume its own runtime hint");
  expect_not_readable(first_descriptor,
                      "first channel should drain its own descriptor");

  expect(second_notifier.notify_input(),
         "second independent notifier should accept its hint");
  expect_readable(second_descriptor, 50,
                  "second channel should signal its own descriptor");
  expect_not_readable(first_descriptor,
                      "second channel should not re-signal first descriptor");
}

void test_with_wakeup_signals_host_descriptor() {
  SessionActivityChannel local_channel;
  SessionActivityChannel host_channel;
  const SessionActivityNotifier notifier =
      local_channel.notifier().with_wakeup(host_channel.notifier());
  const SessionActivityWaiter local_waiter = local_channel.waiter();
  const SessionActivityWaiter host_waiter = host_channel.waiter();
  const int local_descriptor = local_waiter.poll_descriptor();
  const int host_descriptor = host_waiter.poll_descriptor();
  expect_descriptor_flags(local_descriptor);
  expect_descriptor_flags(host_descriptor);

  expect(notifier.notify_input(),
         "with_wakeup input hint should signal both descriptors");
  expect_readable(local_descriptor, 50,
                  "local descriptor should receive forwarded input hint");
  expect_readable(host_descriptor, 50,
                  "host descriptor should receive forwarded input hint");

  expect(host_waiter.wait(0ms).input_ready,
         "host waiter should consume forwarded input hint");
  expect_not_readable(host_descriptor,
                      "consuming host hint should drain host readiness");
  expect(local_waiter.wait(0ms).input_ready,
         "local waiter should consume its independent input hint");
  expect_not_readable(local_descriptor,
                      "consuming local hint should drain local readiness");
}

void test_retained_waiter_survives_move_and_owner_destruction() {
  SessionActivityNotifier late_notifier;
  SessionActivityWaiter retained_waiter;
  int descriptor = -1;
  {
    SessionActivityChannel source;
    late_notifier = source.notifier();
    retained_waiter = source.waiter();
    descriptor = retained_waiter.poll_descriptor();
    SessionActivityChannel moved(std::move(source));
    expect(retained_waiter.poll_descriptor() == descriptor,
           "retained waiter descriptor should survive owner move");
  }

  expect_readable(descriptor, 50,
                  "owner destruction should signal retained waiter descriptor");
  const SessionActivityResult destroyed = retained_waiter.wait(0ms);
  expect(destroyed.shutdown && !destroyed.timed_out,
         "retained waiter should report shutdown after owner destruction");
  expect_readable(descriptor, 0,
                  "retained destruction shutdown should remain readable");
  expect(!late_notifier.notify_runtime() && !late_notifier.notify_input(),
         "weak notifier should fail after moved owner destruction");
  expect(retained_waiter.poll_descriptor() == descriptor,
         "retained waiter descriptor should remain stable after shutdown");
}

} // namespace

int main() {
  test_default_and_stable_descriptors();
  test_hints_before_and_after_descriptor_attachment();
  test_close_before_descriptor_attachment_is_readable();
  test_positive_wait_consumes_descriptor_readiness();
  test_repeated_hints_coalesce_without_stale_readiness();
  test_close_is_sticky_and_keeps_descriptor_readable();
  test_notification_race_with_consumption_drains_exactly();
  test_independent_channels_have_independent_descriptors();
  test_with_wakeup_signals_host_descriptor();
  test_retained_waiter_survives_move_and_owner_destruction();
  std::cout << "iamber_poll_tests ok\n";
  return 0;
}
