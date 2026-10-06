#pragma once

#include "bytecode/format.h"
#include "runtime/heap.h"
#include "runtime/value.h"
#include "runtime/watch.h"
#include "runtime/watch_internal.h"
#include "runtime/world.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace sputnik::runtime {

// Private interpreter state for runtime/vm.cpp. This header exists to make the
// VM implementation editable during the split; it is not a public runtime API.

class NativeRegistry;
class RuntimeDispatchRegistry;
class RuntimeErrorRegistry;
class RuntimeModuleRegistry;
class RuntimeTypeRegistry;

struct PreparedSeqState {
  std::vector<Value> items;
  std::size_t rest_start = 0;
  bool source_was_tuple = false;
  // Upper bound (exclusive) of the rest slice, lowered by each from-end
  // `PGetIndex` so a mid-position rest (`[a, *b, c]`) captures only the middle.
  // SIZE_MAX means "to the end" (tail rest, the common case).
  std::size_t rest_end = SIZE_MAX;
};

struct PreparedMapState {
  std::vector<MapEntry> entries;
  std::unordered_map<std::uint32_t, std::size_t> index_by_key;
  std::vector<std::uint32_t> requested_keys;
  bool needs_full = false;
  bool rest_bound = false;
  std::uint32_t fail_pc = 0;
};

struct CoercedSeqState {
  Value value = Value::null();
  std::vector<Value> items;
  bool source_was_tuple = false;
};

struct CoercedMapState {
  Value value = Value::null();
  std::vector<MapEntry> entries;
};

enum class LazySeqOpKind : std::int64_t {
  Map = 1,
  FlatMap = 2,
  Select = 3,
  Reject = 4,
  FilterMap = 5,
};

struct LazySeqOp {
  LazySeqOpKind kind = LazySeqOpKind::Map;
  Value block = Value::null();
};

struct LazySeqState {
  Value source = Value::null();
  std::vector<LazySeqOp> ops;
};

enum class LazySeqVisitStatus { Continue, Stop, Faulted };

struct CallCacheEntry {
  bool valid = false;
  std::uint32_t receiver_class_index = 0;
  std::uint32_t dispatch_flags = 0;
  std::uint32_t selector_symbol_id = 0;
  std::uint32_t positional_count = 0;
  std::vector<std::uint32_t> keyword_shape;
  bool has_block = false;
  std::uint64_t method_version = 0;
  std::uint64_t world_epoch = 0;
  // Populated only for compiler-generated `attr` readers after their bytecode
  // shape has been validated by the VM. The call cache remains authoritative
  // for method/world invalidation.
  std::optional<std::uint32_t> attr_reader_ivar_symbol_id;
  bytecode::BcMethod method;
  // A collection callback commonly sends to several receiver classes at one
  // bytecode site. Retain a small bounded set instead of replacing the only
  // entry on every layer. Alternatives are flat (their own vectors are empty).
  std::vector<CallCacheEntry> alternatives;
  // Admission hint only, never a dispatch guard. Two consecutive misses for
  // the same shape allow a full cache to adapt to a changed hot receiver.
  mutable std::optional<std::uint64_t> pending_overflow_shape;
};

struct IvarCacheEntry {
  bool valid = false;
  std::uint32_t receiver_class_index = 0;
  std::uint32_t symbol_id = 0;
  // Shape descriptors are immutable and owned by the live instance plus the
  // runtime's root/transition graph. Pointer identity is therefore the exact
  // cache guard and avoids retaining/releasing a shared_ptr on every probe.
  const ShapeDescriptor *shape = nullptr;
  std::uint32_t slot_index = 0;
};

enum class QuickOpcode : std::uint8_t {
  Fallback,
  LoadK,
  LookupConst,
  LoadNull,
  LoadBool,
  Move,
  LoadSelf,
  LoadBlock,
  GetLast,
  SetLast,
  LoadUpval,
  StoreUpval,
  IAdd,
  ISub,
  ILt,
  IGt,
  IMul,
  IDiv,
  IMod,
  IFloorDiv,
  ILe,
  IGe,
  IEq,
  INe,
  ICmp,
  IBitAnd,
  IBitOr,
  IBitXor,
  IShl,
  IShr,
  IAddK,
  ISubK,
  ILtK,
  IGtK,
  IMulK,
  IDivK,
  IModK,
  IFloorDivK,
  ILeK,
  IGeK,
  IEqK,
  INeK,
  ICmpK,
  IBitAndK,
  IBitOrK,
  IBitXorK,
  IShlK,
  IShrK,
  Jump,
  JumpIfTrue,
  JumpIfFalse,
  JumpIfNull,
  Return,
  Raise,
  CloseUpvalues,
  Safepoint,
  ILtJumpIfFalse,
  IGtJumpIfFalse,
  ILtKJumpIfFalse,
  IGtKJumpIfFalse,
  SendIAdd,
  SendISub,
  SendIMul,
  SendIDiv,
  SendIMod,
  SendIFloorDiv,
  SendILt,
  SendIGt,
  SendILe,
  SendIGe,
  SendIEq,
  SendINe,
  SendICmp,
  SendIBitAnd,
  SendIBitOr,
  SendIBitXor,
  SendIShl,
  SendIShr,
  SendIntToStr,
  SendTypeMatches,
  SendSeqIndex,
  SendSeqIndexSet,
  SendSeqContains,
  SendContains,
  SendSeqCount,
  SendLength,
  SendSeqFirst,
  SendSeqEach,
  SendSeqMap,
  SendSeqAll,
  SendSeqAny,
  SendSeqNone,
  SendResultOk,
  SendResultErr,
  SendResultOrRaise,
  SendCached0,
  SendCached1,
  LoadIvar,
  StoreIvar,
};

struct QuickInsn {
  QuickOpcode quick_opcode = QuickOpcode::Fallback;
  bytecode::Opcode opcode = bytecode::Opcode::Return;
  std::uint32_t a = 0;
  std::uint32_t b = 0;
  std::uint32_t c = 0;
  std::int64_t imm = 0;
};

struct QuickCode {
  std::vector<QuickInsn> instructions;
};

