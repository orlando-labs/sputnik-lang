# External Sputnik templates

Implemented 2026-10-26. A `.sv` file is a delimiter-free string-tag body;
its callable signature, imports and public name are declared in ordinary Sputnik.
The compiler loads inputs before macro expansion. Runtime rendering never opens
source files.

## Declaration and registration

```sputnik
package app.views.posts
export index

template "./posts/index.html.sv"
def index(posts, title: "Posts")
```

The annotation directly precedes a declaration-only module function. It accepts
one nonempty literal path, resolved relative to the declaring `.s` file, and
preserves the function's ordinary signature and export. For example, call
`index(posts, title: "Latest")`: positional parameters and keyword parameters
retain the ordinary language distinction. Types, defaults, variadic parameters
and argument validation use the normal function machinery. Several declarations
can share one `.s` module or use the same template body. Class methods, native
functions, macros, existing bodies and unattached annotations are rejected.
An ordinary annotation immediately above `template` remains attached to the
completed declaration.

`template` is a compiler-provided annotation using the ordinary sigil-less call
surface described in the macro-system design, not a decorator syntax. `@name`
and `@@name` keep their instance/class variable meaning, including `@template`.
An explicit module binding/import named `template` shadows this compiler-provided binding;
function-local calls and member access are ordinary expressions. Source loading
belongs to the host's input preparation pass, so the macro VM gains no IO access.

```yaml
schema: sputnik.build.v1
name: app
root: app
profiles:
  required: [core.v1, macro.v1]
source_formats:
  - suffix: .html.sv
    module: orbit.html
    macro: html
  - suffix: .cmd.sv
    module: system
    macro: cmd
  - suffix: .sql.sv
    module: sqlite3
    macro: sql
modules:
  - name: app
    path: src/app.s
  # Add local source provider modules/dependencies as usual.
```

JSON accepts the same `source_formats` array. Suffixes are matched against the
complete filename; the longest matching suffix wins. Duplicate suffix
registrations are errors, including identical duplicates. Providers must export
a `string_tag macro`; ordinary functions and private macros are rejected.
`system.cmd` is a builtin provider. Providers may also come from locally
installed source packages; manifest dependency versions participate in selection.
Registrations are explicit project build configuration. A bare `sputnik run
file.s` has no source-format registrations; use `run`/`build` with the manifest.
There is no runtime/global registry or implicit provider discovery by suffix.

## Body and lexical inputs

`posts/index.html.sv`:

```sputnik-view
<h1>#{title}</h1>
<ul>
%for post in posts:
  <li>#{post.title}</li>
</ul>
```

The entire file is the template body. It needs no opener, closing delimiter,
package statement or export declaration. Literal double/triple quotes are text.
File indentation, blank lines, trailing spaces and the presence/absence of a
final newline are preserved. Escape sequences and interpolation use the ordinary
string parts parser; there is no delimiter-relative dedent or implicit newline.
Template `%for`, `%#` and `%%` use the existing
[loop contract](string-template-loops-v1.md). Only the structural extra indent of
loop bodies is removed.

Names resolve in the declared function: parameters, imports from the declaring
module, ordinary prelude, and nested loop bindings. Unknown names fail compilation
at their external source occurrence. Context such as request/session/current user
is passed explicitly. External bodies are compiled only through a declaration;
they do not independently declare function parameters or a second export.

HTML retains contextual escaping and typed trust wrappers; SQL returns one
parameterized fragment; cmd returns one command with opaque arguments. Command
construction does not execute a process. Providers control the result type.

## Compiler and provider API

`frontend/templates/templates.h` exposes a host-owned `SourceLoader` callback,
`SourceFormat {suffix, module, macro}`, and `prepare`. This pass retains the
function declaration and inserts its expression body plus static provider
imports. Its result includes original input texts, provider uses, and cache
material. The macro expander receives those texts so `Ast.source` and nested
macro arguments refer to the external source, including exact source slices.
The lexer/parser share string-part logic with inline strings and track external
line/column/byte positions, including Unicode and multiline interpolants.

Provider macros can emit `AstQualifiedRef {module, name}` to reference their
runtime exports independently of the caller's imports or shadowing. It lowers
to the ordinary module constant path and uses normal graph linking; it is not a
runtime name search. Orbit HTML and SQLite SQL use these references for their
runtime helpers. A provider module import ensures graph dependency registration.

## Build inputs, errors and delivery

Each module's build cache key includes source-format configuration, external
paths/content digests, and staged provider macro material. Input changes alter
the module ABI digest as well. Inputs are loaded and validated before a cache
hit is accepted, so deletion/invalid syntax cannot silently reuse old output.
The build result lists `template_inputs` with canonical paths and source hashes;
bytecode records the corresponding `sputnik.build.template_inputs` metadata.
Bytecode and strict full-native executables embed compiled rendering code, and
need no `.sv` files at runtime.

Diagnostics use `TEMPLATE_PATH`, `TEMPLATE_DECLARATION`, `TEMPLATE_FORMAT`,
`TEMPLATE_INPUT`, `TEMPLATE_PROVIDER`, or the existing parser/binder/provider
codes. Declaration/path errors point to `.s`; body errors and runtime rendering
frames point to `.sv`. Missing inputs report both the declaration and requested
path through the diagnostic message.

The v1 registry is a build-manifest facility, not a package-manifest extension.
Applications include registrations explicitly; source-distributed view libraries
must arrange the same registrations and files in their host build. Bytecode
view libraries carry the compiled body. No filesystem access was added to macro
execution.

## Validation and editing

- `make test-external-templates`: VM/native parity, input signatures, source
  layout/quotes/Unicode, `Ast.source`, longest suffix matching, cache invalidation,
  source deletion, invalid declarations/providers, and external diagnostics.
- `../orbit/test/external_templates_selftest.build.yaml`: HTML/SQL/cmd together,
  escaping/trusted values and strict native coverage with local source providers.
- Parser/lexer/build, macro VM and persistent notebook regression suites.

VS Code and Vim recognize `.html.sv`, `.cmd.sv`, `.sql.sv` and `.text.sv`, with
embedded Sputnik expressions and control headers. Custom suffixes can be mapped
to the Sputnik View language/filetype. Bare `.sv` remains available for
SystemVerilog. `template` is highlighted in `.s` declarations and notebook code.
