# Module Import Aliases

User package namespaces are supported by the shared graph linker, VM, and
VM-independent native backend. Both `import a.b as mod` and `import a.b`
produce a read-only namespace value (the latter binds `b`). A namespace can
be stored in a collection, captured by a closure, or passed to a function.

Member access returns the original exported function closure or class object.
Calling a function through a namespace preserves captures, defaults, keyword
arguments, and blocks. Re-exports and renamed exports retain the provider's
identity. Private or missing members raise `NoMethodError`; assigning a member
raises `ReadOnlyPropertyError`.

The linker emits `sputnik.merged.module:` metadata for package namespaces and
`sputnik.merged.function:` metadata for exact module-qualified function
resolution. The latter includes private helpers for internal constant lookup,
but namespace member lookup uses only `sputnik.merged.export:` metadata.
This prevents equally named private helpers in separate modules from making
native lookup ambiguous. Multi-clause function metadata points to the actual
module closure's flattened body, rather than the method dispatch descriptor.
The graph linker cache ABI is version 4.

Run the VM/native regression with `make test-module-aliases`. Native builds
use `--require-full-native`, so a bytecode fallback cannot satisfy the test.
