#include "runtime/watch.h"
#include "runtime/watch_internal.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace sputnik::runtime {

namespace {

void notify_watch_activity(
    const std::shared_ptr<const RuntimeWatchActivityNotifier> &notifier)
    noexcept {
  if (notifier && *notifier) {
    try {
      (*notifier)();
    } catch (...) {
      // This is an advisory wake, not part of event publication. In particular
      // a host callback failure must not turn an already committed write into
      // an apparent VM failure or prevent other stream waiters from waking.
    }
  }
}

std::uint64_t allocate_runtime_watch_world_id() {
  static std::atomic<std::uint64_t> next_id{1U};
  std::uint64_t current = next_id.load(std::memory_order_relaxed);
  while (current != 0U &&
         current != std::numeric_limits<std::uint64_t>::max()) {
    if (next_id.compare_exchange_weak(current, current + 1U,
                                      std::memory_order_relaxed,
                                      std::memory_order_relaxed)) {
      return current;
    }
  }
  throw std::overflow_error("runtime watch world id space exhausted");
}

} // namespace

RuntimeWatchStream::RuntimeWatchStream()
    : identity_{allocate_runtime_watch_world_id(), 1U} {}

void RuntimeWatchStream::configure_activity_notifier(
    RuntimeWatchActivityNotifier notifier) {
  std::shared_ptr<const RuntimeWatchActivityNotifier> next;
  if (notifier) {
    next = std::make_shared<const RuntimeWatchActivityNotifier>(
        std::move(notifier));
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    activity_notifier_.swap(next);
  }
  // Release the previous host closure outside the stream mutex as well.
}

void RuntimeWatchStream::configure_capacity(std::size_t capacity) {
  std::shared_ptr<const RuntimeWatchActivityNotifier> notifier;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    capacity_ = std::max<std::size_t>(1U, capacity);
    trim_locked();
    notifier = activity_notifier_;
  }
  // A shrink can turn a retained cursor into an overflow result even without
  // appending an event. Wake every waiter so it can revalidate that cursor.
  condition_.notify_all();
  notify_watch_activity(notifier);
}

std::uint64_t RuntimeWatchStream::allocate_cell_id() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (next_cell_id_ == std::numeric_limits<std::uint64_t>::max()) {
    throw std::overflow_error("runtime watch cell id space exhausted");
  }
  return next_cell_id_++;
}

std::uint64_t RuntimeWatchStream::allocate_handle_id() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (next_handle_id_ == std::numeric_limits<std::uint64_t>::max()) {
    throw std::overflow_error("runtime watch handle id space exhausted");
  }
  return next_handle_id_++;
}

RuntimeWatchEvent RuntimeWatchStream::record(RuntimeWatchEvent event) {
  std::shared_ptr<const RuntimeWatchActivityNotifier> notifier;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (latest_epoch_ == std::numeric_limits<std::uint64_t>::max() - 1U) {
      throw std::overflow_error("runtime watch epoch space exhausted");
    }
    const std::uint64_t next_epoch = latest_epoch_ + 1U;
    event.watch_world_id = identity_.world_id;
    event.watch_generation = identity_.generation;
    event.watch_epoch = next_epoch;
    // Copy/allocate before publishing the scalar epoch. If deque growth throws,
    // the stream remains contiguous and the caller may retry the event.
    events_.push_back(event);
    latest_epoch_ = next_epoch;
    trim_locked();
    notifier = activity_notifier_;
  }
  // Publish the event while holding mutex_, then notify outside it so a woken
  // consumer can acquire the stream immediately.
  condition_.notify_all();
  notify_watch_activity(notifier);
  return event;
}

RuntimeWatchStreamIdentity RuntimeWatchStream::identity() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return identity_;
}

std::uint64_t RuntimeWatchStream::latest_epoch() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return latest_epoch_;
}

RuntimeWatchCursor RuntimeWatchStream::tail_cursor() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return RuntimeWatchCursor{identity_, latest_epoch_ + 1U};
}

RuntimeWatchPollResult
RuntimeWatchStream::poll(const RuntimeWatchCursor &cursor,
                         std::size_t max_events) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return poll_locked(cursor, max_events);
}

