#pragma once

#include <chrono>
#include <optional>

struct RuntimeEventPumpResult;

// Owner-thread scheduling only. No timers, threads, Session, or runtime state.
// The terminal adapter services stdin between bounded runtime pump turns.
class RuntimePumpDispatch {
public:
  using Clock = std::chrono::steady_clock;

  void notify_runtime() noexcept;
  bool pending() const noexcept;
  bool due(Clock::time_point now) const noexcept;
  // Null means genuinely idle: the host can block indefinitely on input and
  // activity. During a pending cooldown, the terminal waits on stdin/signals
  // plus this deadline, not the already-known runtime source (avoids a spin).
  std::optional<std::chrono::milliseconds>
  wait_timeout(Clock::time_point now) const noexcept;
  void complete(const RuntimeEventPumpResult &result,
                Clock::time_point now) noexcept;

private:
  bool pending_ = false;
  Clock::time_point deadline_{};
  std::chrono::milliseconds retry_delay_{100};
};
