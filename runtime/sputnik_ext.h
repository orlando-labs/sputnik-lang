/*
 * sputnik_ext.h — stable C ABI for Sputnik native extension packages.
 *
 * This is the single contract a native extension package programs against. It
 * is plain C so an external author can compile their thunks without building
 * the Sputnik tree; in-tree, the runtime implements this same contract on top of
 * the StdlibHost facade (one marshalling implementation, two consumers — see
 * DESIGN-native-extension-packages-2026-06-20.md §6).
 *
 * Conventions:
 *  - `SputnikValue` is opaque; never inspect it directly, use the readers and
 *    builders below. Values handed to a thunk (self, args) and values produced
 *    by builders are owned by the runtime and remain valid for the duration of
 *    the call only. Do not retain an `SputnikValue` past the thunk's return.
 *  - Borrowed views (`sputnik_str_view`, `sputnik_bytes_view`, `sputnik_handle_ptr`)
 *    point into runtime-owned storage and are valid for the duration of the
 *    call only. Copy out anything you need to keep.
 *  - A thunk reports success by returning SPUTNIK_OK with `*out` set, and failure
 *    by returning the result of `sputnik_fault(...)` (which records the fault on
 *    the active frame). On failure `*out` is ignored.
 */
#ifndef SPUTNIK_EXT_H
#define SPUTNIK_EXT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ABI version. The runtime rejects an extension whose
 * sputnik_ext_abi_version() does not match what the runtime was built with. */
#define SPUTNIK_EXT_ABI_VERSION 1u

/* Opaque runtime call context (the active frame + host facade). */
typedef struct SputnikCtx SputnikCtx;

/* Opaque Sputnik value handle, valid only for the duration of the current call. */
typedef struct SputnikValueObj *SputnikValue;

/* Thunk outcome. SPUTNIK_OK: `*out` holds the result. SPUTNIK_ERR: a fault has been
 * recorded via sputnik_fault (or a propagating exception is pending). */
typedef enum SputnikStatus {
  SPUTNIK_OK = 0,
  SPUTNIK_ERR = 1
} SputnikStatus;

/* ---- value predicates (1 = yes, 0 = no) -------------------------------- */
int sputnik_is_null(SputnikCtx *cx, SputnikValue value);
int sputnik_is_bool(SputnikCtx *cx, SputnikValue value);
int sputnik_is_int(SputnikCtx *cx, SputnikValue value);
int sputnik_is_float(SputnikCtx *cx, SputnikValue value);
int sputnik_is_str(SputnikCtx *cx, SputnikValue value);
int sputnik_is_bytes(SputnikCtx *cx, SputnikValue value);
int sputnik_is_list(SputnikCtx *cx, SputnikValue value);
int sputnik_is_handle(SputnikCtx *cx, SputnikValue value);

/* ---- readers (1 = success, 0 = wrong kind; no fault is set) ------------- */
int sputnik_as_bool(SputnikCtx *cx, SputnikValue value, int *out);
int sputnik_as_int(SputnikCtx *cx, SputnikValue value, int64_t *out);
int sputnik_as_float(SputnikCtx *cx, SputnikValue value, double *out);

/* Borrowed UTF-8 text of a Str value. Valid for the duration of the call. */
int sputnik_str_view(SputnikCtx *cx, SputnikValue value, const char **ptr,
                   size_t *len);

/* Borrowed byte view of a Bytes value (zero-copy). Valid for the duration of
 * the call. */
int sputnik_bytes_view(SputnikCtx *cx, SputnikValue value, const uint8_t **ptr,
                     size_t *len);

size_t sputnik_list_len(SputnikCtx *cx, SputnikValue value);
SputnikValue sputnik_list_at(SputnikCtx *cx, SputnikValue value, size_t index);

/* Recover the foreign pointer behind a `native class` handle. Verifies the
 * value is a handle tagged `tag` and is still live (tombstone check); on a tag
 * mismatch or use-after-destroy it records a fault and returns 0. */
int sputnik_handle_ptr(SputnikCtx *cx, SputnikValue value, const char *tag,
                     void **out);

/* ---- builders (return a value owned by the runtime for this call) ------- */
SputnikValue sputnik_make_null(SputnikCtx *cx);
SputnikValue sputnik_make_bool(SputnikCtx *cx, int value);
SputnikValue sputnik_make_int(SputnikCtx *cx, int64_t value);
SputnikValue sputnik_make_float(SputnikCtx *cx, double value);
SputnikValue sputnik_make_str(SputnikCtx *cx, const char *ptr, size_t len);
SputnikValue sputnik_make_bytes(SputnikCtx *cx, const uint8_t *ptr, size_t len);
SputnikValue sputnik_make_list(SputnikCtx *cx, const SputnikValue *items, size_t count);

