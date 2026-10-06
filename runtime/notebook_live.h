#pragma once

#include "runtime/notebook_display.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace sputnik::runtime {

// Live notebook values deliberately contain no Value, heap pointer, or VM
// object.  A host can retain a snapshot after the VM and its image have gone
// away.  Figure bytes are already immutable PNG bytes at this boundary.
enum class NotebookLiveEventKind { Progress, Figure };

struct NotebookLiveEvent {
  NotebookLiveEventKind kind = NotebookLiveEventKind::Progress;
  std::uint64_t cell_id = 0;
  std::string id;
  std::uint64_t sequence = 0;
  std::uint64_t timestamp_ms = 0;

  // Progress payload.  These fields are ignored for Figure events.
  double current = 0;
  double total = 0;
  std::string description;
  bool done = false;
  // Duration since this progress id's first accepted update in this run.
  std::uint64_t elapsed_ms = 0;

  // Figure payload.  This is ignored for Progress events.
  NotebookDisplay display;
};

enum class NotebookLivePublishResult {
  Accepted,
  Throttled,
  Closed,
  Invalid,
  LimitExceeded,
};

// One run-local immutable snapshot store.  Entries are replacement panels:
// publishing the same kind/cell/id updates that panel while retaining its
// insertion position.  A closed store accepts no further events and releases
// all retained image/text data, which prevents detached work from leaking into
// a subsequent cell run.
class NotebookLiveStore final
    : public std::enable_shared_from_this<NotebookLiveStore> {
public:
  static constexpr std::size_t kMaxEntries = 128;
  static constexpr std::size_t kMaxFigureBytes = 64U * 1024U * 1024U;
  static constexpr std::size_t kMaxFigureBytesPerEntry = 16U * 1024U * 1024U;
  static constexpr std::size_t kMaxTextBytes = 256U * 1024U;
  static constexpr std::size_t kMaxIdBytes = 4096;
  static constexpr std::size_t kMaxDescriptionBytes = 65536;

  NotebookLiveStore() = default;
  NotebookLiveStore(const NotebookLiveStore &) = delete;
  NotebookLiveStore &operator=(const NotebookLiveStore &) = delete;

  bool active() const {
    std::lock_guard<std::mutex> lock(mutex_);
    // A run sink is deliberately shareable with a reporter or worker thread;
    // closure is the lifetime boundary, not the producer thread.
    return open_;
  }

  bool open() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return open_;
  }

  // Check this before rendering a figure.  It only reads the last accepted
  // frame and never reserves a slot, so a failed render does not consume the
  // throttle window.  throttle_ms == 0 always permits a terminal frame.
  bool figure_due(std::uint64_t cell_id, const std::string &id,
                  std::uint64_t throttle_ms,
                  std::uint64_t now_ms = monotonic_millis(),
                  std::uint64_t generation = 0) const {
    if (id.empty() || id.size() > kMaxIdBytes) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_) return false;
    if (generation != 0) {
      const auto found = generations_.find(cell_id);
      if (found == generations_.end() || found->second != generation)
        return false;
    }
    const Entry *entry = find_locked(NotebookLiveEventKind::Figure, cell_id, id);
    if (entry == nullptr || throttle_ms == 0) return true;
    if (now_ms < entry->event.timestamp_ms) return true;
    return now_ms - entry->event.timestamp_ms >= throttle_ms;
  }

  NotebookLivePublishResult publish_progress(
      std::uint64_t cell_id, std::string id, double current,
      double total, std::string description, bool done,
      std::uint64_t now_ms = monotonic_millis(),
      std::uint64_t generation = 0) {
    if (id.empty() || id.size() > kMaxIdBytes ||
        description.size() > kMaxDescriptionBytes ||
        description.size() + id.size() > kMaxTextBytes ||
        !std::isfinite(current) || !std::isfinite(total) || current < 0 ||
        total < 0) {
      return NotebookLivePublishResult::Invalid;
    }
    NotebookLiveEvent event;
    event.kind = NotebookLiveEventKind::Progress;
    event.cell_id = cell_id;
    event.id = std::move(id);
    event.timestamp_ms = now_ms;
    event.current = current;
    event.total = total;
    event.description = std::move(description);
    event.done = done;
    return publish(std::move(event), 0, now_ms, generation);
  }

  // `throttle_ms` is checked before the caller renders via figure_due().  The
  // second check here closes the race between concurrent producers.  Passing
  // zero intentionally accepts the terminal frame even if the prior frame is
  // recent; notebook.show documents this as the explicit final update.
  NotebookLivePublishResult publish_figure(
      std::uint64_t cell_id, std::string id, NotebookDisplay display,
      std::uint64_t throttle_ms = 0,
      std::uint64_t now_ms = monotonic_millis(),
      std::uint64_t generation = 0) {
    if (id.empty() || id.size() > kMaxIdBytes ||
        display.storage_bytes() > kMaxFigureBytesPerEntry || display.plot_scene.size() > 4U * 1024U * 1024U) {
      return NotebookLivePublishResult::Invalid;
    }
    NotebookLiveEvent event;
    event.kind = NotebookLiveEventKind::Figure;
    event.cell_id = cell_id;
    event.id = std::move(id);
    event.timestamp_ms = now_ms;
    event.display = std::move(display);
    return publish(std::move(event), throttle_ms, now_ms, generation);
  }

  // Returns independent data.  No mutex or mutable store is retained by the
  // caller, so a UI may render it after close().
  std::vector<NotebookLiveEvent> snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<NotebookLiveEvent> result;
    result.reserve(entries_.size());
    for (const auto &entry : entries_) result.push_back(entry.event);
    return result;
  }

  // Avoid repeatedly copying large PNGs when no producer changed anything.
  // Cell resets are changes too, even when the resulting snapshot is empty.
  bool snapshot_if_changed(std::uint64_t *revision,
                           std::vector<NotebookLiveEvent> *result) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (*revision == sequence_) return false;
    result->clear();
    result->reserve(entries_.size());
    for (const auto &entry : entries_) result->push_back(entry.event);
    *revision = sequence_;
    return true;
  }

  // Snapshot only one cell's replacement panels.  This keeps the execution
  // result independent when a host deliberately shares one store across a
  // multi-cell run.
  std::vector<NotebookLiveEvent> snapshot_cell(std::uint64_t cell_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<NotebookLiveEvent> result;
    for (const auto &entry : entries_) {
      if (entry.key.cell_id == cell_id) result.push_back(entry.event);
    }
    return result;
  }

  // Hosts call this before re-executing a cell id in a shared run.  Other
  // cells and their already accepted panels remain intact.
  std::uint64_t begin_cell(std::uint64_t cell_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_) return 0;
    const std::uint64_t generation = ++generations_[cell_id];
    ++sequence_;
    auto it = entries_.begin();
    while (it != entries_.end()) {
      if (it->key.cell_id != cell_id) {
        ++it;
        continue;
      }
      figure_bytes_ -= it->event.kind == NotebookLiveEventKind::Figure
          ? it->event.display.storage_bytes() : 0;
      text_bytes_ -= text_size(it->event);
      it = entries_.erase(it);
    }
    return generation;
  }

  void reset_cell(std::uint64_t cell_id) {
    (void)begin_cell(cell_id);
  }

  std::uint64_t cell_generation(std::uint64_t cell_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = generations_.find(cell_id);
    return found == generations_.end() ? 0 : found->second;
  }

  std::size_t size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
  }

  void close() {
    std::lock_guard<std::mutex> lock(mutex_);
    open_ = false;
    entries_.clear();
    figure_bytes_ = 0;
    text_bytes_ = 0;
  }

  static std::uint64_t monotonic_millis() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<
        std::chrono::milliseconds>(now).count());
  }

