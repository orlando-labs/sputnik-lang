#include "runtime/sputnik_ext.h"
#include "runtime/concurrency.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace {
std::atomic<int64_t> pressure_waits{0}, pressure_callbacks{0};
std::atomic<int64_t> cancel_started{0}, cancel_finished{0};
std::mutex pressure_mutex;
std::condition_variable pressure_changed;
bool pressure_released = false;
}

extern "C" {
uint32_t sputnik_ext_abi_version() { return SPUTNIK_EXT_ABI_VERSION; }

SputnikStatus blocking_worker(SputnikCtx *cx, const SputnikValue *, size_t,
                             SputnikValue *out) {
  *out = sputnik_make_bool(cx, sputnik::runtime::current_runtime_is_blocking_ffi_thread());
  return SPUTNIK_OK;
}

SputnikStatus blocking_batch(SputnikCtx *cx, const SputnikValue *args, size_t count,
                            SputnikValue *out) {
  if (count != 1) return sputnik_fault(cx, "TypeError", "expected callback");
  ++pressure_callbacks;
  return sputnik_call_block(cx, args[0], nullptr, 0, out);
}

SputnikStatus blocking_pressure_reset(SputnikCtx *cx, const SputnikValue *args, size_t count,
                                     SputnikValue *out) {
  int64_t mode;
  if (count != 1 || !sputnik_as_int(cx, args[0], &mode))
    return sputnik_fault(cx, "TypeError", "expected pressure mode");
  std::lock_guard<std::mutex> lock(pressure_mutex);
  pressure_waits = 0;
  pressure_callbacks = 0;
  pressure_released = false;
  const auto hardware = std::thread::hardware_concurrency();
  const auto width = mode == 2
      ? std::max(4U, std::min(32U, std::max(1U, hardware) * 2U))
      : (hardware == 0 ? 2U : hardware);
  *out = sputnik_make_int(cx, width);
  return SPUTNIK_OK;
}

SputnikStatus blocking_pressure_arrivals(SputnikCtx *cx, const SputnikValue *args,
                                        size_t count, SputnikValue *out) {
  int64_t mode;
  if (count != 1 || !sputnik_as_int(cx, args[0], &mode))
    return sputnik_fault(cx, "TypeError", "expected pressure mode");
  *out = sputnik_make_int(cx, mode == 0 ? pressure_waits.load() : pressure_callbacks.load());
  return SPUTNIK_OK;
}

SputnikStatus blocking_pressure_wait(SputnikCtx *cx, const SputnikValue *, size_t,
                                    SputnikValue *out) {
  std::unique_lock<std::mutex> lock(pressure_mutex);
  ++pressure_waits;
  if (!pressure_changed.wait_for(lock, std::chrono::seconds(2),
                                [] { return pressure_released; }))
    return sputnik_fault(cx, "ValueError", "scheduler worker remained blocked during FFI wait");
  *out = sputnik_make_int(cx, 42);
  return SPUTNIK_OK;
}

SputnikStatus blocking_pressure_release(SputnikCtx *cx, const SputnikValue *, size_t,
                                       SputnikValue *out) {
  {
    std::lock_guard<std::mutex> lock(pressure_mutex);
    pressure_released = true;
  }
  pressure_changed.notify_all();
  *out = sputnik_make_bool(cx, 1);
  return SPUTNIK_OK;
}

SputnikStatus blocking_context(SputnikCtx *cx, const SputnikValue *, size_t,
                               SputnikValue *out) {
  const SputnikValue values[] = {
      sputnik_make_int(cx, sputnik::runtime::current_runtime_task_id()),
      sputnik_make_int(cx, sputnik::runtime::current_runtime_strand_id())};
  *out = sputnik_make_list(cx, values, 2);
  return SPUTNIK_OK;
}

SputnikStatus blocking_cancel_wait(SputnikCtx *cx, const SputnikValue *, size_t,
                                   SputnikValue *) {
  struct Cleanup { ~Cleanup() { ++cancel_finished; } } cleanup;
  ++cancel_started;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (std::chrono::steady_clock::now() < deadline) {
    if (sputnik_task_cancelled(cx))
      return sputnik_fault(cx, "CancelledError", "foreign wait cancelled");
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return sputnik_fault(cx, "ValueError", "foreign wait did not observe cancellation");
}

SputnikStatus blocking_cancel_counts(SputnikCtx *cx, const SputnikValue *, size_t,
                                     SputnikValue *out) {
  const SputnikValue values[] = {sputnik_make_int(cx, cancel_started.load()),
                                 sputnik_make_int(cx, cancel_finished.load())};
  *out = sputnik_make_list(cx, values, 2);
  return SPUTNIK_OK;
}

SputnikStatus blocking_fail(SputnikCtx *cx, const SputnikValue *, size_t,
                            SputnikValue *) {
  if (!sputnik::runtime::current_runtime_is_blocking_ffi_thread())
    return sputnik_fault(cx, "TypeError", "fault ran outside FFI worker");
  return sputnik_fault(cx, "ValueError", "worker fault");
}

SputnikStatus blocking_probe_new(SputnikCtx *cx, const SputnikValue *, size_t,
                                SputnikValue *out) {
  static bool constructed_on_worker;
  constructed_on_worker = sputnik::runtime::current_runtime_is_blocking_ffi_thread();
  *out = sputnik_make_handle(cx, "blocking.Probe", &constructed_on_worker);
  return SPUTNIK_OK;
}

SputnikStatus blocking_probe_worker(SputnikCtx *cx, SputnikValue,
                                   const SputnikValue *, size_t, SputnikValue *out) {
  return blocking_worker(cx, nullptr, 0, out);
}

SputnikStatus blocking_probe_constructed(SputnikCtx *cx, SputnikValue self,
                                          const SputnikValue *, size_t, SputnikValue *out) {
  void *ptr = nullptr;
  if (!sputnik_handle_ptr(cx, self, "blocking.Probe", &ptr)) return SPUTNIK_ERR;
  *out = sputnik_make_bool(cx, *static_cast<bool *>(ptr));
  return SPUTNIK_OK;
}
SputnikStatus blocking_probe_inline_worker(SputnikCtx *cx, SputnikValue,
                                          const SputnikValue *, size_t, SputnikValue *out) {
  return blocking_worker(cx, nullptr, 0, out);
}
}
