# Shared package/build manifests — 2026-10-29

Authored packages now use `sputnik.build.yaml` (or the existing YAML/JSON
variants) with `files`, `source_root`, optional `module_names`, dependencies,
capabilities, profiles and native metadata. The packer writes canonical
`sputnik.build.json` using the same `sputnik.build.v1` schema. Package TOML
parsing and generation have been removed. Old TOML distributions must be rebuilt;
historical evidence is retained.

All checks used isolated package homes and local filesystem or loopback
registries. Nothing was published to the public registry.

## Workspace packages

`workspace-packages.json` records the artifact digests for all 16 current source
packages: sqlite3, orm, orbit, plot, pycall, rbcall, the six Orbit subpackages,
notebook-core, notebook.ui, progressbar and system_io. Each was built twice with
identical archive bytes, verified and installed through a local filesystem
registry, including declared dependencies.

```sh
python3 tests/workspace_packages_test.py --native --out /tmp/sputnik-yaml-check/workspace-final
```

The installed Python/Ruby bridges ran in VM and full-native modes without SDK
provider bindings. VM execution also disabled the compiler. Installed System
IO SHA-256 and SQLite queries passed with the compiler and SDK bindings disabled.
The harness excludes Torch, which uses its CMake-only package/test workflow.

## Torch

`torch-packages.json` contains a compact matrix inventory; `torch-verification.json`
records all 15 source/bytecode/mixed × source/prebuilt × none/runtime/SDK variants
and two separate native providers. All 15 passed on CPU and actual MPS with
`PYTORCH_ENABLE_MPS_FALLBACK=0`. All five native/bundle combinations passed
`--require-full-native` and ran without SDK bindings or a compiler. Source packing
was reproducible with deliberately unavailable SDK/compiler paths. Missing
providers produced an explicit diagnostic.

```sh
tools/package --matrix --out-dir /tmp/sputnik-yaml-check/torch-matrix
tools/test-packages /tmp/sputnik-yaml-check/torch-matrix
```

Verified platform: macOS arm64, LibTorch 2.14.0. Linux/CUDA execution was not
available in this environment.

`torch-install.json` and `torch-install.log` additionally verify normal
`pkg install` of the prebuilt bytecode/runtime variant into a clean package home.
The installed artifact passed CPU/MPS forward/backward with external providers
empty and the compiler disabled. This check is now included in
`torch/tests/package_install.cmake` and `tools/test-packages`.

Packing also exposed invalid return syntax in `nn.functional.kl_div`; it was
rewritten as equivalent guard returns. The actual current function passed
focused CPU/MPS VM tests for matrix/scalar batchmean, log targets and gradients
with MPS fallback disabled (`torch-kl-div-vm.log`, `kl_div.s`). The earlier
classification of the losses failure as a missing Map runtime feature was
incorrect: `state_dict` used the removed `transform_values` name and now uses
the existing pure `transformed_values` API. Its snapshots stay read-only while
the model's live buffers remain mutable.

Native analysis also incorrectly required a concrete Generator declaration in
the calling library module. Caller-supplied `rand!`, `randn!` and `randperm!`
protocol calls now use the existing native user-method dispatcher. The open
protocol regression passes VM/full-native with full coverage and no bytecode
fallback. Test bootstrap inventories now include only the test/example helpers
imported by the chosen entry, and `--no-notebook` isolates tests from unrelated
local notebook registrations. Follow-up verification is recorded separately in
`torch-extra-checks.json`. The full losses and normalization/regularization
suites pass on CPU and MPS in both VM and full-native modes, including state
snapshots, stochastic dropout, generator replay and gradients. All four native
graphs have full body coverage, zero VM fallback and no runtime bridge; MPS
execution keeps automatic CPU fallback disabled.

## Regression coverage

Passed build, package, distribution (305 checks), store, parser and notebook
dependency suites. `package_manifest_workflow_test.py` covers exact inventories,
inferred/overridden module names, YAML capability metadata, deterministic packing,
source and bytecode installs, transitive registry/path dependencies, unsafe and
duplicate files and version overrides. Invalid native ownership/destructor
fixtures use YAML and check the intended semantic diagnostics.

`package_store_cli_smoke.py` passed with its notebook checks, native preparation,
compiler-free execution, local graph locks, module shadowing, artifact integrity
and prepared-library tampering checks. Directory installation, removal, import
replacement, source-path and relative-import VM/full-native tests passed too.
HTTP client fixtures for registry queries, installs and native variants used
loopback servers only.

The registry was rebuilt with full native coverage (2053/2053 code objects).
Its complete loopback integration suite passed all 772 checks using the new
YAML/JSON fixtures, including source/bytecode/mixed publication, native provider
metadata, CLI install, corrupt archives and metadata limits. See
`registry-integration.json` and `registry-native-build.log`. A preliminary VM
run verified source publication/download before the complete native run.

The checked-in logs and JSON reports are compact evidence; complete intermediate
artifacts and compiler logs were written under `/tmp/sputnik-yaml-check`.
