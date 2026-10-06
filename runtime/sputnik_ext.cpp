// Implementation of the native-extension ABI (runtime/sputnik_ext.h) and its
// in-tree driver surface (runtime/sputnik_ext_runtime.h).
//
// `SputnikValue` is an opaque handle into a per-call arena owned by SputnikCtx. VM
// calls use the runtime::Value arena below. Fully-native generated code supplies
// SputnikExtDirectOps over its NativeValue arena, keeping the stable C ABI while
// avoiding a RuntimeWorld/VM round-trip for every extension leaf.

#include "runtime/sputnik_ext_runtime.h"
#include "runtime/native_call_buffer.h"

#include "runtime/io.h"

#include <algorithm>
#include <cstdint>
#include <forward_list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using sputnik::runtime::RuntimeByteBuffer;
using sputnik::runtime::RuntimeBytes;
using sputnik::runtime::RuntimeByteSlice;
using sputnik::runtime::Value;

// A Bytes-like value (Bytes / ByteSlice / ByteBuffer), checked without faulting
// so the `sputnik_is_bytes` predicate and `sputnik_bytes_view` reader can report a
// wrong kind silently (per the sputnik_ext.h reader contract).
bool value_is_bytes(const Value &value) {
  if (!value.is_io_value()) {
    return false;
  }
  const std::shared_ptr<sputnik::runtime::RuntimeIoValue> io =
      value.as_io_value();
  return std::dynamic_pointer_cast<RuntimeBytes>(io) != nullptr ||
         std::dynamic_pointer_cast<RuntimeByteSlice>(io) != nullptr ||
         std::dynamic_pointer_cast<RuntimeByteBuffer>(io) != nullptr;
}

} // namespace

// The opaque call context. Defined in the global namespace to match the
// `typedef struct SputnikCtx SputnikCtx;` the C header declares inside extern "C".
struct SputnikCtx {
  sputnik::runtime::StdlibHost *host = nullptr;
  const void *frame = nullptr;
  const sputnik::runtime::NativeTagRegistry *tags = nullptr;
  const sputnik::runtime::SputnikExtDirectOps *direct_ops = nullptr;
  void *direct_state = nullptr;
  // The per-call value arena. `SputnikValue` is a 1-based index encoded as a
  // pointer, so reallocation here never invalidates a handle.
  sputnik::runtime::NativeCallValueArena<Value> arena;
  // Stable backing for borrowed string views (`stdlib_text_of` returns a copy);
  // Nodes keep element addresses valid across pushes for the call's duration.
  // Empty contexts allocate no backing storage, including native direct calls
  // whose string views are supplied by direct_ops.
  std::forward_list<std::string> str_keep;
  // Keepalives for borrowed bytes views, retained so the viewed storage
  // outlives the thunk.
  std::vector<Value> keepalives;

  SputnikValue push(Value value) {
    arena.push_back(std::move(value));
    return reinterpret_cast<SputnikValue>(
        static_cast<std::uintptr_t>(arena.size()));
  }

  const Value &resolve(SputnikValue handle) const {
    const std::uintptr_t raw = reinterpret_cast<std::uintptr_t>(handle);
    if (raw == 0 || raw > arena.size()) {
      static const Value kNull = Value::null();
      return kNull;
    }
    return arena[raw - 1];
  }
};

