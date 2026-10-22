#pragma once

#include "runtime/notebook_display.h"
#include "runtime/notebook_chart.h"
#include "runtime/notebook_image.h"
#include "runtime/audio.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <condition_variable>
#include <functional>
#include <iterator>
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
enum class NotebookLiveEventKind { Progress, Figure, ChartPoints, Images, Stopwatch, Audio, AudioControl };

struct NotebookLiveEvent {
  NotebookLiveEventKind kind = NotebookLiveEventKind::Progress;
  std::uint64_t cell_id = 0;
  std::string id;
  std::uint64_t sequence = 0;
  std::uint64_t timestamp_ms = 0;

  // Progress payload. These fields are ignored for other event kinds.
  double current = 0;
  double total = 0;
  std::string description;
  bool done = false;
  // Duration since this progress id's first accepted update in this run.
  std::uint64_t elapsed_ms = 0;
  std::uint64_t stopwatch_token = 0;

  // Figure payload. This is ignored for other event kinds.
  NotebookDisplay display;
  NotebookAudioEvent audio;
  NotebookAudioControl audio_control;

  // A complete replacement image channel, including per-frame captions.
  std::vector<NotebookDisplay> images;
  std::string caption;

  // Live chart payload. Points are deltas unless replace_chart is true.
  // This transfers only numeric samples; the host owns chart style and layout.
  std::vector<NotebookChartPoint> chart_points;
  std::uint64_t chart_total_points = 0;
  bool replace_chart = false;
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

