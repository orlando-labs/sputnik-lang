#pragma once

#include "runtime/notebook_display.h"
#include "runtime/notebook_chart.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <condition_variable>
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
// away. Figure payloads are immutable PNGs or scene-only board frames.
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
  ~NotebookLiveStore() { close(); }
  NotebookLiveStore(const NotebookLiveStore &) = delete;
  NotebookLiveStore &operator=(const NotebookLiveStore &) = delete;

  std::uint64_t begin_chart(std::uint64_t cell, std::uint64_t generation, NotebookChartStyle style) {
    if (style.id.empty() || style.id.size() > kMaxIdBytes || style.width < 160 || style.height < 160 ||
        style.width > 8192 || style.height > 8192 || std::uint64_t(style.width) * style.height > 16000000 ||
        style.throttle_ms > 3600000 || !std::isfinite(style.stroke_width) || style.stroke_width <= 0 || style.stroke_width > 1024)
      throw std::invalid_argument("invalid chart style");
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_ || generations_[cell] != generation) throw std::invalid_argument("chart run is closed or stale");
    if (charts_.size() >= 64) throw std::invalid_argument("chart count exceeds 64");
    for (const auto &c : charts_) if (c.snapshot.cell_id == cell && c.snapshot.style.id == style.id)
      throw std::invalid_argument("chart id is already active in this cell");
    Chart chart;
    chart.snapshot.style = std::move(style); chart.snapshot.cell_id = cell;
    chart.snapshot.generation = generation; chart.snapshot.handle = ++next_chart_handle_;
    chart.snapshot.revision = 1; chart.snapshot.tail.reserve(256);
    charts_.push_back(std::move(chart));
    if (!renderer_.joinable()) {
      try { renderer_ = std::thread([this] { render_charts(); }); }
      catch (...) { charts_.pop_back(); throw; }
    }
    wake_.notify_all();
    return charts_.back().snapshot.handle;
  }

  // Amortized O(1): sealed chunks are shared with the renderer. A snapshot
  // copies at most 255 points under the producer lock, never the full history.
  void append_chart(std::uint64_t handle, NotebookChartPoint point) {
    if (!std::isfinite(point[0]) || !std::isfinite(point[1]) ||
        std::abs(point[0]) >= 1e100 || std::abs(point[1]) >= 1e100)
      throw std::invalid_argument("chart points must be finite with magnitude below 1e100");
    std::lock_guard<std::mutex> lock(mutex_);
    auto &c = chart_locked(handle);
    if (c.finished) throw std::invalid_argument("chart has ended");
    auto &s = c.snapshot;
    // Live components keep a bounded rolling window. Long training must not
    // fail merely because its visualization outlives the display budget.
    if (s.count >= 65536 && !s.chunks.empty()) {
      const auto retired = s.chunks.front()->size();
      s.chunks.pop_front(); s.count -= retired; chart_points_ -= retired;
    }
    if (chart_points_ >= 524288) throw std::invalid_argument("run chart point budget exceeded");
    if (s.count == 0) s.bounds = {point[0], point[1], point[0], point[1]};
    else {
      s.bounds[0] = std::min(s.bounds[0], point[0]); s.bounds[1] = std::min(s.bounds[1], point[1]);
      s.bounds[2] = std::max(s.bounds[2], point[0]); s.bounds[3] = std::max(s.bounds[3], point[1]);
    }
    s.tail.push_back(point); ++s.count; ++s.total_points; ++chart_points_; ++s.revision;
    if (s.tail.size() == 256) {
      s.chunks.push_back(std::make_shared<const NotebookChartChunk>(std::move(s.tail)));
      s.tail = {}; s.tail.reserve(256);
    }
    // The renderer wakes periodically. Per-point notifications would put the
    // render thread in competition with a high-frequency producer.
  }

  void end_chart(std::uint64_t handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto &c = chart_locked(handle); c.finished = true; wake_.notify_all();
  }

  struct ChartResult { std::string id; NotebookDisplay display; NotebookDisplayOrder order; };
  std::vector<ChartResult> finish_charts(std::uint64_t cell, std::uint64_t generation) {
    std::unique_lock<std::mutex> lock(mutex_);
    for (auto &c : charts_) if (c.snapshot.cell_id == cell && c.snapshot.generation == generation) c.finished = true;
    wake_.notify_all();
    wake_.wait(lock, [&] {
      if (!open_) return true;
      for (const auto &c : charts_) if (c.snapshot.cell_id == cell && c.snapshot.generation == generation &&
          (c.busy || c.rendered_revision < c.snapshot.revision)) return false;
      return true;
    });
    std::vector<ChartResult> result;
    for (const auto &c : charts_) if (c.snapshot.cell_id == cell && c.snapshot.generation == generation) {
      if (!c.error.empty()) throw std::runtime_error(c.error);
      const auto *entry = find_locked(NotebookLiveEventKind::Figure, cell, c.snapshot.style.id);
      if (entry) result.push_back({c.snapshot.style.id, entry->event.display, c.snapshot.style.order});
    }
    return result;
  }

  // A kind/cell delta contains all entries of that changed kind, so deletion
  // and ordering are unambiguous while progress never copies image buffers.
  struct Delta { std::vector<NotebookLiveEvent> events; std::vector<std::uint64_t> resets; };
  bool changes_since(std::uint64_t *revision, Delta *result) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (*revision == sequence_) return false;
    result->events.clear(); result->resets.clear();
    for (const auto &r : resets_) if (r.second > *revision) result->resets.push_back(r.first);
    std::vector<std::pair<NotebookLiveEventKind, std::uint64_t>> changed;
    for (const auto &e : entries_) if (e.event.sequence > *revision) {
      const auto key = std::make_pair(e.key.kind, e.key.cell_id);
      if (std::find(changed.begin(), changed.end(), key) == changed.end()) changed.push_back(key);
    }
    for (const auto &e : entries_) if (std::find(changed.begin(), changed.end(),
        std::make_pair(e.key.kind, e.key.cell_id)) != changed.end()) result->events.push_back(e.event);
    *revision = sequence_; return true;
  }

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
    resets_[cell_id] = sequence_;
    for (auto i = charts_.begin(); i != charts_.end();) {
      if (i->snapshot.cell_id == cell_id) { chart_points_ -= i->snapshot.count; i = charts_.erase(i); }
      else ++i;
    }
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
    {
      std::lock_guard<std::mutex> lock(mutex_);
      open_ = false; entries_.clear(); charts_.clear();
      figure_bytes_ = text_bytes_ = chart_points_ = 0;
    }
    wake_.notify_all();
    std::lock_guard<std::mutex> joining(join_mutex_);
    if (renderer_.joinable()) renderer_.join();
  }

  static std::uint64_t monotonic_millis() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<
        std::chrono::milliseconds>(now).count());
  }