RuntimeWatchPollResult RuntimeWatchStream::wait(
    const RuntimeWatchCursor &cursor, std::chrono::milliseconds timeout,
    std::size_t max_events) const {
  std::unique_lock<std::mutex> lock(mutex_);
  if (timeout <= std::chrono::milliseconds::zero()) {
    RuntimeWatchPollResult result = poll_locked(cursor, max_events);
    result.timed_out = result.ok() && result.events.empty();
    return result;
  }

  // Predicate wait_for implementations may add the duration before clamping.
  // Convert only after checking the clock's remaining representable range.
  using Clock = std::chrono::steady_clock;
  const auto now = Clock::now();
  const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
      Clock::time_point::max() - now);
  const auto deadline = timeout >= remaining ? Clock::time_point::max()
                                             : now + timeout;
  const bool ready = condition_.wait_until(
      lock, deadline, [this, &cursor] { return wait_ready_locked(cursor); });
  RuntimeWatchPollResult result = poll_locked(cursor, max_events);
  result.timed_out = !ready && result.ok() && result.events.empty();
  return result;
}

RuntimeWatchPollResult RuntimeWatchStream::poll_locked(
    const RuntimeWatchCursor &cursor, std::size_t max_events) const {
  RuntimeWatchPollResult result;
  result.source = identity_;
  result.requested_cursor = cursor;
  result.latest_epoch = latest_epoch_;
  result.oldest_retained_epoch = oldest_retained_epoch_locked();
  result.next_cursor = RuntimeWatchCursor{identity_, latest_epoch_ + 1U};

  if (!cursor.valid()) {
    result.status = RuntimeWatchPollStatus::InvalidCursor;
    return result;
  }
  if (cursor.source != identity_) {
    result.status = RuntimeWatchPollStatus::SourceChanged;
    return result;
  }
  if (cursor.next_epoch > latest_epoch_ + 1U) {
    result.status = RuntimeWatchPollStatus::InvalidCursor;
    return result;
  }
  if (cursor.next_epoch < result.oldest_retained_epoch) {
    result.status = RuntimeWatchPollStatus::Overflow;
    return result;
  }

  result.status = RuntimeWatchPollStatus::Ok;
  const std::size_t limit =
      max_events == 0U ? std::numeric_limits<std::size_t>::max() : max_events;
  for (const RuntimeWatchEvent &event : events_) {
    if (event.watch_epoch < cursor.next_epoch) {
      continue;
    }
    if (result.events.size() == limit) {
      break;
    }
    result.events.push_back(event);
  }
  if (!result.events.empty()) {
    result.next_cursor.next_epoch = result.events.back().watch_epoch + 1U;
  } else {
    result.next_cursor.next_epoch = cursor.next_epoch;
  }
  return result;
}

std::vector<RuntimeWatchEvent> RuntimeWatchStream::events_snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return std::vector<RuntimeWatchEvent>(events_.begin(), events_.end());
}

std::uint64_t RuntimeWatchStream::oldest_retained_epoch_locked() const
    noexcept {
  return events_.empty() ? latest_epoch_ + 1U : events_.front().watch_epoch;
}

bool RuntimeWatchStream::wait_ready_locked(
    const RuntimeWatchCursor &cursor) const noexcept {
  if (!cursor.valid() || cursor.source != identity_) {
    return true;
  }
  if (cursor.next_epoch > latest_epoch_ + 1U ||
      cursor.next_epoch < oldest_retained_epoch_locked()) {
    return true;
  }
  return cursor.next_epoch <= latest_epoch_;
}

void RuntimeWatchStream::trim_locked() {
  while (events_.size() > capacity_) {
    events_.pop_front();
  }
}

class RuntimeWatchCell::Impl {
public:
  Impl(Value value, std::uint64_t cell_id, std::string target_name,
       RuntimeWatchEventSink event_sink)
      : value_(std::move(value)), cell_id_(cell_id),
        target_name_(std::move(target_name)),
        event_sink_(std::move(event_sink)) {}