enum class ConstantLookupKind : std::uint8_t {
  Unresolved,
  Class,
  ErrorClass,
  NativeType,
  NativeFunction,
  TaskModule,
  FlowModule,
  ErrorNamespace,
  ModuleBinding,
  Missing,
  Ambiguous,
};

struct ConstantLookupCache {
  std::string path;
  std::string binding_key;
  ConstantLookupKind kind = ConstantLookupKind::Unresolved;
  std::uint32_t target = 0;
  // Cache the storage address, never a closure/watch snapshot. unordered_map
  // rehash preserves this address; clearing the map invalidates the generation.
  // The RuntimeState owns both this Value and its GC roots.
  const Value *binding = nullptr;
};

struct PendingThrow {
  Value tag = Value::null();
  Value value = Value::null();
  bool value_present = true;
};

using NonlocalReturnTarget = ClosureValue::NonlocalReturnTarget;

struct PendingNonlocalReturn {
  std::shared_ptr<NonlocalReturnTarget> target;
  Value value = Value::null();
};

// Register-keyed per-frame map for pattern state. These hold at most a
// handful of entries at a time, so a flat vector beats unordered_map: finds
// are short linear scans and clear() keeps capacity, so pooled frames stop
// paying hash-node malloc/free on every pattern prologue.
template <typename T> class FlatRegMap {
public:
  using Entry = std::pair<std::uint32_t, T>;
  using iterator = typename std::vector<Entry>::iterator;
  using const_iterator = typename std::vector<Entry>::const_iterator;

  iterator begin() { return entries_.begin(); }
  iterator end() { return entries_.end(); }
  const_iterator begin() const { return entries_.begin(); }
  const_iterator end() const { return entries_.end(); }
  bool empty() const { return entries_.empty(); }
  std::size_t size() const { return entries_.size(); }
  void clear() { entries_.clear(); }

  iterator find(std::uint32_t key) {
    return std::find_if(
        entries_.begin(), entries_.end(),
        [key](const Entry &entry) { return entry.first == key; });
  }

  const_iterator find(std::uint32_t key) const {
    return std::find_if(
        entries_.begin(), entries_.end(),
        [key](const Entry &entry) { return entry.first == key; });
  }

  T &operator[](std::uint32_t key) {
    iterator found = find(key);
    if (found != entries_.end()) {
      return found->second;
    }
    entries_.emplace_back(key, T{});
    return entries_.back().second;
  }

  std::size_t erase(std::uint32_t key) {
    iterator found = find(key);
    if (found == entries_.end()) {
      return 0;
    }
    if (found + 1 != entries_.end()) {
      *found = std::move(entries_.back());
    }
    entries_.pop_back();
    return 1;
  }

private:
  std::vector<Entry> entries_;
};

struct PreservedRegister {
  std::uint32_t reg = 0;
  Value value = Value::null();
};

// Result hand-off for a native extension thunk executing on the blocking FFI
// executor. The owning VM is quiescent while `ready` is false; the mutex is the
// publication boundary between the executor and the resumed scheduler strand.
struct PendingNativeExtensionCall {
  std::mutex mutex;
  bool ready = false;
  bool ok = false;
  std::string logical;
  std::size_t call_pc = 0;
  Value value = Value::null();
  std::string exception_error_name;
  std::string exception_message;
};

struct Frame {
  const bytecode::BcCode *code = nullptr;
  const QuickCode *quick_code = nullptr;
  std::size_t pc = 0;
  std::vector<Value> regs;
  std::vector<std::uint8_t> initialized;
  std::vector<std::int64_t> int64_regs;
  std::vector<std::uint8_t> int_valid;
  // Kernel.watch is rare, but every ordinary register read/write used to
  // inspect the Value tail kind in case its slot held a watch cell. The flag
  // is conservative: false proves that no register is a cell; true may remain
  // set until the frame is cleared. Creation and bulk frame-copy paths raise
  // it explicitly.
  bool has_watch_registers = false;
  std::vector<Value> captures;
  Value self = Value::null();
  Value block = Value::null();
  Value last_result = Value::null();
  // Property arms are non-suspendable: while a frame marked here (or any
  // frame above it) is live, scheduler suspension points must fault.
  bool no_suspend_extent = false;
  std::string no_suspend_label;
  std::optional<std::uint32_t> caller_result_reg;
  // Synchronous runtime intrinsics (collection iteration, validation helpers)
  // sometimes need a block result before the current opcode can finish. The
  // root block frame writes through this short-lived sink instead of forcing
  // the caller to reserve a bytecode register. It is never retained past the
  // surrounding C++ call.
  std::optional<Value> *direct_return_sink = nullptr;
  // Collection intrinsics invoke the same closure once per item. When this
  // sink is present, a normal return hands the cleaned activation back to the
  // intrinsic instead of cycling it through the per-code pool. Nested calls,
  // exceptional unwinds, and non-local returns continue to use the ordinary
  // frame lifecycle.
  std::unique_ptr<Frame> *direct_return_frame_sink = nullptr;
  std::optional<std::uint32_t> active_call_pc;
  std::optional<Value> return_override;
  // Constructor continuation: run the inherited after_init! hook only after
  // this init activation returns normally, before publishing its instance.
  bool after_init_pending = false;
  bool merge_registers_to_caller = false;
  std::optional<Value> pending_exception_on_return;
  std::optional<PendingThrow> pending_throw_on_return;
  std::optional<PendingNonlocalReturn> pending_nonlocal_return_on_return;
  std::shared_ptr<NonlocalReturnTarget> nonlocal_return_target;
  bool owns_nonlocal_return_target = false;
  std::vector<PreservedRegister> preserved_registers_on_return;
  // Dynamic runtime scope cleanup (currently TaskLocal.with). The cleanup is
  // kept on the resumable frame so it survives cooperative park/migration.
  std::function<void()> scope_exit;
  std::shared_ptr<PendingNativeExtensionCall> pending_native_extension_call;
  FlatRegMap<PreparedSeqState> prepared_seq_regs;
  FlatRegMap<PreparedMapState> prepared_map_regs;
  FlatRegMap<Value> pending_pattern_bindings;
};

// A collection loop owns one of these for the duration of repeated block
// calls. The callback keeps this helper independent of Vm's private layout
// while still returning its retained activation to the ordinary frame pool on
// every exit path.
struct PreparedBlockCall {
  using RecycleFn = void (*)(void *, std::unique_ptr<Frame>);

