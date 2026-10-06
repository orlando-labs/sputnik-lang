#pragma once

#include "runtime/watch.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <vector>

namespace sputnik::runtime {

// One bounded event source shared by every RuntimeState generation belonging
// to a RuntimeWorld. Keeping ids and the event sequence here prevents package
// reload copies, scheduler VMs, and parked child tasks from forking them.
class RuntimeWatchStream {
public:
  static constexpr std::size_t kDefaultCapacity = 65536U;

  RuntimeWatchStream();

  void configure_activity_notifier(RuntimeWatchActivityNotifier notifier);
  void configure_capacity(std::size_t capacity);
  std::uint64_t allocate_cell_id();
  std::uint64_t allocate_handle_id();
  RuntimeWatchEvent record(RuntimeWatchEvent event);

  RuntimeWatchStreamIdentity identity() const;
  std::uint64_t latest_epoch() const;
  RuntimeWatchCursor tail_cursor() const;
  RuntimeWatchPollResult poll(const RuntimeWatchCursor &cursor,
                              std::size_t max_events) const;
  RuntimeWatchPollResult
  wait(const RuntimeWatchCursor &cursor, std::chrono::milliseconds timeout,
       std::size_t max_events) const;
  std::vector<RuntimeWatchEvent> events_snapshot() const;

private:
  std::uint64_t oldest_retained_epoch_locked() const noexcept;
  bool wait_ready_locked(const RuntimeWatchCursor &cursor) const noexcept;
  RuntimeWatchPollResult
  poll_locked(const RuntimeWatchCursor &cursor,
              std::size_t max_events) const;
  void trim_locked();

  mutable std::mutex mutex_;
  mutable std::condition_variable condition_;
  RuntimeWatchStreamIdentity identity_;
  std::size_t capacity_ = kDefaultCapacity;
  std::uint64_t latest_epoch_ = 0;
  std::uint64_t next_cell_id_ = 1;
  std::uint64_t next_handle_id_ = 1;
  std::deque<RuntimeWatchEvent> events_;
  std::shared_ptr<const RuntimeWatchActivityNotifier> activity_notifier_;
};

} // namespace sputnik::runtime
