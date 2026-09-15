#include "notebook/slot_table.h"

#include "runtime/objects.h"

#include <algorithm>
#include <iterator>
#include <utility>

namespace amber::notebook {

namespace {

SlotState make_slot_state(const BindingKey &key) {
  SlotState state;
  state.key = key;
  state.provider.cell_id = key.cell_id;
  state.provider.name = key.name;
  return state;
}

} // namespace

SlotProviderMetadata
NotebookSlotTable::default_provider(const BindingKey &key) {
  SlotProviderMetadata provider;
  provider.cell_id = key.cell_id;
  provider.name = key.name;
  return provider;
}

SlotProviderMetadata
NotebookSlotTable::normalize_provider(const BindingKey &key,
                                      SlotProviderMetadata provider) {
  if (provider.cell_id == 0) {
    provider.cell_id = key.cell_id;
  }
  if (provider.name.empty()) {
    provider.name = key.name;
  }
  return provider;
}

NotebookSlotTable::Transaction NotebookSlotTable::begin_transaction() {
  return Transaction(this);
}

SlotCommitResult NotebookSlotTable::publish(const BindingKey &key,
                                            runtime::Value value) {
  Transaction transaction = begin_transaction();
  transaction.stage(key, std::move(value));
  return transaction.commit();
}

SlotCommitResult NotebookSlotTable::publish(const BindingKey &key,
                                            runtime::Value value,
                                            SlotProviderMetadata provider) {
  Transaction transaction = begin_transaction();
  transaction.stage(key, std::move(value), std::move(provider));
  return transaction.commit();
}

std::optional<SlotState> NotebookSlotTable::find(const BindingKey &key) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = slots_.find(key);
  return it == slots_.end() ? std::nullopt
                            : std::optional<SlotState>(it->second);
}

bool NotebookSlotTable::contains(const BindingKey &key) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return slots_.find(key) != slots_.end();
}

std::optional<runtime::Value>
NotebookSlotTable::read(const BindingKey &key) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = slots_.find(key);
  if (it == slots_.end() || !it->second.initialized) {
    return std::nullopt;
  }
  return it->second.published;
}

std::vector<BindingKey> NotebookSlotTable::keys() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<BindingKey> result;
  result.reserve(slots_.size());
  for (const auto &entry : slots_) {
    result.push_back(entry.first);
  }
  std::sort(result.begin(), result.end());
  return result;
}

std::vector<SlotState> NotebookSlotTable::slots() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<SlotState> result;
  result.reserve(slots_.size());
  for (const auto &entry : slots_) {
    result.push_back(entry.second);
  }
  std::sort(result.begin(), result.end(),
            [](const SlotState &left, const SlotState &right) {
              return left.key < right.key;
            });
  return result;
}

std::size_t NotebookSlotTable::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return slots_.size();
}

bool NotebookSlotTable::mark_stale(const BindingKey &key, bool stale) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = slots_.find(key);
  if (it == slots_.end()) {
    return false;
  }
  it->second.stale = stale;
  return true;
}

bool NotebookSlotTable::mark_stale_batch(
    const std::vector<BindingKey> &keys, bool stale) noexcept {
  try {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const BindingKey &key : keys) {
      const auto it = slots_.find(key);
      if (it != slots_.end()) {
        it->second.stale = stale;
      }
    }
    return true;
  } catch (...) {
    return false;
  }
}

std::size_t NotebookSlotTable::mark_all_stale() {
  std::lock_guard<std::mutex> lock(mutex_);
  std::size_t changed = 0;
  for (auto &entry : slots_) {
    if (!entry.second.stale) {
      entry.second.stale = true;
      ++changed;
    }
  }
  return changed;
}

bool NotebookSlotTable::erase(const BindingKey &key) {
  std::lock_guard<std::mutex> lock(mutex_);
  return slots_.erase(key) != 0U;
}

void NotebookSlotTable::clear() noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  slots_.clear();
  publication_epoch_ = 0;
}

std::vector<runtime::Value> NotebookSlotTable::gc_roots() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<runtime::Value> result;
  result.reserve(slots_.size());
  for (const auto &entry : slots_) {
    if (entry.second.initialized) {
      result.push_back(entry.second.published);
    }
  }
  return result;
}

void NotebookSlotTable::append_gc_roots(
    std::vector<runtime::Value> *roots) const {
  if (roots == nullptr) {
    return;
  }
  std::vector<runtime::Value> values = gc_roots();
  roots->insert(roots->end(), std::make_move_iterator(values.begin()),
                std::make_move_iterator(values.end()));
}

void NotebookSlotTable::visit_values(const ValueVisitor &visitor) const {
  if (!visitor) {
    return;
  }
  // Do not invoke host code under the table lock.  Apart from avoiding a
  // re-entrancy deadlock, this gives a visitor a stable root snapshot for the
  // duration of one call while the copied Values keep their objects alive.
  const std::vector<runtime::Value> values = gc_roots();
  for (const runtime::Value &value : values) {
    visitor(value);
  }
}