extern "C" {

// ---- value predicates ---------------------------------------------------

int sputnik_is_null(SputnikCtx *cx, SputnikValue value) {
  if (cx->direct_ops != nullptr) {
    return cx->direct_ops->is_null(cx->direct_state, value);
  }
  return cx->resolve(value).is_null() ? 1 : 0;
}
int sputnik_is_bool(SputnikCtx *cx, SputnikValue value) {
  if (cx->direct_ops != nullptr) {
    return cx->direct_ops->is_bool(cx->direct_state, value);
  }
  return cx->resolve(value).is_bool() ? 1 : 0;
}
int sputnik_is_int(SputnikCtx *cx, SputnikValue value) {
  if (cx->direct_ops != nullptr) {
    return cx->direct_ops->is_int(cx->direct_state, value);
  }
  return cx->resolve(value).is_integer() ? 1 : 0;
}
int sputnik_is_float(SputnikCtx *cx, SputnikValue value) {
  if (cx->direct_ops != nullptr) {
    return cx->direct_ops->is_float(cx->direct_state, value);
  }
  return cx->resolve(value).is_float() ? 1 : 0;
}
int sputnik_is_str(SputnikCtx *cx, SputnikValue value) {
  if (cx->direct_ops != nullptr) {
    return cx->direct_ops->is_str(cx->direct_state, value);
  }
  return cx->resolve(value).is_string() ? 1 : 0;
}
int sputnik_is_bytes(SputnikCtx *cx, SputnikValue value) {
  if (cx->direct_ops != nullptr) {
    return cx->direct_ops->is_bytes(cx->direct_state, value);
  }
  return value_is_bytes(cx->resolve(value)) ? 1 : 0;
}
int sputnik_is_list(SputnikCtx *cx, SputnikValue value) {
  if (cx->direct_ops != nullptr) {
    return cx->direct_ops->is_list(cx->direct_state, value);
  }
  return cx->resolve(value).is_list() ? 1 : 0;
}
int sputnik_is_handle(SputnikCtx *cx, SputnikValue value) {
  if (cx->direct_ops != nullptr) {
    return cx->direct_ops->is_handle(cx->direct_state, value);
  }
  return cx->resolve(value).is_foreign_handle() ? 1 : 0;
}

// ---- readers ------------------------------------------------------------

int sputnik_as_bool(SputnikCtx *cx, SputnikValue value, int *out) {
  if (cx->direct_ops != nullptr) {
    return cx->direct_ops->as_bool(cx->direct_state, value, out);
  }
  const Value &v = cx->resolve(value);
  if (!v.is_bool()) {
    return 0;
  }
  *out = v.as_bool() ? 1 : 0;
  return 1;
}
int sputnik_as_int(SputnikCtx *cx, SputnikValue value, int64_t *out) {
  if (cx->direct_ops != nullptr) {
    return cx->direct_ops->as_int(cx->direct_state, value, out);
  }
  const Value &v = cx->resolve(value);
  if (!v.is_integer()) {
    return 0;
  }
  *out = v.as_integer();
  return 1;
}
int sputnik_as_float(SputnikCtx *cx, SputnikValue value, double *out) {
  if (cx->direct_ops != nullptr) {
    return cx->direct_ops->as_float(cx->direct_state, value, out);
  }
  const Value &v = cx->resolve(value);
  if (!v.is_float()) {
    return 0;
  }
  *out = v.as_float();
  return 1;
}

int sputnik_str_view(SputnikCtx *cx, SputnikValue value, const char **ptr,
                   size_t *len) {
  if (cx->direct_ops != nullptr) {
    return cx->direct_ops->str_view(cx->direct_state, value, ptr, len);
  }
  const Value &v = cx->resolve(value);
  if (!v.is_string()) {
    return 0;
  }
  const std::optional<std::string> text = cx->host->stdlib_text_of(v);
  if (!text.has_value()) {
    return 0;
  }
  cx->str_keep.push_front(*text);
  const std::string &stored = cx->str_keep.front();
  *ptr = stored.data();
  *len = stored.size();
  return 1;
}

int sputnik_bytes_view(SputnikCtx *cx, SputnikValue value, const uint8_t **ptr,
                     size_t *len) {
  if (cx->direct_ops != nullptr) {
    return cx->direct_ops->bytes_view(cx->direct_state, value, ptr, len);
  }
  const Value &v = cx->resolve(value);
  if (!value_is_bytes(v)) {
    return 0;
  }
  Value keepalive = Value::null();
  if (!cx->host->stdlib_bytes_view(cx->frame, v, ptr, len, &keepalive)) {
    return 0;
  }
  cx->keepalives.push_back(std::move(keepalive));
  return 1;
}

size_t sputnik_list_len(SputnikCtx *cx, SputnikValue value) {
  if (cx->direct_ops != nullptr) {
    return cx->direct_ops->list_len(cx->direct_state, value);
  }
  const Value &v = cx->resolve(value);
  if (!v.is_list()) {
    return 0;
  }
  const sputnik::runtime::IntrusivePtr<sputnik::runtime::ListValue> list =
      v.as_list();
  return list == nullptr ? 0 : list->items.size();
}
SputnikValue sputnik_list_at(SputnikCtx *cx, SputnikValue value, size_t index) {
  if (cx->direct_ops != nullptr) {
    return cx->direct_ops->list_at(cx->direct_state, value, index);
  }
  const Value &v = cx->resolve(value);
  if (!v.is_list()) {
    return cx->push(Value::null());
  }
  const sputnik::runtime::IntrusivePtr<sputnik::runtime::ListValue> list =
      v.as_list();
  if (list == nullptr || index >= list->items.size()) {
    return cx->push(Value::null());
  }
  return cx->push(list->items[index]);
}

int sputnik_handle_ptr(SputnikCtx *cx, SputnikValue value, const char *tag,
                     void **out) {
  if (cx->direct_ops != nullptr) {
    return cx->direct_ops->handle_ptr(cx->direct_state, value, tag, out);
  }
  const Value &v = cx->resolve(value);
  if (!v.is_foreign_handle()) {
    cx->host->stdlib_raise_runtime_error(cx->frame, "TypeError",
                                         "expected a native handle");
    return 0;
  }
  const std::shared_ptr<sputnik::runtime::RuntimeForeignHandle> handle =
      v.as_foreign_handle();
  if (handle == nullptr) {
    cx->host->stdlib_raise_runtime_error(cx->frame, "TypeError",
                                         "native handle is null");
    return 0;
  }
  if (tag != nullptr && handle->tag != tag) {
    cx->host->stdlib_raise_runtime_error(cx->frame, "TypeError",
                                         "native handle tag mismatch");
    return 0;
  }
  if (!handle->live) {
    cx->host->stdlib_raise_runtime_error(cx->frame, "LifetimeError",
                                         "native handle used after destroy!");
    return 0;
  }
  *out = handle->ptr;
  return 1;
}

// ---- builders -----------------------------------------------------------

SputnikValue sputnik_make_null(SputnikCtx *cx) {
  if (cx->direct_ops != nullptr) {
    return cx->direct_ops->make_null(cx->direct_state);
  }
  return cx->push(Value::null());
}
SputnikValue sputnik_make_bool(SputnikCtx *cx, int value) {
  if (cx->direct_ops != nullptr) {
    return cx->direct_ops->make_bool(cx->direct_state, value);
  }
  return cx->push(Value::boolean(value != 0));
}
SputnikValue sputnik_make_int(SputnikCtx *cx, int64_t value) {
  if (cx->direct_ops != nullptr) {
    return cx->direct_ops->make_int(cx->direct_state, value);
  }
  return cx->push(Value::integer(value));
}
SputnikValue sputnik_make_float(SputnikCtx *cx, double value) {
  if (cx->direct_ops != nullptr) {
    return cx->direct_ops->make_float(cx->direct_state, value);
  }
  return cx->push(Value::floating(value));
}
SputnikValue sputnik_make_str(SputnikCtx *cx, const char *ptr, size_t len) {
  if (cx->direct_ops != nullptr) {
    return cx->direct_ops->make_str(cx->direct_state, ptr, len);
  }
  return cx->push(cx->host->stdlib_string_value_from_text(
      std::string(ptr == nullptr ? "" : ptr, ptr == nullptr ? 0 : len)));
}
SputnikValue sputnik_make_bytes(SputnikCtx *cx, const uint8_t *ptr, size_t len) {
  if (cx->direct_ops != nullptr) {
    return cx->direct_ops->make_bytes(cx->direct_state, ptr, len);
  }
  return cx->push(cx->host->stdlib_bytes_value_from_bytes(std::string(
      reinterpret_cast<const char *>(ptr), ptr == nullptr ? 0 : len)));
}
SputnikValue sputnik_make_list(SputnikCtx *cx, const SputnikValue *items,
                           size_t count) {
  if (cx->direct_ops != nullptr) {
    return cx->direct_ops->make_list(cx->direct_state, items, count);
  }
  std::vector<Value> values;
  values.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    values.push_back(cx->resolve(items[i]));
  }
  return cx->push(cx->host->stdlib_make_list(std::move(values)));
}

