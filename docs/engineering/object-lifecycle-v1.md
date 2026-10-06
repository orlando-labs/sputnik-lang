# Object construction and instance field snapshots

Ordinary Amber object construction invokes the most-derived `init` selected by
normal inherited method lookup. After that invocation returns successfully,
the constructor invokes `after_init!` with no arguments or block, if such an
instance method exists. The hook uses normal inherited method lookup and its
return value is ignored. Construction returns the original instance only after
the hook succeeds. This applies to constructor calls and the default `.new`
path, including classes without `init`, auto-assigned parameters, defaults,
early returns and clause constructors.

The hook runs once per construction, not once per ancestor. Overriding `init`
does not suppress an inherited hook. Overriding `after_init!` replaces the
inherited hook by ordinary method dispatch. Explicit sends to `init` and
`init_copy` do not trigger it. A failed initializer skips the hook; a failed
hook fails construction and is catchable at the original constructor call site.
Runtime-native values and foreign-handle constructors are outside this protocol.
The VM keeps the continuation on the constructor activation so ordinary method
execution, including cooperative suspension, retains its usual behavior.

`obj.instance_fields` (also callable as `obj.instance_fields()`) returns a fresh
ordinary Map with Str keys naming the object's actual assigned instance fields
without `@`, in lexicographic name order. Fields assigned by ancestor methods and
fields containing `null` are included. Class variables, properties and unassigned
fields are excluded. Enumerating never invokes getters. This is a shallow
snapshot: modifying the map does not write to the instance, and subsequent field
assignment does not change the snapshot, but referenced mutable values remain
shared. Normal lifecycle/access checks apply. VM reads of watched fields record
dependencies just as individual field reads do.

Both facilities are runtime object protocols; they introduce no source syntax
or bytecode instruction. VM and full native execution provide the same hook
dispatch, sorted snapshot and exception semantics. A user-defined
`instance_fields` member takes precedence over the builtin fallback.

The shared regression source is
`corpus/run/instance_fields_after_init/source.am`. The VM suite executes its
`probe`; the native build manifest is
`tests/fixtures/instance_fields_after_init/amber.build.json`. Run both execution
lanes with `make test-object-lifecycle`; the normal `make test` includes it.
The VM suite also verifies cooperative suspension in both the initializer and
the hook before construction returns.