  void *recycle_context = nullptr;
  RecycleFn recycle = nullptr;
  IntrusivePtr<ClosureValue> closure;
  const bytecode::BcCode *code = nullptr;
  std::size_t arg_count = 0;
  std::unique_ptr<Frame> frame;

  PreparedBlockCall() = default;
  PreparedBlockCall(const PreparedBlockCall &) = delete;
  PreparedBlockCall &operator=(const PreparedBlockCall &) = delete;
  ~PreparedBlockCall() {
    if (frame != nullptr && recycle != nullptr) {
      recycle(recycle_context, std::move(frame));
    }
  }
};

// Active frames live at stable addresses. A Frame owns several register
// vectors and cold side tables; moving that whole aggregate on every
// call/return showed up prominently in interpreted request profiles. The stack
// moves only owning pointers while preserving the existing back/index API.
class FrameStack {
public:
  bool empty() const { return frames_.empty(); }
  std::size_t size() const { return frames_.size(); }

  Frame &back() { return *frames_.back(); }
  const Frame &back() const { return *frames_.back(); }
  Frame &operator[](std::size_t index) { return *frames_[index]; }
  const Frame &operator[](std::size_t index) const { return *frames_[index]; }

  void push_back(std::unique_ptr<Frame> frame) {
    frames_.push_back(std::move(frame));
  }

  // Cold helper paths still prepare a Frame by value. Keep them source
  // compatible; ordinary call dispatch uses the unique_ptr overload.
  void push_back(Frame &&frame) {
    frames_.push_back(std::make_unique<Frame>(std::move(frame)));
  }

  std::unique_ptr<Frame> take_back() {
    std::unique_ptr<Frame> frame = std::move(frames_.back());
    frames_.pop_back();
    return frame;
  }

  void pop_back() { frames_.pop_back(); }
  void clear() { frames_.clear(); }

private:
  std::vector<std::unique_ptr<Frame>> frames_;
};

struct MethodTableDescriptor {
  std::unordered_map<std::uint32_t, bytecode::BcMethod> entries;
};

struct ClassRuntimeState {
  std::unordered_map<std::string, Value> cvars;
  MethodTableDescriptor instance_method_table;
  MethodTableDescriptor class_method_table;
  std::vector<std::uint32_t> direct_include_indices;
  std::vector<std::uint32_t> direct_extend_indices;
  std::uint32_t owner_flags = 0;
  std::uint32_t ivar_schema_id = 0;
  bool has_superclass_ref = false;
  std::uint32_t superclass_ref = 0;
  bool method_range_valid = true;
  std::uint64_t method_version = 1;
};

struct RuntimeState {
  static constexpr std::size_t kClassStateShardCount = 64;
  static constexpr std::size_t kShapeTransitionShardCount = 64;

  struct ShapeTransitionShard {
    mutable std::shared_mutex mutex;
    std::unordered_map<std::string, std::shared_ptr<ShapeDescriptor>>
        transitions;
  };

  RuntimeHeap heap;
  std::vector<ClassRuntimeState> classes;
  std::shared_ptr<std::array<std::shared_mutex, kClassStateShardCount>>
      class_state_mutexes = std::make_shared<
          std::array<std::shared_mutex, kClassStateShardCount>>();
  bool owners_initialized = false;
  // Compile-time step budget (macro expander sandbox, DESIGN-macro-system §10):
  // when enabled, every interpreter step decrements the counter and exhausting
  // it faults instead of hanging the build. Lives on RuntimeState so nested Vm
  // instances (block/callable sub-loops) share one budget; disabled it costs a
  // single predictable branch per step.
  bool step_budget_enabled = false;
  std::int64_t step_budget_remaining = 0;
  // Rest parameters: maps a method body's entry code id to the register index
  // of its `*name` rest parameter, so the closure-call frame setup can pack
  // surplus positional arguments into a Tuple. Built once in
  // initialize_for_module; `has_any_rest_params` keeps the common (no-rest)
  // call path to a single bool check.
  bool has_any_rest_params = false;
  std::unordered_map<std::uint32_t, std::uint32_t> rest_param_index_by_code;
  std::unordered_map<std::uint32_t, std::uint32_t> kw_rest_param_index_by_code;
  // Method bodies whose parameter list needs the param-aware shaping path on
  // closure/direct-entry calls: any rest/keyword-rest pack, and any keyword,
  // block, or defaulted parameter (the raw positional register copy does not
  // bind these channels). Built once in initialize_for_module;
  // `has_any_shaped_params` keeps the common case to a single bool check.
  bool has_any_shaped_params = false;
  std::unordered_set<std::uint32_t> codes_needing_param_shaping;
  // Code ids are stable for one immutable module image, but RuntimeState can
  // survive a package hot reload and serve both old and replacement images
  // during the swap. Cache a separate id-indexed view per owned image. Holding
  // the shared module as the key also keeps every pointer in the view valid.
  using CodeIndex = std::vector<const bytecode::BcCode *>;
  struct CodeIndexCache {
    std::mutex mutex;
    std::map<std::shared_ptr<const bytecode::BcModule>,
             std::shared_ptr<const CodeIndex>,
             std::owner_less<std::shared_ptr<const bytecode::BcModule>>>
        indices;
  };
  // RuntimeState is copied while preparing a hot-reload replacement. Ordinary
  // copies share the complete cache (not only its mutex), which makes that
  // copy a race-free pointer copy even while another task constructs a VM for
  // the current image. A replacement candidate detaches this cache before it
  // prepares its new image index so preparation cannot mutate the published
  // generation.
  std::shared_ptr<CodeIndexCache> code_index_cache =
      std::make_shared<CodeIndexCache>();
  bool world_frozen = false;
  std::uint64_t world_epoch = 1;
  // Notebook/watch bookkeeping is shared by task-local Vm instances. It is
  // outside the ordinary dispatch path, but its ids, event log, and dependency
  // capture must still be race-free when reactive work runs on several
  // strands.
  std::shared_ptr<std::mutex> reactive_state_mutex =
      std::make_shared<std::mutex>();
  // A host-owned snapshot provider (for example persistent notebook slots).
  // The callback is copied into shared state and guarded for concurrent VM
  // safepoints and explicit RuntimeWorld GC calls. Keeping it here means
  // nested, pooled, and scheduler VMs all observe the same provider without
  // retaining notebook/runtime-specific types.
  std::shared_ptr<std::mutex> external_gc_root_provider_mutex =
      std::make_shared<std::mutex>();
  RuntimeGcRootProvider external_gc_root_provider;
  std::shared_ptr<RuntimeWatchStream> watch_stream =
      std::make_shared<RuntimeWatchStream>();
  bool dependency_capture_active = false;
  RuntimeDependencySet dependency_capture;
  std::unordered_map<std::string, std::size_t> dependency_capture_index;
  // Bytecode cache sites are immutable and dense within each code object.
  // Address shared inline caches directly by [code_id][site_id] instead of
  // hashing that pair on every SEND/ivar access. Entries stay indirect so an
  // untouched site costs one pointer rather than a full BcMethod-sized slot;
  // shared_ptr also keeps RuntimeState's hot-reload copy operation valid.
  std::vector<std::vector<std::shared_ptr<CallCacheEntry>>> call_caches;
  std::uint64_t call_cache_entry_count = 0;
  std::uint64_t call_cache_hits = 0;
  std::uint64_t call_cache_misses = 0;
  std::uint64_t call_cache_updates = 0;
  std::vector<std::vector<std::shared_ptr<IvarCacheEntry>>> ivar_caches;
  // Path constants and the bytecode class table are immutable for the
  // lifetime of a RuntimeState. Resolve class references once when the module
  // is installed instead of rebuilding path strings and linearly scanning all
  // classes on every superclass/mixin dispatch.
  static constexpr std::uint32_t kClassRefUnknown =
      std::numeric_limits<std::uint32_t>::max();
  static constexpr std::uint32_t kClassRefAmbiguous =
      std::numeric_limits<std::uint32_t>::max() - 1U;
  std::vector<std::uint32_t> resolved_class_refs;
  bool module_init_completed = false;
  std::unordered_map<std::string, Value> module_bindings;
  std::uint64_t module_bindings_revision = 0;
  std::shared_ptr<std::atomic<std::uint64_t>> next_shape_id =
      std::make_shared<std::atomic<std::uint64_t>>(1);
  std::vector<std::shared_ptr<ShapeDescriptor>> root_shapes;
  std::shared_ptr<
      std::array<ShapeTransitionShard, kShapeTransitionShardCount>>
      shape_transition_shards = std::make_shared<
          std::array<ShapeTransitionShard, kShapeTransitionShardCount>>();
  std::shared_ptr<std::mutex> shape_structure_mutex =
      std::make_shared<std::mutex>();
  std::shared_ptr<ShapeDescriptor> dead_shape;

