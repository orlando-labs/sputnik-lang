#pragma once

#include "runtime/value.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace amber::runtime {

struct RuntimeWatchCellSnapshot {
  std::uint64_t cell_id = 0;
  std::uint64_t revision = 0;
  std::uint64_t subscriber_count = 0;
  std::string target_name;
  bool watched = false;
  Value value = Value::null();
};

struct RuntimeWatchWriteResult {
  bool changed = false;
  Value old_value = Value::null();
  Value new_value = Value::null();
  std::uint64_t old_revision = 0;
  std::uint64_t new_revision = 0;
};

struct RuntimeWatchObjectStateSnapshot {
  std::uint64_t object_id = 0;
  std::uint64_t object_revision = 0;
  std::uint64_t subscriber_count = 0;
  std::unordered_map<std::string, std::uint64_t> field_revisions;
};

struct RuntimeWatchIvarSnapshot {
  std::uint64_t object_id = 0;
  std::uint64_t object_revision = 0;
  std::uint64_t field_revision = 0;
  std::uint64_t subscriber_count = 0;
  std::string field_name;
  bool watched = false;
  Value value = Value::null();
};

struct RuntimeWatchIvarWriteResult {
  bool changed = false;
  std::string field_name;
  Value old_value = Value::null();
  Value new_value = Value::null();
  std::uint64_t old_revision = 0;
  std::uint64_t new_revision = 0;
  std::uint64_t old_object_revision = 0;
  std::uint64_t new_object_revision = 0;
};

struct RuntimeWatchEvent {
  std::string kind;
  std::uint64_t watch_world_id = 0;
  std::uint64_t watch_generation = 0;
  std::uint64_t watch_epoch = 0;
  std::uint64_t cell_id = 0;
  std::uint64_t handle_id = 0;
  std::uint64_t object_id = 0;
  std::uint64_t old_object_revision = 0;
  std::uint64_t new_object_revision = 0;
  std::string target_name;
  std::string field_name;
  std::uint64_t old_revision = 0;
  std::uint64_t new_revision = 0;
  Value old_value = Value::null();
  Value new_value = Value::null();
};

// Optional owner hook used by RuntimeWorld-backed watch storage. It lets a
// host mutate a watch cell obtained from a handle without bypassing the
// world's bounded event stream. Invocations for one binding/object begin in
// revision order and run without its state mutex; same-thread reentrant writes
// are permitted and nest their sink invocation inside the outer callback. A
// sink must publish synchronously if it needs that order reflected externally;
// if it throws, the value/revision mutation remains committed and the exception
// propagates after releasing the state mutex. Custom sinks must not form a
// cross-thread cycle of synchronous writes between different watched sources;
// RuntimeWorld's built-in sink only appends to its stream and has no such edge.
using RuntimeWatchEventSink = std::function<void(RuntimeWatchEvent)>;

// Availability hint only: implementations must be short, thread-safe and must
// not execute UI, enter RuntimeWorld, or mutate watched sources. It is invoked
// outside the stream mutex, but producer/world locks may still be held. Calls
// may overlap across producers. A thrown exception is isolated from the
// already committed event; consumers still use cursor polling for truth.
using RuntimeWatchActivityNotifier = std::function<void()>;

// Identity of one append-only watch-event source. A package hot reload shares
// this identity with the old RuntimeState so parked/old VMs cannot fork the
// sequence or reuse watch-cell ids. A different RuntimeWorld has a different
// world_id; generation is reserved for an explicit future source reset.
struct RuntimeWatchStreamIdentity {
  std::uint64_t world_id = 0;
  std::uint64_t generation = 0;

  bool valid() const noexcept { return world_id != 0U && generation != 0U; }
  bool operator==(const RuntimeWatchStreamIdentity &other) const noexcept {
    return world_id == other.world_id && generation == other.generation;
  }
  bool operator!=(const RuntimeWatchStreamIdentity &other) const noexcept {
    return !(*this == other);
  }
};

// next_epoch is the first event not yet acknowledged by a consumer. Cursors
// are immutable values: polling returns a candidate successor and the caller
// publishes/acks it only after downstream planning succeeds.
struct RuntimeWatchCursor {
  RuntimeWatchStreamIdentity source;
  std::uint64_t next_epoch = 1;

  bool valid() const noexcept {
    return source.valid() && next_epoch != 0U;
  }
};

enum class RuntimeWatchPollStatus {
  Ok,
  Overflow,
  SourceChanged,
  InvalidCursor,
};

struct RuntimeWatchPollResult {
  RuntimeWatchPollStatus status = RuntimeWatchPollStatus::Ok;
  // Set only by the blocking wait API when its deadline expires without an
  // event or cursor-status transition. Ordinary poll results always leave it
  // false. A timeout does not advance or acknowledge requested_cursor.
  bool timed_out = false;
  RuntimeWatchStreamIdentity source;
  // Exact immutable cursor supplied to poll()/wait_watch_events(). Together
  // with next_cursor this makes a successful result a self-contained
  // acknowledgement unit that a host/kernel boundary can validate without
  // trusting out-of-band state.
  RuntimeWatchCursor requested_cursor;
  RuntimeWatchCursor next_cursor;
  std::uint64_t oldest_retained_epoch = 1;
  std::uint64_t latest_epoch = 0;
  std::vector<RuntimeWatchEvent> events;

  bool ok() const noexcept { return status == RuntimeWatchPollStatus::Ok; }
  bool gap() const noexcept { return status == RuntimeWatchPollStatus::Overflow; }
};

enum class RuntimeDependencyKind { Binding, Ivar, Object };

