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
using NotebookInputValue = std::variant<bool, std::int64_t, double, std::string>;
using NotebookInputSnapshot = std::map<std::string, NotebookInputValue>;

class NotebookInputCapture {
public:
  explicit NotebookInputCapture(std::shared_ptr<const NotebookInputSnapshot> values)
      : values_(std::move(values)) {}
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
  std::mutex mutex_;
  bool open_ = true;
  const std::thread::id owner_ = std::this_thread::get_id();
  std::shared_ptr<const NotebookInputSnapshot> values_;
  std::set<std::string> reads_;
};
} // namespace sputnik::runtime
