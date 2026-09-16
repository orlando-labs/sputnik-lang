// Generated native code uses the same partitioning, ordering and isolation
// checks as the VM, but invokes C++ closures directly. No RuntimeWorld is used.
#include "runtime/heap.h"
static bool native_threaded_shareable(
    const NativeValue &value, std::unordered_set<const void *> &visited) {
  using Tag = NativeValue::Tag;
  if (!native_value_tag_refcounted(value.tag)) return true;
  if (value.heap_value == nullptr) return false;
  if (!visited.insert(value.heap_value).second) return true;
  switch (value.tag) {
  case Tag::List:
  case Tag::Tuple:
  case Tag::Set: {
    if (value.tag == Tag::List && !as_list(value).frozen) return false;
    if (value.tag == Tag::Set && !as_set(value).frozen) return false;
    for (const auto &item : native_sequence_items_copy(value))
      if (!native_threaded_shareable(item, visited)) return false;
    return true;
  }
  case Tag::Map:
    if (!as_map(value).frozen) return false;
    for (const auto &entry : as_map(value).entries)
      if (!native_threaded_shareable(entry.first, visited) ||
          !native_threaded_shareable(entry.second, visited)) return false;
    return true;
  case Tag::Closure: {
    const auto *closure = as_closure(value);
    for (const auto *capture : closure->captures)
      if (!native_threaded_shareable(native_cell_read(capture), visited)) return false;
    return native_threaded_shareable(closure->self, visited);
  }
  case Tag::Atomic:
  case Tag::Mutex:
  case Tag::Task:
  case Tag::TaskLocal:
    return true;
  case Tag::RuntimeHandle: {
    const auto &runtime = as_native_runtime_handle(value)->value;
    return !runtime.is_io_value() ||
           (runtime.as_io_value() && runtime.as_io_value()->shareable());
  }
  default:
    return native_channel_payload_shareable(value);
  }
}

struct NativeThreadedPayload final : amber::runtime::RuntimeIoValue {
  NativeValue value;
  bool transferable;
  explicit NativeThreadedPayload(NativeValue item) : value(std::move(item)) {
    std::unordered_set<const void *> visited;
    transferable = native_threaded_shareable(value, visited);
  }
  ~NativeThreadedPayload() override {
    // Runtime worker queues may release their last reference outside a native
    // callback. Participate in the native GC protocol while releasing values.
    NativeCycleScope scope;
    value = NativeValue::nullv();
  }
  const char *type_name() const override { return "native.ThreadedPayload"; }
  bool shareable() const override { return transferable; }
};

static amber::runtime::Value native_threaded_pack(NativeValue value) {
  // filter_map tests null/false in the shared runtime before unpacking.
  if (value.tag == NativeValue::Tag::Null) return amber::runtime::Value::null();
  if (value.tag == NativeValue::Tag::Bool)
    return amber::runtime::Value::boolean(value.scalar_value != 0);
  return amber::runtime::Value::io_value(
      std::make_shared<NativeThreadedPayload>(std::move(value)));
}

static NativeValue native_threaded_unpack(const amber::runtime::Value &value) {
  if (value.is_null()) return NativeValue::nullv();
  if (value.is_bool()) return NativeValue::boolean(value.as_bool());
  if (value.is_io_value()) {
    if (auto item = std::dynamic_pointer_cast<NativeThreadedPayload>(value.as_io_value()))
      return item->value;
  }
  // The runtime constructs frozen rows for combination and permutation.
  if (value.is_list()) {
    std::vector<NativeValue> items;
    for (const auto &item : value.as_list()->items)
      items.push_back(native_threaded_unpack(item));
    auto result = NativeValue::list(std::move(items));
    as_mutable_list(result).frozen = value.as_list()->frozen;
    return result;
  }
  throw NativeRaised{native_named_error("TypeError", "invalid native threaded result")};
}

