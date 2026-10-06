#pragma once

// In-tree C++ surface for the native-extension ABI (native-packages design §6,
// 5c-ii dispatch). `runtime/amber_ext.h` is the stable C contract an external
// author compiles against; this header is how the runtime *drives* that
// contract from inside the tree: a process-global registration table populated
// either by a generated native binary or by the interpreted manifest loader
// after `dlopen`, and the bridge helpers the VM SEND path uses to marshal a
// call across the ABI to a linked C thunk.
//
// The marshalling itself (AmberCtx, AmberValue arena, every amber_ext.h
// function) lives in `runtime/amber_ext.cpp`; AmberCtx is opaque here, exactly
// as it is to an extension author.

#include "runtime/amber_ext.h"
#include "runtime/stdlib_registry.h" // StdlibHost, NativeTagRegistry, Value

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace amber::runtime {

using NativeExtErrorDescriptor = RuntimeNativePackageErrorDescriptor;

// The host's logical-name -> thunk table, foreign-handle tag table, and package
// error descriptors. Populated before RuntimeWorld construction by generated
// registration calls or by the scoped shared-library loader, then imported
// into the active runtime registries. Empty in a plain bytecode run, which is
// exactly what makes a `native def` fall back to its Amber body when no thunk
// is registered.
//
// A thunk is stored as a raw `void *`: the registration table is generated from
// the build manifest, which does not record whether a symbol is a free function
// or a handle method. That distinction lives in the bytecode (the binding's
// code object is or is not a method), so the VM SEND path supplies it when it
// reinterpret_casts the pointer to `AmberFreeFn` / `AmberMethodFn`.
class NativeExtRegistry {
public:
  struct ResolvedThunk {
    void *fn = nullptr;
    bool blocking = false;
  };

  void register_thunk(const std::string &logical, void *fn);
  void register_type(NativeTypeDescriptor descriptor);
  void register_error(NativeExtErrorDescriptor descriptor);
  void register_package(RuntimeNativePackageDescriptor descriptor);
  void contribute_to(RuntimeNativePackageDescriptor &descriptor) const;

  void register_thunks(RuntimeDispatchRegistry &dispatch) const;
  void register_types(RuntimeTypeRegistry &types) const;
  void register_errors(RuntimeErrorRegistry &errors) const;
  void register_runtime_contributions(RuntimeDispatchRegistry &dispatch,
                                      RuntimeTypeRegistry &types,
                                      RuntimeErrorRegistry &errors) const;
  std::optional<ResolvedThunk>
  resolve_thunk(const std::string &logical) const;
  const NativeTypeDescriptor *find_type(const std::string &tag) const;

  // The single process-global instance the native host fills and runtime worlds
  // import from.
  static NativeExtRegistry &global();

private:
  RuntimeNativePackageDescriptor descriptor_;
};

// Outcome of bridging a native-extension call through the C ABI. On `!ok` a
// fault or exception has already been recorded on the active frame via the
// host, so the VM only has to stop and let it propagate.
struct NativeExtCallOutcome {
  bool ok = false;
  Value value = Value::null();
};

// Backend-neutral operations for an AmberCtx driven by a fully-native host.
// The public C ABI keeps AmberCtx/AmberValue opaque; generated native code owns
// the value arena and supplies these operations so a C thunk can work directly
// on NativeValue without round-tripping through RuntimeWorld and a VM frame.
// This is an in-tree C++ contract, not part of the stable extension ABI.
struct AmberExtDirectOps {
  int (*is_null)(void *, AmberValue) = nullptr;
  int (*is_bool)(void *, AmberValue) = nullptr;
  int (*is_int)(void *, AmberValue) = nullptr;
  int (*is_float)(void *, AmberValue) = nullptr;
  int (*is_str)(void *, AmberValue) = nullptr;
  int (*is_bytes)(void *, AmberValue) = nullptr;
  int (*is_list)(void *, AmberValue) = nullptr;
  int (*is_handle)(void *, AmberValue) = nullptr;

  int (*as_bool)(void *, AmberValue, int *) = nullptr;
  int (*as_int)(void *, AmberValue, std::int64_t *) = nullptr;
  int (*as_float)(void *, AmberValue, double *) = nullptr;
  int (*str_view)(void *, AmberValue, const char **, std::size_t *) = nullptr;
  int (*bytes_view)(void *, AmberValue, const std::uint8_t **,
                    std::size_t *) = nullptr;
  std::size_t (*list_len)(void *, AmberValue) = nullptr;
  AmberValue (*list_at)(void *, AmberValue, std::size_t) = nullptr;
  int (*handle_ptr)(void *, AmberValue, const char *, void **) = nullptr;

  AmberValue (*make_null)(void *) = nullptr;
  AmberValue (*make_bool)(void *, int) = nullptr;
  AmberValue (*make_int)(void *, std::int64_t) = nullptr;
  AmberValue (*make_float)(void *, double) = nullptr;
  AmberValue (*make_str)(void *, const char *, std::size_t) = nullptr;
  AmberValue (*make_bytes)(void *, const std::uint8_t *, std::size_t) = nullptr;
  AmberValue (*make_list)(void *, const AmberValue *, std::size_t) = nullptr;
  AmberValue (*make_handle)(void *, const char *, void *) = nullptr;

  AmberStatus (*fault)(void *, const char *, const char *) = nullptr;
  AmberStatus (*call_block)(void *, AmberValue, const AmberValue *,
                            std::size_t, AmberValue *) = nullptr;
};

struct AmberExtDirectCallOutcome {
  AmberStatus status = AMBER_ERR;
  AmberValue value = nullptr;
};

// Stack-construct an AmberCtx backed by `ops`, invoke a linked C thunk, and
// return its opaque result handle. The generated owner resolves that handle
// from its NativeValue arena before the arena goes out of scope.
AmberExtDirectCallOutcome
amber_ext_invoke_direct_free(const AmberExtDirectOps &ops, void *state,
                             AmberFreeFn fn, const AmberValue *args,
                             std::size_t argc);
AmberExtDirectCallOutcome
amber_ext_invoke_direct_method(const AmberExtDirectOps &ops, void *state,
                               AmberMethodFn fn, AmberValue self,
                               const AmberValue *args, std::size_t argc);

// Deterministic teardown needs a live ABI context, but a destructor has a
// different signature from a method thunk. Preserve the handle tombstone.
bool amber_ext_destroy_direct(const AmberExtDirectOps &ops, void *state,
                              RuntimeForeignHandle &handle);

// Build an AmberCtx over (host, frame, tags), marshal `args` (and `self` for a
// method) into the per-call arena, invoke the thunk, and marshal the result
// back. The frame is type-erased to `const void *` exactly as in StdlibHost.
NativeExtCallOutcome amber_ext_invoke_free(StdlibHost &host, const void *frame,
                                           const NativeTagRegistry &tags,
                                           AmberFreeFn fn,
                                           const std::vector<Value> &args);
NativeExtCallOutcome
amber_ext_invoke_method(StdlibHost &host, const void *frame,
                        const NativeTagRegistry &tags, AmberMethodFn fn,
                        const Value &self, const std::vector<Value> &args);

// Direct AmberCtx surface, used by the bridge helpers above and by unit tests
// that exercise individual amber_ext.h functions. `import` pushes a runtime
// Value into the arena and returns its opaque handle; `export` reads a handle
// back out. Handles are valid until `amber_ext_ctx_close`.
AmberCtx *amber_ext_ctx_open(StdlibHost &host, const void *frame,
                             const NativeTagRegistry &tags);
void amber_ext_ctx_close(AmberCtx *ctx);
AmberValue amber_ext_ctx_import(AmberCtx *ctx, Value value);
Value amber_ext_ctx_export(AmberCtx *ctx, AmberValue handle);

} // namespace amber::runtime
