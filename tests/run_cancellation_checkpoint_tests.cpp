#include "runtime/context.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

using namespace sputnik::runtime;
namespace {
void require(bool ok, const char *message) {
  if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
void expect_view(const RuntimeRunCancellationCheckpoint &view, bool requested,
                 const char *message) {
  require(view.requested() == requested, message);
  require(runtime_run_cancel_checkpoint_requested() == requested, "cached and ordinary checkpoints diverged");
}
}
int main() {
  // Retain one dispatch view through run changes/reentry and nested cleanup.
  const auto &view = current_runtime_run_cancel_checkpoint();
  require(view.cancel_flag == nullptr, "idle checkpoint must have no borrowed owner");
  expect_view(view, false, "idle checkpoint cancelled");
  auto outer = std::make_shared<RuntimeRunState>();
  auto inner = std::make_shared<RuntimeRunState>();
  {
    RuntimeRunCancellationScope scope(outer);
    require(view.cancel_flag == outer->cancel_flag(), "scope did not update existing dispatch view");
    expect_view(view, false, "fresh run cancelled");
    outer->request_cancel();
    expect_view(view, true, "cached view missed cancellation");
    {
      RuntimeRunCancellationMask cleanup(true);
      expect_view(view, false, "cleanup checkpoint was not masked");
      require(runtime_run_cancel_requested(), "cleanup cleared permanent stop");
      require(runtime_wait_cancel_flag() == nullptr, "cleanup wait must hide stopped run");
      {
        RuntimeRunCancellationScope reentry(inner);
        require(view.cancel_flag == inner->cancel_flag(), "reentry retained previous owner");
        expect_view(view, false, "new nested run inherited cleanup stop");
        inner->request_cancel();
        expect_view(view, true, "nested run stop not observed");
        RuntimeRunCancellationMask inner_cleanup(true);
        expect_view(view, false, "nested cleanup not masked");
      }
      require(view.cancel_flag == outer->cancel_flag() && view.mask_depth == 1,
              "reentry did not restore owner and cleanup depth");
      expect_view(view, false, "reentry lost cleanup mask");
      { RuntimeRunCancellationMask nested_mask(true); require(view.mask_depth == 2, "nested cleanup depth"); }
      { RuntimeRunCancellationScope idle(nullptr); require(view.cancel_flag == nullptr, "idle reentry retained flag"); }
      require(view.mask_depth == 1, "idle reentry did not restore cleanup mask");
    }
    expect_view(view, true, "cleanup permanently hid stop");
  }
  require(view.cancel_flag == nullptr && view.mask_depth == 0, "run scope leaked borrowed owner");
  expect_view(view, false, "completed run poisoned idle dispatch");

  // The same logical owner may resume on another worker. Each dispatch binds
  // that worker's view; cancellation stays an atomic check on every poll.
  for (int dispatch = 0; dispatch < 2; ++dispatch) {
    auto owner = std::make_shared<RuntimeRunState>();
    std::atomic<bool> ready{false}, observed{false};
    std::thread worker([&] {
      RuntimeRunCancellationScope scope(owner);
      const auto &worker_view = current_runtime_run_cancel_checkpoint();
      require(worker_view.cancel_flag == owner->cancel_flag(), "worker binding wrong");
      ready.store(true, std::memory_order_release);
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
      while (!worker_view.requested() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
      observed.store(worker_view.requested(), std::memory_order_release);
    });
    while (!ready.load(std::memory_order_acquire)) std::this_thread::yield();
    owner->request_cancel(); worker.join();
    require(observed.load(), "migrated dispatch missed atomic cancellation");
    expect_view(view, false, "worker cancellation contaminated parent TLS");
  }
  std::cout << "run cancellation checkpoint tests: ok\n";
}
