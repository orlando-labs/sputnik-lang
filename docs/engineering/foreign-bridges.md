# Foreign runtime bridge packages

`pycall/` and `rbcall/` are independent sibling packages. Each exposes Sputnik
source over native C ABI leaves and is configured against an installed runtime.
The compiler itself links neither CPython nor CRuby. Both packages can be installed
with `sputnik pkg install` and loaded into one VM or full-native application.

## Imports

The parser's existing dotted import/alias grammar is unchanged. HIR recognizes
`pycall.<module>` and `rbcall.<require-path>`. The dependency is the bridge package
(`pycall` or `rbcall`), represented by `HImportForeign`; foreign names never become
Sputnik module dependencies or static export lookups.

Module initialization fills the original immutable binder slots by calling
`pycall.import_module` / `pycall.import_name` / `rbcall.import_name`. Initializers
run before user closures capture their slots and do not update `$_`. Ordinary
Sputnik import metadata, readonly checks and package resolution remain intact.
A plain `import rbcall.json` is a binder error: Ruby require paths do not identify
namespace values. The supported form is `from rbcall.json import JSON as json`.

## Values and ownership

Public proxies use `method_missing`, callable-object dispatch, indexing and
explicit conversions. Private native handles use collected ownership. The ABI
carries tagged list packets rather than assuming maps/kwargs are ABI values;
Sputnik wrappers pack dictionaries and keyword arguments separately. Foreign
containers retain identity and copy only when explicitly converted.

CPython calls attach/acquire the GIL on Sputnik's FFI thread. CRuby has a dedicated
carrier and a GC-rooted object-id table; its queue contains owned data only.
Ruby yield requests return to the invoking Sputnik FFI thread, which executes
the closure while the Ruby carrier services reentrant jobs. No Sputnik frame
pointer or call-scoped SputnikValue is retained by a foreign runtime.

Sputnik module registries root captured callbacks. Foreign callable destruction
queues registry ids for removal on subsequent bridge entries. Foreign-handle
reclaim queues releases instead of running Python/Ruby destructors on collector
threads. Runtime/trampoline libraries remain process-lived. Calls originating on
autonomous foreign threads are explicitly rejected.

## Ruby block control

`BREAK_FOREIGN` (0x39) is a dedicated terminal instruction reading one result
register. `kCodeFlagRubyBreakBlock` marks a block requiring a Ruby invocation;
this includes breaks inside protected regions. Explicit Sputnik loops still emit
local jumps. Known literal collection receivers diagnose foreign break during
compilation; dynamic Sputnik iterators reject it at block entry.

`sputnik_call_ruby_block` supplies a dynamic Ruby callback scope. VM execution
uses the existing pending non-local control/ensure machinery with a reserved null
return target. The direct native backend uses the equivalent pending control
signal and runs native ensure handlers. At the ABI callback boundary this becomes
`SPUTNIK_BREAK` with a raw result value; rbcall encodes it and calls Ruby's
`rb_iter_break_value`. Ruby Proc data records the original invocation lifetime;
a later invocation cannot adopt an expired break target. Ordinary exceptions and
non-local return remain errors/pending control, allowing Ruby ensure to run before
Sputnik resumes unwinding. No C++ exception crosses a C runtime boundary.

`sputnik_block_required_args` exposes the minimum positional arity of a Sputnik
block, or -1 for other callables. The Ruby adapter uses this to unpack single-array
yields for multi-parameter blocks and fill missing parameters with Null. Ordinary
Sputnik block binding is unchanged. Keyword yields and rebinding Sputnik self for
Ruby instance_eval DSLs are not implemented.

## Native parity and tests

The native backend now supports ordinary method_missing fallback, including
positional arguments, kwargs and the block channel, and matches registry-backed
native errors against imported Sputnik NativeError classes.

Configure the sibling packages, then run `make test-foreign-bridges`. Each package
runner checks VM and strict full native and executes the generated binary. The
installed-package integration checks both interpreters in one process, including
Ruby-to-Sputnik-to-Python and Python-to-Sputnik-to-Ruby calls. NumPy integration is
optional via `python3 ../pycall/test/run.py --numpy`. Existing emitter, bytecode,
ABI, graph linker and VM suites also cover adjacent behavior.