  std::shared_mutex &class_state_mutex(std::uint32_t class_index) const {
    return (*class_state_mutexes)[static_cast<std::size_t>(class_index) %
                                  kClassStateShardCount];
  }

  Value load_cvar(std::uint32_t class_index, const std::string &name) const {
    if (class_index >= classes.size()) {
      return Value::null();
    }
    std::shared_lock<std::shared_mutex> lock(class_state_mutex(class_index));
    const auto found = classes[class_index].cvars.find(name);
    return found == classes[class_index].cvars.end() ? Value::null()
                                                     : found->second;
  }

  void store_cvar(std::uint32_t class_index, const std::string &name,
                  Value value) {
    if (class_index >= classes.size()) {
      return;
    }
    std::unique_lock<std::shared_mutex> lock(class_state_mutex(class_index));
    classes[class_index].cvars[name] = std::move(value);
  }

  std::vector<Value> cvar_roots_snapshot() const {
    std::vector<Value> roots;
    for (std::uint32_t class_index = 0; class_index < classes.size();
         ++class_index) {
      std::shared_lock<std::shared_mutex> lock(
          class_state_mutex(class_index));
      roots.reserve(roots.size() + classes[class_index].cvars.size());
      for (const auto &[name, value] : classes[class_index].cvars) {
        (void)name;
        roots.push_back(value);
      }
    }
    return roots;
  }

  static std::string shape_transition_key(std::uint64_t parent_id,
                                          const std::string &name) {
    return std::to_string(parent_id) + "\x1f" + name;
  }

  std::shared_ptr<RuntimeWatchCell> make_watch_cell(Value value,
                                                    std::string target_name) {
    const std::weak_ptr<RuntimeWatchStream> weak_stream = watch_stream;
    return std::make_shared<RuntimeWatchCell>(
        std::move(value), watch_stream->allocate_cell_id(),
        std::move(target_name), [weak_stream](RuntimeWatchEvent event) {
          if (const std::shared_ptr<RuntimeWatchStream> stream =
                  weak_stream.lock()) {
            (void)stream->record(std::move(event));
          }
        });
  }

  std::shared_ptr<RuntimeWatchObjectState>
  make_watch_object_state(std::uint64_t object_id) {
    const std::weak_ptr<RuntimeWatchStream> weak_stream = watch_stream;
    return std::make_shared<RuntimeWatchObjectState>(
        object_id, [weak_stream](RuntimeWatchEvent event) {
          if (const std::shared_ptr<RuntimeWatchStream> stream =
                  weak_stream.lock()) {
            (void)stream->record(std::move(event));
          }
        });
  }

  std::shared_ptr<RuntimeWatchHandle>
  make_watch_handle(std::shared_ptr<RuntimeWatchCell> cell,
                    std::string target_name) {
    return std::make_shared<RuntimeWatchHandle>(
        std::move(cell), watch_stream->allocate_handle_id(),
        std::move(target_name));
  }

  std::shared_ptr<RuntimeWatchHandle>
  make_watch_handle(std::shared_ptr<RuntimeWatchObjectState> object_state,
                    std::string target_name, std::string field_name) {
    return std::make_shared<RuntimeWatchHandle>(
        std::move(object_state), watch_stream->allocate_handle_id(),
        std::move(target_name), std::move(field_name));
  }

  RuntimeWatchEvent record_watch_event(RuntimeWatchEvent event) {
    return watch_stream->record(std::move(event));
  }

  std::uint64_t watch_epoch_snapshot() const {
    return watch_stream->latest_epoch();
  }