private:
  struct EntryKey {
    NotebookLiveEventKind kind;
    std::uint64_t cell_id;
    std::string id;
  };
  struct Entry {
    EntryKey key;
    NotebookLiveEvent event;
    std::uint64_t start_ms = 0;
  };

  static std::size_t text_size(const NotebookLiveEvent &event) {
    return event.id.size() +
           (event.kind == NotebookLiveEventKind::Progress
                ? event.description.size()
                : event.display.caption.size());
  }

  Entry *find_locked(NotebookLiveEventKind kind, std::uint64_t cell_id,
                     const std::string &id) {
    for (auto &entry : entries_) {
      if (entry.key.kind == kind && entry.key.cell_id == cell_id &&
          entry.key.id == id)
        return &entry;
    }
    return nullptr;
  }
  const Entry *find_locked(NotebookLiveEventKind kind, std::uint64_t cell_id,
                           const std::string &id) const {
    for (const auto &entry : entries_) {
      if (entry.key.kind == kind && entry.key.cell_id == cell_id &&
          entry.key.id == id)
        return &entry;
    }
    return nullptr;
  }

  NotebookLivePublishResult publish(NotebookLiveEvent event,
                                    std::uint64_t throttle_ms,
                                    std::uint64_t now_ms,
                                    std::uint64_t generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_) return NotebookLivePublishResult::Closed;
    if (generation != 0) {
      const auto found = generations_.find(event.cell_id);
      if (found == generations_.end() || found->second != generation)
        return NotebookLivePublishResult::Closed;
    }
    if (event.kind == NotebookLiveEventKind::Figure && throttle_ms != 0) {
      const Entry *old = find_locked(event.kind, event.cell_id, event.id);
      if (old != nullptr && event.timestamp_ms >= old->event.timestamp_ms &&
          event.timestamp_ms - old->event.timestamp_ms < throttle_ms)
        return NotebookLivePublishResult::Throttled;
    }

    const std::size_t event_figure_bytes =
        event.kind == NotebookLiveEventKind::Figure ? event.display.storage_bytes() : 0;
    const std::size_t event_text_bytes = text_size(event);
    Entry *old = find_locked(event.kind, event.cell_id, event.id);
    if (old == nullptr && entries_.size() >= kMaxEntries)
      return NotebookLivePublishResult::LimitExceeded;
    const std::size_t previous_figure_bytes = old == nullptr
        ? 0 : (old->event.kind == NotebookLiveEventKind::Figure
                   ? old->event.display.storage_bytes() : 0);
    const std::size_t previous_text_bytes = old == nullptr ? 0 : text_size(old->event);
    if (figure_bytes_ - previous_figure_bytes + event_figure_bytes > kMaxFigureBytes ||
        text_bytes_ - previous_text_bytes + event_text_bytes > kMaxTextBytes)
      return NotebookLivePublishResult::LimitExceeded;

    event.sequence = ++sequence_;
    if (old != nullptr) {
      event.elapsed_ms = now_ms >= old->start_ms ? now_ms - old->start_ms : 0;
      old->event = std::move(event);
    } else {
      EntryKey key{event.kind, event.cell_id, event.id};
      entries_.push_back(Entry{std::move(key), std::move(event), now_ms});
    }
    figure_bytes_ = figure_bytes_ - previous_figure_bytes + event_figure_bytes;
    text_bytes_ = text_bytes_ - previous_text_bytes + event_text_bytes;
    return NotebookLivePublishResult::Accepted;
  }

  mutable std::mutex mutex_;
  bool open_ = true;
  std::uint64_t sequence_ = 0;
  std::size_t figure_bytes_ = 0;
  std::size_t text_bytes_ = 0;
  std::vector<Entry> entries_;
  std::unordered_map<std::uint64_t, std::uint64_t> generations_;
};

