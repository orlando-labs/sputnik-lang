#include "runtime/context.h"
#include "runtime/reactor.h"

#include <memory>
#include <limits>
#include <utility>

namespace sputnik::runtime {

thread_local std::uint64_t tls_runtime_worker_id = 0;
thread_local std::uint64_t tls_runtime_strand_id = 0;
thread_local std::uint64_t tls_runtime_task_id = 0;
thread_local const void *tls_runtime_scheduler_identity = nullptr;
thread_local std::uint64_t tls_runtime_sync_owner_id = 0;
thread_local const std::atomic<bool> *tls_runtime_task_cancel_flag = nullptr;
thread_local std::shared_ptr<RuntimeTaskContext> tls_runtime_task_context;
thread_local std::uint32_t tls_runtime_task_sync_depth = 0;
thread_local std::shared_ptr<RuntimeTextWriter> tls_runtime_stdout;
thread_local std::shared_ptr<RuntimeTextWriter> tls_runtime_stderr;
thread_local std::string tls_runtime_task_annotation;
thread_local std::uint64_t tls_runtime_native_thread_id = 0;
thread_local RuntimeTextSourceLocation tls_runtime_text_source_location;
thread_local RuntimeTextSourceLocationProviderFn
    tls_runtime_text_source_location_provider = nullptr;
thread_local const void *tls_runtime_text_source_location_provider_ctx =
    nullptr;
thread_local const RuntimeIoWaitObserver *tls_runtime_io_wait_observer =
    nullptr;
thread_local std::uint32_t tls_runtime_io_wait_depth = 0;
namespace {
thread_local RuntimeRunCancellation tls_run_cancellation;
thread_local unsigned tls_run_cancellation_mask = 0;
thread_local std::shared_ptr<std::atomic<bool>> tls_task_cancel_owner;
}
std::shared_ptr<std::atomic<bool>> current_runtime_task_cancel_owner() {
  return tls_task_cancel_owner;
}

RuntimeRunCancellation current_runtime_run_cancellation() {
  return tls_run_cancellation;
}
bool runtime_run_cancel_requested() {
  return tls_run_cancellation && tls_run_cancellation->cancelled();
}
bool runtime_run_cancel_checkpoint_requested() {
  return tls_run_cancellation_mask == 0 && runtime_run_cancel_requested();
}
const std::atomic<bool> *runtime_wait_cancel_flag() {
  if (tls_run_cancellation_mask && runtime_run_cancel_requested()) return nullptr;
  return tls_runtime_task_cancel_flag != nullptr ? tls_runtime_task_cancel_flag
      : tls_run_cancellation ? tls_run_cancellation->cancel_flag() : nullptr;
}

std::shared_ptr<RuntimeRunState::Task> RuntimeRunState::register_task() {
  auto task = std::shared_ptr<Task>(new Task(shared_from_this()));
  std::lock_guard<std::mutex> lock(mutex_);
  if (root_closed_ && tasks_.empty())
    throw RuntimeTaskFailure("LifetimeError", "execution run has already drained");
  if (next_task_ == std::numeric_limits<std::uint64_t>::max())
    throw RuntimeTaskFailure("RuntimeError", "run task identity exhausted");
  const auto id = next_task_++;
  tasks_.emplace(id, std::function<void()>{});
  task->id_ = id;
  return task;
}
RuntimeRunState::Task::~Task() {
  if (!id_) return;
  std::function<void()> retired;
  {
    std::lock_guard<std::mutex> lock(run_->mutex_);
    auto found = run_->tasks_.find(id_);
    if (found != run_->tasks_.end()) {
      retired = std::move(found->second);
      run_->tasks_.erase(found);
    }
  }
  run_->idle_.notify_all();
}
void RuntimeRunState::Task::on_cancel(std::function<void()> callback) {
  bool cancel_now;
  {
    std::lock_guard<std::mutex> lock(run_->mutex_);
    cancel_now = run_->cancelled();
    if (!cancel_now) run_->tasks_.at(id_) = callback;
  }
  if (cancel_now && callback) callback();
}
void RuntimeRunState::request_cancel() {
  std::vector<std::function<void()>> callbacks;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (cancelled_.exchange(true)) return;
    for (const auto &entry : tasks_) if (entry.second) callbacks.push_back(entry.second);
  }
  // Never take a scheduler/world lock under the run ledger lock. Completion
  // and concurrent descendant registration may re-enter this ledger.
  std::exception_ptr failure;
  for (const auto &callback : callbacks) {
    try { callback(); } catch (...) { if (!failure) failure = std::current_exception(); }
  }
  RuntimeReactor::instance().kick();
  if (failure) std::rethrow_exception(failure);
}
std::size_t RuntimeRunState::active_tasks() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return tasks_.size();
}
void RuntimeRunState::close_root() {
  { std::lock_guard<std::mutex> lock(mutex_); root_closed_ = true; }
  idle_.notify_all();
}
bool RuntimeRunState::wait_for_idle(std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(mutex_);
  const auto drained = [&] { return root_closed_ && tasks_.empty(); };
  if (timeout == std::chrono::milliseconds::max()) { idle_.wait(lock, drained); return true; }
  return idle_.wait_for(lock, timeout, drained);
}
std::shared_ptr<RuntimeRunFailureReceipt>
RuntimeRunState::record_failure(RuntimeRunTaskFailure failure) {
  auto receipt = std::make_shared<RuntimeRunFailureReceipt>(std::move(failure));
  std::lock_guard<std::mutex> lock(mutex_);
  return failures_.emplace(receipt->failure.task_id, receipt).first->second;
}
std::vector<RuntimeRunTaskFailure> RuntimeRunState::unobserved_failures() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<RuntimeRunTaskFailure> result;
  for (const auto &entry : failures_)
    if (!entry.second->observed.load()) result.push_back(entry.second->failure);
  return result;
}
RuntimeRunCancellationScope::RuntimeRunCancellationScope(RuntimeRunCancellation token)
    : previous_(std::move(tls_run_cancellation)),
      previous_mask_(tls_run_cancellation_mask) {
  tls_run_cancellation = std::move(token);
  tls_run_cancellation_mask = 0;
}
RuntimeRunCancellationScope::~RuntimeRunCancellationScope() {
  tls_run_cancellation = std::move(previous_);
  tls_run_cancellation_mask = previous_mask_;
}
RuntimeRunCancellationMask::RuntimeRunCancellationMask(bool enabled) : enabled_(enabled) {
  if (enabled_) ++tls_run_cancellation_mask;
}
RuntimeRunCancellationMask::~RuntimeRunCancellationMask() {
  if (enabled_) --tls_run_cancellation_mask;
}
thread_local bool tls_runtime_io_park_enabled = false;
thread_local bool tls_runtime_io_park_requested = false;
thread_local RuntimeIoParkRequest tls_runtime_io_park_request{};

