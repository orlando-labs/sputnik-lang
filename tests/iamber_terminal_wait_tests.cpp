#include "tools/iamber/terminal_wait.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <pthread.h>
#include <stdexcept>
#include <thread>
#include <type_traits>

#include <unistd.h>

namespace {

using namespace std::chrono_literals;

void expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "iamber terminal wait test failed: " << message << "\n";
    std::exit(1);
  }
}

template <typename Function>
void expect_invalid_argument(Function &&function, const char *message) {
  bool threw = false;
  try {
    function();
  } catch (const std::invalid_argument &) {
    threw = true;
  } catch (...) {
  }
  expect(threw, message);
}

void close_pipe(int descriptors[2]) {
  if (descriptors[0] >= 0) {
    (void)::close(descriptors[0]);
    descriptors[0] = -1;
  }
  if (descriptors[1] >= 0) {
    (void)::close(descriptors[1]);
    descriptors[1] = -1;
  }
}

void test_traits_and_timeouts() {
  static_assert(!std::is_copy_constructible_v<TerminalEventWaiter>);
  static_assert(!std::is_copy_assignable_v<TerminalEventWaiter>);
  static_assert(!std::is_move_constructible_v<TerminalEventWaiter>);
  static_assert(!std::is_move_assignable_v<TerminalEventWaiter>);

  TerminalEventWaiter waiter;
  const TerminalEventWaitResult result = waiter.wait(-1, -1, 0ms);
  expect(result.timed_out && !result.input_ready && !result.activity_ready &&
             !result.interrupted,
         "zero-time wait without descriptors should time out");

  const auto before = std::chrono::steady_clock::now();
  const TerminalEventWaitResult bounded = waiter.wait(-1, -1, 25ms);
  const auto elapsed = std::chrono::steady_clock::now() - before;
  expect(bounded.timed_out && !bounded.interrupted,
         "bounded wait without descriptors should time out");
  expect(elapsed >= 10ms,
         "bounded wait should not return before its requested interval");
}

void test_pipe_readiness_and_eof() {
  int input_pipe[2] = {-1, -1};
  int activity_pipe[2] = {-1, -1};
  expect(::pipe(input_pipe) == 0, "input pipe should be created");
  expect(::pipe(activity_pipe) == 0, "activity pipe should be created");

  {
    TerminalEventWaiter waiter;
    const TerminalEventWaitResult idle =
        waiter.wait(input_pipe[0], activity_pipe[0], 0ms);
    expect(idle.timed_out && !idle.input_ready && !idle.activity_ready,
           "idle pipes should not be reported ready");

    const char input_byte = 'i';
    const char activity_byte = 'a';
    expect(::write(input_pipe[1], &input_byte, 1) == 1,
           "input byte should be written");
    expect(::write(activity_pipe[1], &activity_byte, 1) == 1,
           "activity byte should be written");
    const TerminalEventWaitResult ready =
        waiter.wait(input_pipe[0], activity_pipe[0], 100ms);
    expect(!ready.timed_out && ready.input_ready && ready.activity_ready,
           "both pipe sources should be reported ready");

    char received = 0;
    expect(::read(input_pipe[0], &received, 1) == 1 && received == input_byte,
           "input readiness should not consume input");
    expect(::read(activity_pipe[0], &received, 1) == 1 &&
               received == activity_byte,
           "activity readiness should not consume activity data");

    // HUP can be reported together with readable buffered data.  The probe
    // must not consume that data while classifying the descriptor as closed.
    const char eof_byte = 'e';
    expect(::write(input_pipe[1], &eof_byte, 1) == 1,
           "EOF probe byte should be written");
    (void)::close(input_pipe[1]);
    input_pipe[1] = -1;
    expect(TerminalEventWaiter::input_closed(input_pipe[0]),
           "writer close should report input HUP");
    expect(waiter.wait(input_pipe[0], -1, 0ms).input_ready,
           "HUP should make the input descriptor selectable");
    expect(::read(input_pipe[0], &received, 1) == 1 && received == eof_byte,
           "input_closed must not consume buffered input");
    expect(TerminalEventWaiter::input_closed(input_pipe[0]),
           "input remains closed after buffered EOF data is consumed");

    (void)::close(activity_pipe[1]);
    activity_pipe[1] = -1;
    expect(waiter.wait(-1, activity_pipe[0], 0ms).activity_ready,
           "activity HUP should be reported as activity readiness");
  }

  close_pipe(input_pipe);
  close_pipe(activity_pipe);
}

volatile sig_atomic_t sigwinch_a_count = 0;
volatile sig_atomic_t sigwinch_b_count = 0;

void sigwinch_handler_a(int) { ++sigwinch_a_count; }
void sigwinch_handler_b(int) { ++sigwinch_b_count; }