  Value read() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return value_;
  }

  RuntimeWatchWriteResult write(Value value) {
    // Keep the state mutation and event-sink invocation in one per-source
    // delivery order. The state mutex is still released before invoking host
    // code, so snapshots/reads from a sink remain safe. Recursive delivery
    // permits a sink to synchronously write the same source; invocation order
    // is then nested but still begins in revision order.
    std::lock_guard<std::recursive_mutex> delivery_lock(delivery_mutex_);
    RuntimeWatchWriteResult result;
    RuntimeWatchEventSink event_sink;
    RuntimeWatchEvent event;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      result.old_value = value_;
      result.new_value = value;
      result.old_revision = revision_;
      result.new_revision = revision_;
      if (!watched_) {
        value_ = std::move(value);
        result.new_value = value_;
        return result;
      }
      if (revision_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error(
            "runtime watch cell revision space exhausted");
      }
      const std::uint64_t next_revision = revision_ + 1U;
      // Finish every potentially throwing string/function copy before the
      // value and revision become visible. Value operations below are noexcept.
      event.kind = "watch.write";
      event.cell_id = cell_id_;
      event.target_name = target_name_;
      event.old_revision = result.old_revision;
      event.new_revision = next_revision;
      event.old_value = result.old_value;
      event.new_value = value;
      event_sink = event_sink_;
      value_ = std::move(value);
      revision_ = next_revision;
      result.changed = true;
      result.new_revision = revision_;
      result.new_value = value_;
    }
    if (event_sink) {
      event_sink(std::move(event));
    }
    return result;
  }

  RuntimeWatchCellSnapshot snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    RuntimeWatchCellSnapshot snapshot;
    snapshot.cell_id = cell_id_;
    snapshot.revision = revision_;
    snapshot.subscriber_count = subscriber_count_;
    snapshot.target_name = target_name_;
    snapshot.watched = watched_;
    snapshot.value = value_;
    return snapshot;
  }

  bool watched() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return watched_;
  }

  void enable_watch(std::string target_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    watched_ = true;
    if (!target_name.empty()) {
      target_name_ = std::move(target_name);
    }
  }

  void subscribe() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (subscriber_count_ == std::numeric_limits<std::uint64_t>::max()) {
      throw std::overflow_error(
          "runtime watch cell subscriber space exhausted");
    }
    watched_ = true;
    ++subscriber_count_;
  }

  void unsubscribe() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (subscriber_count_ > 0) {
      --subscriber_count_;
    }
  }

  std::uint64_t cell_id() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cell_id_;
  }

private:
  mutable std::recursive_mutex delivery_mutex_;
  mutable std::mutex mutex_;
  Value value_ = Value::null();
  std::uint64_t cell_id_ = 0;
  std::uint64_t revision_ = 0;
  std::uint64_t subscriber_count_ = 0;
  std::string target_name_;
  bool watched_ = false;
  RuntimeWatchEventSink event_sink_;
};

RuntimeWatchCell::RuntimeWatchCell(Value value, std::uint64_t cell_id,
                                   std::string target_name,
                                   RuntimeWatchEventSink event_sink)
    : impl_(std::make_shared<Impl>(std::move(value), cell_id,
                                   std::move(target_name),
                                   std::move(event_sink))) {}

Value RuntimeWatchCell::read() const { return impl_->read(); }

RuntimeWatchWriteResult RuntimeWatchCell::write(Value value) {
  return impl_->write(std::move(value));
}

RuntimeWatchCellSnapshot RuntimeWatchCell::snapshot() const {
  return impl_->snapshot();
}

bool RuntimeWatchCell::watched() const { return impl_->watched(); }

void RuntimeWatchCell::enable_watch(std::string target_name) {
  impl_->enable_watch(std::move(target_name));
}

void RuntimeWatchCell::subscribe() { impl_->subscribe(); }

void RuntimeWatchCell::unsubscribe() { impl_->unsubscribe(); }

std::uint64_t RuntimeWatchCell::cell_id() const { return impl_->cell_id(); }

class RuntimeWatchObjectState::Impl {
public:
  explicit Impl(std::uint64_t object_id, RuntimeWatchEventSink event_sink)
      : object_id_(object_id), event_sink_(std::move(event_sink)) {}

