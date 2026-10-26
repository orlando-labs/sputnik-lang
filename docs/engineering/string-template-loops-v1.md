# Runtime loops in string-tag templates

Implemented in the local SDK and the Orbit HTML / SQLite SQL providers on
2026-10-26. This is runtime template iteration, distinct from `%for` inside a
macro definition, which iterates during macro expansion.

## Surface and evaluation

```sputnik
from orbit.html import html, escape

def list(items):
  html"""
    <ul>
    %for item with_index index in items:
      <li data-index="#{index}">#{item.name}</li>
    </ul>
    """
```

Only multiline tagged templates recognize control lines. After the normal block
string dedent, a literal leading `%for` opens an indented template region. Its
header uses the ordinary one-line Sputnik `for` grammar: patterns, Map key/value
arguments and `with_index` work as in normal code. Write the iterable expression
directly, without `#{...}`; interpolation inside an ordinary quoted header
expression is supported. The expression is evaluated once per entry into the
loop, and iteration uses the ordinary `for_each` protocol. It is not expanded or
materialized at compile time. Nested loops retain lexical block scope.

The directive line emits nothing, including its newline. Dedent closes the
body. The body's extra indentation relative to the directive is stripped from
each output line; remaining whitespace and line endings are preserved. Empty
iteration emits nothing. A `%#` comment line emits nothing. `%%` at the first
non-whitespace position emits one literal `%`. Escaped characters and runtime
interpolants are data: they cannot introduce a directive or structural indent.
The parser rejects missing bodies, malformed headers, inconsistent indentation
and nesting beyond 64 levels with `STRING_TEMPLATE_CONTROL` and a source location.
Other directives, including `%if` and `%while`, are not implemented in this
version. Ordinary untagged strings retain literal percent lines.

## Contextual providers

Looping repeats the template's structure, while its tag retains the meaning of
interpolation:

- Orbit `html` escapes text/ordinary quoted attribute values and preserves typed
  `Html` fragments. URL, script and style interpolations retain their existing
  typed trust requirements. A loop must start and finish in HTML text context;
  loops within tag syntax, attributes, comments, script or style, and bodies
  leaving such a context open fail with `ORBIT_HTML_CONTROL_CONTEXT`. This is a
  lexical output-context check, not DOM validation.
- SQLite `sql` appends static SQL and adds a separate `?`/bound value for each
  evaluated interpolant in traversal order. It returns one `SqlFragment` and
  executes no query. The caller supplies separators and valid SQL structure;
  there is no implicit list expansion or per-iteration execution.
- `system.cmd` appends static command chunks and opaque argument values, then
  constructs one `Command`. It does not run a shell or execute each iteration.
  A loop starts and ends at an argument boundary, with quotes/escapes closed;
  violations fail with `CMD_TEMPLATE_CONTROL`. Values containing spaces, quotes,
  shell-looking text or directive-looking text remain argument data.

## Provider AST contract

F1.5 re-kinds a tagged literal as `AstStringTemplate`. Existing text, escape and
expression parts remain unchanged for templates without control lines. Structured
parts add:

```text
AstStringFor {
  iteration: <ordinary for_each AST with the parsed header and a dummy body>,
  parts: [AstStringText | AstStringEscape | AstStringExpr | AstStringFor, ...]
}
```

A provider recursively transforms leaves into expressions of its own chunk
representation, preserving `iteration` and nested loop structure. It returns:

```text
AstTemplateRender {
  parts: [<runtime chunk expression> | AstStringFor, ...],
  finish: <callee expression accepting the completed chunk Array>
}
```

The common expander lowers this to ordinary assignment, `for_each`, append and
finalizer calls before binding. The accumulator has a fresh hygiene context;
provider/interpolant ASTs retain their own context. Internal append/assignment
steps preserve `$_`; only the completed source expression updates it. No hidden callable is added,
so caller control flow keeps the ordinary language semantics. Providers must
handle or explicitly reject `AstStringFor`; treating it as a static text part is
invalid. The current renderer buffers chunks in an Array, not a streaming sink.
There is no built-in `text` tag; the test fixture demonstrates a text provider.

## Validation

- `make test-string-template-loops`: generic runtime/native parity, patterns,
  indices, nesting, empty sources, once-only evaluation, hygiene, escapes,
  malformed syntax, and provider context errors.
- `python3 tests/system_backend_test.py build/sputnik`: process behavior and
  full-native / one-worker async regression tests.
- `build/isputnik_tests`: persistent notebook command templates and image rebuilds.
- `../orbit/test/html_loop_selftest.build.yaml` and
  `../sqlite3/test/template_loops_selftest.build.yaml`: provider integration with
  actual HTML values and SQLite execution, in VM and strict full-native builds.
