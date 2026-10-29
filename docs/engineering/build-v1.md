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

## Packages use the same manifest

`sputnik.build.yaml`, `.yml` or `.json` is the only authored configuration for
builds, source dependencies, package installation and packing. Multiple formats
in one directory are an error. A package needs `name`, `version`, `source_root`
and an explicit `files` inventory; libraries need no `root` or `entry`.

```yaml
name: example
version: "1.0.0"
source_root: src
files:
  - src/example.s
  - src/example/http.s
  - resources/defaults.json
  - LICENSE
dependencies:
  sqlite3: "0.1.0"
capabilities:
  fs.read: ["./data"]
```

`src/example/http.s` provides `example.http`. `source_root` controls naming,
without implicitly including files. An optional `module_names` mapping overrides
individual files, for example `src/legacy.s: example.compat`. Source package
declarations must agree with the resulting names. Duplicate files/module names,
escaping paths and overrides for unlisted files are errors. Files are regular
files, not recursive directory inventories. Native source/header/library and
provider assets must also appear in `files`. Dependencies and capabilities use
the same syntax for applications and packages; capability requests do not grant
host permissions. Build profiles and native metadata remain in this file.

`sputnik pkg build . --out example.sputnikpkg` uses this configuration. The
archive contains a generated canonical `sputnik.build.json` with the same
schema, followed by `checksums.sha256` and the sorted payload. Authors maintain
one inventory. Bytecode delivery replaces selected sources with compiled files
and generated name overrides. Local path dependencies are validated and pinned
to their declared versions in the distribution. Installing a source directory
also installs its path dependencies recursively before installing the parent.

## Manifest Shape

A plain application needs no manifest:

```sh
sputnik run backend/main.s
sputnik build backend/main.s -o build/notebook
sputnik pkg lock backend/main.s
```

The entry file determines the root. Local imports map dotted module names to
paths below the importing file's directory, with the entry directory as a
fallback; the entry package prefix can be omitted from
the filesystem path (`package demo.main` importing `demo.views` finds
`views.s`). Only reachable files are compiled. Duplicate local providers fail
with an ambiguity diagnostic; unrelated files are not scanned.

The nearest optional `sputnik.build.json`, `.yaml` or `.yml` above the entry
supplies settings such as `dependencies`, `source_formats` and profiles. It
needs no `name`, `root`, `modules`, `sources` or `entry`. Relative settings and
`sputnik.lock` are anchored to that configuration directory. Without settings,
the package/lock anchor is the entry directory. Multiple configuration formats
in the same directory are an error. This lookup never downloads packages.

Filesystem imports use static relative string paths:

```sputnik
import "../backend/models" as models
from "../backend/models.s" import Account, Project
```

`./` and `../` are resolved against the importing file, independently of cwd.
An omitted extension searches `.s`, `.spu`, `.sputnik`; distinct matches are an
error. The default namespace alias is the filename stem. Interpolation, absolute
paths, NUL and line breaks are rejected. A source need not declare `package`;
its canonical path still provides one identity. Multiple paths/symlinks to the
same file initialize it once; distinct files declaring the same module fail.
Relative imports are resolved before macro staging and binding, tracked as build
inputs and serialized as ordinary module dependencies. Package settings and lock
remain anchored to the entry's configuration. No extra source inventory is needed.

For explicitly invoking a configuration as a build target, `entry: backend/main.s`
is enough to infer its name and root. The legacy explicit module shape remains
available for bootstrap builds and custom mappings:

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
An optional `dependencies` object constrains package selection with exact
versions or live source directories. No source inventory is needed:

```yaml
dependencies:
  orbit: {path: ../orbit}
  orbit-session: {path: ../orbit/packages/orbit-session}
  another-package: "1.2.3"
```

The JSON form is `"orbit": {"path": "../orbit"}`. An optional `version`
inside a path object checks the directory's declared version; it does not
override it. Relative paths are resolved against the declaring manifest,
independently of the process working directory. The directory's `sputnik.build.*` owns its file inventory, module naming and native build facts.
No recursive source glob is used. Imported modules are discovered before
macro expansion, so ordinary code, macro providers and external `.sv` views
use one build-scoped package resolver. The application still declares its
`source_formats` registry when using external templates.

Omit `dependencies` when imports can be resolved from installed packages,
adjacent `packages/`, or a graph lock. Declarations express selection policy;
`sputnik pkg lock <source-or-manifest>` records the resolved graph. Local source edits
invalidate compilation caches without changing the graph lock. Changing local
package identity, source directory or dependency edges requires an explicit
lock update. Immutable installed artifacts retain checksum verification.

Build summaries expose absolute `inputs`: project/package source files,
external template inputs, package manifests and native source/header inputs.
Configured source entries and manifest VM runs also write that summary to `<SPUTNIK_VM_CACHE>/inputs.json`
(default `<manifest-directory>/.sputnik/vm/inputs.json`) for development tools.
Watchers need not infer the dependency graph from the consumer's `modules`.

See
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
