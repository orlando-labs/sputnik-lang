# Typed resource leaf ABI v2

The optional `SPUTNIK_LEAF_HANDLE_INT_HANDLE` signature is a synchronous method
on a foreign handle. It accepts one signed 64-bit Integer and returns a fresh
resource of the receiver's collected type:

```c
SputnikStatus relu_leaf(const void *receiver, int64_t mode, void **out,
                        const SputnikLeafFault *fault);
const SputnikLeafDescriptor *example_relu_sputnik_leaf_v2(void);
```

The sidecar returns an immutable, image-lifetime descriptor with
`abi_version = SPUTNIK_LEAF_ABI_VERSION_2`, exact `struct_size`,
`signature = SPUTNIK_LEAF_HANDLE_INT_HANDLE`, the receiver's tag and
`handle_int_handle = relu_leaf`. The union containing the entry pointers keeps
the v1 descriptor's size and offsets. VM and generated native hosts prefer the
v2 sidecar, then try v1 when no v2 descriptor is supplied. Older hosts ignore
the v2 export and call the ordinary `SputnikMethodFn`, which must remain.
Malformed descriptors are rejected rather than silently ignored.

Method resolution and language argument shaping precede the boundary. For one
Integer argument, the host validates the receiver's tag and shared tombstone,
and requires a collected type with a reclaim function. It passes the raw
receiver pointer and `int64_t` directly. Other call shapes use the ordinary ABI
and its existing diagnostics. The leaf must obey the v1 effect rules: no pointer
retention, runtime reentry, callbacks, suspension, blocking waits or C++
exceptions crossing the boundary. A `blocking` declaration cannot use a leaf.

The call remains synchronous. CPU computation and GPU kernel submission occupy
the caller until return; the ABI promises no latency bound or CPU speedup.
Explicit I/O and device synchronization use ordinary blocking bindings. Backend
parallel work or allocator activity can delay a compute call without suspending
the Sputnik task.

Before calling the leaf, the host prepares a result wrapper with a null pointer
and borrowed ownership. A successful call must return a nonnull, fresh resource.
The host then adopts it with the type's collected reclaim function. The result
uses the coallocated storage described in [foreign result storage](foreign-result-storage.md).
There is no `SputnikCtx`, argument/result arena, opaque `SputnikValue` handle,
value-reader callback or result-builder callback on the successful path.

The fault callback copies diagnostics without unwinding the Sputnik caller.
After the leaf returns, the host reclaims any unadopted output before raising
the recorded error. A fault takes precedence over `SPUTNIK_OK`. An error without
diagnostics or a successful null result becomes `NativeError`. Final Value
release and explicit `destroy!` retain the same shared lifetime rules as an
ordinary C ABI result. Borrowing a receiver adds no concurrent-destroy guarantee.

`sputnik_ext_tests` verifies full-width signed arguments, aliases and final
release, incompatible call shapes, tag/tombstone/ownership guards, fault-before-
unwind ordering, outputs returned on failure and missing output diagnostics.
`test-native-call-buffers` proves both VM dynamic discovery and native static
discovery with a diagnostic thunk that fails if the typed entry is not selected.
Its `compat.build.json` fixture can run on a v1 host with the current SDK header
to verify that an ignored v2 sidecar still has a working ordinary thunk.