SputnikValue sputnik_make_handle(SputnikCtx *cx, const char *tag, void *ptr) {
  if (cx->direct_ops != nullptr) {
    return cx->direct_ops->make_handle(cx->direct_state, tag, ptr);
  }
  std::string tag_str = tag == nullptr ? std::string() : tag;
  const sputnik::runtime::NativeTypeDescriptor *descriptor =
      cx->tags == nullptr ? nullptr : cx->tags->lookup(tag_str);
  if (descriptor == nullptr) {
    cx->host->stdlib_raise_runtime_error(
        cx->frame, "TypeError", "unknown native handle tag '" + tag_str + "'");
    return cx->push(Value::null());
  }
  auto handle = std::make_shared<sputnik::runtime::RuntimeForeignHandle>();
  handle->tag = std::move(tag_str);
  handle->ptr = ptr;
  handle->ownership = descriptor->ownership;
  switch (descriptor->ownership) {
  case sputnik::runtime::RuntimeForeignHandle::Ownership::Owned: {
    // The descriptor already type-erases the ctx to void* (vm.h stays free of
    // the C ABI types); the value handed in at destroy!-time is the SputnikCtx*.
    void (*destroy)(void *, void *) = descriptor->owned_destructor;
    handle->teardown = [destroy](void *ctx, void *resource) {
      if (destroy != nullptr) {
        destroy(ctx, resource);
      }
    };
    break;
  }
  case sputnik::runtime::RuntimeForeignHandle::Ownership::Collected: {
    void (*reclaim)(void *) = descriptor->collected_reclaim;
    handle->teardown = [reclaim](void *, void *resource) {
      if (reclaim != nullptr) {
        reclaim(resource);
      }
    };
    break;
  }
  case sputnik::runtime::RuntimeForeignHandle::Ownership::Borrowed:
    break; // Sputnik never frees a borrowed handle.
  }
  return cx->push(Value::foreign_handle(std::move(handle)));
}

