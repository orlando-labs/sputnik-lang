#include "tools/iamber/activity.h"

#include <condition_variable>
#include <mutex>
#include <utility>

#if defined(__unix__) || defined(__APPLE__)
#include <cerrno>
#include <fcntl.h>
#include <system_error>
#include <unistd.h>
#endif

struct SessionActivityState {
  std::mutex mutex;
  std::condition_variable condition;
  bool runtime_ready = false;
  bool input_ready = false;
  bool closed = false;

#if defined(__unix__) || defined(__APPLE__)
  int read_descriptor = -1;
  int write_descriptor = -1;

  ~SessionActivityState() {
    // Both ends live as long as any waiter or in-flight notifier. A notifier
    // can never write after the read end closes, avoiding SIGPIPE/fd reuse.
    if (read_descriptor >= 0) {
      ::close(read_descriptor);
      ::close(write_descriptor);
    }
  }

  void signal_descriptor_locked() noexcept {
    if (write_descriptor >= 0) {
      const char byte = 1;
      while (::write(write_descriptor, &byte, 1U) < 0 && errno == EINTR) {
      }
      // EAGAIN means readiness is already latched. Other errors require
      // violating the borrowed-descriptor contract; no user callbacks run.
    }
  }

  void drain_descriptor_locked() noexcept {
    if (read_descriptor >= 0) {
      char bytes[64];
      while (true) {
        const auto count = ::read(read_descriptor, bytes, sizeof(bytes));
        if (count > 0 || (count < 0 && errno == EINTR)) {
          continue;
        }
        break;
      }
    }
  }

  int attach_descriptor_locked() {
    if (read_descriptor >= 0) {
      return read_descriptor;
    }
    int descriptors[2];
#if defined(__linux__)
    if (::pipe2(descriptors, O_NONBLOCK | O_CLOEXEC) != 0) {
      throw std::system_error(errno, std::generic_category(), "activity pipe");
    }
#else
    if (::pipe(descriptors) != 0) {
      throw std::system_error(errno, std::generic_category(), "activity pipe");
    }
    // Attach before starting workers on platforms without atomic pipe2.
    for (const int descriptor : descriptors) {
      if (::fcntl(descriptor, F_SETFL, O_NONBLOCK) < 0 ||
          ::fcntl(descriptor, F_SETFD, FD_CLOEXEC) < 0) {
        const int error = errno;
        ::close(descriptors[0]);
        ::close(descriptors[1]);
        throw std::system_error(error, std::generic_category(),
                                "activity pipe flags");
      }
    }
#endif
    read_descriptor = descriptors[0];
    write_descriptor = descriptors[1];
    if (closed || runtime_ready || input_ready) {
      signal_descriptor_locked();
    }
    return read_descriptor;
  }
#else
  void signal_descriptor_locked() noexcept {}
  void drain_descriptor_locked() noexcept {}
  int attach_descriptor_locked() { return -1; }
#endif
};

SessionActivityNotifier::SessionActivityNotifier(
    std::weak_ptr<SessionActivityState> state)
    : state_(std::move(state)) {}

SessionActivityNotifier SessionActivityNotifier::with_wakeup(
    const SessionActivityNotifier &host) const noexcept {
  SessionActivityNotifier notifier(*this);
  // Copy only host's local state. In particular, do not copy host.host_state_:
  // forwarding is deliberately one level and cannot form a notification
  // chain.
  notifier.host_state_ = host.state_;
  return notifier;
}

bool SessionActivityNotifier::notify(bool runtime) const noexcept {
  const auto local_state = state_.lock();
  if (!local_state) {
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(local_state->mutex);
    if (local_state->closed) {
      return false;
    }
    bool &pending =
        runtime ? local_state->runtime_ready : local_state->input_ready;
    const bool already_ready =
        local_state->runtime_ready || local_state->input_ready;
    pending = true;
    if (!already_ready) {
      local_state->signal_descriptor_locked();
    }
  }

  // Notify the local mailbox first, then release its mutex before looking at
  // the optional host mailbox. This ordering makes it impossible for this
  // method to hold both state mutexes at once.
  local_state->condition.notify_all();

  const auto host_state = host_state_.lock();
  if (host_state && host_state.get() != local_state.get()) {
    bool host_open = false;
    {
      std::lock_guard<std::mutex> lock(host_state->mutex);
      if (!host_state->closed) {
        bool &pending =
            runtime ? host_state->runtime_ready : host_state->input_ready;
        const bool already_ready =
            host_state->runtime_ready || host_state->input_ready;
        pending = true;
        if (!already_ready) {
          host_state->signal_descriptor_locked();
        }
        host_open = true;
      }
    }
    if (host_open) {
      host_state->condition.notify_all();
    }
  }

  return true;
}

bool SessionActivityNotifier::notify_runtime() const noexcept {
  return notify(true);
}

bool SessionActivityNotifier::notify_input() const noexcept {
  return notify(false);
}

SessionActivityWaiter::SessionActivityWaiter(
    std::shared_ptr<SessionActivityState> state)
    : state_(std::move(state)) {}

int SessionActivityWaiter::poll_descriptor() const {
  if (!state_) {
    return -1;
  }
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->attach_descriptor_locked();
}

SessionActivityResult
SessionActivityWaiter::wait(std::chrono::milliseconds timeout) const {
  SessionActivityResult result;
  if (!state_) {
    result.shutdown = true;
    return result;
  }
  std::unique_lock<std::mutex> lock(state_->mutex);
  const auto ready = [this] {
    return state_->closed || state_->runtime_ready || state_->input_ready;
  };
  if (!ready()) {
    if (timeout <= std::chrono::milliseconds::zero()) {
      result.timed_out = true;
      return result;
    }
    // Predicate wait_for may overflow while adding a very large duration.
    // Compare in milliseconds before converting to the clock's finer ticks.
    using Clock = std::chrono::steady_clock;
    const auto now = Clock::now();
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::time_point::max() - now);
    const auto deadline =
        timeout >= remaining ? Clock::time_point::max() : now + timeout;
    if (!state_->condition.wait_until(lock, deadline, ready)) {
      result.timed_out = true;
      return result;
    }
  }
  if (state_->closed) {
    result.shutdown = true;
    return result;
  }
  result.runtime_ready = std::exchange(state_->runtime_ready, false);
  result.input_ready = std::exchange(state_->input_ready, false);
  state_->drain_descriptor_locked();
  return result;
}

SessionActivityChannel::SessionActivityChannel()
    : state_(std::make_shared<SessionActivityState>()) {}

SessionActivityChannel::~SessionActivityChannel() { close(); }

SessionActivityChannel::SessionActivityChannel(
    SessionActivityChannel &&other) noexcept
    : state_(std::move(other.state_)) {}

SessionActivityChannel &
SessionActivityChannel::operator=(SessionActivityChannel &&other) noexcept {
  if (this != &other) {
    close();
    state_ = std::move(other.state_);
  }
  return *this;
}

SessionActivityNotifier SessionActivityChannel::notifier() const noexcept {
  return SessionActivityNotifier(state_);
}

SessionActivityWaiter SessionActivityChannel::waiter() const noexcept {
  return SessionActivityWaiter(state_);
}

void SessionActivityChannel::close() noexcept {
  if (!state_) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->closed) {
      return;
    }
    const bool already_ready = state_->runtime_ready || state_->input_ready;
    state_->closed = true;
    state_->runtime_ready = false;
    state_->input_ready = false;
    if (!already_ready) {
      state_->signal_descriptor_locked();
    }
  }
  state_->condition.notify_all();
}
