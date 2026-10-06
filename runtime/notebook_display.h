#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace sputnik::runtime {

// Immutable, VM-independent copies. Neither a UI snapshot nor a parked task
// can retain a live graph or a mutable sort key through this boundary.
struct NotebookDisplayOrder {
  enum class Kind { Number, Bool, String, Symbol, List, Tuple, Desc };
  Kind kind = Kind::Number;
  bool integer = true;
  std::int64_t int_value = 0;
  double float_value = 0;
  std::string text;
  std::vector<NotebookDisplayOrder> items;

  int compare(const NotebookDisplayOrder &other) const {
    if (kind != other.kind)
      throw std::invalid_argument("notebook.show order values are not comparable");
    if (kind == Kind::Number || kind == Kind::Bool) {
      if (integer && other.integer)
        return int_value < other.int_value ? -1 : int_value > other.int_value;
      // Handle mixed Int/Float without rounding large integers to doubles.
      if (integer != other.integer) {
        const auto &i = integer ? *this : other;
        const auto &f = integer ? other : *this;
        int cmp;
        if (f.float_value >= 9223372036854775808.0) cmp = -1;
        else if (f.float_value < -9223372036854775808.0) cmp = 1;
        else {
          const auto whole = static_cast<std::int64_t>(f.float_value);
          cmp = i.int_value < whole ? -1 : i.int_value > whole ? 1 :
              (f.float_value > static_cast<double>(whole) ? -1 :
               f.float_value < static_cast<double>(whole) ? 1 : 0);
        }
        return integer ? cmp : -cmp;
      }
      return float_value < other.float_value ? -1 : float_value > other.float_value;
    }
    if (kind == Kind::String || kind == Kind::Symbol)
      return text < other.text ? -1 : text > other.text;
    for (std::size_t i = 0; i < std::min(items.size(), other.items.size()); ++i) {
      const int cmp = items[i].compare(other.items[i]);
      if (cmp != 0) return kind == Kind::Desc ? -cmp : cmp;
    }
    return items.size() < other.items.size() ? -1 : items.size() > other.items.size();
  }
};

struct NotebookDisplay {
  std::string mime = "image/png";
  std::string bytes;
  std::string caption;
  std::uint32_t width = 0, height = 0;
  // Bounded JSON, inert data only; empty for raster-only/3D/older renderers.
  std::string plot_scene;
  std::size_t storage_bytes() const { return bytes.size() + plot_scene.size(); }
};

// One synchronous cell run. Calls from detached/parallel work are rejected;
// synchronous nested Sputnik calls share the collector on its owner thread.
// Closed collectors cannot leak output into a subsequent evaluation.
class NotebookDisplayCollector {
public:
  bool active() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return open_ && owner_ == std::this_thread::get_id();
  }
  void append(NotebookDisplay display, NotebookDisplayOrder order) {
    replace(std::move(display), std::move(order), {});
  }
  void replace(NotebookDisplay display, NotebookDisplayOrder order, std::string id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_ || owner_ != std::this_thread::get_id())
      throw std::invalid_argument("notebook.show requires an active synchronous notebook cell");
    const auto previous = id.empty() ? entries_.end() : std::find_if(entries_.begin(), entries_.end(),
        [&](const auto &entry) { return entry.id == id; });
    const auto previous_bytes = previous == entries_.end() ? 0 : previous->display.storage_bytes();
    if ((previous == entries_.end() && entries_.size() >= 64) || display.bytes.size() > 16U * 1024U * 1024U ||
        display.plot_scene.size() > 4U * 1024U * 1024U ||
        bytes_ - previous_bytes + display.storage_bytes() > 64U * 1024U * 1024U)
      throw std::invalid_argument("notebook figure exceeds its 64-panel / 64 MiB limit");
    // Compare every key before modifying the batch; invalid mixed keys do
    // not leave a partially appended panel, even if the caller rescues.
    for (const auto &entry : entries_) (void)order.compare(entry.order);
    if (previous != entries_.end()) entries_.erase(previous);
    const auto position = std::find_if(entries_.begin(), entries_.end(),
        [&](const auto &entry) { return order.compare(entry.order) < 0; });
    bytes_ = bytes_ - previous_bytes + display.storage_bytes();
    entries_.insert(position, Entry{std::move(display), std::move(order), std::move(id)});
  }
  void close() {
    std::lock_guard<std::mutex> lock(mutex_);
    open_ = false;
    // Detached tasks may retain the closed context, but must not keep failed
    // runs' image buffers alive indefinitely.
    entries_.clear();
    bytes_ = 0;
  }
  std::vector<NotebookDisplay> take() {
    std::lock_guard<std::mutex> lock(mutex_);
    open_ = false;
    std::vector<NotebookDisplay> result;
    for (auto &entry : entries_) result.push_back(std::move(entry.display));
    entries_.clear();
    return result;
  }
private:
  struct Entry { NotebookDisplay display; NotebookDisplayOrder order; std::string id; };
  mutable std::mutex mutex_;
  const std::thread::id owner_ = std::this_thread::get_id();
  bool open_ = true;
  std::size_t bytes_ = 0;
  std::vector<Entry> entries_;
};
} // namespace sputnik::runtime
