# System

`system` starts subprocesses on macOS and Linux. Commands contain an executable
and an argument vector; constructing one has no process side effects.

```amber
import system
from system import cmd

help = cmd"bin/run --help".output(timeout: 30.0)
amber_code = "print(42)"
result = cmd'bin/iamber --eval "#{amber_code}"'.capture()
```

In an interactive notebook, start `build/iamber --grant process.spawn` to
authorize child processes (or scope the grant to an executable, for example
`--grant process.spawn=/usr/bin/printf`). Native stdlib imports work in the cell:

```amber
import system
system.cmd"/usr/bin/printf hello".output
```

The same leading `--grant` options apply to `--project ... --run-sheet`.
Notebook image rebuilds retain the session's grants. Without a grant, process
creation reports `CapabilityError`. Restart an already running `iamber` to use
the rebuilt executable and pass the desired grants.

Call-site blocks, including threaded chains and nested blocks that capture
values from preceding cells, work in persistent notebook cells:

```amber
import system
command = system.cmd"/usr/bin/printf output"
10_000.times.threaded(500).map: command.output .group: $it .transform_values: $it.size
print $_
```

The threaded collection example runs in `iamber` and `amberc run`. The native
compiler currently rejects the general collection selector `threaded` under
`--require-full-native`; use `task.async` for native subprocess concurrency.

## Commands and string tags

`system.command(program, *arguments, cwd:, env:, clear_env:, group:)` constructs
a reusable command. Every argument must be a string. `cwd` applies only to the
child. `env` overlays the inherited environment; a null value removes a variable.
`clear_env: true` starts from an empty environment. Executable lookup uses the
child's `PATH` and working directory. `group: "new"` creates a process group;
the default is `"inherit"`. `.with(...)` returns a copy with updated options.
`.argv`, `.program`, and `.cwd` expose the command configuration.

`from system import cmd` imports a string-tag macro. An alias is supported:

```amber
from system import cmd as system
command = system'printf "%s" #{"hello world"}'
```

Static command text understands whitespace, single/double quotes, and escapes.
Interpolated strings are inserted into the current argument verbatim: quotes,
spaces, semicolons, and `$()` inside a value cannot become command syntax.
An empty interpolant still contributes an argument. Adjacent static text and
interpolants concatenate into one argument. Interpolants are evaluated once.
Unquoted shell operators, variable expansion, and globs in static text are
rejected. Argument-array interpolation is not supported; use
`system.command(program, *arguments)` for a dynamic argument list.

Use `system.shell(script, executable: "/bin/sh", args: [...], ...)` explicitly
for shell syntax. Additional arguments are available as `$1`, `$2`, etc.; `$0`
is `amber-shell`. Command options also apply to shell commands.

Both `tag"..."` and `tag'...'` work for **all** `string_tag macro def` macros.
They require adjacency between the tag and the opening quote. Both forms support
`#{...}` interpolation; the single-quoted form allows `"` without escaping.
An ordinary untagged single-quoted string remains non-interpolating.
The macro receives `Ast.StringTemplate` with `quote_kind` `"double"` or `"single"`.
An immediately following method call operates on the expanded tag result.
Multiline `tag"""` remains supported with a newline after the opener;
`tag"""inline"""` is not a text-block form.

## Capture and multiblock

```amber
command = system.command("/bin/cat")
request_header = "header\n"
request_body = "body".bytes()

result = command.capture(timeout: 30.0) with:
  stdin |input|:
    input.write_all!(request_header.bytes())
    input.write_all!(request_body)
  stdout |output|:
    chunks = []
    output.each_chunk(size: 65536) |chunk|:
      chunks.push!(chunk)
    chunks
  stderr |errors|:
    errors.read_all!()
```

The named blocks run concurrently with pipe I/O. Each endpoint has one logical
reader or writer. Returning from a block closes its endpoint. Amber instructions
in handlers share a serialized execution context; blocking stream and task
operations release it so the other handlers can progress. Completion waits for
all handlers and reaps the child. Handler exceptions propagate after cleanup.

Without a `stdin` block, `input:` accepts `Str` or `Bytes` and closes stdin after
writing; without input it closes stdin immediately. `input:` and a `stdin`
block are mutually exclusive. Missing stdout/stderr handlers cause those streams
to be drained concurrently into `Bytes`.

`result.stdout` and `.stderr` contain automatically collected bytes, or `null`
for a stream handled by a block. `.stdin_value`, `.stdout_value`, and
`.stderr_value` contain the corresponding block return values. `record_input:
true` stores successfully written stdin bytes in `.stdin`; otherwise it is null.