  RuntimeWatchObjectStateSnapshot snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    RuntimeWatchObjectStateSnapshot snapshot;
    snapshot.object_id = object_id_;
    snapshot.object_revision = object_revision_;
    snapshot.subscriber_count = subscriber_count_;
    for (const auto &[name, field] : fields_) {
      snapshot.field_revisions[name] = field.revision;
    }
    return snapshot;
  }

  RuntimeWatchIvarSnapshot snapshot_field(const std::string &field_name,
                                          Value current_value) const {
    std::lock_guard<std::mutex> lock(mutex_);
    RuntimeWatchIvarSnapshot snapshot;
    snapshot.object_id = object_id_;
    snapshot.object_revision = object_revision_;
    snapshot.field_name = field_name;
    snapshot.value = std::move(current_value);
    const auto found = fields_.find(field_name);
    if (found != fields_.end()) {
      snapshot.field_revision = found->second.revision;
      snapshot.subscriber_count = found->second.subscriber_count;
      snapshot.watched = found->second.watched;
    }
    return snapshot;
  }

  RuntimeWatchIvarWriteResult write_field(std::string field_name,
                                          Value old_value, Value new_value) {
    // Object revision is shared by all fields, so serialize mutation plus
    // delivery for the whole object rather than independently per field.
    std::lock_guard<std::recursive_mutex> delivery_lock(delivery_mutex_);
    RuntimeWatchIvarWriteResult result;
    RuntimeWatchEventSink event_sink;
    RuntimeWatchEvent event;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      result.field_name = field_name;
      result.old_value = std::move(old_value);
      result.new_value = std::move(new_value);
      FieldState &field = fields_[field_name];
      result.old_revision = field.revision;
      result.new_revision = field.revision;
      result.old_object_revision = object_revision_;
      result.new_object_revision = object_revision_;
      if (!field.watched) {
        return result;
      }
      if (field.revision == std::numeric_limits<std::uint64_t>::max() ||
          object_revision_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error(
            "runtime watch object revision space exhausted");
      }
      const std::uint64_t next_field_revision = field.revision + 1U;
      const std::uint64_t next_object_revision = object_revision_ + 1U;
      event.kind = "watch.ivar.write";
      event.object_id = object_id_;
      event.old_object_revision = result.old_object_revision;
      event.new_object_revision = next_object_revision;
      event.target_name = "@" + field_name;
      event.field_name = field_name;
      event.old_revision = result.old_revision;
      event.new_revision = next_field_revision;
      event.old_value = result.old_value;
      event.new_value = result.new_value;
      event_sink = event_sink_;
      field.revision = next_field_revision;
      object_revision_ = next_object_revision;
      result.changed = true;
      result.new_revision = field.revision;
      result.new_object_revision = object_revision_;
    }
    if (event_sink) {
      event_sink(std::move(event));
    }
    return result;
  }

  void subscribe_field(std::string field_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (subscriber_count_ == std::numeric_limits<std::uint64_t>::max()) {
      throw std::overflow_error(
          "runtime watch object subscriber space exhausted");
    }
    FieldState &field = fields_[std::move(field_name)];
    if (field.subscriber_count ==
        std::numeric_limits<std::uint64_t>::max()) {
      throw std::overflow_error(
          "runtime watch field subscriber space exhausted");
    }
    field.watched = true;
    ++field.subscriber_count;
    ++subscriber_count_;
  }

  void unsubscribe_field(const std::string &field_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = fields_.find(field_name);
    if (found == fields_.end()) {
      return;
    }
    if (found->second.subscriber_count > 0) {
      --found->second.subscriber_count;
    }
    if (subscriber_count_ > 0) {
      --subscriber_count_;
    }
  }

  bool field_watched(const std::string &field_name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = fields_.find(field_name);
    return found != fields_.end() && found->second.watched;
  }

  std::uint64_t object_id() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return object_id_;
  }

private:
  struct FieldState {
    std::uint64_t revision = 0;
    std::uint64_t subscriber_count = 0;
    bool watched = false;
  };

  mutable std::recursive_mutex delivery_mutex_;
  mutable std::mutex mutex_;
  std::uint64_t object_id_ = 0;
  std::uint64_t object_revision_ = 0;
  std::uint64_t subscriber_count_ = 0;
  std::unordered_map<std::string, FieldState> fields_;
  RuntimeWatchEventSink event_sink_;
};

RuntimeWatchObjectState::RuntimeWatchObjectState(
    std::uint64_t object_id, RuntimeWatchEventSink event_sink)
    : impl_(std::make_shared<Impl>(object_id, std::move(event_sink))) {}

RuntimeWatchObjectStateSnapshot RuntimeWatchObjectState::snapshot() const {
  return impl_->snapshot();
}

RuntimeWatchIvarSnapshot
RuntimeWatchObjectState::snapshot_field(const std::string &field_name,
                                        Value current_value) const {
  return impl_->snapshot_field(field_name, std::move(current_value));
}

RuntimeWatchIvarWriteResult
RuntimeWatchObjectState::write_field(std::string field_name, Value old_value,
                                     Value new_value) {
  return impl_->write_field(std::move(field_name), std::move(old_value),
                            std::move(new_value));
}

