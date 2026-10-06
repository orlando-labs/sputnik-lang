#pragma once

#include "runtime/concurrency.h"
#include "runtime/text.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace sputnik::runtime {

extern thread_local std::uint64_t tls_runtime_worker_id;
extern thread_local std::uint64_t tls_runtime_strand_id;
extern thread_local std::uint64_t tls_runtime_task_id;
extern thread_local const void *tls_runtime_scheduler_identity;
extern thread_local std::uint64_t tls_runtime_sync_owner_id;
extern thread_local const std::atomic<bool> *tls_runtime_task_cancel_flag;
std::shared_ptr<std::atomic<bool>> current_runtime_task_cancel_owner();
extern thread_local std::shared_ptr<RuntimeTaskContext>
    tls_runtime_task_context;
extern thread_local std::uint32_t tls_runtime_task_sync_depth;
extern thread_local std::shared_ptr<RuntimeTextWriter> tls_runtime_stdout;
extern thread_local std::shared_ptr<RuntimeTextWriter> tls_runtime_stderr;
extern thread_local std::string tls_runtime_task_annotation;
extern thread_local std::uint64_t tls_runtime_native_thread_id;
extern thread_local RuntimeTextSourceLocation tls_runtime_text_source_location;
extern thread_local const RuntimeIoWaitObserver *tls_runtime_io_wait_observer;
extern thread_local std::uint32_t tls_runtime_io_wait_depth;

// Host execution cancellation is separate from a scheduler task's identity.
// Shared ownership lets migrated tasks and blocking FFI retain the token safely.
struct RuntimeRunTaskFailure {
  // Run-local registration ID, unique even across different schedulers.
  std::uint64_t task_id = 0;
  std::string error_name, message;
  RuntimeTextSourceLocation spawn_source;
};
struct RuntimeRunFailureReceipt {
  explicit RuntimeRunFailureReceipt(RuntimeRunTaskFailure value)
      : failure(std::move(value)) {}
  const RuntimeRunTaskFailure failure;
  std::atomic<bool> observed{false};
};
class RuntimeRunState : public std::enable_shared_from_this<RuntimeRunState> {
public:
  class Task {
  public:
    ~Task();
    Task(const Task &) = delete;
    Task &operator=(const Task &) = delete;
    // Bind after releasing scheduler locks: an already-cancelled run invokes
    // the callback immediately. The callback must not retain the scheduler.
    void on_cancel(std::function<void()> callback);
    std::uint64_t id() const noexcept { return id_; }
  private:
    friend class RuntimeRunState;
    explicit Task(std::shared_ptr<RuntimeRunState> run) : run_(std::move(run)) {}
    std::shared_ptr<RuntimeRunState> run_;
    std::uint64_t id_ = 0;
  };

  bool cancelled() const noexcept { return cancelled_.load(std::memory_order_relaxed); }
  const std::atomic<bool> *cancel_flag() const noexcept { return &cancelled_; }
  void request_cancel();
  std::shared_ptr<Task> register_task();
  std::size_t active_tasks() const;
  // Root return begins draining. Descendants may still spawn while registered
  // work is alive; once drained the run is sealed against late callbacks.
  void close_root();
  bool wait_for_idle(std::chrono::milliseconds timeout);
  // Called at scheduler terminal publication, before executable captures retire.
  // Receipts contain no Values or scheduler/world ownership. Propagated child
  // failures reuse their receipt so a structured failure is reported only once.
  std::shared_ptr<RuntimeRunFailureReceipt> record_failure(RuntimeRunTaskFailure failure);
  std::vector<RuntimeRunTaskFailure> unobserved_failures() const;
private:
  std::atomic<bool> cancelled_{false};
  mutable std::mutex mutex_;
  std::condition_variable idle_;
  bool root_closed_ = false;
  std::uint64_t next_task_ = 1;
  std::map<std::uint64_t, std::function<void()>> tasks_;
  std::map<std::uint64_t, std::shared_ptr<RuntimeRunFailureReceipt>> failures_;
};
using RuntimeRunCancellation = std::shared_ptr<RuntimeRunState>;
RuntimeRunCancellation current_runtime_run_cancellation();
bool runtime_run_cancel_requested();
class RuntimeRunCancellationScope {
public:
  explicit RuntimeRunCancellationScope(RuntimeRunCancellation token);
  ~RuntimeRunCancellationScope();
  RuntimeRunCancellationScope(const RuntimeRunCancellationScope &) = delete;
  RuntimeRunCancellationScope &operator=(const RuntimeRunCancellationScope &) = delete;
private:
  RuntimeRunCancellation previous_;
  unsigned previous_mask_;
};

