# sputnik.build.v1

Status: implemented for `W14` build/bootstrap/conformance closure.

`sputnik.build.v1` is the repository-local build manifest consumed by
`sputnik build`. It is intentionally small and deterministic: source paths are
manifest-relative, module output names are derived from module ids, profile
features are sorted/deduplicated, and all output `.sputnikbc` artifacts are
decoded and verified before the command reports success.

Implemented surface:

- manifest parser and stable JSON summary in [buildsys/build.h](../../buildsys/build.h:1)
- build command in [tools/sputnik/main.cpp](../../tools/sputnik/main.cpp:1)
- `PROF` bytecode metadata in [bytecode/format.h](../../bytecode/format.h:1)
- unsupported-profile loader rejection in [runtime/module_loader.cpp](../../runtime/module_loader.cpp:1)
- CLI smoke fixture in [tests/fixtures/w14_build/sputnik.build.json](../../tests/fixtures/w14_build/sputnik.build.json:1)

## Manifest Shape

Minimal manifest:

```json
{
  "schema": "sputnik.build.v1",
  "name": "demo",
  "root": "demo.main",
  "profiles": {
    "required": ["core.v1"],
    "optional": ["typed.v1"],
    "forbidden": ["ffi.v1"]
  },
  "stdlib": [
    {"name": "sputnik.core", "path": "stdlib/core.s", "bootstrap": "B2"}
  ],
  "modules": [
    {"name": "demo.main", "path": "src/main.s"}
  ]
}
```

`stdlib` entries are compiled through the same source-to-bytecode pipeline as
ordinary modules. B2 stdlib modules receive `sputnik.bootstrap.layer` attributes
and ABI hashes. Ordinary modules receive dependency entries pinned to the
compiled stdlib ABI hashes.

## CLI

```sh
sputnik build sputnik.build.json --out-dir build/sputnik --cache-dir build/sputnik/.cache
sputnik build src/main.s -o build/main
```

Manifest builds emit `sputnik.build.result.v2` JSON with per-module source
hashes, cache keys, artifact hashes, ABI hashes, output paths, native sidecar
metadata, and cache-hit state. The default target is `both`: `.sputnikbc`
artifacts remain deterministic cacheable sidecars, and the root module also
gets a host native executable at `<out-dir>/<root-module>`. Use
`--target bytecode` for bytecode-only builds.

`sputnik run <manifest>` always executes the Sputnik graph in the bytecode VM.
Missing source-module dependencies are resolved from the adjacent `packages/`
directory and `SPUTNIK_PKG_HOME`; explicit manifest modules take priority.
An optional `dependencies` object pins package names to exact versions. See
[the package store](package-store-v1.md) for installation and lockfiles.
Native definitions use their Sputnik fallback bodies unless the consumer passes
`--grant ffi`. That alias grants both `ffi.load` and `ffi.call`: `sputnik`
compiles the manifest extension units into a temporary `.dylib`/`.so`, checks
the extension ABI and every declared symbol, loads the package with `dlopen`,
and registers its thunks, foreign-handle types, destructors, and errors before
creating the `RuntimeWorld`. The shared-library handle remains live until the
world is destroyed; no native Sputnik executable is produced.

Single-file builds emit `sputnik.executable.build.v2` JSON and create an
executable at `-o <path>`, `--out-dir <dir>/<source-stem>`, or the source path
without the `.s` extension. The default target is `native`: eligible bytecode
is lowered to generated C++ and compiled with `SPUTNIK_NATIVE_CXX`, `CXX`, or
`clang++`; unsupported bytecode remains correct through an embedded verified VM
fallback. Use `--target bytecode-wrapper` for the legacy shell wrapper that
re-enters `sputnik run-embedded`.

Native build metadata separates generated body coverage from execution-model
independence. `native_body_coverage_full` (or the manifest graph equivalent)
only means every Sputnik code object has a generated C++ body. The stricter
`native_full_coverage` additionally requires `native_vm_independent: true`, so
a `vm-stdlib-send-v1` bridge can no longer be reported as fully native. Use
`--require-native-body-coverage` for the former contract and
`--require-full-native` for the latter. Both result schemas also report the
final native binary size in bytes.

### Parallel optimized native builds on Linux

With Clang 19 or newer and matching `llvm-dis`, `llvm-as`, `llvm-split`, `opt`
and `ld.lld` tools installed, graphs with at least 512 code objects automatically
use parallel LLVM compilation. The C++ frontend runs once; LLVM optimization
runs on partitions, followed by ThinLTO linking with cross-partition inlining.
Generated code and the cached runtime retain `-O2`. Native extension units retain
their existing `-O3` setting. Generated program semantics and coverage selection
are unchanged.

For Debian 12, install `clang-19 llvm-19 lld-19`, then set
`SPUTNIK_NATIVE_CXX=clang++-19`. `SPUTNIK_NATIVE_JOBS=0` (the default) selects up
to eight workers automatically, `1` selects serial compilation, and `2` through
`64` explicitly select the worker count and require a compatible toolchain.
Automatic selection falls back to serial compilation when tools are unavailable;
small programs and macOS keep the existing serial path. Explicit parallel builds
also work for small programs, which is useful for equivalence tests.

The driver writes `<generated-source>.compile.json` with the compilation mode,
optimization level, worker/partition counts and compile/link wall time. LLVM
intermediates and the ThinLTO cache live under `<generated-source>.llvm/` in the
build directory. The build remains usable without Python; the optional
`make test-native-parallel` gate uses Python to compare serial and parallel
executables, exercise FFI/closures/exceptions and verify large-graph selection.

## Conformance

`make test` runs the W14 fixture twice and verifies the emitted root module with
both `sputnik sputnikbc-verify` and the public
`sputnik verify <file.sputnikbc> --json` surface. It also smoke-checks
`sputnik metadata <file.sputnikbc> --json`, a corrupted-bytecode verifier
failure, direct `sputnik <file.s>` execution, and
`sputnik build <file.s>` executable execution. `make conformance` runs
`sputniktest run corpus --bundle M11`, which adds a compile-all postpass over the
compile/disasm/run/load corpus.
