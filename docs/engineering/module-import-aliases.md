# Module Import Aliases

User package namespaces are supported by the shared graph linker, VM, and
VM-independent native backend. Both `import a.b as mod` and `import a.b`
produce a read-only namespace value (the latter binds `b`). A namespace can
be stored in a collection, captured by a closure, or passed to a function.

`import provider as name` also declares a module-path replacement. Both short
and qualified names are accepted after `as`:

```sputnik
import my.orm as orm
import orm
from orm import Model

# Alternatively, retain an existing publisher-qualified import path:
import my.orm as upstream.orm
from upstream.orm import Model
```

The alias binds its final segment (`orm` in both examples). Repeated imports
that bind the same canonical module/export to the same local name are
idempotent. Different bindings still conflict. Renaming an exported member
with `from ... import Model as Record` remains a local member alias and does
not replace a module path.

The entry program's import-as declarations are collected before graph
resolution. They apply to `import`, `from`, re-exports, and imports inside
dependencies, including dependencies distributed as bytecode. A matching
submodule suffix is preserved (`orm.utils` resolves to `my.orm.utils`); the
longest matching path wins, and unrelated prefixes are unaffected. Chains are
resolved transitively. Conflicting targets and replacement cycles are errors.
The canonical provider is linked once, preserving shared state, function and
class identity, and a single initialization. Literal strings are unchanged.
Dependencies' own import aliases remain local to those modules; they do not
change the entry program's replacement table.

Scripts need no application manifest. A notebook collects declarations from
its code cells into the environment generation, so an import-as in one cell
also applies to imports in other cells and their dependencies. Editing the
declaration uses the existing transactional Apply/rebuild path: failed
preparation preserves the previous environment. No project dependency links
or additional notebook metadata are required. The existing notebook cell
profile still requires module member access or `from` for source namespaces.

Bare function member access invokes the exported function with no arguments;
`&module.function` obtains the original function closure without invoking it.
Class member access returns the exported class object. Calling a function
through a namespace preserves captures, defaults, keyword
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
The graph linker cache ABI is version 6; source build cache revision 6 records
the new `sputnik.import.redirect:` attributes independently of local slots.

Run the VM/native regression with `make test-module-aliases`. Native builds
use `--require-full-native`, so a bytecode fallback cannot satisfy the test.
`make test-import-replacements` covers manifest-free scripts, VM/native
execution, installed source/bytecode dependencies, submodules, re-exports,
notebook cells, identity, initialization and conflict diagnostics.