  RuntimeWatchCursor watch_cursor_snapshot() const {
    return watch_stream->tail_cursor();
  }

  RuntimeWatchPollResult poll_watch_events(const RuntimeWatchCursor &cursor,
                                           std::size_t max_events) const {
    return watch_stream->poll(cursor, max_events);
  }

  RuntimeWatchPollResult
  wait_watch_events(const RuntimeWatchCursor &cursor,
                    std::chrono::milliseconds timeout,
                    std::size_t max_events) const {
    return watch_stream->wait(cursor, timeout, max_events);
  }

  void set_external_gc_root_provider(RuntimeGcRootProvider provider) {
    std::lock_guard<std::mutex> lock(*external_gc_root_provider_mutex);
    external_gc_root_provider = std::move(provider);
  }

  std::vector<Value> external_gc_roots_snapshot() const {
    RuntimeGcRootProvider provider;
    {
      std::lock_guard<std::mutex> lock(*external_gc_root_provider_mutex);
      provider = external_gc_root_provider;
    }
    // Host code must not run while the provider mutex is held.  Apart from
    // reducing lock contention this permits a provider to take its own
    // snapshot locks without creating a runtime/host lock inversion.
    if (!provider) {
      return {};
    }
    return provider();
  }

  std::vector<RuntimeWatchEvent> watch_events_snapshot() const {
    return watch_stream->events_snapshot();
  }

  static std::string dependency_key(const RuntimeDependency &dependency) {
    switch (dependency.kind) {
    case RuntimeDependencyKind::Binding:
      return "binding:" + std::to_string(dependency.cell_id);
    case RuntimeDependencyKind::Ivar:
      return "ivar:" + std::to_string(dependency.object_id) + ":" +
             dependency.field_name;
    case RuntimeDependencyKind::Object:
      return "object:" + std::to_string(dependency.object_id);
    }
    return {};
  }

  void begin_dependency_capture(std::uint64_t notebook_cell_id) {
    std::lock_guard<std::mutex> lock(*reactive_state_mutex);
    dependency_capture_active = true;
    dependency_capture = RuntimeDependencySet{};
    dependency_capture.notebook_cell_id = notebook_cell_id;
    dependency_capture.source = watch_stream->identity();
    dependency_capture_index.clear();
  }

  RuntimeDependencySet end_dependency_capture() {
    std::lock_guard<std::mutex> lock(*reactive_state_mutex);
    RuntimeDependencySet result = dependency_capture;
    dependency_capture_active = false;
    dependency_capture = RuntimeDependencySet{};
    dependency_capture_index.clear();
    return result;
  }

  RuntimeDependencySet dependency_capture_snapshot() const {
    std::lock_guard<std::mutex> lock(*reactive_state_mutex);
    return dependency_capture_active ? dependency_capture
                                     : RuntimeDependencySet{};
  }

  void record_dependency(RuntimeDependency dependency) {
    std::lock_guard<std::mutex> lock(*reactive_state_mutex);
    if (!dependency_capture_active) {
      return;
    }
    const std::string key = dependency_key(dependency);
    if (key.empty()) {
      return;
    }
    const auto found = dependency_capture_index.find(key);
    if (found == dependency_capture_index.end()) {
      dependency_capture_index.emplace(key,
                                       dependency_capture.dependencies.size());
      dependency_capture.dependencies.push_back(std::move(dependency));
      return;
    }
    dependency_capture.dependencies[found->second] = std::move(dependency);
  }

  void resolve_class_refs_for_module(const bytecode::BcModule &module) {
    std::unordered_map<std::string, std::uint32_t> full_names;
    std::unordered_map<std::string, std::uint32_t> leaf_names;
    std::unordered_set<std::string> ambiguous_leaves;
    full_names.reserve(module.classes.size());
    leaf_names.reserve(module.classes.size());
    for (std::uint32_t index = 0; index < module.classes.size(); ++index) {
      const std::uint32_t symbol_id =
          module.classes[index].class_name_sym_id;
      if (symbol_id >= module.symbols.size()) {
        continue;
      }
      const std::string &name = module.symbols[symbol_id];
      full_names.emplace(name, index);
      const std::size_t separator = name.rfind('.');
      const std::string leaf =
          separator == std::string::npos ? name : name.substr(separator + 1U);
      const auto [found, inserted] = leaf_names.emplace(leaf, index);
      if (!inserted && found->second != index) {
        ambiguous_leaves.insert(leaf);
      }
    }

    resolved_class_refs.assign(module.const_pool.size(), kClassRefUnknown);
    for (std::uint32_t ref = 0; ref < module.const_pool.size(); ++ref) {
      const bytecode::Constant &constant = module.const_pool[ref];
      if (constant.kind != bytecode::ConstantKind::Path ||
          constant.items.empty()) {
        continue;
      }
      std::string full_path;
      bool valid = true;
      for (const std::uint32_t symbol_id : constant.items) {
        if (symbol_id >= module.symbols.size()) {
          valid = false;
          break;
        }
        if (!full_path.empty()) {
          full_path += '.';
        }
        full_path += module.symbols[symbol_id];
      }
      if (!valid) {
        continue;
      }
      const auto exact = full_names.find(full_path);
      if (exact != full_names.end()) {
        resolved_class_refs[ref] = exact->second;
        continue;
      }
      const std::string &leaf = module.symbols[constant.items.back()];
      if (ambiguous_leaves.find(leaf) != ambiguous_leaves.end()) {
        resolved_class_refs[ref] = kClassRefAmbiguous;
        continue;
      }
      const auto short_name = leaf_names.find(leaf);
      if (short_name != leaf_names.end()) {
        resolved_class_refs[ref] = short_name->second;
      }
    }
  }

  void initialize_inline_cache_layout(const bytecode::BcModule &module) {
    std::uint32_t max_code_id = 0;
    for (const bytecode::BcCode &code : module.code_objects) {
      max_code_id = std::max(max_code_id, code.code_id);
    }
    call_caches.clear();
    ivar_caches.clear();
    call_caches.resize(static_cast<std::size_t>(max_code_id) + 1U);
    ivar_caches.resize(static_cast<std::size_t>(max_code_id) + 1U);
    for (const bytecode::BcCode &code : module.code_objects) {
      call_caches[code.code_id].resize(code.call_site_table.size());
      ivar_caches[code.code_id].resize(code.ivar_site_table.size());
    }
    call_cache_entry_count = 0;
  }

