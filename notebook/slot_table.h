#pragma once

#include "notebook/model.h"
#include "runtime/value.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace amber::notebook {

// Metadata carried by a published binding.  The BindingKey remains the
// identity of a slot; these fields describe the source that produced its
// current value and are deliberately independent of the UI.
struct SlotProviderMetadata {
  CellId cell_id = 0;
  std::string name;
  std::string file;
  std::string module;
  std::uint64_t generation = 0;

  bool operator==(const SlotProviderMetadata &other) const {
    return cell_id == other.cell_id && name == other.name &&
           file == other.file && module == other.module &&
           generation == other.generation;
  }
  bool operator!=(const SlotProviderMetadata &other) const {
    return !(*this == other);
  }
};

using ProviderMetadata = SlotProviderMetadata;

// A copyable snapshot of one persistent notebook binding.  published is kept
// when stale is true: stale means that the producer needs recomputation, not
// that the last successful value has become unsafe to retain.
struct SlotState {
  BindingKey key;
  runtime::Value published = runtime::Value::null();
  bool initialized = false;
  bool stale = false;
  std::uint64_t revision = 0;
  SlotProviderMetadata provider;

  const runtime::Value &value() const noexcept { return published; }
  const SlotProviderMetadata &provider_metadata() const noexcept {
    return provider;
  }
};

using NotebookSlotState = SlotState;
using NotebookSlot = SlotState;

// A publication event is intentionally independent of RuntimeWatchCell.  A
// host can feed these events into its own invalidation/watch machinery after a
// successful commit without making the slot table depend on that machinery.
struct SlotPublicationEvent {
  BindingKey key;
  runtime::Value old_value = runtime::Value::null();
  runtime::Value new_value = runtime::Value::null();
  bool old_initialized = false;
  bool initialized = false;
  bool old_stale = false;
  bool stale = false;
  std::uint64_t old_revision = 0;
  std::uint64_t new_revision = 0;
  std::uint64_t publication_epoch = 0;

  // This layer emits events only for a newly initialized or value-changing
  // publication.  Exposing the bit makes consumers robust if event kinds are
  // extended later.
  bool changed = true;
};

using PublicationEvent = SlotPublicationEvent;

struct SlotCommitResult {
  bool committed = false;
  std::uint64_t publication_epoch = 0;
  std::vector<SlotPublicationEvent> events;

  bool ok() const noexcept { return committed; }
  bool changed() const noexcept { return !events.empty(); }
  std::size_t size() const noexcept { return events.size(); }
  bool empty() const noexcept { return events.empty(); }
  const SlotPublicationEvent &operator[](std::size_t index) const {
    return events[index];
  }
  std::vector<SlotPublicationEvent>::const_iterator begin() const {
    return events.begin();
  }
  std::vector<SlotPublicationEvent>::const_iterator end() const {
    return events.end();
  }

  // This deliberately permits the common `if (transaction.commit())` form
  // while retaining the event batch for hosts that need it.
  operator bool() const noexcept { return committed; }
};

class NotebookSlotTable {
public:
  using ValueVisitor = std::function<void(const runtime::Value &)>;

  class Transaction;

  NotebookSlotTable() = default;
  NotebookSlotTable(const NotebookSlotTable &) = delete;
  NotebookSlotTable &operator=(const NotebookSlotTable &) = delete;

  Transaction begin_transaction();
  Transaction transaction();
  Transaction begin();

  // Convenience for a one-binding transaction.  For a set of writes use a
  // Transaction so all values become visible at one publication point.
  SlotCommitResult publish(const BindingKey &key, runtime::Value value);
  SlotCommitResult publish(const BindingKey &key, runtime::Value value,
                           SlotProviderMetadata provider);
  SlotCommitResult write(const BindingKey &key, runtime::Value value) {
    return publish(key, std::move(value));
  }

  std::optional<SlotState> find(const BindingKey &key) const;
  std::optional<SlotState> snapshot(const BindingKey &key) const {
    return find(key);
  }
  bool contains(const BindingKey &key) const;

  // Returns the last successfully published value, including when the slot
  // is stale.  Callers that need to reject stale inputs should inspect find().
  std::optional<runtime::Value> read(const BindingKey &key) const;
  std::optional<runtime::Value> read_published(const BindingKey &key) const {
    return read(key);
  }