void RuntimeWatchObjectState::subscribe_field(std::string field_name) {
  impl_->subscribe_field(std::move(field_name));
}

void RuntimeWatchObjectState::unsubscribe_field(const std::string &field_name) {
  impl_->unsubscribe_field(field_name);
}

bool RuntimeWatchObjectState::field_watched(
    const std::string &field_name) const {
  return impl_->field_watched(field_name);
}

std::uint64_t RuntimeWatchObjectState::object_id() const {
  return impl_->object_id();
}

class RuntimeWatchHandle::Impl {
public:
  Impl(std::shared_ptr<RuntimeWatchCell> cell, std::uint64_t handle_id,
       std::string target_name)
      : cell_(std::move(cell)), handle_id_(handle_id),
        target_name_(std::move(target_name)) {
    if (cell_ != nullptr) {
      cell_->enable_watch(target_name_);
      cell_->subscribe();
      active_ = true;
    }
  }

  Impl(std::shared_ptr<RuntimeWatchObjectState> object_state,
       std::uint64_t handle_id, std::string target_name, std::string field_name)
      : object_state_(std::move(object_state)), handle_id_(handle_id),
        target_name_(std::move(target_name)),
        field_name_(std::move(field_name)) {
    if (object_state_ != nullptr) {
      object_state_->subscribe_field(field_name_);
      active_ = true;
    }
  }

  ~Impl() { unwatch(); }

  bool active() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_;
  }

  bool unwatch() {
    std::shared_ptr<RuntimeWatchCell> cell;
    std::shared_ptr<RuntimeWatchObjectState> object_state;
    std::string field_name;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!active_) {
        return false;
      }
      active_ = false;
      cell = cell_;
      object_state = object_state_;
      field_name = field_name_;
    }
    if (cell != nullptr) {
      cell->unsubscribe();
    }
    if (object_state != nullptr) {
      object_state->unsubscribe_field(field_name);
    }
    return true;
  }

  std::uint64_t handle_id() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return handle_id_;
  }

  std::shared_ptr<RuntimeWatchCell> cell() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cell_;
  }

  RuntimeWatchCellSnapshot snapshot() const {
    std::shared_ptr<RuntimeWatchCell> cell;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      cell = cell_;
    }
    return cell == nullptr ? RuntimeWatchCellSnapshot{} : cell->snapshot();
  }

private:
  mutable std::mutex mutex_;
  std::shared_ptr<RuntimeWatchCell> cell_;
  std::shared_ptr<RuntimeWatchObjectState> object_state_;
  std::uint64_t handle_id_ = 0;
  std::string target_name_;
  std::string field_name_;
  bool active_ = false;
};

RuntimeWatchHandle::RuntimeWatchHandle()
    : impl_(std::make_shared<Impl>(nullptr, 0, "")) {}

RuntimeWatchHandle::RuntimeWatchHandle(std::shared_ptr<RuntimeWatchCell> cell,
                                       std::uint64_t handle_id,
                                       std::string target_name)
    : impl_(std::make_shared<Impl>(std::move(cell), handle_id,
                                   std::move(target_name))) {}

RuntimeWatchHandle::RuntimeWatchHandle(
    std::shared_ptr<RuntimeWatchObjectState> object_state,
    std::uint64_t handle_id, std::string target_name, std::string field_name)
    : impl_(std::make_shared<Impl>(std::move(object_state), handle_id,
                                   std::move(target_name),
                                   std::move(field_name))) {}

RuntimeWatchHandle::RuntimeWatchHandle(RuntimeWatchHandle &&) noexcept =
    default;

RuntimeWatchHandle &
RuntimeWatchHandle::operator=(RuntimeWatchHandle &&) noexcept = default;

RuntimeWatchHandle::~RuntimeWatchHandle() = default;

bool RuntimeWatchHandle::active() const { return impl_->active(); }

bool RuntimeWatchHandle::unwatch() { return impl_->unwatch(); }

std::uint64_t RuntimeWatchHandle::handle_id() const {
  return impl_->handle_id();
}

std::shared_ptr<RuntimeWatchCell> RuntimeWatchHandle::cell() const {
  return impl_->cell();
}

RuntimeWatchCellSnapshot RuntimeWatchHandle::snapshot() const {
  return impl_->snapshot();
}

} // namespace sputnik::runtime
