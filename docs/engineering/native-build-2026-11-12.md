# Small native builds — 2026-11-12

The reproducer is `print("Ohai")` in a `.spu` file. Measurements below use
Apple Clang 21.0.0, macOS arm64, and the existing optimized C++ backend (`-O2`).
They include the entire `sputnik build` process, not just the final compiler
invocation. The generated program prints exactly `Ohai\n` and has full native
coverage, without bytecode execution or an interpreter dependency.

| Build | Wall time | Executable bytes |
|---|---:|---:|
| Before, empty runtime cache | 45.280 s | 291,208 |
| Before, warm runtime cache | 2.935 s | 291,208 |
| After, empty runtime cache with bundled runtime | 1.793–1.824 s (3 runs) | 199,032 |

The median after the change is 1.817 s: about 25× faster than the original cold
build and 38% faster than the original warm build. The executable is 31.7%
smaller. Runtime preparation takes 38–39 ms; C++ compilation/linking still takes
1.74–1.77 s. These are local measurements, not portable performance thresholds.

## Causes and changes

- Every first build compiled 58 runtime translation units, including the VM,
  networking and package machinery. `make` now assembles a companion runtime
  archive from the existing interpreter objects. A content/toolchain/ABI key
  controls reuse. Different compilers or changed runtime sources still produce
  a cached archive; their first build remains more expensive. A forced cache
  miss with `/usr/bin/clang++` took 42.48 s, emitted no warnings, and removed its
  temporary object directory after creating the cached archive.
- The old runtime-archive error path retried all runtime sources as part of the
  final link, potentially doubling failed compilation work. Failures now stop
  with their compiler diagnostics. Successful internal runtime builds are quiet.
- Known `print` values went through the general callable dispatcher. The existing
  register-flow analysis now tracks that prelude value through moves and joins,
  allowing a direct call to the same print implementation. Unknown callees,
  captured locals, handlers and pattern-binding code retain general dispatch.
  This removes otherwise reachable constructor and ArgParser code. It does not
  recognize or replace the whole source program as a special case.
- Shutdown formerly constructed the task scheduler even in programs without
  tasks. Shutdown is now registered when the scheduler is actually used.
  Registration is thread-safe and shutdown still happens before the root task
  scope is destroyed. Unused dynamic libraries are also removed on macOS when
  there are no native extensions requiring their initialization.
- Ordinary source and manifest builds retain only their binary. C++, extension
  objects, LLVM intermediates, symbol dumps and reports live in owned temporary
  directories. Configured source builds no longer write the interpreter's local
  `.sputnik/vm` cache. Publication happens after linking and symbol checks and
  preserves an existing binary on compiler failure.

`emit-cpp <source|manifest> [-o program.cpp]` exports C++ without a C++ toolchain.
`build --target native-debug` deliberately retains intermediate files for backend
investigation. `--target both` explicitly retains manifest bytecode outputs.
Existing tests that inspect C++ select `native-debug`.

## Remaining cost

The generated translation unit is still roughly 600 KB even for this program.
Clang parses the shared native helpers and eliminates unused functions; the
remaining executable includes generic value storage/display, text output and
error handling. Making native binaries comparable to a tiny C `puts` executable
would require finer runtime/helper separation. The measurements above do not
claim that this remaining fixed cost has been removed.

The focused CLI regression covers clean outputs, configured entries, manifests,
C++ export without a compiler, bytecode opt-in, native C extensions, atomic
failure handling, concurrent builds, and VM/native parity for known and dynamic
print calls, capture mutation, errors and task execution. Scalar safepoint and
bare-namespace native regression suites exercise numeric/capture/exception and
shadowing behavior independently.

Raw measurements and compiler logs are in the ignored
`build/native-build-investigation/` directory.