  std::vector<BindingKey> keys() const;
  std::vector<SlotState> slots() const;
  std::size_t size() const;
  bool empty() const { return size() == 0U; }

  // Staleness is host invalidation state, not a value publication and hence
  // does not advance a binding revision or emit a publication event.
  bool mark_stale(const BindingKey &key, bool stale = true);
  // No-allocation batch used by a prepared kernel-generation commit.  Either
  // the table lock is acquired before any mutation or the call returns false.
  bool mark_stale_batch(const std::vector<BindingKey> &keys,
                        bool stale = true) noexcept;
  std::size_t mark_all_stale();

  // Removing a slot is used by a document reset/rebuild.  It is intentionally
  // not represented as a publication event: consumers must discard their
  // dependency graph together with the table.
  bool erase(const BindingKey &key);
  void clear() noexcept;
  void reset() noexcept { clear(); }

  // RuntimeWorld can append these values to its root vector before a GC cycle.
  // A stale but initialized value remains a root until the table is reset or
  // the producer successfully publishes a replacement.
  std::vector<runtime::Value> gc_roots() const;
  void append_gc_roots(std::vector<runtime::Value> *roots) const;
  void visit_values(const ValueVisitor &visitor) const;
  void visit_gc_roots(const ValueVisitor &visitor) const {
    visit_values(visitor);
  }
  void for_each_root(const ValueVisitor &visitor) const {
    visit_values(visitor);
  }

private:
  struct TransactionWrite {
    std::optional<runtime::Value> value;
    std::optional<SlotProviderMetadata> provider;
    std::optional<bool> stale;
  };

  SlotCommitResult
  commit_transaction(std::map<BindingKey, TransactionWrite> writes);
  static SlotProviderMetadata default_provider(const BindingKey &key);
  static SlotProviderMetadata normalize_provider(const BindingKey &key,
                                                 SlotProviderMetadata provider);

  mutable std::mutex mutex_;
  std::unordered_map<BindingKey, SlotState, BindingKeyHash> slots_;
  std::uint64_t publication_epoch_ = 0;

  friend class Transaction;
};

class NotebookSlotTable::Transaction {
public:
  Transaction() = default;
  explicit Transaction(NotebookSlotTable *owner) : owner_(owner) {}
  Transaction(const Transaction &) = delete;
  Transaction &operator=(const Transaction &) = delete;
  Transaction(Transaction &&other) noexcept;
  Transaction &operator=(Transaction &&other) noexcept;
  ~Transaction();

  // Staging only changes this transaction.  If metadata is omitted, the
  // existing provider metadata is retained (or a key-derived default is used
  // for a new slot).
  bool stage(const BindingKey &key, runtime::Value value);
  bool stage(const BindingKey &key, runtime::Value value,
             SlotProviderMetadata provider);
  bool stage_write(const BindingKey &key, runtime::Value value) {
    return stage(key, std::move(value));
  }
  bool stage_write(const BindingKey &key, runtime::Value value,
                   SlotProviderMetadata provider) {
    return stage(key, std::move(value), std::move(provider));
  }
  bool write(const BindingKey &key, runtime::Value value) {
    return stage(key, std::move(value));
  }
  bool write(const BindingKey &key, runtime::Value value,
             SlotProviderMetadata provider) {
    return stage(key, std::move(value), std::move(provider));
  }

  bool stage_stale(const BindingKey &key, bool stale = true);
  bool mark_stale(const BindingKey &key, bool stale = true) {
    return stage_stale(key, stale);
  }

  SlotCommitResult commit();
  void rollback() noexcept;
  bool active() const noexcept { return owner_ != nullptr && !finished_; }
  std::size_t staged_count() const noexcept { return writes_.size(); }

private:
  NotebookSlotTable *owner_ = nullptr;
  std::map<BindingKey, TransactionWrite> writes_;
  bool finished_ = false;
};

using SlotTable = NotebookSlotTable;

inline NotebookSlotTable::Transaction NotebookSlotTable::transaction() {
  return begin_transaction();
}

inline NotebookSlotTable::Transaction NotebookSlotTable::begin() {
  return begin_transaction();
}

} // namespace amber::notebook
