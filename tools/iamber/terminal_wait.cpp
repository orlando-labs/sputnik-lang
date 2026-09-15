#include "tools/iamber/terminal_wait.h"

#include <cerrno>
#include <limits>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <system_error>

#include <sys/select.h>

namespace {

[[noreturn]] void throw_errno(const char *operation) {
  throw std::system_error(errno, std::generic_category(), operation);
}

[[noreturn]] void throw_pthread_error(int error, const char *operation) {
  throw std::system_error(error, std::generic_category(), operation);
}

void validate_wait_fd(int fd, const char *which) {
  if (fd < -1 || fd >= FD_SETSIZE) {
    throw std::invalid_argument(std::string(which) +
                                " must be -1 or less than FD_SETSIZE");
  }
}

timespec make_timeout(const std::optional<std::chrono::milliseconds> &timeout) {
  timespec result{};
  if (!timeout || timeout->count() <= 0) {
    return result;
  }

  using Milliseconds = std::chrono::milliseconds;
  using Rep = Milliseconds::rep;
  const Rep count = timeout->count();
  const Rep seconds = count / static_cast<Rep>(1000);
  const Rep remainder = count % static_cast<Rep>(1000);
  const Rep max_seconds = static_cast<Rep>(std::numeric_limits<time_t>::max());
  if (seconds > max_seconds) {
    result.tv_sec = std::numeric_limits<time_t>::max();
    result.tv_nsec = 0;
    return result;
  }

  result.tv_sec = static_cast<time_t>(seconds);
  result.tv_nsec = static_cast<long>(remainder) * 1000000L;
  return result;
}

} // namespace

TerminalEventWaiter::TerminalEventWaiter() {
  sigset_t sigwinch_set{};
  if (sigemptyset(&sigwinch_set) != 0 ||
      sigaddset(&sigwinch_set, SIGWINCH) != 0) {
    throw_errno("prepare SIGWINCH mask");
  }

  // pthread_sigmask both snapshots the exact owner mask and blocks SIGWINCH
  // in one operation.  This closes the signal-arrival window before the
  // signal disposition is queried and before runtime workers can be created.
  const int mask_error =
      ::pthread_sigmask(SIG_BLOCK, &sigwinch_set, &saved_signal_mask_);
  if (mask_error != 0) {
    throw_pthread_error(mask_error, "block SIGWINCH");
  }

  if (::sigaction(SIGWINCH, nullptr, &saved_sigwinch_action_) != 0) {
    const int error = errno;
    (void)::pthread_sigmask(SIG_SETMASK, &saved_signal_mask_, nullptr);
    errno = error;
    throw_errno("save SIGWINCH action");
  }

  dispatch_signal_mask_ = saved_signal_mask_;
  if (sigdelset(&dispatch_signal_mask_, SIGWINCH) != 0) {
    const int error = errno;
    (void)::pthread_sigmask(SIG_SETMASK, &saved_signal_mask_, nullptr);
    errno = error;
    throw_errno("prepare SIGWINCH dispatch mask");
  }
  initialized_ = true;
}

TerminalEventWaiter::~TerminalEventWaiter() noexcept {
  if (!initialized_) {
    return;
  }

  // Keep this order deliberate.  If curses installed/changed its handler,
  // restore the saved disposition before unblocking any pending SIGWINCH.
  (void)::sigaction(SIGWINCH, &saved_sigwinch_action_, nullptr);
  (void)::pthread_sigmask(SIG_SETMASK, &saved_signal_mask_, nullptr);
}

TerminalEventWaitResult TerminalEventWaiter::wait(
    int input_fd, int activity_fd,
    std::optional<std::chrono::milliseconds> timeout) const {
  validate_wait_fd(input_fd, "input_fd");
  validate_wait_fd(activity_fd, "activity_fd");

  fd_set read_fds;
  FD_ZERO(&read_fds);
  int highest_fd = -1;
  if (input_fd >= 0) {
    FD_SET(input_fd, &read_fds);
    highest_fd = input_fd;
  }
  if (activity_fd >= 0) {
    FD_SET(activity_fd, &read_fds);
    if (activity_fd > highest_fd) {
      highest_fd = activity_fd;
    }
  }

  timespec timeout_spec{};
  timespec *timeout_pointer = nullptr;
  if (timeout.has_value()) {
    timeout_spec = make_timeout(timeout);
    timeout_pointer = &timeout_spec;
  }

  const int result = ::pselect(highest_fd + 1, &read_fds, nullptr, nullptr,
                               timeout_pointer, &dispatch_signal_mask_);
  const int selected = result;
  if (selected < 0) {
    if (errno == EINTR) {
      TerminalEventWaitResult interrupted;
      interrupted.interrupted = true;
      return interrupted;
    }
    throw_errno("pselect");
  }

  TerminalEventWaitResult waited;
  waited.input_ready = input_fd >= 0 && FD_ISSET(input_fd, &read_fds);
  waited.activity_ready = activity_fd >= 0 && FD_ISSET(activity_fd, &read_fds);
  waited.timed_out = selected == 0;
  return waited;
}

void TerminalEventWaiter::dispatch_pending_signals() const {
  sigset_t sigwinch_set{};
  if (sigemptyset(&sigwinch_set) != 0 ||
      sigaddset(&sigwinch_set, SIGWINCH) != 0) {
    throw_errno("prepare SIGWINCH dispatch");
  }

  sigset_t owner_mask{};
  const int unblock_error =
      ::pthread_sigmask(SIG_UNBLOCK, &sigwinch_set, &owner_mask);
  if (unblock_error != 0) {
    throw_pthread_error(unblock_error, "dispatch SIGWINCH");
  }
  const int restore_error =
      ::pthread_sigmask(SIG_SETMASK, &owner_mask, nullptr);
  if (restore_error != 0) {
    throw_pthread_error(restore_error, "restore SIGWINCH mask");
  }
}

bool TerminalEventWaiter::input_closed(int fd) {
  if (fd < 0) {
    throw std::invalid_argument("input_fd must be nonnegative");
  }

  // Request readability as the wake condition: on some POSIX poll
  // implementations a zero-events entry does not surface EOF/HUP, even
  // though the descriptor is closed.  poll never consumes the bytes.
  pollfd polled{fd, POLLIN, 0};
  const int result = ::poll(&polled, 1, 0);
  if (result < 0) {
    if (errno == EINTR) {
      return false;
    }
    throw_errno("poll input descriptor");
  }
  return result > 0 && (polled.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0;
}