// ---- faults and callbacks ----------------------------------------------

SputnikStatus sputnik_fault(SputnikCtx *cx, const char *error_class,
                        const char *message) {
  if (cx->direct_ops != nullptr) {
    return cx->direct_ops->fault(cx->direct_state, error_class, message);
  }
  // Rescuable: maps to an Sputnik error class an enclosing `rescue` can catch
  // (native-packages design §6), not a terminal fault.
  cx->host->stdlib_raise_runtime_error(
      cx->frame, error_class == nullptr ? "RuntimeError" : error_class,
      message == nullptr ? "" : message);
  return SPUTNIK_ERR;
}

SputnikStatus sputnik_call_block(SputnikCtx *cx, SputnikValue block,
                             const SputnikValue *args, size_t argc,
                             SputnikValue *out) {
  if (cx->direct_ops != nullptr) {
    return cx->direct_ops->call_block(cx->direct_state, block, args, argc,
                                      out);
  }
  std::vector<Value> block_args;
  block_args.reserve(argc);
  for (size_t i = 0; i < argc; ++i) {
    block_args.push_back(cx->resolve(args[i]));
  }
  sputnik::runtime::StdlibBlockResult result = cx->host->stdlib_call_block(
      cx->frame, cx->resolve(block), std::move(block_args));
  switch (result.status) {
  case sputnik::runtime::StdlibBlockStatus::Returned:
    *out = cx->push(std::move(result.value));
    return SPUTNIK_OK;
  case sputnik::runtime::StdlibBlockStatus::Raised:
    cx->host->stdlib_raise_exception(cx->frame, std::move(result.exception));
    return SPUTNIK_ERR;
  case sputnik::runtime::StdlibBlockStatus::Stopped:
  case sputnik::runtime::StdlibBlockStatus::Faulted:
    break;
  }
  return SPUTNIK_ERR; // a fault/throw has already been recorded on the frame.
}

int sputnik_task_cancelled(SputnikCtx *cx) {
  (void)cx;
  return sputnik::runtime::current_runtime_task_cancel_requested() ? 1 : 0;
}

uint32_t sputnik_ext_abi_version(void) { return SPUTNIK_EXT_ABI_VERSION; }

} // extern "C"

