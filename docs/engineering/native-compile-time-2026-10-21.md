# Native compilation investigation — 2026-10-21

Registry exposed a scaling problem in the generated-C++ backend: its entire
1928-code-object graph was optimized and compiled by one Clang process. LLVM 14
spent excessive time in inlining, InstCombine and SLP; lowering the optimization
level made deployment possible but produced a 133,394,904-byte executable.

The driver now supports partitioned LLVM compilation on Linux with Clang/LLVM/
LLD 19+. It runs the C++ frontend once, partitions the resulting LLVM module,
runs the ThinLTO pre-link pipeline in parallel and uses the ThinLTO linker for
cross-partition optimization and parallel code generation. Generated code and
runtime remain at `-O2`; extension objects retain `-O3`. The registry Dockerfile
installs that toolchain and no longer contains the `-O0` wrapper.

## Measurements

Same frozen registry C++ source, Debian 12 x86_64 container on the requested
`.136` builder. The isolated container had 16 CPUs and 20 GiB available. These
initial comparisons exclude runtime-archive and SQLite-extension compilation.

| Compilation | Wall time | Result |
|---|---:|---|
| Clang 14, serial `-O2` | >900 s | Stopped at the time limit |
| Clang 19, serial `-O2` | 556.33 s | Complete |
| Clang 19, serial, dispatch table experiment | 474.05 s | Complete; experiment not adopted |
| Clang 19, partitioned `-O2`, 8 workers / 16 partitions | About 133 s summed measured stages | Complete, linked executable |

The parallel experiment's major stages were frontend 28.69 s, LLVM text/bitcode
conversion about 19.2 s, splitting 13.31 s, two waves of pre-link optimization
about 8.6 s, and ThinLTO linking/code generation 63.05 s. Its executable was
27,886,840 bytes. Full Docker-build measurements and the deployed image are
recorded in the registry deployment runbook and image evidence.

The integrated Docker build with the final driver measured **167.751 s (2:48)**
for the native compile/link stage, including extension compilation. The first
registry build, including a cold runtime archive, took **298.8 s (4:59)**.
The final executable was **27,990,232 bytes**, still with 1928/1928 native code
objects and no VM fallback. These integrated times are broader than the initial
compile-only comparison above.

Both the serial `-O2` and parallel registry executables passed all 677 Linux
integration checks. Timed test-suite runs took 39.54 s and 39.31 s respectively;
this suite includes waits and subprocess startup and is not a throughput test.

Separate warmed workload-body comparisons used seven interleaved runs, identical
sources and matching checksums. Times are medians per workload invocation:

| Workload | Serial `-O2` | Parallel / ThinLTO `-O2` | Difference |
|---|---:|---:|---:|
| Arithmetic, 1,000,000 iterations | 5.142 ms | 5.228 ms | +1.68% |
| String operations, 4,000 iterations | 15.528 ms | 15.829 ms | +1.94% |
| Calls and collections, 20,000 rounds | 14.499 ms | 14.552 ms | +0.37% |

These measurements bound the observed difference on these workloads; they do not
establish identical performance for every program. Small programs stay on the
serial path automatically, avoiding partitioning overhead.

## Correctness and controls

Clang's `available_externally` and `linkonce_odr` definitions need special care
when splitting before optimization: a definition can be discarded in its owning
partition before another partition's call is seen. The driver retains those
definitions with `weak_odr` linkage until ThinLTO has seen the whole graph. It
edits only linkage positions in LLVM definitions, preserving string and embedded
bytecode payloads. It does not duplicate mutable globals or runtime instances.

LLVM's splitter externalizes private symbols with hidden visibility and preserves
COMDAT/alias groups; see its [implementation](https://github.com/llvm/llvm-project/blob/llvmorg-19.1.7/llvm/lib/Transforms/Utils/SplitModule.cpp).

`SPUTNIK_NATIVE_JOBS=0` selects automatic behavior for graphs with at least 512
code objects, using at most eight workers. `1` forces the serial baseline.
`2`–`64` force parallel compilation and require a matching toolchain. The
automatic path retains serial compatibility on older toolchains and on macOS.
Each build writes `<native-source>.compile.json`, including mode, optimization,
workers, partitions and compile/link wall time. LLVM intermediates and the
ThinLTO cache are retained under `<native-source>.llvm/`.

The opt-in `make test-native-parallel` gate compares serial and parallel execution
of the existing FFI/call-buffer fixture (closures, ownership, default/keyword
arguments, native handles and exceptions), checks paths containing spaces and
commas, and verifies automatic selection on a 600-function graph. A string
payload containing LLVM linkage keywords verifies that IR rewriting preserves
program data. The existing macOS serial path was also built and run with the
times-counter fixture.

Partitioning distributes functions rather than splitting a single very large
function. It also retains a single C++ frontend pass. Those remain limits for
future work; no optimization passes were disabled to obtain this improvement.

Raw initial evidence is in the ignored workspace directory
`build/compile-time-investigation/evidence/`. Production image evidence is
retained independently by the registry deployment.
