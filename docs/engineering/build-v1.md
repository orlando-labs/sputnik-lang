# amber.build.v1

Status: implemented for `W14` build/bootstrap/conformance closure.

`amber.build.v1` is the repository-local build manifest consumed by
`amberc build`. It is intentionally small and deterministic: source paths are
manifest-relative, module output names are derived from module ids, profile
features are sorted/deduplicated, and all output `.amberbc` artifacts are
decoded and verified before the command reports success.

Implemented surface:

- manifest parser and stable JSON summary in [buildsys/build.h](../../buildsys/build.h:1)
- build command in [tools/amberc/main.cpp](../../tools/amberc/main.cpp:1)
- `PROF` bytecode metadata in [bytecode/format.h](../../bytecode/format.h:1)
- unsupported-profile loader rejection in [runtime/module_loader.cpp](../../runtime/module_loader.cpp:1)
- CLI smoke fixture in [tests/fixtures/w14_build/amber.build.json](../../tests/fixtures/w14_build/amber.build.json:1)

## Manifest Shape

Minimal manifest:

```json
{
  "schema": "amber.build.v1",
  "name": "demo",
  "root": "demo.main",
  "profiles": {
    "required": ["core.v1"],
    "optional": ["typed.v1"],
    "forbidden": ["ffi.v1"]
  },
  "stdlib": [
    {"name": "amber.core", "path": "stdlib/core.am", "bootstrap": "B2"}
  ],
  "modules": [
    {"name": "demo.main", "path": "src/main.am"}
  ]
}
```

`stdlib` entries are compiled through the same source-to-bytecode pipeline as
ordinary modules. B2 stdlib modules receive `amber.bootstrap.layer` attributes
and ABI hashes. Ordinary modules receive dependency entries pinned to the
compiled stdlib ABI hashes.

## CLI

```sh
amberc build amber.build.json --out-dir build/amber --cache-dir build/amber/.cache
amberc build src/main.am -o build/main
```

Manifest builds emit `amber.build.result.v2` JSON with per-module source
hashes, cache keys, artifact hashes, ABI hashes, output paths, native sidecar
metadata, and cache-hit state. The default target is `both`: `.amberbc`
artifacts remain deterministic cacheable sidecars, and the root module also
gets a host native executable at `<out-dir>/<root-module>`. Use
`--target bytecode` for bytecode-only builds.

`amberc run <manifest>` always executes the Amber graph in the bytecode VM.
Native definitions use their Amber fallback bodies unless the consumer passes
`--grant ffi`. That alias grants both `ffi.load` and `ffi.call`: `amberc`
compiles the manifest extension units into a temporary `.dylib`/`.so`, checks
the extension ABI and every declared symbol, loads the package with `dlopen`,
and registers its thunks, foreign-handle types, destructors, and errors before
creating the `RuntimeWorld`. The shared-library handle remains live until the
world is destroyed; no native Amber executable is produced.

Single-file builds emit `amber.executable.build.v2` JSON and create an
executable at `-o <path>`, `--out-dir <dir>/<source-stem>`, or the source path
without the `.am` extension. The default target is `native`: eligible bytecode
is lowered to generated C++ and compiled with `AMBER_NATIVE_CXX`, `CXX`, or
`clang++`; unsupported bytecode remains correct through an embedded verified VM
fallback. Use `--target bytecode-wrapper` for the legacy shell wrapper that
re-enters `amberc run-embedded`.

Native build metadata separates generated body coverage from execution-model
independence. `native_body_coverage_full` (or the manifest graph equivalent)
only means every Amber code object has a generated C++ body. The stricter
`native_full_coverage` additionally requires `native_vm_independent: true`, so
a `vm-stdlib-send-v1` bridge can no longer be reported as fully native. Use
`--require-native-body-coverage` for the former contract and
`--require-full-native` for the latter. Both result schemas also report the
final native binary size in bytes.

## Conformance

`make test` runs the W14 fixture twice and verifies the emitted root module with
both `amberc amberbc-verify` and the public
`amberc verify <file.amberbc> --json` surface. It also smoke-checks
`amberc metadata <file.amberbc> --json`, a corrupted-bytecode verifier
failure, direct `amberc <file.am>` execution, and
`amberc build <file.am>` executable execution. `make conformance` runs
`ambertest run corpus --bundle M11`, which adds a compile-all postpass over the
compile/disasm/run/load corpus.