namespace sputnik::runtime {

SputnikExtDirectCallOutcome
sputnik_ext_invoke_direct_free(const SputnikExtDirectOps &ops, void *state,
                             SputnikFreeFn fn, const SputnikValue *args,
                             std::size_t argc) {
  SputnikCtx ctx;
  ctx.direct_ops = &ops;
  ctx.direct_state = state;
  SputnikExtDirectCallOutcome outcome;
  outcome.status = fn(&ctx, args, argc, &outcome.value);
  return outcome;
}

SputnikExtDirectCallOutcome
sputnik_ext_invoke_direct_method(const SputnikExtDirectOps &ops, void *state,
                               SputnikMethodFn fn, SputnikValue self,
                               const SputnikValue *args, std::size_t argc) {
  SputnikCtx ctx;
  ctx.direct_ops = &ops;
  ctx.direct_state = state;
  SputnikExtDirectCallOutcome outcome;
  outcome.status = fn(&ctx, self, args, argc, &outcome.value);
  return outcome;
}

bool sputnik_ext_destroy_direct(const SputnikExtDirectOps &ops, void *state,
                              RuntimeForeignHandle &handle) {
  SputnikCtx ctx;
  ctx.direct_ops = &ops;
  ctx.direct_state = state;
  return handle.destroy(&ctx);
}

// ---- process-global registration ---------------------------------------

void NativeExtRegistry::register_thunk(const std::string &logical, void *fn) {
  const auto found = std::find_if(
      descriptor_.thunks.begin(), descriptor_.thunks.end(),
      [&](const RuntimeNativePackageThunkDescriptor &descriptor) {
        return descriptor.logical == logical;
      });
  if (found != descriptor_.thunks.end()) {
    found->fn = fn;
    return;
  }
  descriptor_.thunks.push_back({logical, fn, false});
}
void NativeExtRegistry::register_type(NativeTypeDescriptor descriptor) {
  const auto found = std::find_if(
      descriptor_.types.begin(), descriptor_.types.end(),
      [&](const NativeTypeDescriptor &registered) {
        return registered.tag == descriptor.tag;
      });
  if (found != descriptor_.types.end()) {
    *found = std::move(descriptor);
    return;
  }
  descriptor_.types.push_back(std::move(descriptor));
}
void NativeExtRegistry::register_error(NativeExtErrorDescriptor descriptor) {
  descriptor_.errors.push_back(std::move(descriptor));
}
void NativeExtRegistry::register_package(
    RuntimeNativePackageDescriptor descriptor) {
  for (const RuntimeNativePackageThunkDescriptor &thunk : descriptor.thunks) {
    const auto found = std::find_if(
        descriptor_.thunks.begin(), descriptor_.thunks.end(),
        [&](const RuntimeNativePackageThunkDescriptor &registered) {
          return registered.logical == thunk.logical;
        });
    if (found != descriptor_.thunks.end()) {
      found->fn = thunk.fn;
      found->blocking = thunk.blocking;
    } else {
      descriptor_.thunks.push_back(thunk);
    }
  }
  for (NativeTypeDescriptor &type : descriptor.types) {
    register_type(std::move(type));
  }
  for (RuntimeNativePackageCodeBindingDescriptor &binding :
       descriptor.code_bindings) {
    descriptor_.code_bindings.push_back(std::move(binding));
  }
  for (RuntimeNativePackageMethodBindingDescriptor &binding :
       descriptor.method_bindings) {
    descriptor_.method_bindings.push_back(std::move(binding));
  }
  for (RuntimeNativePackageErrorDescriptor &error : descriptor.errors) {
    register_error(std::move(error));
  }
}
void NativeExtRegistry::contribute_to(
    RuntimeNativePackageDescriptor &descriptor) const {
  descriptor.thunks.insert(descriptor.thunks.end(), descriptor_.thunks.begin(),
                           descriptor_.thunks.end());
  descriptor.types.insert(descriptor.types.end(), descriptor_.types.begin(),
                          descriptor_.types.end());
  descriptor.code_bindings.insert(descriptor.code_bindings.end(),
                                  descriptor_.code_bindings.begin(),
                                  descriptor_.code_bindings.end());
  descriptor.method_bindings.insert(descriptor.method_bindings.end(),
                                    descriptor_.method_bindings.begin(),
                                    descriptor_.method_bindings.end());
  descriptor.errors.insert(descriptor.errors.end(), descriptor_.errors.begin(),
                           descriptor_.errors.end());
}
void NativeExtRegistry::register_thunks(
    RuntimeDispatchRegistry &dispatch) const {
  for (const RuntimeNativePackageThunkDescriptor &thunk : descriptor_.thunks) {
    dispatch.register_native_package_thunk(thunk.logical, thunk.fn,
                                           thunk.blocking);
  }
}
void NativeExtRegistry::register_types(RuntimeTypeRegistry &types) const {
  for (NativeTypeDescriptor type : descriptor_.types) {
    types.register_native_package_type(std::move(type));
  }
}
void NativeExtRegistry::register_errors(RuntimeErrorRegistry &errors) const {
  for (const RuntimeNativePackageErrorDescriptor &error : descriptor_.errors) {
    errors.register_error(error.name,
                          error.parent.empty() ? "NativeError" : error.parent,
                          error.default_message, error.default_exit_code,
                          error.field_mask);
  }
}
void NativeExtRegistry::register_runtime_contributions(
    RuntimeDispatchRegistry &dispatch, RuntimeTypeRegistry &types,
    RuntimeErrorRegistry &errors) const {
  RuntimeNativePackageDescriptor descriptor;
  contribute_to(descriptor);
  register_runtime_native_package_descriptor(dispatch, types, errors,
                                            descriptor);
}
std::optional<NativeExtRegistry::ResolvedThunk>
NativeExtRegistry::resolve_thunk(const std::string &logical) const {
  const auto found = std::find_if(
      descriptor_.thunks.begin(), descriptor_.thunks.end(),
      [&](const RuntimeNativePackageThunkDescriptor &descriptor) {
        return descriptor.logical == logical;
      });
  if (found == descriptor_.thunks.end()) {
    return std::nullopt;
  }
  return ResolvedThunk{found->fn, found->blocking};
}
const NativeTypeDescriptor *
NativeExtRegistry::find_type(const std::string &tag) const {
  const auto found = std::find_if(
      descriptor_.types.begin(), descriptor_.types.end(),
      [&](const NativeTypeDescriptor &descriptor) {
        return descriptor.tag == tag;
      });
  return found == descriptor_.types.end() ? nullptr : &*found;
}
NativeExtRegistry &NativeExtRegistry::global() {
  static NativeExtRegistry registry;
  return registry;
}

// ---- bridge helpers (the VM SEND path) ---------------------------------

NativeExtCallOutcome sputnik_ext_invoke_free(StdlibHost &host, const void *frame,
                                           const NativeTagRegistry &tags,
                                           SputnikFreeFn fn,
                                           const std::vector<Value> &args) {
  SputnikCtx ctx;
  ctx.host = &host;
  ctx.frame = frame;
  ctx.tags = &tags;
  ctx.arena.reserve(args.size() + 1U);
  NativeCallHandleBuffer<SputnikValue> handles(args.size());
  for (std::size_t index = 0; index < args.size(); ++index) {
    handles[index] = ctx.push(args[index]);
  }
  SputnikValue out = nullptr;
  const SputnikStatus status = fn(&ctx, handles.data(), handles.size(), &out);
  NativeExtCallOutcome outcome;
  if (status == SPUTNIK_OK) {
    outcome.ok = true;
    outcome.value = ctx.resolve(out);
  }
  return outcome;
}

NativeExtCallOutcome
sputnik_ext_invoke_method(StdlibHost &host, const void *frame,
                        const NativeTagRegistry &tags, SputnikMethodFn fn,
                        const Value &self, const std::vector<Value> &args) {
  SputnikCtx ctx;
  ctx.host = &host;
  ctx.frame = frame;
  ctx.tags = &tags;
  ctx.arena.reserve(args.size() + 2U);
  const SputnikValue self_handle = ctx.push(self);
  NativeCallHandleBuffer<SputnikValue> handles(args.size());
  for (std::size_t index = 0; index < args.size(); ++index) {
    handles[index] = ctx.push(args[index]);
  }
  SputnikValue out = nullptr;
  const SputnikStatus status =
      fn(&ctx, self_handle, handles.data(), handles.size(), &out);
  NativeExtCallOutcome outcome;
  if (status == SPUTNIK_OK) {
    outcome.ok = true;
    outcome.value = ctx.resolve(out);
  }
  return outcome;
}

// ---- direct ctx surface (bridge helpers + unit tests) ------------------

SputnikCtx *sputnik_ext_ctx_open(StdlibHost &host, const void *frame,
                             const NativeTagRegistry &tags) {
  SputnikCtx *ctx = new SputnikCtx();
  ctx->host = &host;
  ctx->frame = frame;
  ctx->tags = &tags;
  return ctx;
}
void sputnik_ext_ctx_close(SputnikCtx *ctx) { delete ctx; }
SputnikValue sputnik_ext_ctx_import(SputnikCtx *ctx, Value value) {
  return ctx->push(std::move(value));
}
Value sputnik_ext_ctx_export(SputnikCtx *ctx, SputnikValue handle) {
  return ctx->resolve(handle);
}

} // namespace sputnik::runtime
