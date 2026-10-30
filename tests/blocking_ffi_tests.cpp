#include "bytecode/emitter.h"
#include "frontend/binder/binder.h"
#include "frontend/hir/hir.h"
#include "frontend/lexer/lexer.h"
#include "frontend/parser/parser.h"
#include "runtime/sputnik_ext_runtime.h"
#include "runtime/context.h"
#include "runtime/vm.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {
constexpr const char *gate_tag = "test.blocking.WaitGate";

// Compile the exact stack driver emitted into native programs, with a
// one-worker runtime rather than the hardware-sized generated singleton.
sputnik::runtime::RuntimeTaskModule *native_test_runtime = nullptr;
sputnik::runtime::RuntimeTaskModule &native_task_runtime() {
  return *native_test_runtime;
}
#include "runtime/system_native_task.inc"

void expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "blocking ffi test failed: " << message << '\n';
    std::exit(1);
  }
}

struct Gate {
  int fds[2]{-1, -1};
  std::atomic<int> waits{0}, releases{0}, callbacks{0}, completed{0};
  ~Gate() { for (int fd : fds) if (fd >= 0) ::close(fd); }
};

Gate *gate(SputnikCtx *cx, SputnikValue self) {
  void *pointer = nullptr;
  return sputnik_handle_ptr(cx, self, gate_tag, &pointer)
             ? static_cast<Gate *>(pointer) : nullptr;
}

SputnikStatus wait_socket(SputnikCtx *cx, SputnikValue self,
                         const SputnikValue *, size_t, SputnikValue *out) {
  auto *state = gate(cx, self);
  if (!state) return SPUTNIK_ERR;
  if (!sputnik::runtime::current_runtime_is_blocking_ffi_thread())
    return sputnik_fault(cx, "ValueError", "wait did not run on FFI worker");
  ++state->waits;
  pollfd fd{state->fds[0], POLLIN, 0};
  int ready;
  do { ready = ::poll(&fd, 1, 2000); } while (ready < 0 && errno == EINTR);
  unsigned char byte = 0;
  if (ready != 1 || ::read(state->fds[0], &byte, 1) != 1)
    return sputnik_fault(cx, "ValueError", "scheduler did not run socket producer");
  *out = sputnik_make_int(cx, byte);
  return SPUTNIK_OK;
}

SputnikStatus release_socket(SputnikCtx *cx, SputnikValue self,
                            const SputnikValue *, size_t, SputnikValue *out) {
  auto *state = gate(cx, self);
  if (!state) return SPUTNIK_ERR;
  ++state->releases;
  const unsigned char byte = 42;
  if (::write(state->fds[1], &byte, 1) != 1)
    return sputnik_fault(cx, "ValueError", "socket producer failed");
  *out = sputnik_make_bool(cx, 1);
  return SPUTNIK_OK;
}

SputnikStatus batch(SputnikCtx *cx, SputnikValue self, const SputnikValue *args,
                    size_t count, SputnikValue *out) {
  auto *state = gate(cx, self);
  if (!state) return SPUTNIK_ERR;
  if (count != 1 || !sputnik::runtime::current_runtime_is_blocking_ffi_thread())
    return sputnik_fault(cx, "ValueError", "invalid callback boundary");
  ++state->callbacks;
  const auto status = sputnik_call_block(cx, args[0], nullptr, 0, out);
  ++state->completed; // Native continuation after the callback, exactly once.
  return status;
}

SputnikStatus counts(SputnikCtx *cx, SputnikValue self, const SputnikValue *,
                     size_t, SputnikValue *out) {
  auto *state = gate(cx, self);
  if (!state) return SPUTNIK_ERR;
  const SputnikValue values[] = {
      sputnik_make_int(cx, state->waits.load()),
      sputnik_make_int(cx, state->releases.load()),
      sputnik_make_int(cx, state->callbacks.load()),
      sputnik_make_int(cx, state->completed.load())};
  *out = sputnik_make_list(cx, values, 4);
  return SPUTNIK_OK;
}

SputnikStatus on_worker(SputnikCtx *cx, SputnikValue, const SputnikValue *,
                        size_t, SputnikValue *out) {
  *out = sputnik_make_bool(cx, sputnik::runtime::current_runtime_is_blocking_ffi_thread());
  return SPUTNIK_OK;
}

SputnikStatus task_identity(SputnikCtx *cx, SputnikValue, const SputnikValue *,
                            size_t, SputnikValue *out) {
  *out = sputnik_make_int(cx, sputnik::runtime::current_runtime_task_id());
  return SPUTNIK_OK;
}

