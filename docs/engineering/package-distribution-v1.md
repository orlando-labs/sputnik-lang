# Building package distributions v1

Implemented on 2026-10-19. Public `.sputnikpkg` files are deterministic tar.gz
archives. The manifest schema remains `sputnik.package-manifest.v1`.

```sh
build/sputnik pkg build packages/example --version 0.1.0 --out example.sputnikpkg
build/sputnik pkg build packages/example --version 0.1.0 --modules bytecode --out example-bc.sputnikpkg
build/sputnik pkg build packages/example --version 0.1.0 --modules mixed --bytecode-module example.internal --out example-mixed.sputnikpkg
build/sputnik pkg inspect example.sputnikpkg
build/sputnik pkg verify example.sputnikpkg
build/sputnik pkg extract example.sputnikpkg /path/to/new-directory
tar -tzf example.sputnikpkg
```

`pkg build` accepts a directory with `sputnik.toml` or a build manifest, or an
explicit TOML/JSON/YAML manifest. Build manifests without package versions need
`--version`. Paths must stay within the input manifest directory. Third-party
native dependencies are declared separately from source/prebuilt adapter facts.
`[artifact].bundle` selects `none` (default), `runtime` or `sdk`; SDK bundles need
at least one native source adapter. `sputniktorch/tools/package --matrix` prepares
all 15 current-platform combinations and separate runtime/SDK provider artifacts.
See the [provider/bundle design](../../../sputnik-registry/docs/NATIVE_BUNDLES.md).

`--modules source` is the default. `bytecode` compiles each module with the
existing compiler. `mixed` accepts repeated `--bytecode-module`; without an
explicit selection it compiles all non-root modules except macro providers.
Macro-provider modules must retain source because the current compiler needs
their expansion metadata. A module has one source or bytecode member, never
both. Existing bytecode inputs are verified and preserved.

Archives contain `sputnik.toml` first, `checksums.sha256` second, followed by
sorted payload paths. Source modules and declared native sources/headers/libraries
are included automatically. Other files require an explicit packlist:

```toml
[files]
include = ["README.md", "LICENSE", "resources"]
```

Directory entries in the packlist are traversed explicitly. Build caches and
checkout contents are not included by default. Closed module sources cannot be
reintroduced through a directory packlist. Generated archive metadata cannot be
overridden by the packlist.

Bytecode/native packages receive a canonical `meta/build.json` with schema
`sputnik.package-build.v1`. It records the producing compiler executable's SHA-256,
decoded bytecode format/language/profile/ABI facts and payload digests, native
kind/ext ABI/target and input/library digests. The inspector regenerates the
payload assertions and checks them against the archive. Publisher-specific build
notes belong at another path, such as `meta/libtorch.json`.

`[artifact]` selects `variant` and `limit_profile`; `[target]` declares OS,
architecture, minimum OS, libc and compute compatibility. `[[native]]` supports
`kind = "source"` or `kind = "prebuilt"` with `library` and
`sputnik_ext_abi = 1`. `[[native.libraries]]` enumerates bundled dependencies.
New distributions use typed `[[native_requirements]]`, per-extension
`requirements`, and `[[native_providers]]` with provider-owned libraries/assets.
Native-only `[package].kind = "native"` allows providers without modules/root.
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