  std::uint64_t begin_audio(std::uint64_t cell, std::uint64_t generation,
      std::string format, std::uint32_t rate, std::uint32_t channels) {
    (void)audio_pcm({}, format, rate, channels, true);
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_ || !generation || generations_[cell] != generation)
      throw std::invalid_argument("audio stream requires an active notebook run");
    if (audio_.size() >= 8) throw std::invalid_argument("run supports at most eight PCM streams");
    AudioStream value; value.cell = cell; value.generation = generation;
    value.handle = ++next_audio_token_; value.value.token = value.handle;
    value.value.sample_format = std::move(format); value.value.rate = rate; value.value.channels = channels;
    value.first = value.last = ++sequence_; value.value.revision = value.last;
    audio_.push_back(std::move(value)); return audio_.back().handle;
  }
  void append_audio(std::uint64_t handle, std::string bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto &stream = audio_locked(handle);
    if (stream.value.finished) throw std::invalid_argument("audio stream is finished; reset it before appending");
    bytes = audio_pcm(std::move(bytes), stream.value.sample_format, stream.value.rate, stream.value.channels, true);
    if (bytes.empty()) return;
    // Explicit bounded backpressure: never evict unplayed audio. The producer
    // can finish/reset the track or publish another track when the cap is hit.
    if (bytes.size() > kAudioMaxBytes - 128 - stream.value.bytes.size() ||
        bytes.size() > kMaxFigureBytes - audio_bytes_ || stream.chunks.size() >= 65536)
      throw std::invalid_argument("PCM stream buffer is full (16 MiB/stream, 64 MiB/run); finish or reset before appending");
    const auto offset = stream.value.bytes.size();
    stream.last = ++sequence_; stream.chunks.push_back({stream.last, offset});
    audio_bytes_ += bytes.size(); stream.value.bytes += bytes;
    stream.value.frames = stream.value.bytes.size() / (stream.value.channels * (stream.value.sample_format == "s16le" ? 2 : 4));
    stream.value.revision = stream.last;
  }
  void finish_audio(std::uint64_t handle) {
    std::lock_guard<std::mutex> lock(mutex_); auto &s = audio_locked(handle);
    if (!s.value.finished) { s.value.finished = true; s.value.revision = s.last = ++sequence_; }
  }
  void finish_audio_streams() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto &stream : audio_) if (!stream.value.finished) {
      stream.value.finished = true; stream.value.revision = stream.last = ++sequence_;
    }
  }
  void reset_audio(std::uint64_t handle) {
    std::lock_guard<std::mutex> lock(mutex_); auto &s = audio_locked(handle);
    audio_bytes_ -= s.value.bytes.size(); s.value.bytes.clear(); s.chunks.clear();
    s.value.token = ++next_audio_token_; s.value.frames = 0; s.value.finished = false;
    s.value.revision = s.first = s.last = ++sequence_;
  }
  std::string audio_data(std::uint64_t handle) {
    std::lock_guard<std::mutex> lock(mutex_); return audio_locked(handle).value.bytes;
  }
  void bind_audio(std::uint64_t handle, std::uint64_t cell, std::uint64_t generation,
      std::string id, AudioDisplay player, std::string caption) {
    if (id.empty() || id.size() > 256 || id.find('\0') != std::string::npos || caption.size() > 65536 ||
        !std::isfinite(player.position) || player.position < 0 || !std::isfinite(player.volume) || player.volume < 0 || player.volume > 1)
      throw std::invalid_argument("invalid audio player identity/options");
    std::lock_guard<std::mutex> lock(mutex_); (void)audio_locked(handle);
    if (!generation || generations_[cell] != generation) throw std::invalid_argument("audio player run is stale");
    for (auto &old : audio_bindings_) if (old.cell == cell && old.id == id) {
      old = AudioBinding{handle, cell, generation, ++sequence_, std::move(id), std::move(caption), std::move(player)}; return;
    }
    if (audio_bindings_.size() >= 64) throw std::invalid_argument("too many audio players");
    audio_bindings_.push_back(AudioBinding{handle, cell, generation, ++sequence_, std::move(id), std::move(caption), std::move(player)});
  }

  void control_audio(std::uint64_t owner, std::uint64_t generation, std::string id,
      std::uint64_t target, std::string channel, const std::string &action, double value) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto *old = find_locked(NotebookLiveEventKind::AudioControl, owner, id);
    NotebookLiveEvent event; event.kind = NotebookLiveEventKind::AudioControl; event.cell_id = owner; event.id = std::move(id);
    if (old) event.audio_control = old->event.audio_control;
    auto &c = event.audio_control; c.cell = target; c.channel = std::move(channel);
    const auto revision = ++next_audio_command_;
    if (action == "seek") { c.position = value; c.seek_revision = revision; }
    else if (action == "volume") { c.volume = value; c.volume_revision = revision; }
    else if (action == "play" || action == "pause" || action == "stop") {
      c.playback = action == "play" ? 1 : action == "pause" ? 2 : 3; c.playback_revision = revision;
    } else throw std::invalid_argument("unknown audio command");
    audio_validate_control(c);
    if (publish_locked(std::move(event), 0, monotonic_millis(), generation) != NotebookLivePublishResult::Accepted)
      throw std::invalid_argument("audio command run is closed or stale");
  }
  std::uint64_t bound_audio(std::uint64_t cell, const std::string &channel) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::uint64_t handle = 0;
    for (const auto &b : audio_bindings_) if (b.cell == cell && (channel.empty() || b.id == channel)) {
      if (handle && handle != b.handle) throw std::invalid_argument("audio component must select one published channel");
      handle = b.handle;
    }
    if (!handle) throw std::invalid_argument("audio component has no open PCM stream");
    (void)audio_locked(handle); return handle;
  }

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

  // Publish numeric points to a board-owned chart component. The component ID
  // is its stable project ID; no chart object or rendering style is created in
  // notebook code. The host retains bounded point history and lays it out at
  // the component's current size.
  void append_board_chart(std::uint64_t cell, std::uint64_t generation,
                          std::string id, NotebookChartPoint point) {
    if (id.empty() || id.size() > 256 || id.find('\0') != std::string::npos ||
        !std::isfinite(point[0]) || !std::isfinite(point[1]) ||
        std::abs(point[0]) >= 1e100 || std::abs(point[1]) >= 1e100)
      throw std::invalid_argument("invalid board chart point");
    std::lock_guard<std::mutex> lock(mutex_);
    const auto active_generation = generations_.find(cell);
    if (!open_ || generation == 0 || active_generation == generations_.end() || active_generation->second != generation)
      throw std::invalid_argument("chart run is closed or stale");
    auto found = std::find_if(board_charts_.begin(), board_charts_.end(), [&](const auto &chart) {
      return chart.cell_id == cell && chart.id == id;
    });
    if (found == board_charts_.end()) {
      if (board_charts_.size() >= 64) throw std::invalid_argument("chart count exceeds 64");
      board_charts_.push_back(BoardChart{cell, generation, std::move(id)});
      found = std::prev(board_charts_.end());
      found->first_sequence = sequence_ + 1;
    }
    if (found->generation != generation) throw std::invalid_argument("chart run is stale");
    if (found->points.size() == 65536) {
      found->points.pop_front();
      --chart_points_;
    }
    if (chart_points_ >= 524288) throw std::invalid_argument("run chart point budget exceeded");
    const auto point_sequence = ++sequence_;
    found->points.push_back({point_sequence, point});
    ++found->total_points;
    found->last_sequence = point_sequence;
    ++chart_points_;
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
    for (const auto &chart : board_charts_) if (chart.last_sequence > *revision) {
      NotebookLiveEvent event;
      event.kind = NotebookLiveEventKind::ChartPoints;
      event.cell_id = chart.cell_id;
      event.id = chart.id;
      event.sequence = chart.last_sequence;
      event.chart_total_points = chart.total_points;
      event.replace_chart = chart.first_sequence > *revision;
      const auto first = event.replace_chart ? chart.points.begin() :
          std::upper_bound(chart.points.begin(), chart.points.end(), *revision,
              [](auto sequence, const auto &point) { return sequence < point.sequence; });
      for (auto point = first; point != chart.points.end(); ++point)
        event.chart_points.push_back(point->value);
      result->events.push_back(std::move(event));
    }
    for (const auto &binding : audio_bindings_) {
      const auto stream = std::find_if(audio_.begin(), audio_.end(), [&](const auto &s) { return s.handle == binding.handle; });
      if (stream != audio_.end() && std::max(stream->last, binding.sequence) > *revision)
        result->events.push_back(audio_snapshot(*stream, binding, *revision));
    }
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

  std::uint64_t begin_stopwatch(std::uint64_t cell, std::uint64_t generation,
                                std::string id) {
    if (id.empty() || id.size() > 256 || id.find('\0') != std::string::npos)
      throw std::invalid_argument("Stopwatch id must contain 1..256 bytes without NUL");
    std::lock_guard<std::mutex> lock(mutex_);
    if (const auto old = find_locked(NotebookLiveEventKind::Stopwatch, cell, id); old && !old->event.done)
      throw std::invalid_argument("Stopwatch id is already running in this cell");
    NotebookLiveEvent event;
    event.kind = NotebookLiveEventKind::Stopwatch; event.cell_id = cell; event.id = std::move(id);
    event.timestamp_ms = monotonic_millis(); event.stopwatch_token = ++next_stopwatch_token_;
    const auto token = event.stopwatch_token;
    const auto name = event.id;
    const auto now = event.timestamp_ms;
    if (publish_locked(std::move(event), 0, now, generation) != NotebookLivePublishResult::Accepted)
      throw std::invalid_argument("Stopwatch run is closed, stale or exceeds its channel budget");
    auto *entry = find_locked(NotebookLiveEventKind::Stopwatch, cell, name);
    entry->start_ms = now; entry->event.elapsed_ms = 0;
    return token;
  }

  void end_stopwatch(std::uint64_t token) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_) return;
    for (auto &entry : entries_) if (entry.event.kind == NotebookLiveEventKind::Stopwatch && entry.event.stopwatch_token == token) {
      entry.event.done = true;
      entry.event.timestamp_ms = monotonic_millis();
      entry.event.elapsed_ms = entry.event.timestamp_ms - entry.start_ms;
      entry.event.sequence = ++sequence_;
      return;
    }
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
    display.id = id;
    event.id = std::move(id);
    event.timestamp_ms = now_ms;
    event.display = std::move(display);
    return publish(std::move(event), throttle_ms, now_ms, generation);
  }

  NotebookLivePublishResult publish_images(
      std::uint64_t cell, std::uint64_t generation, std::string id,
      std::vector<NotebookDisplay> images, std::optional<std::string> caption,
      bool append = false) {
    if (id.empty() || id.size() > 256 || id.find('\0') != std::string::npos ||
        images.size() > 64 || (caption && caption->size() > kMaxDescriptionBytes))
      return NotebookLivePublishResult::Invalid;
    for (const auto &image : images) {
      try { notebook_validate_image(image); }
      catch (const std::exception &) { return NotebookLivePublishResult::Invalid; }
      if (!image.plot_scene.empty() || image.storage_bytes() > kMaxFigureBytesPerEntry || image.caption.size() > kMaxDescriptionBytes)
        return NotebookLivePublishResult::Invalid;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto active = generations_.find(cell);
    if (!open_ || generation == 0 || active == generations_.end() || active->second != generation)
      return NotebookLivePublishResult::Closed;
    NotebookLiveEvent event;
    event.kind = NotebookLiveEventKind::Images;
    event.cell_id = cell; event.id = std::move(id);
    event.timestamp_ms = monotonic_millis();
    if (append) if (const auto old = find_locked(event.kind, cell, event.id)) {
      event.images = old->event.images; event.caption = old->event.caption;
    }
    if (event.images.size() + images.size() > 64) return NotebookLivePublishResult::LimitExceeded;
    event.images.insert(event.images.end(), std::make_move_iterator(images.begin()), std::make_move_iterator(images.end()));
    if (caption) event.caption = std::move(*caption);
    return publish_locked(std::move(event), 0, monotonic_millis(), generation);
  }

  // Returns independent data.  No mutex or mutable store is retained by the
  // caller, so a UI may render it after close().
  std::vector<NotebookLiveEvent> snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<NotebookLiveEvent> result;
    result.reserve(entries_.size());
    for (const auto &entry : entries_) result.push_back(entry.event);
    for (const auto &chart : board_charts_) result.push_back(chart_snapshot(chart));
    audio_snapshots(result, 0);
    return result;
  }

  std::optional<NotebookLiveEvent> image_channel(const std::string &id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_) return {};
    const NotebookLiveEvent *latest = nullptr;
    for (const auto &entry : entries_) if (entry.event.kind == NotebookLiveEventKind::Images && entry.event.id == id &&
        (!latest || entry.event.sequence > latest->sequence)) latest = &entry.event;
    return latest ? std::optional<NotebookLiveEvent>(*latest) : std::nullopt;
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
    for (const auto &chart : board_charts_) result->push_back(chart_snapshot(chart));
    audio_snapshots(*result, 0);
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
    for (const auto &chart : board_charts_) if (chart.cell_id == cell_id)
      result.push_back(chart_snapshot(chart));
    audio_snapshots(result, cell_id);
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
    for (auto i = board_charts_.begin(); i != board_charts_.end();) {
      if (i->cell_id == cell_id) { chart_points_ -= i->points.size(); i = board_charts_.erase(i); }
      else ++i;
    }
    for (auto it = audio_.begin(); it != audio_.end();) {
      if (it->cell == cell_id) { audio_bytes_ -= it->value.bytes.size(); it = audio_.erase(it); } else ++it;
    }
    audio_bindings_.erase(std::remove_if(audio_bindings_.begin(), audio_bindings_.end(), [&](const auto &b) { return b.cell == cell_id || std::none_of(audio_.begin(), audio_.end(), [&](const auto &s) { return s.handle == b.handle; }); }), audio_bindings_.end());
    auto it = entries_.begin();
    while (it != entries_.end()) {
      if (it->key.cell_id != cell_id) {
        ++it;
        continue;
      }
      figure_bytes_ -= image_size(it->event);
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
      open_ = false; entries_.clear(); charts_.clear(); board_charts_.clear();
      audio_.clear(); audio_bindings_.clear(); audio_bytes_ = 0;
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
  struct AudioStream {
    std::uint64_t handle = 0, cell = 0, generation = 0, first = 0, last = 0;
    NotebookAudioEvent value;
    std::vector<std::pair<std::uint64_t, std::size_t>> chunks;
  };
  struct AudioBinding {
    std::uint64_t handle, cell, generation, sequence;
    std::string id, caption; AudioDisplay player;
  };
  AudioStream &audio_locked(std::uint64_t handle) {
    auto found = std::find_if(audio_.begin(), audio_.end(), [&](const auto &s) { return s.handle == handle; });
    if (!open_ || found == audio_.end() || generations_[found->cell] != found->generation)
      throw std::invalid_argument("audio stream is closed or stale");
    return *found;
  }
  static NotebookLiveEvent audio_snapshot(const AudioStream &s, const AudioBinding &b, std::uint64_t after) {
    NotebookLiveEvent e; e.kind = NotebookLiveEventKind::Audio; e.cell_id = b.cell; e.id = b.id; e.caption = b.caption;
    e.sequence = std::max(s.last, b.sequence); e.audio = s.value; e.audio.player = b.player;
    e.audio.revision = e.sequence; e.audio.replace = after < std::max(s.first, b.sequence);
    std::size_t offset = 0;
    if (!e.audio.replace) {
      offset = s.value.bytes.size();
      for (const auto &chunk : s.chunks) if (chunk.first > after) { offset = chunk.second; break; }
    }
    e.audio.offset = offset / (s.value.channels * (s.value.sample_format == "s16le" ? 2 : 4));
    e.audio.bytes = s.value.bytes.substr(offset); return e;
  }
  void audio_snapshots(std::vector<NotebookLiveEvent> &out, std::uint64_t cell) const {
    for (const auto &b : audio_bindings_) if (!cell || b.cell == cell) {
      const auto s = std::find_if(audio_.begin(), audio_.end(), [&](const auto &s) { return s.handle == b.handle; });
      if (s != audio_.end()) out.push_back(audio_snapshot(*s, b, 0));
    }
  }
  std::vector<AudioStream> audio_;
  std::vector<AudioBinding> audio_bindings_;
  std::size_t audio_bytes_ = 0;
  static inline std::atomic<std::uint64_t> next_audio_token_{static_cast<std::uint64_t>(std::chrono::system_clock::now().time_since_epoch().count())},
      next_audio_command_{static_cast<std::uint64_t>(std::chrono::system_clock::now().time_since_epoch().count())};
  struct SequencedChartPoint { std::uint64_t sequence; NotebookChartPoint value; };
  struct BoardChart {
    std::uint64_t cell_id = 0, generation = 0;
    std::string id;
    std::deque<SequencedChartPoint> points;
    std::uint64_t total_points = 0, last_sequence = 0, first_sequence = 0;
    BoardChart(std::uint64_t cell, std::uint64_t run, std::string chart_id)
        : cell_id(cell), generation(run), id(std::move(chart_id)) {}
  };
  static NotebookLiveEvent chart_snapshot(const BoardChart &chart) {
    NotebookLiveEvent event;
    event.kind = NotebookLiveEventKind::ChartPoints;
    event.cell_id = chart.cell_id;
    event.id = chart.id;
    event.sequence = chart.last_sequence;
    event.chart_total_points = chart.total_points;
    event.replace_chart = true;
    for (const auto &point : chart.points) event.chart_points.push_back(point.value);
    return event;
  }
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
  NotebookAudioEvent audio;
  NotebookAudioControl audio_control;
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
    if (event.kind == NotebookLiveEventKind::Images) {
      std::size_t size = event.id.size() + event.caption.size();
      for (const auto &image : event.images) size += image.caption.size();
      return size;
    }
    return event.id.size() +
           (event.kind == NotebookLiveEventKind::Progress
                ? event.description.size()
                : event.display.caption.size());
  }

  static std::size_t image_size(const NotebookLiveEvent &event) {
    if (event.kind == NotebookLiveEventKind::Figure) return event.display.storage_bytes();
    std::size_t size = 0;
    for (const auto &image : event.images) size += image.storage_bytes();
    return size;
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
    return publish_locked(std::move(event), throttle_ms, now_ms, generation);
  }

  NotebookLivePublishResult publish_locked(NotebookLiveEvent event,
                                    std::uint64_t throttle_ms,
                                    std::uint64_t now_ms,
                                    std::uint64_t generation) {
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

    const std::size_t event_figure_bytes = image_size(event);
    const std::size_t event_text_bytes = text_size(event);
    Entry *old = find_locked(event.kind, event.cell_id, event.id);
    if (old == nullptr && entries_.size() >= kMaxEntries)
      return NotebookLivePublishResult::LimitExceeded;
    const std::size_t previous_figure_bytes = old == nullptr ? 0 : image_size(old->event);
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
  std::uint64_t next_stopwatch_token_ = 0;
  std::size_t figure_bytes_ = 0;
  std::size_t text_bytes_ = 0;
  std::vector<Entry> entries_;
  std::vector<BoardChart> board_charts_;
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