sputnik::bytecode::BcModule compile(const std::string &source) {
  sputnik::lexer::Lexer lexer(source, "<blocking-ffi-test>");
  auto lex = lexer.lex();
  expect(lex.ok(), "lex");
  sputnik::parser::Parser parser(lex.tokens);
  auto parsed = parser.parse_module_unit();
  if (!parsed.ok()) std::cerr << sputnik::lexer::diagnostics_to_json(parsed.diagnostics);
  expect(parsed.ok(), "parse");
  auto bound = sputnik::binder::bind_module(parsed.items, parsed.module_name);
  expect(bound.ok(), "bind");
  auto hir = sputnik::hir::lower_module(parsed.items, parsed.module_name, bound.graph);
  auto emitted = sputnik::bytecode::emit_program(hir, parsed.module_name);
  if (!emitted.ok()) std::cerr << sputnik::lexer::diagnostics_to_json(emitted.diagnostics);
  expect(emitted.ok(), "emit");
  auto decoded = sputnik::bytecode::deserialize_module(
      sputnik::bytecode::serialize_module(emitted.module));
  expect(decoded.ok(), "decode");
  return std::move(decoded.module);
}

void test_native_stack_driver() {
  using namespace sputnik::runtime;
  RuntimeTaskModule runtime(1);
  native_test_runtime = &runtime;
  std::mutex mutex;
  std::condition_variable changed;
  bool entered = false, released = false;
  std::atomic<int> continuations{0};
  RuntimeTaskLocalKey key;
  auto reader = runtime.spawn_resumable(native_system_task_driver([&] {
    const auto task = current_runtime_task_id();
    const auto context = current_runtime_task_context();
    context->set(key, std::make_shared<RuntimeTaskLocalStoredValue<Value>>(Value::integer(7)));
    // Two ordinary C++ stack frames, representing native -> Sputnik callback.
    const auto callback = [&] {
      runtime.sleep(std::chrono::milliseconds(20));
      std::unique_lock<std::mutex> lock(mutex);
      entered = true;
      changed.notify_all();
      expect(changed.wait_for(lock, std::chrono::seconds(2), [&] { return released; }),
             "native callback must not occupy the only scheduler worker");
      expect(current_runtime_task_id() == task && current_runtime_task_context() == context &&
             context->binding(key.slot_id()).has_value(), "native callback retains logical context");
    };
    callback();
    ++continuations;
    return Value::null();
  }));
  {
    std::unique_lock<std::mutex> lock(mutex);
    expect(changed.wait_for(lock, std::chrono::seconds(2), [&] { return entered; }),
           "native stack starts");
  }
  auto producer = runtime.spawn([&] {
    std::lock_guard<std::mutex> lock(mutex);
    released = true;
    changed.notify_all();
    return Value::null();
  });
  expect(reader.wait(std::chrono::seconds(2)).ok && producer.wait(std::chrono::seconds(2)).ok &&
         continuations == 1, "native callback continuation executes once");

  // Completion can precede the scheduler's park publication. It must neither
  // lose its wake nor restart a completed C++ activation.
  std::vector<RuntimeTaskHandle> immediate;
  for (int i = 0; i < 64; ++i)
    immediate.push_back(runtime.spawn_resumable(native_system_task_driver([&] {
      ++continuations;
      return Value::null();
    })));
  for (auto &task : immediate)
    expect(task.wait(std::chrono::seconds(2)).ok, "immediate native completion resumes");
  expect(continuations == 65, "immediate activations execute exactly once");

  std::atomic<bool> cancellable_started{false};
  std::atomic<int> cleanup{0};
  auto cancelled = runtime.spawn_resumable(native_system_task_driver([&] {
    struct Cleanup { std::atomic<int> &count; ~Cleanup() { ++count; } } guard{cleanup};
    cancellable_started = true;
    runtime.sleep(std::chrono::seconds(2));
    return Value::null();
  }));
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!cancellable_started && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
  expect(cancellable_started && cancelled.cancel(), "cancel an active native stack");
  const auto result = cancelled.wait(std::chrono::seconds(2));
  expect(result.ready && result.cancelled && cleanup == 1,
         "cancelled native stack unwinds before publishing completion");
  expect(runtime.scheduler().stats().worker_count == 1 &&
         runtime.scheduler().stats().parks >= 66, "native stacks park the one-worker runtime");
  native_test_runtime = nullptr;
}

