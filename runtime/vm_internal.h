#pragma once

#include "bytecode/format.h"
#include "runtime/heap.h"
#include "runtime/value.h"
#include "runtime/watch.h"
#include "runtime/world.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace amber::runtime {

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
};

struct IvarCacheEntry {
  bool valid = false;
  std::uint32_t receiver_class_index = 0;
  std::uint32_t symbol_id = 0;
  std::uint64_t shape_id = 0;
  std::uint64_t shape_version = 0;
  std::uint32_t slot_index = 0;
};

enum class QuickOpcode : std::uint8_t {
  Fallback,
  LoadK,
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

struct Frame {
  const bytecode::BcCode *code = nullptr;
  const QuickCode *quick_code = nullptr;
  std::size_t pc = 0;
  std::vector<Value> regs;
  std::vector<std::uint8_t> initialized;
  std::vector<std::int64_t> int64_regs;
  std::vector<std::uint8_t> int_valid;
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
  std::optional<std::uint32_t> active_call_pc;
  std::optional<Value> return_override;
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
  FlatRegMap<PreparedSeqState> prepared_seq_regs;
  FlatRegMap<PreparedMapState> prepared_map_regs;
  FlatRegMap<Value> pending_pattern_bindings;
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
  bool world_frozen = false;
  std::uint64_t world_epoch = 1;
  // Notebook/watch bookkeeping is shared by task-local Vm instances. It is
  // outside the ordinary dispatch path, but its ids, event log, and dependency
  // capture must still be race-free when reactive work runs on several
  // strands.
  std::shared_ptr<std::mutex> reactive_state_mutex =
      std::make_shared<std::mutex>();
  std::uint64_t watch_epoch = 0;
  std::uint64_t next_watch_cell_id = 1;
  std::uint64_t next_watch_handle_id = 1;
  std::vector<RuntimeWatchEvent> watch_events;
  bool dependency_capture_active = false;
  RuntimeDependencySet dependency_capture;
  std::unordered_map<std::string, std::size_t> dependency_capture_index;
  std::unordered_map<std::uint64_t, CallCacheEntry> call_caches;
  std::uint64_t call_cache_hits = 0;
  std::uint64_t call_cache_misses = 0;
  std::uint64_t call_cache_updates = 0;
  std::unordered_map<std::uint64_t, IvarCacheEntry> ivar_caches;
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
    std::lock_guard<std::mutex> lock(*reactive_state_mutex);
    return std::make_shared<RuntimeWatchCell>(
        std::move(value), next_watch_cell_id++, std::move(target_name));
  }

  std::shared_ptr<RuntimeWatchHandle>
  make_watch_handle(std::shared_ptr<RuntimeWatchCell> cell,
                    std::string target_name) {
    std::lock_guard<std::mutex> lock(*reactive_state_mutex);
    return std::make_shared<RuntimeWatchHandle>(
        std::move(cell), next_watch_handle_id++, std::move(target_name));
  }

  std::shared_ptr<RuntimeWatchHandle>
  make_watch_handle(std::shared_ptr<RuntimeWatchObjectState> object_state,
                    std::string target_name, std::string field_name) {
    std::lock_guard<std::mutex> lock(*reactive_state_mutex);
    return std::make_shared<RuntimeWatchHandle>(
        std::move(object_state), next_watch_handle_id++, std::move(target_name),
        std::move(field_name));
  }

  RuntimeWatchEvent record_watch_event(RuntimeWatchEvent event) {
    std::lock_guard<std::mutex> lock(*reactive_state_mutex);
    event.watch_epoch = ++watch_epoch;
    watch_events.push_back(event);
    return event;
  }

  std::size_t watch_event_count() const {
    std::lock_guard<std::mutex> lock(*reactive_state_mutex);
    return watch_events.size();
  }

  std::pair<std::vector<RuntimeWatchEvent>, std::uint64_t>
  watch_events_since(std::size_t start) const {
    std::lock_guard<std::mutex> lock(*reactive_state_mutex);
    std::vector<RuntimeWatchEvent> events;
    if (start <= watch_events.size()) {
      events.assign(
          watch_events.begin() + static_cast<std::ptrdiff_t>(start),
          watch_events.end());
    }
    return {std::move(events), watch_epoch};
  }

  std::uint64_t watch_epoch_snapshot() const {
    std::lock_guard<std::mutex> lock(*reactive_state_mutex);
    return watch_epoch;
  }

  std::vector<RuntimeWatchEvent> watch_events_snapshot() const {
    std::lock_guard<std::mutex> lock(*reactive_state_mutex);
    return watch_events;
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

  void replace_module_runtime_state(const bytecode::BcModule &module) {
    std::vector<ClassRuntimeState> previous_classes = std::move(classes);
    classes.clear();
    classes.resize(module.classes.size());
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
    call_caches.clear();
    ivar_caches.clear();
    module_init_completed = false;
    module_bindings.clear();
    ++world_epoch;
  }
};

struct RuntimeVmExecutionContext {
  std::shared_ptr<RuntimeState> state;
  std::string module_id;
  const RuntimeWorldOptions *world_options = nullptr;
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

} // namespace amber::runtime