static NativeValue native_threaded_create(
    const NativeValue &receiver, const NativeArgsView &args,
    const NativeKeywordArgsView &kwargs, NativeValue block) {
  using Policy = amber::runtime::RuntimeFlowPartitionPolicy;
  if (receiver.tag != NativeValue::Tag::List &&
      receiver.tag != NativeValue::Tag::Tuple && receiver.tag != NativeValue::Tag::Set)
    throw NativeRaised{native_named_error("TypeError", "threaded requires Array, Tuple, or Set")};
  if (block.tag != NativeValue::Tag::Null)
    throw NativeRaised{native_named_error("TypeError", "threaded does not accept block")};
  if (args.size() > 1U || (!args.empty() &&
      (args[0].tag != NativeValue::Tag::Integer || args[0].scalar_value < 0)))
    throw NativeRaised{native_named_error("TypeError", "worker count must be a non-negative Integer")};
  Policy policy = Policy::Atomic;
  if (!kwargs.empty()) {
    if (kwargs.size() != 1U || kwargs[0].name != "scatter")
      throw NativeRaised{native_named_error("TypeError", "unknown threaded keyword")};
    const auto &value = kwargs[0].value;
    std::string name;
    if (value.tag == NativeValue::Tag::Symbol) name = native_symbol_text(value.scalar_value);
    else if (native_value_is_string(value)) name = native_string_text(value);
    if (name == "atomic" || name == "shared" || name == "dynamic") policy = Policy::Atomic;
    else if (name == "chunks" || name == "chunk" || name == "fixed" || name == "fixed_chunks") policy = Policy::Chunks;
    else if (name == "stride" || name == "strided") policy = Policy::Stride;
    else if (name == "items" || name == "item" || name == "per_item") policy = Policy::Items;
    else throw NativeRaised{native_named_error("TypeError", "invalid threaded scatter policy")};
  }
  std::vector<amber::runtime::Value> items;
  for (const auto &item : native_sequence_items_copy(receiver))
    items.push_back(native_threaded_pack(item));
  const auto workers = args.empty() ? 0U : static_cast<std::size_t>(args[0].scalar_value);
  native_require_suspendable("threaded");
  native_commit_effect();
  return NativeValue::runtime_handle(amber::runtime::Value::threaded_collection(
      std::make_shared<amber::runtime::RuntimeThreadedCollection>(
          std::move(items), workers, amber::runtime::RuntimeFlowOptions{}, policy)));
}

// A worker gets its own invocation frame and cannot return through the caller's
// stack. Captured cells remain shared, as in the VM; native cell access is
// synchronized in modules using threaded collections.
static NativeValue native_threaded_detach(const NativeValue &block) {
  if (block.tag != NativeValue::Tag::Closure)
    throw NativeRaised{native_named_error("TypeError", "threaded operation requires block")};
  const auto *source = as_closure(block);
  auto *closure = make_native_closure();
  auto result = NativeValue::closure(closure);
  closure->code_id = source->code_id;
  closure->self = source->self;
  for (auto *capture : source->captures)
    native_closure_capture(closure, capture);
  return result;
}

struct NativeThreadedFailures {
  std::mutex mutex;
  std::map<std::size_t, NativeRaised> exceptions;
  ~NativeThreadedFailures() {
    NativeCycleScope scope;
    exceptions.clear();
  }
};

