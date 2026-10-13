# Foreign result storage

Fresh C ABI handle results use one shared allocation for the
`RuntimeForeignHandle`, its shared ownership control block and its tagged VM tail box
or native wrapper. The extension's resource remains a separate allocation.
Importing an existing runtime handle still creates a wrapper around the same
resource; it must preserve identity and the `destroy!` tombstone.
The alternative variant Value representation holds the shared resource directly
and needs no tail box.

The embedded wrapper holds a `shared_ptr` alias to the resource in its own
allocation. Its intrusive reference count tracks language Values. On the final
Value release, the wrapper moves that alias into a local owner instead of
deleting the embedded wrapper. Releasing the local owner destroys the allocation
unless a runtime bridge has exported another shared alias. Such an alias can
outlive all original Values and can be imported again. The embedded wrapper then
has zero intrusive references and no owning alias; only the exported resource's
shared ownership keeps the allocation alive.

The resource precedes the wrapper in the storage object, so normal destruction
releases the wrapper before reclaiming the resource. Collected resources reclaim
once on the final shared release. Explicit destruction first sets the shared
tombstone, preventing a second reclaim. Borrowed and owned resources retain
their existing lifetime rules.

Handle construction prepares the tag, destructor/reclaim closure, native class
identity and arena slot while the handle is still borrowed with a null pointer.
Only after every potentially throwing operation succeeds does the host assign
the resource pointer and ownership. Failure therefore leaves ownership with
the extension. An unknown tag returns a null opaque handle and records a fault;
it does not return a nonzero arena handle containing a Null Value.

`sputnik_ext_tests` covers Value copies, exported aliases outliving their
original wrapper, reimport, deterministic destruction and arena closure.
`test-native-call-buffers` covers nested callbacks, arena growth, collected
result release and shared tombstones in VM and full native execution.