// Stable identity of a runtime dependency source.  Revisions deliberately do
// not participate in this key: a dependency remains subscribed to the same
// source while its observed revision is replaced by a later successful cell
// run.  Binding target names are diagnostic metadata, not identity; runtime
// watch cells already provide the stable binding identity.
struct RuntimeDependencySourceKey {
  RuntimeDependencyKind kind = RuntimeDependencyKind::Binding;
  std::uint64_t cell_id = 0;
  std::uint64_t object_id = 0;
  std::string field_name;

  bool operator==(const RuntimeDependencySourceKey &other) const {
    return kind == other.kind && cell_id == other.cell_id &&
           object_id == other.object_id && field_name == other.field_name;
  }
  bool operator!=(const RuntimeDependencySourceKey &other) const {
    return !(*this == other);
  }
  bool operator<(const RuntimeDependencySourceKey &other) const {
    const int this_kind = static_cast<int>(kind);
    const int other_kind = static_cast<int>(other.kind);
    if (this_kind != other_kind) {
      return this_kind < other_kind;
    }
    if (cell_id != other.cell_id) {
      return cell_id < other.cell_id;
    }
    if (object_id != other.object_id) {
      return object_id < other.object_id;
    }
    return field_name < other.field_name;
  }
};

struct RuntimeDependencySourceKeyHash {
  std::size_t operator()(const RuntimeDependencySourceKey &key) const noexcept {
    const std::size_t kind =
        std::hash<int>{}(static_cast<int>(key.kind));
    const std::size_t cell = std::hash<std::uint64_t>{}(key.cell_id);
    const std::size_t object = std::hash<std::uint64_t>{}(key.object_id);
    const std::size_t field = std::hash<std::string>{}(key.field_name);
    std::size_t result = kind;
    result ^= cell + static_cast<std::size_t>(0x9e3779b9U) +
              (result << 6U) + (result >> 2U);
    result ^= object + static_cast<std::size_t>(0x9e3779b9U) +
              (result << 6U) + (result >> 2U);
    result ^= field + static_cast<std::size_t>(0x9e3779b9U) +
              (result << 6U) + (result >> 2U);
    return result;
  }
};

struct RuntimeDependency {
  RuntimeDependencyKind kind = RuntimeDependencyKind::Binding;
  std::uint64_t cell_id = 0;
  std::uint64_t object_id = 0;
  std::string target_name;
  std::string field_name;
  std::uint64_t revision = 0;
  std::uint64_t object_revision = 0;

  RuntimeDependencySourceKey source_key() const {
    RuntimeDependencySourceKey key;
    key.kind = kind;
    switch (kind) {
    case RuntimeDependencyKind::Binding:
      key.cell_id = cell_id;
      break;
    case RuntimeDependencyKind::Ivar:
      key.object_id = object_id;
      key.field_name = field_name;
      break;
    case RuntimeDependencyKind::Object:
      key.object_id = object_id;
      break;
    }
    return key;
  }
};

struct RuntimeDependencySet {
  std::uint64_t notebook_cell_id = 0;
  // Numeric watch-cell/object ids are meaningful only inside this stream
  // namespace. Every world-produced capture, including a successful empty
  // one, carries a valid source identity.
  RuntimeWatchStreamIdentity source;
  std::vector<RuntimeDependency> dependencies;
};

class RuntimeWatchCell {
public:
  explicit RuntimeWatchCell(Value value = Value::null(),
                            std::uint64_t cell_id = 0,
                            std::string target_name = {},
                            RuntimeWatchEventSink event_sink = {});

  Value read() const;
  RuntimeWatchWriteResult write(Value value);
  RuntimeWatchCellSnapshot snapshot() const;
  bool watched() const;
  void enable_watch(std::string target_name = {});
  void subscribe();
  void unsubscribe();
  std::uint64_t cell_id() const;

private:
  class Impl;
  std::shared_ptr<Impl> impl_;
};

class RuntimeWatchObjectState {
public:
  explicit RuntimeWatchObjectState(std::uint64_t object_id = 0,
                                   RuntimeWatchEventSink event_sink = {});

  RuntimeWatchObjectStateSnapshot snapshot() const;
  RuntimeWatchIvarSnapshot snapshot_field(const std::string &field_name,
                                          Value current_value) const;
  RuntimeWatchIvarWriteResult write_field(std::string field_name,
                                          Value old_value, Value new_value);
  void subscribe_field(std::string field_name);
  void unsubscribe_field(const std::string &field_name);
  bool field_watched(const std::string &field_name) const;
  std::uint64_t object_id() const;

private:
  class Impl;
  std::shared_ptr<Impl> impl_;
};

class RuntimeWatchHandle {
public:
  RuntimeWatchHandle();
  RuntimeWatchHandle(std::shared_ptr<RuntimeWatchCell> cell,
                     std::uint64_t handle_id, std::string target_name);
  RuntimeWatchHandle(std::shared_ptr<RuntimeWatchObjectState> object_state,
                     std::uint64_t handle_id, std::string target_name,
                     std::string field_name);
  RuntimeWatchHandle(const RuntimeWatchHandle &) = delete;
  RuntimeWatchHandle &operator=(const RuntimeWatchHandle &) = delete;
  RuntimeWatchHandle(RuntimeWatchHandle &&) noexcept;
  RuntimeWatchHandle &operator=(RuntimeWatchHandle &&) noexcept;
  ~RuntimeWatchHandle();

  bool active() const;
  bool unwatch();
  std::uint64_t handle_id() const;
  std::shared_ptr<RuntimeWatchCell> cell() const;
  RuntimeWatchCellSnapshot snapshot() const;

private:
  class Impl;
  std::shared_ptr<Impl> impl_;
};

} // namespace amber::runtime