// A scope propagates the current run sink through ordinary native/module code
// without coupling that code to Value or to the VM.  It is intentionally
// thread-local: hosts that execute detached work must create a scope on that
// worker explicitly, and the closed store still rejects late events.
class NotebookLiveScope final {
private:
  struct State {
    std::shared_ptr<NotebookLiveStore> store;
    std::uint64_t cell_id;
    std::uint64_t generation;
    State() : cell_id(0), generation(0) {}
    State(std::shared_ptr<NotebookLiveStore> next_store,
          std::uint64_t next_cell_id, std::uint64_t next_generation)
        : store(std::move(next_store)), cell_id(next_cell_id),
          generation(next_generation) {}
  };
  State previous_;
  static inline thread_local State current_{};

public:
  explicit NotebookLiveScope(std::shared_ptr<NotebookLiveStore> store,
                             std::uint64_t cell_id = 0,
                             std::uint64_t generation = 0)
      : previous_(current_) {
    current_ = State{std::move(store), cell_id, generation};
    if (current_.store != nullptr && current_.generation == 0)
      current_.generation = current_.store->cell_generation(cell_id);
  }
  NotebookLiveScope(const NotebookLiveScope &) = delete;
  NotebookLiveScope &operator=(const NotebookLiveScope &) = delete;
  ~NotebookLiveScope() { current_ = std::move(previous_); }

  static std::shared_ptr<NotebookLiveStore> store() { return current_.store; }
  static std::shared_ptr<NotebookLiveStore> current() { return current_.store; }
  static std::uint64_t cell_id() { return current_.cell_id; }
  static std::uint64_t current_cell_id() { return current_.cell_id; }
  static std::uint64_t cell_generation() { return current_.generation; }

  static NotebookLivePublishResult progress(
      std::string id, double current, double total,
      std::string description = {}, bool done = false) {
    auto sink = store();
    if (!sink) return NotebookLivePublishResult::Closed;
    return sink->publish_progress(cell_id(), std::move(id), current, total,
                                  std::move(description), done,
                                  NotebookLiveStore::monotonic_millis(),
                                  cell_generation());
  }

};

} // namespace sputnik::runtime