static NativeValue native_threaded_send(
    const NativeValue &receiver, const std::string &selector,
    const NativeArgsView &args, const NativeKeywordArgsView &kwargs,
    NativeValue block) {
  auto threaded = as_native_runtime_handle(receiver)->value.as_threaded_collection();
  const auto operation = native_collection_canonical_selector(selector);
  const bool generated = operation == "combination" || operation == "permutation";
  if (!kwargs.empty() || args.size() != (generated ? 1U : 0U))
    throw NativeRaised{native_named_error("TypeError", "invalid threaded operation arguments")};
  if (generated && (block.tag != NativeValue::Tag::Null ||
      args[0].tag != NativeValue::Tag::Integer || args[0].scalar_value < 0))
    throw NativeRaised{native_named_error("TypeError", "generated count must be non-negative, without block")};
  if (!generated && operation != "map" && operation != "select" &&
      operation != "reject" && operation != "filter_map" &&
      operation != "flat_map" && operation != "each")
    throw NativeRaised{native_named_error("TypeError", "unsupported threaded operation: " + selector)};
  auto callback = generated ? std::shared_ptr<NativeThreadedPayload>{}
      : std::make_shared<NativeThreadedPayload>(native_threaded_detach(block));
  auto failures = std::make_shared<NativeThreadedFailures>();
  const auto stdout_writer = amber::runtime::current_runtime_stdout();
  const auto stderr_writer = amber::runtime::current_runtime_stderr();
  // Produce a runtime value while still in the native mutator scope. Worker
  // completion/queue destruction can then release that value safely.
  auto invoke = [callback, failures, stdout_writer, stderr_writer, operation](
      const amber::runtime::Value &item, std::size_t index) {
    NativeCycleScope scope;
    amber::runtime::RuntimeOutputScope output_scope(stdout_writer, stderr_writer);
    try {
      auto closure = native_threaded_detach(callback->value);
      auto result = amber_native_call_closure(closure, {
          native_threaded_unpack(item), NativeValue::integer(static_cast<std::int64_t>(index))});
      if (operation == "each") return amber::runtime::Value::null();
      if (operation == "select" || operation == "reject")
        return amber::runtime::Value::boolean(native_truthy(result));
      if (operation == "flat_map") {
        if (result.tag != NativeValue::Tag::List && result.tag != NativeValue::Tag::Tuple &&
            result.tag != NativeValue::Tag::Set)
          throw NativeRaised{native_named_error("TypeError", "threaded flat_map block must return sequence")};
        std::vector<amber::runtime::Value> values;
        for (const auto &value : native_sequence_items_copy(result))
          values.push_back(native_threaded_pack(value));
        return amber::runtime::make_list_value(std::move(values), true);
      }
      return native_threaded_pack(std::move(result));
    } catch (const NativeRaised &raised) {
      {
        std::lock_guard<std::mutex> lock(failures->mutex);
        failures->exceptions.emplace(index, raised);
      }
      throw amber::runtime::RuntimeTaskFailure("RuntimeError", "native threaded block raised");
    } catch (const NativeBailout &error) {
      throw amber::runtime::RuntimeTaskFailure("NativeCodeError", error.what());
    }
  };
  native_require_suspendable("threaded operation");
  native_commit_effect();
  amber::runtime::RuntimeFlowGatherResult result;
  {
    NativeCycleSuspension suspension;
    if (operation == "map" || operation == "filter_map")
      result = operation == "map" ? threaded->map(invoke) : threaded->filter_map(invoke);
    else if (operation == "each")
      result = threaded->each([invoke](const auto &value, auto index) { (void)invoke(value, index); });
    else if (operation == "select" || operation == "reject") {
      auto predicate = [invoke](const auto &value, auto index) { return invoke(value, index).as_bool(); };
      result = operation == "select" ? threaded->select(predicate) : threaded->reject(predicate);
    } else if (operation == "flat_map")
      result = threaded->flat_map([invoke](const auto &value, auto index) {
        return invoke(value, index).as_list()->items;
      });
    else if (operation == "combination")
      result = threaded->combination(static_cast<std::size_t>(args[0].scalar_value));
    else
      result = threaded->permutation(static_cast<std::size_t>(args[0].scalar_value));
  }
  if (!result.ok || result.failed) {
    if (!result.failures.empty()) {
      std::lock_guard<std::mutex> lock(failures->mutex);
      const auto found = failures->exceptions.find(result.failures.front().index);
      if (found != failures->exceptions.end()) throw found->second;
    }
    throw NativeRaised{native_named_error(
        result.error_name.empty() ? "FlowError" : result.error_name,
        result.message.empty() ? "threaded operation failed" : result.message)};
  }
  std::vector<NativeValue> values;
  for (const auto &value : result.values) values.push_back(native_threaded_unpack(value));
  return NativeValue::list(std::move(values));
}
