# Building package distributions v1

Implemented on 2026-10-19. Public `.sputnikpkg` files are deterministic tar.gz
archives. The generated manifest uses the shared `sputnik.build.v1` schema.

```sh
build/sputnik pkg build packages/example --version 0.1.0 --out example.sputnikpkg
build/sputnik pkg build packages/example --version 0.1.0 --modules bytecode --out example-bc.sputnikpkg
build/sputnik pkg build packages/example --version 0.1.0 --modules mixed --bytecode-module example.internal --out example-mixed.sputnikpkg
build/sputnik pkg inspect example.sputnikpkg
build/sputnik pkg verify example.sputnikpkg
build/sputnik pkg extract example.sputnikpkg /path/to/new-directory
tar -tzf example.sputnikpkg
```

`pkg build` accepts a directory with one `sputnik.build.yaml`, `.yml` or `.json`,
or an explicit YAML/JSON build manifest. `version` can be supplied with
`--version` when absent. See [the shared manifest](build-v1.md#packages-use-the-same-manifest).
Paths must stay within the input manifest directory. Third-party
native dependencies are declared separately from source/prebuilt adapter facts.
`artifact.bundle` selects `none` (default), `runtime` or `sdk`; SDK bundles need
at least one native source adapter. `torch/tools/package --matrix` prepares
all 15 current-platform combinations and separate runtime/SDK provider artifacts.
See the [provider/bundle design](../../../sputnik-registry/docs/NATIVE_BUNDLES.md).

`--modules source` is the default. `bytecode` compiles each module with the
existing compiler. `mixed` accepts repeated `--bytecode-module`; without an
explicit selection it compiles all non-root modules except macro providers.
Macro-provider modules must retain source because the current compiler needs
their expansion metadata. A module has one source or bytecode member, never
both. Existing bytecode inputs are verified and preserved.

Archives contain `sputnik.build.json` first, `checksums.sha256` second, followed by
sorted payload paths. The authored `files` list is the exact package inventory,
including native source/header/library inputs:

```yaml
files:
  - src/example.s
  - README.md
  - LICENSE
  - resources/defaults.json
```

Directories are not recursively packed. Every listed file must be regular and
inside the package root. Native inputs and provider assets must be listed too.
The packer generates metadata and replaces selected source files with bytecode;
source inventory cannot reintroduce the replaced source.

Bytecode/native packages receive a canonical `meta/build.json` with schema
`sputnik.package-build.v1`. It records the producing compiler executable's SHA-256,
decoded bytecode format/language/profile/ABI facts and payload digests, native
kind/ext ABI/target and input/library digests. The inspector regenerates the
payload assertions and checks them against the archive. Publisher-specific build
notes belong at another path, such as `meta/libtorch.json`.

`artifact` selects `variant` and `limit_profile`; `target` declares OS,
architecture, minimum OS, libc and compute compatibility. `native_extensions`
supports `kind: source` or `kind: prebuilt` with `library` and
`sputnik_ext_abi: 1`. Each extension's `libraries` list enumerates bundled dependencies.
New distributions use typed `native_requirements`, per-extension
`requirements`, and `native_providers` with provider-owned libraries/assets.
Top-level `kind: native` allows providers without modules/root.
An explicit `SPUTNIK_NATIVE_PROVIDERS` binding lists extracted native-only
provider directories separated by `:`. Runtime/SDK version/ABI/build constraints,
SDK/runtime pairing and inventories are checked before preparation; no ambient
prefix search occurs. Cached provider roots and prebuilt dependency views retain
signed bytes. Provider fingerprints participate in native compilation cache keys.
Missing, ambiguous, mismatched and tampered providers fail before compilation or
loading. Use `SPUTNIK_NATIVE_EXT_CACHE` to choose a writable persistent cache.
Managed package-provider resolution and application lock integration remain
planned; local bindings do not implement those contracts.

Prebuilt macOS libraries must have matching Mach-O headers and resolvable
package-relative imports, declared external provider imports, or allowed system framework imports. The LibTorch bundle
preparer handles current-platform macOS arm64/x86_64; Linux/Windows bundle
preparation and complete ELF/PE validation remain future work.

Inspection checks the inventory/member set, digests, path and type constraints,
source declarations/syntax, bytecode, budgets and complete gzip/tar framing,
including CRC and trailing data. It does not load native libraries. `extract`
verifies first, requires a nonexistent destination and removes partial output on
failure. `verify` checks integrity and payload assertions; publisher signatures
and the registry protocol are separate future work.

Extracted source/bytecode/mixed distributions work through local `packages/`
overrides. One prebuilt native unit can load through the existing explicit FFI
grants without invoking a C++ compiler. The new tar distributions are not yet
wired into shared `pkg install`/variant selection. Existing `package-*` commands
and shared installation still use the unreleased embedded-image prototype; use
the `pkg build/inspect/verify/extract` commands above for distribution archives.

Validation: `make test-package-distribution`; the tests cover reproducibility,
roundtrip extraction, source privacy, malformed payloads/metadata, traversal,
case and parent collisions, symlinks, inventory mismatch, CRC, truncation and
appended archives. `sputniktorch/tools/test-packages` exercises real packages.
