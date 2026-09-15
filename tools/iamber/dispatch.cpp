#include "tools/iamber/dispatch.h"

#include "tools/iamber/session.h"

#include <algorithm>

void RuntimePumpDispatch::notify_runtime() noexcept { pending_ = true; }

bool RuntimePumpDispatch::pending() const noexcept { return pending_; }

bool RuntimePumpDispatch::due(Clock::time_point now) const noexcept {
  return pending_ && now >= deadline_;
}

std::optional<std::chrono::milliseconds>
RuntimePumpDispatch::wait_timeout(Clock::time_point now) const noexcept {
  if (!pending_) {
    return std::nullopt;
  }
  if (now >= deadline_) {
    return std::chrono::milliseconds::zero();
  }
  return std::chrono::ceil<std::chrono::milliseconds>(deadline_ - now);
}

void RuntimePumpDispatch::complete(const RuntimeEventPumpResult &result,
                                   Clock::time_point now) noexcept {
  if (result.needs_recovery()) {
    pending_ = true;
    deadline_ = now + retry_delay_;
    retry_delay_ = std::min(retry_delay_ * 2, std::chrono::milliseconds(1000));
    return;
  }
  retry_delay_ = std::chrono::milliseconds(100);
  pending_ = result.drain_limit_reached ||
             result.disposition == RuntimeEventPumpDisposition::Busy;
  deadline_ = pending_ ? now + std::chrono::milliseconds(16) : now;
}
