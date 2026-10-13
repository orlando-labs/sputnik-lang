#pragma once

#include <chrono>
#include <memory>

// Notification-only mailbox. It never holds a Session, runtime Value, kernel,
// or watch cursor; consuming a hint is not acknowledgement of runtime events.
struct SessionActivityState;

struct SessionActivityResult {
  bool runtime_ready = false;
  bool input_ready = false;
  bool shutdown = false;
  bool timed_out = false;
};

class SessionActivityNotifier {
public:
  SessionActivityNotifier() = default;
  // Thread-safe, coalesced, allocation-free hints. False means that the owner
  // has closed/expired. Callers enqueue input before notifying. These methods
  // use a short mutex critical section and are NOT async-signal-safe.
  bool notify_runtime() const noexcept;
  bool notify_input() const noexcept;

  // Return a notifier that keeps this notifier's local mailbox and also
  // forwards each accepted hint to host's local mailbox. Forwarding is
  // intentionally one level: host's own forwarding target is ignored.
  SessionActivityNotifier
  with_wakeup(const SessionActivityNotifier &host) const noexcept;

private:
  friend class SessionActivityChannel;
  explicit SessionActivityNotifier(std::weak_ptr<SessionActivityState> state);
  bool notify(bool runtime) const noexcept;
  std::weak_ptr<SessionActivityState> state_;
  std::weak_ptr<SessionActivityState> host_state_;
};

class SessionActivityWaiter {
public:
  SessionActivityWaiter() = default;
  // Atomically consumes input/runtime hints, including ones delivered before
  // wait starts. One dispatch consumer should own wait(); copied handles may
  // outlive the Session safely. Close wakes all waiters and returns sticky
  // Shutdown, discarding pending hints. Empty handles also return Shutdown.
  // Nonpositive timeout is a nonblocking check; oversized durations saturate
  // at the steady clock's maximum deadline instead of overflowing.
  SessionActivityResult wait(std::chrono::milliseconds timeout) const;

  // POSIX event-loop integration; lazy, may throw on pipe/fcntl failure.
  // Obtain on the owner thread before starting workers. Returns a borrowed,
  // nonblocking close-on-exec read descriptor, valid while this waiter or a
  // copy lives. Poll it but NEVER read from or close it: wait() atomically
  // consumes both the hints and pipe readiness. Shutdown stays readable.
  // Returns -1 for empty handles or platforms without POSIX support.
  int poll_descriptor() const;

private:
  friend class SessionActivityChannel;
  explicit SessionActivityWaiter(std::shared_ptr<SessionActivityState> state);
  std::shared_ptr<SessionActivityState> state_;
};

class SessionActivityChannel {
public:
  SessionActivityChannel();
  ~SessionActivityChannel();
  SessionActivityChannel(const SessionActivityChannel &) = delete;
  SessionActivityChannel &operator=(const SessionActivityChannel &) = delete;
  SessionActivityChannel(SessionActivityChannel &&other) noexcept;
  SessionActivityChannel &operator=(SessionActivityChannel &&other) noexcept;

  // Obtain handles only on the owner's thread, before dispatching workers.
  SessionActivityNotifier notifier() const noexcept;
  SessionActivityWaiter waiter() const noexcept;
  void close() noexcept;

private:
  std::shared_ptr<SessionActivityState> state_;
};