std::atomic<std::uint64_t> g_runtime_output_order{1};
std::atomic<std::uint64_t> g_runtime_native_thread_ids{1};
std::atomic<std::uint64_t> g_runtime_io_wait_ids{1};

namespace {
std::shared_ptr<const std::vector<std::string>> g_runtime_process_arguments =
    std::make_shared<const std::vector<std::string>>();
} // namespace

void set_runtime_process_arguments(std::vector<std::string> arguments) {
  std::atomic_store_explicit(
      &g_runtime_process_arguments,
      std::make_shared<const std::vector<std::string>>(std::move(arguments)),
      std::memory_order_release);
}

std::vector<std::string> current_runtime_process_arguments() {
  const std::shared_ptr<const std::vector<std::string>> snapshot =
      std::atomic_load_explicit(&g_runtime_process_arguments,
                                std::memory_order_acquire);
  return *snapshot;
}

RuntimeTextSourceLocationScope::RuntimeTextSourceLocationScope(
    RuntimeTextSourceLocation location)
    : previous_(std::move(tls_runtime_text_source_location)),
      previous_provider_(tls_runtime_text_source_location_provider),
      previous_provider_ctx_(tls_runtime_text_source_location_provider_ctx) {
  tls_runtime_text_source_location = std::move(location);
  tls_runtime_text_source_location_provider = nullptr;
  tls_runtime_text_source_location_provider_ctx = nullptr;
}

RuntimeTextSourceLocationScope::~RuntimeTextSourceLocationScope() {
  tls_runtime_text_source_location = std::move(previous_);
  tls_runtime_text_source_location_provider = previous_provider_;
  tls_runtime_text_source_location_provider_ctx = previous_provider_ctx_;
}

RuntimeTextSourceLocationProviderScope::RuntimeTextSourceLocationProviderScope(
    RuntimeTextSourceLocationProviderFn provider, const void *ctx)
    : previous_provider_(tls_runtime_text_source_location_provider),
      previous_provider_ctx_(tls_runtime_text_source_location_provider_ctx) {
  tls_runtime_text_source_location_provider = provider;
  tls_runtime_text_source_location_provider_ctx = ctx;
}