  void initialize_for_module(const bytecode::BcModule &module) {
    if (dead_shape == nullptr) {
      dead_shape = std::make_shared<ShapeDescriptor>();
      dead_shape->shape_id = 0;
      dead_shape->shape_version = 0;
      dead_shape->dead = true;
    }
    if (classes.size() < module.classes.size()) {
      classes.resize(module.classes.size());
    }
    if (root_shapes.size() < module.classes.size()) {
      root_shapes.resize(module.classes.size());
    }
    if (owners_initialized) {
      return;
    }
    initialize_inline_cache_layout(module);
    resolve_class_refs_for_module(module);
    for (const bytecode::BcMethod &method : module.methods) {
      for (std::uint32_t i = 0; i < method.params.size(); ++i) {
        const std::uint32_t flags = method.params[i].flags;
        if ((flags & bytecode::kMethodParamFlagRest) != 0U) {
          rest_param_index_by_code[method.entry_code_id] = i;
          has_any_rest_params = true;
        } else if ((flags & bytecode::kMethodParamFlagKwRest) != 0U) {
          kw_rest_param_index_by_code[method.entry_code_id] = i;
          has_any_rest_params = true;
        }
        if ((flags & (bytecode::kMethodParamFlagRest |
                      bytecode::kMethodParamFlagKwRest |
                      bytecode::kMethodParamFlagKeyword |
                      bytecode::kMethodParamFlagBlock |
                      bytecode::kMethodParamFlagHasDefault)) != 0U) {
          codes_needing_param_shaping.insert(method.entry_code_id);
          has_any_shaped_params = true;
        }
      }
    }
    for (const bytecode::BcCode &code : module.code_objects) {
      if ((code.flags & bytecode::kCodeFlagRestParam) == 0U) {
        continue;
      }
      rest_param_index_by_code[code.code_id] =
          code.flags >> bytecode::kCodeRestParamIndexShift;
      codes_needing_param_shaping.insert(code.code_id);
      has_any_rest_params = true;
      has_any_shaped_params = true;
    }
    for (std::uint32_t index = 0; index < module.classes.size(); ++index) {
      ClassRuntimeState &runtime = classes[index];
      const bytecode::BcClass &owner = module.classes[index];
      runtime.owner_flags = owner.flags;
      runtime.ivar_schema_id = owner.ivar_schema_id;
      runtime.has_superclass_ref = owner.has_superclass_ref;
      runtime.superclass_ref = owner.superclass_ref;
      runtime.method_range_valid =
          owner.method_range_start + owner.method_range_count <=
          module.methods.size();
      if (!runtime.method_range_valid) {
        continue;
      }
      for (std::uint32_t offset = 0; offset < owner.method_range_count;
           ++offset) {
        bytecode::BcMethod method =
            module.methods[owner.method_range_start + offset];
        MethodTableDescriptor &table =
            (method.flags & bytecode::kMethodFlagClass) != 0U
                ? runtime.class_method_table
                : runtime.instance_method_table;
        table.entries[method.selector_sym_id] = std::move(method);
      }
    }
    owners_initialized = true;
  }

  std::shared_ptr<const CodeIndex> code_index_for_module(
      const std::shared_ptr<const bytecode::BcModule> &module,
      bool *inserted = nullptr) {
    std::lock_guard<std::mutex> lock(code_index_cache->mutex);
    const auto found = code_index_cache->indices.find(module);
    if (found != code_index_cache->indices.end()) {
      if (inserted != nullptr) {
        *inserted = false;
      }
      return found->second;
    }
    std::uint32_t max_code_id = 0;
    for (const bytecode::BcCode &code : module->code_objects) {
      max_code_id = std::max(max_code_id, code.code_id);
    }
    auto index = std::make_shared<CodeIndex>(
        static_cast<std::size_t>(max_code_id) + 1U, nullptr);
    for (const bytecode::BcCode &code : module->code_objects) {
      (*index)[code.code_id] = &code;
    }
    code_index_cache->indices.emplace(module, index);
    if (inserted != nullptr) {
      *inserted = true;
    }
    return index;
  }

  // A hot-reload candidate must own its cache before preparing a code index.
  // The cache is intentionally shared by ordinary RuntimeState copies, so
  // this explicit detach is the boundary that prevents pre-publication cache
  // inserts from becoming visible through the active state.
  void detach_code_index_cache_for_replacement() {
    code_index_cache = std::make_shared<CodeIndexCache>();
  }

  // Roll back a cache entry prepared for a module that was never published.
  // The caller serializes this with RuntimeWorld execution, and the cache
  // entry itself is immutable, so erasing it cannot invalidate an active VM.
  void erase_code_index_for_module(
      const std::shared_ptr<const bytecode::BcModule> &module) noexcept {
    if (module == nullptr) {
      return;
    }
    std::lock_guard<std::mutex> lock(code_index_cache->mutex);
    code_index_cache->indices.erase(module);
  }

  // Notebook image replacement runs while RuntimeWorld's execution mutex is
  // held, so no VM can still be starting from an older notebook image. Drop
  // those strong-key cache entries at commit; otherwise every edit would keep
  // the complete old bytecode/string/metadata image alive indefinitely.
  void retain_only_code_index_for_module(
      const std::shared_ptr<const bytecode::BcModule> &module) noexcept {
    if (module == nullptr) {
      return;
    }
    std::lock_guard<std::mutex> lock(code_index_cache->mutex);
    for (auto it = code_index_cache->indices.begin();
         it != code_index_cache->indices.end();) {
      if (it->first.owner_before(module) || module.owner_before(it->first)) {
        it = code_index_cache->indices.erase(it);
      } else {
        ++it;
      }
    }
  }

