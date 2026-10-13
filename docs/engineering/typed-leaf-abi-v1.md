# Typed leaf ABI v1

The resource-returning signature is documented in [typed leaf ABI v2](typed-leaf-abi-v2.md).

An extension can opt in to a typed leaf entry without changing its ordinary
`SputnikFreeFn` / `SputnikMethodFn` thunk or the manifest. For a physical C
symbol `example_read`, export an additional C function:

```c
const SputnikLeafDescriptor *example_read_sputnik_leaf_v1(void);
```

It returns an immutable descriptor that remains valid for the lifetime of the
loaded image. The host validates its ABI version, exact structure size,
signature, receiver tag and entry pointer before registration. A malformed
descriptor or a leaf on a manifest-declared blocking symbol is rejected.
VM libraries discover the optional export with `dlsym`; generated native
binaries supply weak null defaults overridden by the package's strong export
on GNU-compatible toolchains. Other toolchains keep the ordinary ABI. Older
hosts ignore the extra export.

The first signature is `SPUTNIK_LEAF_HANDLE_INT`: a nullary method on a foreign
handle returning a signed 64-bit Integer:

```c
SputnikStatus read_leaf(const void *receiver, int64_t *out,
                        const SputnikLeafFault *fault);
```

The host checks the foreign receiver's tag and tombstone, borrows its pointer
for this synchronous call, and converts the raw result to its own immediate
Integer. A successful call creates no `SputnikCtx`, argument/result arena or
`SputnikValue` handles and invokes no value-reader/builder callbacks. Calls
with positional arguments and free-function bindings retain the ordinary ABI.
Language method resolution, overrides, property syntax and argument shaping
still run before this boundary; this ABI does not eliminate those costs.

A leaf must not retain the receiver pointer, suspend/block, reenter Sputnik,
invoke a user callback, or propagate a C++ exception across the C boundary.
It can report a rescuable failure through
`sputnik_leaf_fault(fault, error_class, message)`. The host consumes both strings
synchronously, so exception message storage remains valid during reporting.
This callback runs only on failure. `SPUTNIK_ERR` without a reported fault becomes
`RuntimeError`; a reported fault takes precedence even if the entry mistakenly
returns `SPUTNIK_OK`. Error traces use the enclosing ordinary call boundary.

The receiver remains owned by the synchronous caller. Borrowing obeys the same
foreign-handle lifetime rules as `sputnik_handle_ptr`; it does not add a new
concurrent-destroy guarantee. Foreign extensions must honor the declared leaf
effects just as they honor the ordinary C ABI's borrowed-view contract.

`native_call_buffers` tests both dynamic VM discovery and static native
discovery, successful Int return, rescuable failure/recovery and collected
receiver lifetime. `sputnik_ext_tests` additionally checks full-width signed
results, non-nullary fallback, wrong tags, tombstones, descriptor rejection,
blocking rejection, missing diagnostics and stale-descriptor removal.