RuntimeTextSourceLocationProviderScope::
    ~RuntimeTextSourceLocationProviderScope() {
  tls_runtime_text_source_location_provider = previous_provider_;
  tls_runtime_text_source_location_provider_ctx = previous_provider_ctx_;
}

RuntimeTextSourceLocation resolve_runtime_text_source_location() {
  if (tls_runtime_text_source_location_provider != nullptr) {
    return tls_runtime_text_source_location_provider(
        tls_runtime_text_source_location_provider_ctx);
  }
  return tls_runtime_text_source_location;
}

RuntimeTaskScope::RuntimeTaskScope(std::uint64_t task_id,
                                   const std::atomic<bool> *cancel_flag,
                                   std::uint64_t sync_owner_id,
                                   std::shared_ptr<RuntimeTaskContext> task_context,
                                   const void *scheduler_identity,
                                   std::shared_ptr<std::atomic<bool>> cancel_owner)
    : previous_task_id_(tls_runtime_task_id),
      previous_scheduler_identity_(tls_runtime_scheduler_identity),
      previous_sync_owner_id_(tls_runtime_sync_owner_id),
      previous_cancel_flag_(tls_runtime_task_cancel_flag),
      previous_cancel_owner_(std::move(tls_task_cancel_owner)),
      previous_task_context_(std::move(tls_runtime_task_context)) {
  tls_runtime_task_id = task_id;
  tls_runtime_scheduler_identity = scheduler_identity;
  tls_runtime_sync_owner_id =
      sync_owner_id == 0 ? (task_id << 1U) : sync_owner_id;
  tls_runtime_task_cancel_flag = cancel_flag;
  tls_task_cancel_owner = std::move(cancel_owner);
  tls_runtime_task_context = std::move(task_context);
}

RuntimeTaskScope::~RuntimeTaskScope() {
  tls_runtime_task_id = previous_task_id_;
  tls_runtime_scheduler_identity = previous_scheduler_identity_;
  tls_runtime_sync_owner_id = previous_sync_owner_id_;
  tls_runtime_task_cancel_flag = previous_cancel_flag_;
  tls_task_cancel_owner = std::move(previous_cancel_owner_);
  tls_runtime_task_context = std::move(previous_task_context_);
}

std::shared_ptr<RuntimeTaskContext> current_runtime_task_context() {
  return tls_runtime_task_context;
}

const void *current_runtime_scheduler_identity() {
  return tls_runtime_scheduler_identity;
}

RuntimeTaskSyncScope::RuntimeTaskSyncScope() { ++tls_runtime_task_sync_depth; }

RuntimeTaskSyncScope::~RuntimeTaskSyncScope() {
  if (tls_runtime_task_sync_depth > 0) {
    --tls_runtime_task_sync_depth;
  }
}

RuntimeIoWaitObserverScope::RuntimeIoWaitObserverScope(
    const RuntimeIoWaitObserver *observer)
    : previous_observer_(tls_runtime_io_wait_observer) {
  tls_runtime_io_wait_observer = observer;
}

RuntimeIoWaitObserverScope::~RuntimeIoWaitObserverScope() {
  tls_runtime_io_wait_observer = previous_observer_;
}

RuntimeIoWaitScope::RuntimeIoWaitScope(
    std::string operation, std::string resource, RuntimeIoWaitInterest interest,
    std::uint64_t resource_id, std::optional<std::chrono::milliseconds> timeout)
    : observer_(tls_runtime_io_wait_observer), active_(observer_ != nullptr) {
  ++tls_runtime_io_wait_depth;
  if (!active_) {
    return;
  }
  record_.wait_id =
      g_runtime_io_wait_ids.fetch_add(1, std::memory_order_relaxed);
  record_.task_id = tls_runtime_task_id;
  record_.strand_id = tls_runtime_strand_id;
  record_.worker_id = tls_runtime_worker_id;
  record_.resource_id = resource_id;
  record_.interest = interest;
  record_.operation = std::move(operation);
  record_.resource = std::move(resource);
  if (timeout.has_value()) {
    record_.has_timeout = true;
    record_.timeout_millis = timeout->count();
  }
  (*observer_)(record_, true);
}

RuntimeIoWaitScope::~RuntimeIoWaitScope() {
  if (active_ && observer_ != nullptr) {
    (*observer_)(record_, false);
  }
  if (tls_runtime_io_wait_depth > 0) {
    --tls_runtime_io_wait_depth;
  }
}