  std::shared_ptr<const ShapeDescriptor>
  root_shape_for_class(std::uint32_t class_index) {
    if (root_shapes.size() <= class_index) {
      std::lock_guard<std::mutex> structure_lock(*shape_structure_mutex);
      if (root_shapes.size() <= class_index) {
        root_shapes.resize(static_cast<std::size_t>(class_index) + 1U);
      }
    }
    std::shared_mutex &mutex = class_state_mutex(class_index);
    {
      std::shared_lock<std::shared_mutex> lock(mutex);
      if (root_shapes[class_index] != nullptr) {
        return root_shapes[class_index];
      }
    }
    std::unique_lock<std::shared_mutex> lock(mutex);
    if (root_shapes[class_index] == nullptr) {
      auto shape = std::make_shared<ShapeDescriptor>();
      shape->shape_id =
          next_shape_id->fetch_add(1U, std::memory_order_relaxed);
      shape->shape_version = shape->shape_id;
      root_shapes[class_index] = shape;
    }
    return root_shapes[class_index];
  }

  std::shared_ptr<const ShapeDescriptor>
  transition_shape(std::shared_ptr<const ShapeDescriptor> parent,
                   const std::string &name) {
    if (parent == nullptr || parent->dead) {
      return parent;
    }
    if (parent->ivar_slots.find(name) != parent->ivar_slots.end()) {
      return parent;
    }
    const std::string key = shape_transition_key(parent->shape_id, name);
    ShapeTransitionShard &shard =
        (*shape_transition_shards)[parent->shape_id %
                                   kShapeTransitionShardCount];
    {
      std::shared_lock<std::shared_mutex> lock(shard.mutex);
      const auto found = shard.transitions.find(key);
      if (found != shard.transitions.end()) {
        return found->second;
      }
    }
    std::unique_lock<std::shared_mutex> lock(shard.mutex);
    const auto found = shard.transitions.find(key);
    if (found != shard.transitions.end()) {
      return found->second;
    }
    auto next = std::make_shared<ShapeDescriptor>();
    next->shape_id =
        next_shape_id->fetch_add(1U, std::memory_order_relaxed);
    next->shape_version = next->shape_id;
    next->ivar_slots = parent->ivar_slots;
    next->slot_names = parent->slot_names;
    next->parent_shape = std::move(parent);
    next->ivar_slots[name] =
        static_cast<std::uint32_t>(next->slot_names.size());
    next->slot_names.push_back(name);
    shard.transitions[key] = next;
    return next;
  }

  void invalidate_dispatch_owner(std::uint32_t class_index) {
    if (class_index >= classes.size()) {
      return;
    }
    ++classes[class_index].method_version;
    ++world_epoch;
  }

  struct NotebookModuleRuntimeStatePreparation {
    std::vector<std::vector<std::shared_ptr<CallCacheEntry>>> call_caches;
    std::vector<std::vector<std::shared_ptr<IvarCacheEntry>>> ivar_caches;
    std::vector<std::uint32_t> resolved_class_refs;
    std::unordered_map<std::uint32_t, std::uint32_t>
        rest_param_index_by_code;
    std::unordered_map<std::uint32_t, std::uint32_t>
        kw_rest_param_index_by_code;
    std::unordered_set<std::uint32_t> codes_needing_param_shaping;
    bool has_any_rest_params = false;
    bool has_any_shaped_params = false;
  };

  // A notebook image has no classes or methods. Prepare all allocations for
  // its code-indexed caches without touching the active RuntimeState.
  // RuntimeWorld validates the shape and bounds before calling this helper.
  NotebookModuleRuntimeStatePreparation
  prepare_notebook_module_runtime_state(const bytecode::BcModule &module) const {
    std::uint32_t max_code_id = 0;
    for (const bytecode::BcCode &code : module.code_objects) {
      max_code_id = std::max(max_code_id, code.code_id);
    }

    NotebookModuleRuntimeStatePreparation prepared;
    prepared.call_caches.resize(static_cast<std::size_t>(max_code_id) + 1U);
    prepared.ivar_caches.resize(static_cast<std::size_t>(max_code_id) + 1U);
    for (const bytecode::BcCode &code : module.code_objects) {
      prepared.call_caches[code.code_id].resize(code.call_site_table.size());
      prepared.ivar_caches[code.code_id].resize(code.ivar_site_table.size());
    }

    prepared.resolved_class_refs.assign(
        module.const_pool.size(), kClassRefUnknown);
    for (const bytecode::BcCode &code : module.code_objects) {
      if ((code.flags & bytecode::kCodeFlagRestParam) == 0U) {
        continue;
      }
      prepared.rest_param_index_by_code[code.code_id] =
          code.flags >> bytecode::kCodeRestParamIndexShift;
      prepared.codes_needing_param_shaping.insert(code.code_id);
      prepared.has_any_rest_params = true;
      prepared.has_any_shaped_params = true;
    }

    return prepared;
  }

  // Publish a prepared notebook runtime view. Every operation here is a
  // scalar store or a standard-container swap/clear, so the active state and
  // the active module can never be left at different generations by an
  // allocation exception.
  void commit_notebook_module_runtime_state(
      NotebookModuleRuntimeStatePreparation prepared) noexcept {
    // The notebook-only contract has no class state to resolve.  Keep the
    // existing class vector untouched; the public boundary rejects both an
    // old and a replacement image that contain classes or methods.
    call_caches.swap(prepared.call_caches);
    ivar_caches.swap(prepared.ivar_caches);
    resolved_class_refs.swap(prepared.resolved_class_refs);
    rest_param_index_by_code.swap(prepared.rest_param_index_by_code);
    kw_rest_param_index_by_code.swap(prepared.kw_rest_param_index_by_code);
    codes_needing_param_shaping.swap(prepared.codes_needing_param_shaping);
    has_any_rest_params = prepared.has_any_rest_params;
    has_any_shaped_params = prepared.has_any_shaped_params;
    owners_initialized = true;
    call_cache_entry_count = 0;
    module_init_completed = false;
    module_bindings.clear();
    ++module_bindings_revision;
    ++world_epoch;
  }

  void replace_notebook_module_runtime_state(
      const bytecode::BcModule &module) {
    commit_notebook_module_runtime_state(
        prepare_notebook_module_runtime_state(module));
  }