SlotCommitResult NotebookSlotTable::commit_transaction(
    std::map<BindingKey, TransactionWrite> writes) {
  SlotCommitResult result;
  result.events.reserve(writes.size());

  std::lock_guard<std::mutex> lock(mutex_);

  // Prepare a complete replacement map before changing the published map.
  // This is intentionally a copy-on-commit boundary: allocation, Value copy,
  // or event construction failures leave the previous publication intact.
  auto next_slots = slots_;
  std::vector<SlotPublicationEvent> events;
  events.reserve(writes.size());

  const std::uint64_t event_epoch =
      writes.empty() ? publication_epoch_ : publication_epoch_ + 1U;
  for (auto &entry : writes) {
    const BindingKey &key = entry.first;
    TransactionWrite &pending = entry.second;
    auto slot_it = next_slots.find(key);
    if (slot_it == next_slots.end()) {
      slot_it = next_slots.emplace(key, make_slot_state(key)).first;
    }
    SlotState &slot = slot_it->second;
    // Older maps cannot exist through this class API, but maintaining these
    // identities here keeps snapshots coherent if the representation evolves.
    slot.key = key;
    if (slot.provider.cell_id == 0 && slot.provider.name.empty()) {
      slot.provider = default_provider(key);
    }

    if (pending.value.has_value()) {
      runtime::Value next_value = std::move(*pending.value);
      const bool value_equal =
          slot.initialized && runtime::value_equals(slot.published, next_value);
      const SlotState old = slot;

      if (pending.provider.has_value()) {
        slot.provider = normalize_provider(key, std::move(*pending.provider));
      }
      slot.published = std::move(next_value);
      slot.initialized = true;
      slot.stale = false;

      // Initializing a slot is a publication even when its representation is
      // the default null Value.  Once initialized, semantic equality is a
      // no-op for revision and publication purposes.
      if (!value_equal) {
        ++slot.revision;
        SlotPublicationEvent event;
        event.key = key;
        event.old_value = old.published;
        event.new_value = slot.published;
        event.old_initialized = old.initialized;
        event.initialized = slot.initialized;
        event.old_stale = old.stale;
        event.stale = slot.stale;
        event.old_revision = old.revision;
        event.new_revision = slot.revision;
        event.publication_epoch = event_epoch;
        events.push_back(std::move(event));
      }
    } else if (pending.provider.has_value()) {
      slot.provider = normalize_provider(key, std::move(*pending.provider));
    }

    if (pending.stale.has_value()) {
      slot.stale = *pending.stale;
    }
  }

  if (!events.empty()) {
    ++publication_epoch_;
    result.publication_epoch = publication_epoch_;
  } else {
    result.publication_epoch = publication_epoch_;
  }
  slots_.swap(next_slots);
  result.committed = true;
  result.events = std::move(events);
  return result;
}

NotebookSlotTable::Transaction::Transaction(Transaction &&other) noexcept
    : owner_(other.owner_), writes_(std::move(other.writes_)),
      finished_(other.finished_) {
  other.owner_ = nullptr;
  other.finished_ = true;
}

NotebookSlotTable::Transaction &
NotebookSlotTable::Transaction::operator=(Transaction &&other) noexcept {
  if (this != &other) {
    rollback();
    owner_ = other.owner_;
    writes_ = std::move(other.writes_);
    finished_ = other.finished_;
    other.owner_ = nullptr;
    other.finished_ = true;
  }
  return *this;
}

NotebookSlotTable::Transaction::~Transaction() { rollback(); }

bool NotebookSlotTable::Transaction::stage(const BindingKey &key,
                                           runtime::Value value) {
  if (!active()) {
    return false;
  }
  TransactionWrite &write = writes_[key];
  write.value = std::move(value);
  write.provider.reset();
  write.stale.reset();
  return true;
}

bool NotebookSlotTable::Transaction::stage(const BindingKey &key,
                                           runtime::Value value,
                                           SlotProviderMetadata provider) {
  if (!active()) {
    return false;
  }
  TransactionWrite &write = writes_[key];
  write.value = std::move(value);
  write.provider = std::move(provider);
  write.stale.reset();
  return true;
}

bool NotebookSlotTable::Transaction::stage_stale(const BindingKey &key,
                                                 bool stale) {
  if (!active()) {
    return false;
  }
  writes_[key].stale = stale;
  return true;
}

SlotCommitResult NotebookSlotTable::Transaction::commit() {
  if (!active()) {
    return {};
  }
  NotebookSlotTable *owner = owner_;
  owner_ = nullptr;
  finished_ = true;
  return owner->commit_transaction(std::move(writes_));
}

void NotebookSlotTable::Transaction::rollback() noexcept {
  writes_.clear();
  owner_ = nullptr;
  finished_ = true;
}

} // namespace amber::notebook