// A VM unwinding cancellation must be allowed to execute ensure bodies. This
// masks cooperative checkpoints only, never the host's publication/Stop state.
class RuntimeRunCancellationMask {
public:
  explicit RuntimeRunCancellationMask(bool enabled);
  ~RuntimeRunCancellationMask();
  RuntimeRunCancellationMask(const RuntimeRunCancellationMask &) = delete;
  RuntimeRunCancellationMask &operator=(const RuntimeRunCancellationMask &) = delete;
private:
  bool enabled_;
};
bool runtime_run_cancel_checkpoint_requested();
// Borrowed only while the current task/run scope stays alive. Cleanup masks
// hide cancellation from new waits without clearing the permanent Stop flag.
const std::atomic<bool> *runtime_wait_cancel_flag();

// Layer B cooperative IO yield. When the VM drives a parkable task body it sets
// tls_runtime_io_park_enabled; io.cpp's wait_fd then, instead of blocking on
// the reactor, records the fd it would have waited on into
// tls_runtime_io_park_request and returns a RuntimeIoStatus with park=true. The
// VM turns that into a strand park plus a reactor wait_async registration that
// resumes the strand.
struct RuntimeIoParkRequest {
  int fd = -1;
  bool want_write = false;
  std::optional<std::chrono::steady_clock::time_point> deadline;
};
extern thread_local bool tls_runtime_io_park_enabled;
extern thread_local bool tls_runtime_io_park_requested;
extern thread_local RuntimeIoParkRequest tls_runtime_io_park_request;

extern std::atomic<std::uint64_t> g_runtime_output_order;
extern std::atomic<std::uint64_t> g_runtime_native_thread_ids;

// Resolves the source location attributed to text output events. A fixed
// value scope ("this exact location") and a provider scope ("ask the running
// VM at output time") share the same binding: whichever scope was entered
// most recently wins, so log replay can pin a captured location while a VM
// is live on the same thread.
using RuntimeTextSourceLocationProviderFn =
    RuntimeTextSourceLocation (*)(const void *ctx);

extern thread_local RuntimeTextSourceLocationProviderFn
    tls_runtime_text_source_location_provider;
extern thread_local const void *tls_runtime_text_source_location_provider_ctx;

RuntimeTextSourceLocation resolve_runtime_text_source_location();

class RuntimeTextSourceLocationScope {
public:
  explicit RuntimeTextSourceLocationScope(RuntimeTextSourceLocation location);
  RuntimeTextSourceLocationScope(const RuntimeTextSourceLocationScope &) =
      delete;
  RuntimeTextSourceLocationScope &
  operator=(const RuntimeTextSourceLocationScope &) = delete;
  ~RuntimeTextSourceLocationScope();

private:
  RuntimeTextSourceLocation previous_;
  RuntimeTextSourceLocationProviderFn previous_provider_ = nullptr;
  const void *previous_provider_ctx_ = nullptr;
};

class RuntimeTextSourceLocationProviderScope {
public:
  RuntimeTextSourceLocationProviderScope(
      RuntimeTextSourceLocationProviderFn provider, const void *ctx);
  RuntimeTextSourceLocationProviderScope(
      const RuntimeTextSourceLocationProviderScope &) = delete;
  RuntimeTextSourceLocationProviderScope &
  operator=(const RuntimeTextSourceLocationProviderScope &) = delete;
  ~RuntimeTextSourceLocationProviderScope();

private:
  RuntimeTextSourceLocationProviderFn previous_provider_ = nullptr;
  const void *previous_provider_ctx_ = nullptr;
};

