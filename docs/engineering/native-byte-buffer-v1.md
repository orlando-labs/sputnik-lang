# Native ByteBuffer and map transform regressions

The cpp-bytecode-direct backend supports `io.ByteBuffer(capacity)`,
`io.ByteBuffer.new(capacity)`, and an imported `ByteBuffer.new(capacity)`.
The namespace's zero-argument `ByteBuffer` lookup returns the constructor type.
Constructor argument checks match the VM: capacity is a nonnegative Int.

`runtime/byte_buffer_native.inc` adapts RuntimeByteBuffer/RuntimeByteSlice
directly, sharing allocation, active ranges, ownership checks, and buffer
operations with the VM. It supports wrap, read/write operations, phase changes,
size/cursor inspection, slices, and byte snapshots without a RuntimeWorld or
bytecode dispatch. The native eligibility gate includes these selectors.

Map transforms must canonicalize name keys before collision detection.
Previously the VM's `transformed` and `transformed_keys` created String-key entries
without the canonical symbol ID used by ordinary map equality, so distinct
names with the default ID 0 collided. These paths now use canonical map entries.
The VM also retains StrictMap semantics across transformed, transformed_keys, and
transformed_values, matching the existing native backend.

The canonical pure names are `transformed`, `transformed_keys`, and
`transformed_values`. `transform!` changes keys and values together and returns
the receiver; `transform_keys!` / `transform_values!` change one side. Mutating
forms stage results from a snapshot before replacing entries, so a block failure
does not apply a partial transformation. Both lanes preserve strictness, alias
identity, collision order, and the receiver's frozen checks. Invalid pairs raise
rescuable TypeError; the old pure names are not retained as aliases.

Run `make test-native-byte-buffer` to compare VM output with strict native
executables. The test checks full coverage, VM independence, and the absence of
the VM stdlib bridge. Fixtures cover the short/explicit/imported constructors,
byte operations, slices, errors, task confinement, distinct dynamically produced
String keys, deliberate collisions, and StrictMap key identity.