void test_signal_dispatch_inheritance_and_restoration() {
  struct sigaction original_action{};
  expect(::sigaction(SIGWINCH, nullptr, &original_action) == 0,
         "SIGWINCH action should be readable");
  sigset_t original_mask{};
  expect(::pthread_sigmask(SIG_SETMASK, nullptr, &original_mask) == 0,
         "owner signal mask should be readable");

  sigset_t baseline_mask = original_mask;
  expect(sigdelset(&baseline_mask, SIGWINCH) == 0,
         "SIGWINCH should be removable from baseline mask");
  expect(::pthread_sigmask(SIG_SETMASK, &baseline_mask, nullptr) == 0,
         "SIGWINCH should be unblocked for the restoration test");

  struct sigaction first_action{};
  expect(sigemptyset(&first_action.sa_mask) == 0,
         "first signal action mask should initialize");
  first_action.sa_handler = sigwinch_handler_a;
  expect(::sigaction(SIGWINCH, &first_action, nullptr) == 0,
         "first SIGWINCH action should install");
  sigwinch_a_count = 0;
  sigwinch_b_count = 0;

  const pthread_t owner = ::pthread_self();
  {
    TerminalEventWaiter waiter;

    sigset_t owner_mask{};
    expect(::pthread_sigmask(SIG_SETMASK, nullptr, &owner_mask) == 0,
           "owner mask should be readable while guarded");
    expect(sigismember(&owner_mask, SIGWINCH) == 1,
           "TerminalEventWaiter should block SIGWINCH in its owner");

    std::atomic<bool> worker_mask_ok{false};
    std::thread worker([&] {
      sigset_t worker_mask{};
      if (::pthread_sigmask(SIG_SETMASK, nullptr, &worker_mask) == 0) {
        worker_mask_ok.store(sigismember(&worker_mask, SIGWINCH) == 1,
                             std::memory_order_release);
      }
    });
    worker.join();
    expect(worker_mask_ok.load(std::memory_order_acquire),
           "runtime worker should inherit blocked SIGWINCH");

    struct sigaction second_action{};
    expect(sigemptyset(&second_action.sa_mask) == 0,
           "second signal action mask should initialize");
    second_action.sa_handler = sigwinch_handler_b;
    expect(::sigaction(SIGWINCH, &second_action, nullptr) == 0,
           "second SIGWINCH action should install");

    // A pending signal must interrupt a real blocking pselect even though it
    // was queued before the wait began.
    expect(::pthread_kill(owner, SIGWINCH) == 0,
           "pending SIGWINCH should be queued to the owner");
    const TerminalEventWaitResult pending = waiter.wait(-1, -1, 1s);
    expect(pending.interrupted && !pending.timed_out,
           "pending SIGWINCH should interrupt a blocking pselect");
    expect(sigwinch_b_count == 1,
           "pending SIGWINCH should reach the active action");

    // The nonblocking curses turn explicitly dispatches a pending resize
    // before calling getch().  The helper must leave SIGWINCH blocked again.
    expect(::pthread_kill(owner, SIGWINCH) == 0,
           "second pending SIGWINCH should be queued to the owner");
    waiter.dispatch_pending_signals();
    expect(sigwinch_b_count == 2,
           "explicit pending-signal dispatch should run the active action");

    std::atomic<int> signal_error{0};
    std::thread signaler([&] {
      std::this_thread::sleep_for(20ms);
      signal_error.store(::pthread_kill(owner, SIGWINCH),
                         std::memory_order_release);
    });
    const TerminalEventWaitResult interrupted = waiter.wait(-1, -1, 1s);
    signaler.join();
    expect(signal_error.load(std::memory_order_acquire) == 0,
           "SIGWINCH should be queued to the owner");
    expect(interrupted.interrupted && !interrupted.timed_out,
           "queued SIGWINCH should interrupt pselect");
    expect(sigwinch_b_count == 3,
           "the active SIGWINCH action should receive the queued signal");

    expect(::pthread_sigmask(SIG_SETMASK, nullptr, &owner_mask) == 0,
           "owner mask should be readable after interrupted wait");
    expect(sigismember(&owner_mask, SIGWINCH) == 1,
           "pselect should restore the blocked owner mask after EINTR");
  }

  struct sigaction restored_action{};
  expect(::sigaction(SIGWINCH, nullptr, &restored_action) == 0,
         "restored SIGWINCH action should be readable");
  expect(restored_action.sa_handler == sigwinch_handler_a,
         "destructor should restore the saved SIGWINCH action");
  sigset_t restored_mask{};
  expect(::pthread_sigmask(SIG_SETMASK, nullptr, &restored_mask) == 0,
         "restored owner mask should be readable");
  expect(sigismember(&restored_mask, SIGWINCH) == 0,
         "destructor should restore the saved unblocked mask");

  expect(::sigaction(SIGWINCH, &original_action, nullptr) == 0,
         "original SIGWINCH action should be restored");
  expect(::pthread_sigmask(SIG_SETMASK, &original_mask, nullptr) == 0,
         "original owner signal mask should be restored");
}

void test_fd_validation() {
  TerminalEventWaiter waiter;
  expect_invalid_argument([&] { (void)waiter.wait(-2, -1, 0ms); },
                          "input fd below -1 should be rejected before FD_SET");
  expect_invalid_argument(
      [&] { (void)waiter.wait(-1, -2, 0ms); },
      "activity fd below -1 should be rejected before FD_SET");
  expect_invalid_argument(
      [&] { (void)waiter.wait(FD_SETSIZE, -1, 0ms); },
      "input fd at FD_SETSIZE should be rejected before FD_SET");
  expect_invalid_argument(
      [&] { (void)waiter.wait(-1, FD_SETSIZE, 0ms); },
      "activity fd at FD_SETSIZE should be rejected before FD_SET");
  expect_invalid_argument([&] { (void)TerminalEventWaiter::input_closed(-1); },
                          "input_closed should reject a negative descriptor");
}

} // namespace

int main() {
  test_traits_and_timeouts();
  test_pipe_readiness_and_eof();
  test_signal_dispatch_inheritance_and_restoration();
  test_fd_validation();
  std::cout << "iamber_terminal_wait_tests ok\n";
  return 0;
}
