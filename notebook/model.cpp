#include "notebook/model.h"

#include <atomic>
#include <functional>
#include <limits>
#include <stdexcept>

namespace amber::notebook {
namespace {
std::atomic<CellId> next_cell_id{1};
}

std::size_t BindingKeyHash::operator()(const BindingKey &key) const noexcept {
  const std::size_t h1 = std::hash<CellId>{}(key.cell_id);
  const std::size_t h2 = std::hash<std::string>{}(key.name);
  return h1 ^
         (h2 + static_cast<std::size_t>(0x9e3779b9U) + (h1 << 6U) + (h1 >> 2U));
}

CellId allocate_cell_id() {
  auto next = next_cell_id.load(std::memory_order_relaxed);
  while (true) {
    if (next == std::numeric_limits<CellId>::max()) {
      throw std::overflow_error("notebook cell identity space exhausted");
    }
    if (next_cell_id.compare_exchange_weak(next, next + 1,
                                           std::memory_order_relaxed)) {
      return next;
    }
  }
}

void reserve_cell_id(CellId id) {
  if (id == 0 || id == std::numeric_limits<CellId>::max()) {
    throw std::invalid_argument("invalid notebook cell identity");
  }
  auto next = next_cell_id.load(std::memory_order_relaxed);
  while (next <= id && !next_cell_id.compare_exchange_weak(
                           next, id + 1, std::memory_order_relaxed)) {
  }
}

} // namespace amber::notebook