void test_ffi_saturated_producer() {
  using namespace sputnik::runtime;
  const auto hardware = std::max(1U, std::thread::hardware_concurrency());
  const auto width = std::max(4U, std::min(32U, hardware * 2U));
  std::mutex mutex;
  std::condition_variable changed;
  unsigned entered = 0, completed = 0;
  bool released = false;
  for (unsigned i = 0; i < width; ++i) {
    RuntimeBlockingFfiExecutor::instance().submit([&] {
      std::unique_lock<std::mutex> lock(mutex);
      ++entered;
      changed.notify_all();
      expect(changed.wait_for(lock, std::chrono::seconds(2), [&] { return released; }),
             "occupied FFI carriers must allow a producer to enter FFI");
      ++completed;
      changed.notify_all();
    });
  }
  {
    std::unique_lock<std::mutex> lock(mutex);
    expect(changed.wait_for(lock, std::chrono::seconds(2), [&] { return entered == width; }),
           "fill the FFI idle pool");
  }
  RuntimeBlockingFfiExecutor::instance().submit([&] {
    std::lock_guard<std::mutex> lock(mutex);
    released = true;
    changed.notify_all();
  });
  std::unique_lock<std::mutex> lock(mutex);
  expect(changed.wait_for(lock, std::chrono::seconds(2), [&] { return completed == width; }),
         "FFI producer breaks saturated callback waits");
}
} // namespace

int main() {
  using namespace sputnik::runtime;
  test_native_stack_driver();
  test_ffi_saturated_producer();
  auto &registry = NativeExtRegistry::global();
  registry.register_thunk("test.blocking.wait", reinterpret_cast<void *>(&wait_socket));
  registry.register_thunk("test.blocking.release", reinterpret_cast<void *>(&release_socket));
  registry.register_thunk("test.blocking.batch", reinterpret_cast<void *>(&batch));
  registry.register_thunk("test.blocking.counts", reinterpret_cast<void *>(&counts));
  registry.register_thunk("test.blocking.worker", reinterpret_cast<void *>(&on_worker));
  registry.register_thunk("test.blocking.identity", reinterpret_cast<void *>(&task_identity));
  const auto module = compile(R"SPUTNIK(
package test.ffi_wait.single_worker

native class WaitGate from "test.blocking.WaitGate" borrowed:
  def wait() from "test.blocking.wait" blocking
  def release() from "test.blocking.release"
  def batch(callback) from "test.blocking.batch" blocking
  def counts() from "test.blocking.counts"
  def worker?() from "test.blocking.worker"
  def identity() from "test.blocking.identity"

def probe(workers, gate):
  reader = workers.async: gate.wait
  until gate.counts[0] == 1 or reader.done?: workers.sleep(1)
  producer = workers.async: gate.release
  raise ValueError("socket producer") unless reader.wait == 42 and producer.wait
  channel = Channel.new(capacity: 0)
  callback = workers.async:
    identity = gate.identity
    nested = | |:
      workers.sleep(20)
      channel.recv(timeout: 1.0)
    outer = | |:
      raise ValueError("callback identity") unless gate.worker? and gate.identity == identity
      gate.batch((&nested))
    gate.batch((&outer))
  until gate.counts[2] == 2 or callback.done?: workers.sleep(1)
  producer = workers.async: channel.send(43)
  raise ValueError("callback producer") unless callback.wait == 43 and producer.wait
  gate.counts == [1, 1, 2, 2]
)SPUTNIK");
  std::uint32_t code = 0;
  for (const auto &method : module.methods)
    if (module.symbols[method.selector_sym_id] == "probe") code = method.entry_code_id;
  expect(code != 0, "probe method");
  Gate state;
  expect(::socketpair(AF_UNIX, SOCK_STREAM, 0, state.fds) == 0, "socketpair");
  auto resource = std::make_shared<RuntimeForeignHandle>();
  resource->tag = gate_tag;
  resource->ptr = &state;
  auto workers = std::make_shared<RuntimeTaskModule>(1);
  const auto started = std::chrono::steady_clock::now();
  const auto result = execute_code(module, code,
      {Value::task_module(workers), Value::foreign_handle(resource)});
  if (result.fault)
    std::cerr << result.fault->error_name << ": " << result.fault->message << '\n';
  expect(result.ok() && result.value.is_bool() && result.value.as_bool(),
         "native wait and nested callback wait must release the only scheduler worker");
  expect(std::chrono::steady_clock::now() - started >= std::chrono::milliseconds(20),
         "callback sleep must actually wait");
  const auto stats = workers->scheduler().stats();
  expect(stats.worker_count == 1 && stats.parks >= 2 && stats.park_resumes >= 2,
         "one-worker execution must park and resume both foreign boundaries");
  std::cout << "blocking_ffi_tests: ok\n";
}