  void replace_module_runtime_state(const bytecode::BcModule &module) {
    std::vector<ClassRuntimeState> previous_classes = std::move(classes);
    classes.clear();
    classes.resize(module.classes.size());
    // These maps describe code entry parameters, not persistent runtime
    // state. A notebook image swap keeps the heap/state object but must never
    // let a VM for the replacement image consult stale entries from the old
    // image.
    rest_param_index_by_code.clear();
    kw_rest_param_index_by_code.clear();
    codes_needing_param_shaping.clear();
    has_any_rest_params = false;
    has_any_shaped_params = false;
    if (root_shapes.size() < module.classes.size()) {
      root_shapes.resize(module.classes.size());
    }
    resolve_class_refs_for_module(module);

    for (std::uint32_t index = 0; index < module.classes.size(); ++index) {
      ClassRuntimeState &runtime = classes[index];
      if (index < previous_classes.size()) {
        runtime.cvars = std::move(previous_classes[index].cvars);
        runtime.direct_include_indices =
            std::move(previous_classes[index].direct_include_indices);
        runtime.direct_extend_indices =
            std::move(previous_classes[index].direct_extend_indices);
        runtime.method_version = previous_classes[index].method_version + 1U;
      }

      const bytecode::BcClass &owner = module.classes[index];
      runtime.owner_flags = owner.flags;
      runtime.ivar_schema_id = owner.ivar_schema_id;
      runtime.has_superclass_ref = owner.has_superclass_ref;
      runtime.superclass_ref = owner.superclass_ref;
      runtime.method_range_valid =
          owner.method_range_start + owner.method_range_count <=
          module.methods.size();
      if (!runtime.method_range_valid) {
        continue;
      }
      for (std::uint32_t offset = 0; offset < owner.method_range_count;
           ++offset) {
        bytecode::BcMethod method =
            module.methods[owner.method_range_start + offset];
        MethodTableDescriptor &table =
            (method.flags & bytecode::kMethodFlagClass) != 0U
                ? runtime.class_method_table
                : runtime.instance_method_table;
        table.entries[method.selector_sym_id] = std::move(method);
      }
    }

    owners_initialized = true;
    initialize_inline_cache_layout(module);
    module_init_completed = false;
    module_bindings.clear();
    ++module_bindings_revision;
    ++world_epoch;
  }
};

struct RuntimeVmExecutionContext {
  std::shared_ptr<RuntimeState> state;
  std::string module_id;
  const RuntimeWorldOptions *world_options = nullptr;
  // Copied into RuntimeState when a VM is constructed. This avoids retaining
  // a pointer into RuntimeWorld's options from parked/persistent VMs.
  RuntimeGcRootProvider external_gc_root_provider;
  RuntimeNotebookCellContext notebook_cell_context;
  bool notebook_cell_execution = false;
  const RuntimeCapabilityResolution *capabilities = nullptr;
  const RuntimeEffectValidation *effects = nullptr;
  std::function<void(RuntimeTraceEvent)> trace_recorder;
  const NativeRegistry *native_registry = nullptr;
  const RuntimeModuleRegistry *module_registry = nullptr;
  const RuntimeTypeRegistry *type_registry = nullptr;
  const RuntimeDispatchRegistry *dispatch_registry = nullptr;
  const RuntimeErrorRegistry *error_registry = nullptr;
  // Macro-profile only: invoking an Ast.Block through the ordinary call
  // opcode recursively expands its nested macro calls and returns the
  // expanded block. Normal runtime executions leave this empty, so Ast values
  // are not generally callable.
  std::function<ExecutionResult(const Value &)> macro_block_executor;
  // Interpreter step budget for this execution; 0 = unlimited. Used by the
  // macro expander so a looping macro is a diagnostic, not a compiler hang.
  std::int64_t step_budget = 0;
};

// Reusable narrow VM facade for native bridge calls made through RuntimeWorld.
// Native executables make many small stdlib sends and native-extension calls
// through the same world; a session keeps the decoded module and VM-side
// lookup caches instead of copying the complete BcModule for every call.
class RuntimeNativeBridgeSession {
public:
  RuntimeNativeBridgeSession(const bytecode::BcModule &module,
                             RuntimeVmExecutionContext context);
  RuntimeNativeBridgeSession(
      std::shared_ptr<const bytecode::BcModule> module,
      RuntimeVmExecutionContext context);
  ~RuntimeNativeBridgeSession();
  RuntimeNativeBridgeSession(const RuntimeNativeBridgeSession &) = delete;
  RuntimeNativeBridgeSession &
  operator=(const RuntimeNativeBridgeSession &) = delete;

  void synchronize_runtime_names(
      const std::vector<std::string> &strings,
      const std::vector<std::string> &symbols);
  ExecutionResult invoke_extension(std::uint32_t code_id,
                                   const std::vector<Value> &args, Value self);
  ExecutionResult invoke_stdlib_send(
      Value receiver, std::string selector, const std::vector<Value> &args,
      const std::vector<std::pair<std::string, Value>> &keyword_args,
      Value block);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

ExecutionResult execute_runtime_vm(const bytecode::BcModule &module,
                                   RuntimeVmExecutionContext context,
                                   std::uint32_t code_id,
                                   const std::vector<Value> &args, Value self,
                                   Value block);

ExecutionResult execute_runtime_vm(
    std::shared_ptr<const bytecode::BcModule> module,
    const std::vector<std::string> &runtime_strings,
    const std::vector<std::string> &runtime_symbols,
    RuntimeVmExecutionContext context, std::uint32_t code_id,
    const std::vector<Value> &args, Value self, Value block);

ExecutionResult invoke_runtime_native_extension(
    const bytecode::BcModule &module, RuntimeVmExecutionContext context,
    std::uint32_t code_id, const std::vector<Value> &args, Value self);

ExecutionResult invoke_runtime_native_stdlib_send(
    const bytecode::BcModule &module, RuntimeVmExecutionContext context,
    Value receiver, std::string selector, const std::vector<Value> &args,
    const std::vector<std::pair<std::string, Value>> &keyword_args,
    Value block);

// Keyword-argument entry: `kw_args` pairs the keyword's symbol id (in
// `module`'s symbol table) with its value. Requires a single-signature method
// entry for the code id; used by the macro expander's keyword call channel.
ExecutionResult execute_runtime_vm(
    const bytecode::BcModule &module, RuntimeVmExecutionContext context,
    std::uint32_t code_id, const std::vector<Value> &args,
    const std::vector<std::pair<std::uint32_t, Value>> &kw_args, Value self,
    Value block);

} // namespace sputnik::runtime
