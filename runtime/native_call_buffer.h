#pragma once

#include <array>
#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

namespace amber::runtime {

// Call-local values use index handles, so overflow need not move the inline
// entries. Construct only occupied slots: initializing/destroying a full array
// would add work to every scalar extension call.
template <class T, std::size_t InlineCapacity = 16>
class NativeCallValueArena {
public:
  NativeCallValueArena() = default;
  NativeCallValueArena(const NativeCallValueArena &) = delete;
  NativeCallValueArena &operator=(const NativeCallValueArena &) = delete;
  ~NativeCallValueArena() {
    for (std::size_t index = inline_size_; index != 0; --index) {
      inline_at(index - 1)->~T();
    }
  }

  void reserve(std::size_t capacity) {
    if (capacity > InlineCapacity) overflow_.reserve(capacity - InlineCapacity);
  }
  void push_back(T value) {
    if (inline_size_ < InlineCapacity) {
      ::new (static_cast<void *>(&inline_[inline_size_])) T(std::move(value));
      ++inline_size_;
    } else {
      overflow_.push_back(std::move(value));
    }
  }
  std::size_t size() const noexcept { return inline_size_ + overflow_.size(); }
  const T &operator[](std::size_t index) const noexcept {
    return index < InlineCapacity ? *inline_at(index)
                                  : overflow_[index - InlineCapacity];
  }

private:
  const T *inline_at(std::size_t index) const noexcept {
    return std::launder(reinterpret_cast<const T *>(&inline_[index]));
  }
  T *inline_at(std::size_t index) noexcept {
    return std::launder(reinterpret_cast<T *>(&inline_[index]));
  }
  std::array<std::aligned_storage_t<sizeof(T), alignof(T)>, InlineCapacity> inline_;
  std::size_t inline_size_ = 0;
  std::vector<T> overflow_;
};

// Unlike the value arena, the ABI argument handles must remain contiguous.
template <class T, std::size_t InlineCapacity = 8>
class NativeCallHandleBuffer {
public:
  explicit NativeCallHandleBuffer(std::size_t size) : size_(size) {
    if (size > InlineCapacity) overflow_.resize(size);
  }
  T *data() noexcept {
    return size_ <= InlineCapacity ? inline_.data() : overflow_.data();
  }
  std::size_t size() const noexcept { return size_; }
  T &operator[](std::size_t index) noexcept { return data()[index]; }

private:
  std::array<T, InlineCapacity> inline_;
  std::size_t size_;
  std::vector<T> overflow_;
};

} // namespace amber::runtime
