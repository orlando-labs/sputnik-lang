#pragma once

// In-tree C++ surface for the native-extension ABI (native-packages design §6,
// 5c-ii dispatch). `runtime/sputnik_ext.h` is the stable C contract an external
// author compiles against; this header is how the runtime *drives* that
// contract from inside the tree: a process-global registration table populated
// either by a generated native binary or by the interpreted manifest loader
// after `dlopen`, and the bridge helpers the VM SEND path uses to marshal a
// call across the ABI to a linked C thunk.
//
// The marshalling itself (SputnikCtx, SputnikValue arena, every sputnik_ext.h
// function) lives in `runtime/sputnik_ext.cpp`; SputnikCtx is opaque here, exactly
// as it is to an extension author.

#include "runtime/sputnik_ext.h"
#include "runtime/stdlib_registry.h" // StdlibHost, NativeTagRegistry, Value

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sputnik::runtime {

// Validate an optional leaf sidecar once, before publishing the package.
// Reject incompatible descriptors and blocking thunks rather than calling a
// function with an incompatible signature/effect contract.
void validate_native_leaf_descriptor(const SputnikLeafDescriptor *leaf,
                                     bool blocking);

using NativeExtErrorDescriptor = RuntimeNativePackageErrorDescriptor;

// The host's logical-name -> thunk table, foreign-handle tag table, and package
// error descriptors. Populated before RuntimeWorld construction by generated
// registration calls or by the scoped shared-library loader, then imported
// into the active runtime registries. Empty in a plain bytecode run, which is
// exactly what makes a `native def` fall back to its Sputnik body when no thunk
// is registered.
//
// A thunk is stored as a raw `void *`: the registration table is generated from
// the build manifest, which does not record whether a symbol is a free function
// or a handle method. That distinction lives in the bytecode (the binding's
// code object is or is not a method), so the VM SEND path supplies it when it
// reinterpret_casts the pointer to `SputnikFreeFn` / `SputnikMethodFn`.
class NativeExtRegistry {
public:
  struct ResolvedThunk {
    void *fn = nullptr;
    bool blocking = false;
    const SputnikLeafDescriptor *leaf = nullptr;
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

// Backend-neutral operations for an SputnikCtx driven by a fully-native host.
// The public C ABI keeps SputnikCtx/SputnikValue opaque; generated native code owns
// the value arena and supplies these operations so a C thunk can work directly
// on NativeValue without round-tripping through RuntimeWorld and a VM frame.
// This is an in-tree C++ contract, not part of the stable extension ABI.
struct SputnikExtDirectOps {
  int (*is_null)(void *, SputnikValue) = nullptr;
  int (*is_bool)(void *, SputnikValue) = nullptr;
  int (*is_int)(void *, SputnikValue) = nullptr;
  int (*is_float)(void *, SputnikValue) = nullptr;
  int (*is_str)(void *, SputnikValue) = nullptr;
  int (*is_bytes)(void *, SputnikValue) = nullptr;
  int (*is_list)(void *, SputnikValue) = nullptr;
  int (*is_handle)(void *, SputnikValue) = nullptr;

  int (*as_bool)(void *, SputnikValue, int *) = nullptr;
  int (*as_int)(void *, SputnikValue, std::int64_t *) = nullptr;
  int (*as_float)(void *, SputnikValue, double *) = nullptr;
  int (*str_view)(void *, SputnikValue, const char **, std::size_t *) = nullptr;
  int (*bytes_view)(void *, SputnikValue, const std::uint8_t **,
                    std::size_t *) = nullptr;
  std::size_t (*list_len)(void *, SputnikValue) = nullptr;
  SputnikValue (*list_at)(void *, SputnikValue, std::size_t) = nullptr;
  int (*handle_ptr)(void *, SputnikValue, const char *, void **) = nullptr;

  SputnikValue (*make_null)(void *) = nullptr;
  SputnikValue (*make_bool)(void *, int) = nullptr;
  SputnikValue (*make_int)(void *, std::int64_t) = nullptr;
  SputnikValue (*make_float)(void *, double) = nullptr;
  SputnikValue (*make_str)(void *, const char *, std::size_t) = nullptr;
  SputnikValue (*make_bytes)(void *, const std::uint8_t *, std::size_t) = nullptr;
  SputnikValue (*make_list)(void *, const SputnikValue *, std::size_t) = nullptr;
  SputnikValue (*make_handle)(void *, const char *, void *) = nullptr;

  SputnikStatus (*fault)(void *, const char *, const char *) = nullptr;
  SputnikStatus (*call_block)(void *, SputnikValue, const SputnikValue *,
                            std::size_t, SputnikValue *) = nullptr;
};

struct SputnikExtDirectCallOutcome {
  SputnikStatus status = SPUTNIK_ERR;
  SputnikValue value = nullptr;
};

// Stack-construct an SputnikCtx backed by `ops`, invoke a linked C thunk, and
// return its opaque result handle. The generated owner resolves that handle
// from its NativeValue arena before the arena goes out of scope.
SputnikExtDirectCallOutcome
sputnik_ext_invoke_direct_free(const SputnikExtDirectOps &ops, void *state,
                             SputnikFreeFn fn, const SputnikValue *args,
                             std::size_t argc);
SputnikExtDirectCallOutcome
sputnik_ext_invoke_direct_method(const SputnikExtDirectOps &ops, void *state,
                               SputnikMethodFn fn, SputnikValue self,
                               const SputnikValue *args, std::size_t argc);

// Deterministic teardown needs a live ABI context, but a destructor has a
// different signature from a method thunk. Preserve the handle tombstone.
bool sputnik_ext_destroy_direct(const SputnikExtDirectOps &ops, void *state,
                              RuntimeForeignHandle &handle);

// Build an SputnikCtx over (host, frame, tags), marshal `args` (and `self` for a
// method) into the per-call arena, invoke the thunk, and marshal the result
// back. The frame is type-erased to `const void *` exactly as in StdlibHost.
NativeExtCallOutcome sputnik_ext_invoke_free(StdlibHost &host, const void *frame,
                                           const NativeTagRegistry &tags,
                                           SputnikFreeFn fn,
                                           const std::vector<Value> &args);
NativeExtCallOutcome
sputnik_ext_invoke_method(StdlibHost &host, const void *frame,
                        const NativeTagRegistry &tags, SputnikMethodFn fn,
                        const Value &self, const std::vector<Value> &args,
                        const SputnikLeafDescriptor *leaf = nullptr);

// Direct SputnikCtx surface, used by the bridge helpers above and by unit tests
// that exercise individual sputnik_ext.h functions. `import` pushes a runtime
// Value into the arena and returns its opaque handle; `export` reads a handle
// back out. Handles are valid until `sputnik_ext_ctx_close`.
SputnikCtx *sputnik_ext_ctx_open(StdlibHost &host, const void *frame,
                             const NativeTagRegistry &tags);
void sputnik_ext_ctx_close(SputnikCtx *ctx);
SputnikValue sputnik_ext_ctx_import(SputnikCtx *ctx, Value value);
Value sputnik_ext_ctx_export(SputnikCtx *ctx, SputnikValue handle);

} // namespace sputnik::runtime
