#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <variant>

namespace sputnik::runtime {
// Portable values: never retain another world's heap through project inputs.
using NotebookInputNumber = std::variant<std::int64_t, double>;
struct NotebookInputRange {
  NotebookInputNumber start = std::int64_t(0), finish = std::int64_t(15), step = std::int64_t(1);
  bool inclusive_end = true;
  bool operator==(const NotebookInputRange &other) const {
    return start == other.start && finish == other.finish && step == other.step && inclusive_end == other.inclusive_end;
  }
  bool operator!=(const NotebookInputRange &other) const { return !(*this == other); }
};
using NotebookInputValue = std::variant<bool, std::int64_t, double, std::string, NotebookInputRange>;
using NotebookInputSnapshot = std::map<std::string, NotebookInputValue>;

// Internal dependency identity; authored project IDs cannot contain NUL.
inline std::string notebook_cell_input_key(std::uint64_t cell, const std::string &name) {
  return std::string(1, '\0') + "cell:" + std::to_string(cell) + ":" + name;
}

inline std::string notebook_component_data_key(const std::string &kind, const std::string &channel) {
  return std::string(1, '\0') + kind + ":" + channel;
}

class NotebookInputCapture {
public:
  explicit NotebookInputCapture(std::shared_ptr<const NotebookInputSnapshot> values)
      : values_(std::move(values)) {}
  bool active() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return open_ && owner_ == std::this_thread::get_id();
  }
  std::optional<NotebookInputValue> read(const std::string &key) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_ || owner_ != std::this_thread::get_id() || !values_) return {};
    const auto found = values_->find(key);
    if (found == values_->end()) return {};
    reads_.insert(key);
    return found->second;
  }
  std::set<std::string> finish() {
    std::lock_guard<std::mutex> lock(mutex_);
    open_ = false;
    values_.reset();
    return std::move(reads_);
  }
private:
  mutable std::mutex mutex_;
  bool open_ = true;
  const std::thread::id owner_ = std::this_thread::get_id();
  std::shared_ptr<const NotebookInputSnapshot> values_;
  std::set<std::string> reads_;
};
} // namespace sputnik::runtime
