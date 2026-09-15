#pragma once

#include <chrono>
#include <optional>

#include <signal.h>

struct TerminalEventWaitResult {
  bool input_ready = false;
  bool activity_ready = false;
  bool interrupted = false;
  bool timed_out = false;
};

// Owner-thread wait helper for the curses terminal loop.  Construction must
// happen before creating runtime workers: SIGWINCH is blocked in the owner and
// that blocked mask is inherited by subsequently-created threads.  The object
// is intentionally non-copyable/non-movable because its saved signal state
// belongs to the constructing thread.
//
// The caller must keep this object alive until after curses and its Session
// have been destroyed.  The destructor restores the SIGWINCH disposition
// first, then the constructing thread's original signal mask.
class TerminalEventWaiter {
public:
  TerminalEventWaiter();
  ~TerminalEventWaiter() noexcept;

  TerminalEventWaiter(const TerminalEventWaiter &) = delete;
  TerminalEventWaiter &operator=(const TerminalEventWaiter &) = delete;
  TerminalEventWaiter(TerminalEventWaiter &&) = delete;
  TerminalEventWaiter &operator=(TerminalEventWaiter &&) = delete;

  // Wait for terminal input, activity notifications, or SIGWINCH.  A file
  // descriptor of -1 omits that source.  A non-null nonpositive timeout is a
  // zero-time readiness check.  EINTR is reported in the result and is never
  // retried.  Other invalid descriptors (< -1 or >= FD_SETSIZE) throw before
  // FD_SET.
  TerminalEventWaitResult
  wait(int input_fd, int activity_fd,
       std::optional<std::chrono::milliseconds> timeout) const;

  // Temporarily unblock SIGWINCH on this owner thread so pending delivery can
  // be handled, then restore the prior mask.  Call this before nonblocking
  // curses input; blocking wait() also dispatches SIGWINCH atomically through
  // pselect.  Owner-thread only; a concurrent storm of other signals may
  // defer a particular SIGWINCH until the next dispatch point.
  void dispatch_pending_signals() const;

  // Probe a terminal/input descriptor without consuming bytes.  HUP, ERR,
  // and NVAL are treated as closed.  This uses a zero-time poll and throws on
  // a negative descriptor; a descriptor returned by poll as POLLNVAL is
  // therefore reported as closed.
  static bool input_closed(int fd);

private:
  struct sigaction saved_sigwinch_action_{};
  sigset_t saved_signal_mask_{};
  sigset_t dispatch_signal_mask_{};
  bool initialized_ = false;
};