/* Wrap a foreign pointer as a `native class` handle tagged `tag`. The
 * ownership/reclaim declared for `tag` in the package manifest governs teardown
 * (deterministic destroy! for owned/collected; GC reclaim only for collected). */
SputnikValue sputnik_make_handle(SputnikCtx *cx, const char *tag, void *ptr);

/* ---- faults and callbacks --------------------------------------------- */
/* Record a fault (mapped to the rescuable error class named `error_class`,
 * e.g. "TypeError", "LifetimeError") and return SPUTNIK_ERR for the thunk to
 * propagate. */
SputnikStatus sputnik_fault(SputnikCtx *cx, const char *error_class,
                        const char *message);

/* Invoke an Sputnik block value with `argc` arguments. On SPUTNIK_OK `*out` holds
 * the block result; on SPUTNIK_ERR a fault/exception is propagating. */
SputnikStatus sputnik_call_block(SputnikCtx *cx, SputnikValue block,
                             const SputnikValue *args, size_t argc,
                             SputnikValue *out);

/* True when the logical Sputnik task executing this thunk has been cancelled.
 * Blocking thunks should poll this at bounded intervals and return a
 * CancelledError fault instead of retaining an executor thread indefinitely. */
int sputnik_task_cancelled(SputnikCtx *cx);

/* ---- thunk and lifetime function signatures --------------------------- */
/* Free function / constructor: `(args) -> out`. */
typedef SputnikStatus (*SputnikFreeFn)(SputnikCtx *cx, const SputnikValue *args,
                                   size_t argc, SputnikValue *out);

/* Method on a handle: `(self, args) -> out`. */
typedef SputnikStatus (*SputnikMethodFn)(SputnikCtx *cx, SputnikValue self,
                                     const SputnikValue *args, size_t argc,
                                     SputnikValue *out);

/* Optional typed leaf ABI v1. A thunk may additionally export
 * `<physical_thunk_symbol>_sputnik_leaf_v1(void)` returning a pointer to an
 * immutable, image-lifetime SputnikLeafDescriptor. Old hosts ignore it.
 *
 * HANDLE_INT is a nullary method: the host validates receiver tag/liveness and
 * borrows its pointer for this synchronous call; the result is a raw int64_t.
 * A leaf must not retain that pointer, suspend/block, reenter Sputnik, invoke
 * user callbacks, or let a C++ exception cross the C boundary. Fault reporting
 * is the only permitted runtime call, on failure only. Keep the ordinary
 * thunk as the compatible fallback for hosts/call shapes without leaf support.
 */
#define SPUTNIK_LEAF_ABI_VERSION 1u
typedef struct SputnikLeafFault {
  void *state;
  SputnikStatus (*raise)(void *state, const char *error_class,
                         const char *message);
} SputnikLeafFault;

static inline SputnikStatus sputnik_leaf_fault(const SputnikLeafFault *fault,
                                               const char *error_class,
                                               const char *message) {
  return fault->raise(fault->state, error_class, message);
}

typedef SputnikStatus (*SputnikLeafHandleIntFn)(const void *receiver,
                                               int64_t *out,
                                               const SputnikLeafFault *fault);
typedef enum SputnikLeafSignature {
  SPUTNIK_LEAF_HANDLE_INT = 1
} SputnikLeafSignature;
typedef struct SputnikLeafDescriptor {
  uint32_t abi_version;
  uint32_t struct_size;
  SputnikLeafSignature signature;
  const char *receiver_tag;
  SputnikLeafHandleIntFn handle_int;
} SputnikLeafDescriptor;
typedef const SputnikLeafDescriptor *(*SputnikLeafDescriptorFn)(void);

/* Destructor for an `owned` handle: full runtime context permitted; invoked
 * only via deterministic destroy!/memory.dealloc, never by the GC. */
typedef void (*SputnikOwnedDestructor)(SputnikCtx *cx, void *handle);

/* Reclaim for a `collected` handle: context-free by construction so it cannot
 * reenter the runtime; may be invoked by deterministic destroy! OR by the GC. */
typedef void (*SputnikCollectedReclaim)(void *handle);

/* ---- load-time handshake ---------------------------------------------- */
/* Every extension translation unit must export this, returning
 * SPUTNIK_EXT_ABI_VERSION, so the runtime can reject an ABI mismatch at load. */
uint32_t sputnik_ext_abi_version(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SPUTNIK_EXT_H */