`result.status` contains `.pid`, `.success?()`, `.exit_code`, `.signal`, and
`.signaled?()`. These properties are also available directly on the result.
An exit code is null for a signal termination, and a signal is null for a normal
exit.

Execution options:

| Option | Default | Meaning |
| --- | --- | --- |
| `timeout` | none | Deadline for the operation, including handlers |
| `kill_after` | `1.0` | Delay between TERM and KILL during cleanup |
| `drain_timeout` | `1.0` | Maximum pipe wait after the direct child exits |
| `check` | `true` | Raise `ProcessExitError` on unsuccessful exit |
| `limit` | 16777216 | Combined automatic stdout/stderr byte limit; also a separate stdin recording limit |
| `record_input` | `false` | Retain bytes sent to stdin |

Durations follow Amber task conventions: `Float` is seconds and `Int` is
milliseconds. Handlers process raw bytes. `read!(size: 65536)` returns a chunk
or null at EOF; `read_all!(size:, limit:)` collects bytes; `each_chunk(size:)`
yields until EOF. `write_all!(bytes)` and `write!(bytes)` write the entire input
and return its byte count. `close()` is idempotent. Streaming handlers control
their own accumulation; the capture limit bounds automatic collection.

## Simple execution and manual lifetime

`.output(...)` returns stdout as a UTF-8 string and preserves trailing newlines.
It accepts capture options except a stdout handler. Invalid UTF-8 raises
`ProcessDecodeError`; use `.capture()` for binary output.

`.run(timeout:, kill_after:, drain_timeout:, check:)` inherits stdout/stderr,
connects stdin to `/dev/null`, and returns a status.

`.spawn(timeout:, kill_after:, drain_timeout:)` returns a `Process` with piped
stdin/stdout/stderr. `.communicate(input:, check:, limit:, record_input:)`
supports the same named stream blocks as capture and may be called once.
It closes the streams and waits for exit. Set a subprocess deadline at spawn.

`.wait(timeout:)` waits only for exit, without draining pipes; waiting on a
process producing unread output can block. A wait timeout leaves the child
running. `.pid`, `.running?()`, `.stdin`, `.stdout`, `.stderr`, `.signal(number)`,
`.terminate()`, `.kill()`, and `.close()` provide manual control. Closing a live
process terminates and reaps it. A scoped spawn guarantees cleanup even when its
block raises:

```amber
cmd'printf hello'.spawn() |process|:
  process.communicate().stdout.to_str()
```

## Async, errors, and native compilation

```amber
work = task.async:
  cmd'printf hello'.capture(timeout: 5.0)
result = work.wait()
```

Subprocess waits release the Amber scheduler worker. Cancellation terminates and
reaps a scoped subprocess before the task finishes with `CancelledError`.
Native programs use the same POSIX process engine and native callbacks, without
a VM bridge or bytecode fallback. The current implementation preserves blocking
call stacks on host threads, including native async tasks in programs using
`system`; thread usage therefore grows with concurrency. It is not an fd-reactor
implementation. A deadline interrupts pipe/task waits, but cannot preempt an
arbitrary blocking extension callback.

Process errors are rescuable: `ProcessSpawnError`, `ProcessExitError`,
`ProcessTimeoutError`, `ProcessOutputLimitError`, `ProcessDrainTimeoutError`,
`ProcessClosedError`, and `ProcessDecodeError` derive from `ProcessError`/`IOError`.
Writing to a pipe closed by the child raises `BrokenPipeError`. Errors currently
carry a message, without a structured partial-result attachment.

Launches require `process.spawn` (optionally scoped to the resolved executable)
when capability enforcement is active. Explicit signals require `process.signal`.
Automatic cleanup is covered by launch ownership. Process I/O is not replayable.

```sh
build/amberc run example.am --grant process.spawn
build/amberc build example.am --target native --require-full-native \
  --grant process.spawn -o build/example
python3 tests/system_backend_test.py build/amberc
```

The backend test verifies tags, multiblock, async, errors, cancellation, denied
capabilities, and full native coverage; it also runs generated native async/error
programs with a single scheduler worker. Linux/macOS use POSIX spawn and pipes;
this change has been exercised on macOS. Windows, arbitrary stdio redirection,
automatic pipelines, and structured partial results are outside this API version.
`group: "new"` signals the group while its direct child is alive. A descendant
retaining a pipe after the direct child exits causes a drain timeout; it is not
an owned descendant-tree supervisor.