class RuntimeTaskScope {
public:
  RuntimeTaskScope(std::uint64_t task_id, const std::atomic<bool> *cancel_flag,
                   std::uint64_t sync_owner_id = 0,
                   std::shared_ptr<RuntimeTaskContext> task_context = nullptr,
                   const void *scheduler_identity = nullptr,
                   std::shared_ptr<std::atomic<bool>> cancel_owner = nullptr);
  RuntimeTaskScope(const RuntimeTaskScope &) = delete;
  RuntimeTaskScope &operator=(const RuntimeTaskScope &) = delete;
  ~RuntimeTaskScope();

private:
  std::uint64_t previous_task_id_ = 0;
  const void *previous_scheduler_identity_ = nullptr;
  std::uint64_t previous_sync_owner_id_ = 0;
  const std::atomic<bool> *previous_cancel_flag_ = nullptr;
  std::shared_ptr<std::atomic<bool>> previous_cancel_owner_;
  std::shared_ptr<RuntimeTaskContext> previous_task_context_;
};

class RuntimeTaskSyncScope {
public:
  RuntimeTaskSyncScope();
  RuntimeTaskSyncScope(const RuntimeTaskSyncScope &) = delete;
  RuntimeTaskSyncScope &operator=(const RuntimeTaskSyncScope &) = delete;
  ~RuntimeTaskSyncScope();
};

class RuntimeIoWaitObserverScope {
public:
  explicit RuntimeIoWaitObserverScope(const RuntimeIoWaitObserver *observer);
  RuntimeIoWaitObserverScope(const RuntimeIoWaitObserverScope &) = delete;
  RuntimeIoWaitObserverScope &
  operator=(const RuntimeIoWaitObserverScope &) = delete;
  ~RuntimeIoWaitObserverScope();

private:
  const RuntimeIoWaitObserver *previous_observer_ = nullptr;
};

class RuntimeIoWaitScope {
public:
  RuntimeIoWaitScope(
      std::string operation, std::string resource,
      RuntimeIoWaitInterest interest, std::uint64_t resource_id = 0,
      std::optional<std::chrono::milliseconds> timeout = std::nullopt);
  RuntimeIoWaitScope(const RuntimeIoWaitScope &) = delete;
  RuntimeIoWaitScope &operator=(const RuntimeIoWaitScope &) = delete;
  ~RuntimeIoWaitScope();

private:
  RuntimeIoWaitRecord record_;
  const RuntimeIoWaitObserver *observer_ = nullptr;
  bool active_ = false;
};

std::uint64_t current_runtime_owner_strand_id();

// Strand-confinement owner identity for IO resources and byte buffers.
//
// Unlike current_runtime_owner_strand_id() -- which returns a bare strand id, a
// bare worker id, or 0, three values drawn from independent counters that all
// start at 1 -- this returns a single id whose low bits tag the namespace it
// came from (strand / worker / native thread). Tagging makes the id collision-
// free across namespaces, so a resource confined to strand N is never confused
// with one confined to worker N (or native thread N) just because the raw
// counters happen to coincide. Resource constructors stamp this value and the
// access checks compare against it, so a comparison can never put, say, a
// worker id on one side and a native-thread id on the other.
std::uint64_t current_runtime_resource_owner_id();

std::uint32_t current_runtime_io_wait_depth();

// Application arguments visible to ArgParser() when no explicit `cmdline:` is
// supplied. Launchers set this once before Sputnik code starts; callers receive a
// copy so parsing cannot mutate process-global state.
void set_runtime_process_arguments(std::vector<std::string> arguments);
std::vector<std::string> current_runtime_process_arguments();

} // namespace sputnik::runtime