std::uint64_t current_runtime_owner_strand_id() {
  return tls_runtime_strand_id != 0 ? tls_runtime_strand_id
                                    : tls_runtime_worker_id;
}

namespace {
// Low-bit namespace tags for current_runtime_resource_owner_id(). Any non-zero,
// pairwise-distinct tags work: they exist only to keep ids minted by the three
// independent counters (strand / worker / native thread) out of one another's
// numeric range. Every minted id is therefore non-zero, leaving 0 free as a
// "no owner" sentinel for the checks.
constexpr unsigned kResourceOwnerTagBits = 2;
constexpr std::uint64_t kResourceOwnerTagNative = 1;
constexpr std::uint64_t kResourceOwnerTagWorker = 2;
constexpr std::uint64_t kResourceOwnerTagStrand = 3;
} // namespace

std::uint64_t current_runtime_resource_owner_id() {
  if (tls_runtime_strand_id != 0) {
    return (tls_runtime_strand_id << kResourceOwnerTagBits) |
           kResourceOwnerTagStrand;
  }
  if (tls_runtime_worker_id != 0) {
    return (tls_runtime_worker_id << kResourceOwnerTagBits) |
           kResourceOwnerTagWorker;
  }
  return (current_runtime_native_thread_id() << kResourceOwnerTagBits) |
         kResourceOwnerTagNative;
}

std::uint32_t current_runtime_io_wait_depth() {
  return tls_runtime_io_wait_depth;
}

std::uint64_t current_runtime_worker_id() { return tls_runtime_worker_id; }

std::uint64_t current_runtime_strand_id() { return tls_runtime_strand_id; }

std::uint64_t current_runtime_task_id() { return tls_runtime_task_id; }

std::uint64_t current_runtime_native_thread_id() {
  if (tls_runtime_native_thread_id == 0) {
    tls_runtime_native_thread_id =
        g_runtime_native_thread_ids.fetch_add(1, std::memory_order_relaxed);
  }
  return tls_runtime_native_thread_id;
}

std::string current_runtime_task_annotation() {
  return tls_runtime_task_annotation;
}

bool current_runtime_task_cancel_requested() {
  if (tls_run_cancellation_mask && runtime_run_cancel_requested()) return false;
  return runtime_run_cancel_checkpoint_requested() ||
         (tls_runtime_task_cancel_flag != nullptr &&
          tls_runtime_task_cancel_flag->load());
}

bool current_runtime_task_sync_active() {
  return tls_runtime_task_sync_depth != 0;
}

RuntimeTaskFailure::RuntimeTaskFailure(std::string error_name,
                                       std::string message)
    : error_name_(std::move(error_name)), message_(std::move(message)),
      what_(error_name_ + ": " + message_) {}

const char *RuntimeTaskFailure::what() const noexcept { return what_.c_str(); }

const std::string &RuntimeTaskFailure::error_name() const {
  return error_name_;
}

const std::string &RuntimeTaskFailure::message() const { return message_; }

RuntimeTaskCancelled::RuntimeTaskCancelled() = default;

const char *RuntimeTaskCancelled::what() const noexcept {
  return "CancelledError: task cancelled";
}

void throw_if_runtime_task_cancelled() {
  if (current_runtime_task_cancel_requested()) {
    throw RuntimeTaskCancelled();
  }
}

RuntimeTaskAnnotationScope::RuntimeTaskAnnotationScope(std::string annotation)
    : previous_annotation_(std::move(tls_runtime_task_annotation)) {
  tls_runtime_task_annotation = std::move(annotation);
}

RuntimeTaskAnnotationScope::~RuntimeTaskAnnotationScope() {
  tls_runtime_task_annotation = std::move(previous_annotation_);
}

RuntimeWorkerScope::RuntimeWorkerScope(std::uint64_t worker_id)
    : previous_worker_id_(tls_runtime_worker_id) {
  tls_runtime_worker_id = worker_id;
}

RuntimeWorkerScope::~RuntimeWorkerScope() {
  tls_runtime_worker_id = previous_worker_id_;
}

RuntimeStrandScope::RuntimeStrandScope(std::uint64_t strand_id)
    : previous_strand_id_(tls_runtime_strand_id) {
  tls_runtime_strand_id = strand_id;
}

RuntimeStrandScope::~RuntimeStrandScope() {
  tls_runtime_strand_id = previous_strand_id_;
}

} // namespace sputnik::runtime