private:
  struct Chart {
    NotebookChartSnapshot snapshot;
    std::uint64_t rendered_revision = 0, next_render_ms = 0;
    bool finished = false, busy = false;
    std::string error;
  };
  Chart &chart_locked(std::uint64_t handle) {
    if (!open_) throw std::invalid_argument("chart run is closed");
    for (auto &c : charts_) if (c.snapshot.handle == handle) return c;
    throw std::invalid_argument("unknown or stale chart handle");
  }
  void render_charts() noexcept {
    std::unique_lock<std::mutex> lock(mutex_);
    while (open_) {
      NotebookChartSnapshot snapshot;
      bool found = false;
      const auto now = monotonic_millis();
      for (auto &c : charts_) if (c.rendered_revision < c.snapshot.revision &&
          (c.finished || now >= c.next_render_ms)) {
        try { snapshot = c.snapshot; } catch (...) { c.error = "unable to snapshot chart"; c.rendered_revision = c.snapshot.revision; continue; }
        c.busy = true; found = true; break;
      }
      if (!found) {
        wake_.notify_all();
        const bool active = std::any_of(charts_.begin(), charts_.end(), [](const auto &c) { return !c.finished; });
        if (active) wake_.wait_for(lock, std::chrono::milliseconds(20));
        else wake_.wait(lock);
        continue;
      }
      lock.unlock();
      NotebookDisplay display;
      std::string error;
      try { display = notebook_chart_render(snapshot); }
      catch (...) { error = "unable to render chart scene"; }
      lock.lock();
      // A reset can retire an in-flight snapshot. Never publish it into a new
      // generation even if its id is reused there.
      auto c = std::find_if(charts_.begin(), charts_.end(), [&](const auto &v) { return v.snapshot.handle == snapshot.handle; });
      if (c == charts_.end() || !open_) continue;
      lock.unlock();
      if (error.empty()) {
        try {
          if (publish_figure(snapshot.cell_id, snapshot.style.id, std::move(display), 0,
                             monotonic_millis(), snapshot.generation) != NotebookLivePublishResult::Accepted)
            error = "chart scene exceeds output limits or run is stale";
        } catch (...) { error = "unable to publish chart scene"; }
      }
      lock.lock();
      c = std::find_if(charts_.begin(), charts_.end(), [&](const auto &v) { return v.snapshot.handle == snapshot.handle; });
      if (c != charts_.end()) {
        c->busy = false; c->error = std::move(error); c->rendered_revision = snapshot.revision;
        c->next_render_ms = monotonic_millis() + snapshot.style.throttle_ms;
      }
      wake_.notify_all();
    }
  }
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
  std::unordered_map<std::uint64_t, std::uint64_t> resets_;
  std::vector<Chart> charts_;
  std::size_t chart_points_ = 0;
  static inline std::atomic<std::uint64_t> next_chart_handle_{0};
  std::thread renderer_;
  std::condition_variable wake_;
  std::mutex join_mutex_;
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
