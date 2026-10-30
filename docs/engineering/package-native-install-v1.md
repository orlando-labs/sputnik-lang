# Native registry installation and technical dependencies

Native distributions share a release but have independent immutable variants.
An installation selects a variant for the current process, verifies the exact
archive and inventory, prepares its native library without loading it, then
publishes the complete variant directory. Native execution still requires the
normal FFI grant or notebook native trust.

Registry discovery reads metadata without downloading archives or checking the
current host's GPU unless `--check-host` is requested. Root aliases and canonical scoped names both work:

```sh
sputnik pkg versions sputniktorch
sputnik pkg variants slowpilot/sputniktorch@0.1.2
sputnik pkg variants sputniktorch@0.1.2 --json
sputnik pkg variants sputniktorch@0.1.2 --json --check-host
```

Each command requests its index/release endpoint directly; resolving a root
alias does not require a preliminary package request or another TLS handshake.

Text output contains one version or available variant per line. Versions are
ordered by SemVer descending, including prereleases; withdrawn releases have a
`(yanked)` marker. Quarantined releases/variants are unavailable. `--json` returns
the complete index or release metadata, including target, CUDA/LibTorch
requirements and archive hashes. Index pages are collected automatically, with
bounded retries if publication changes the index during pagination.
`pkg variants --json --check-host` adds the current process host (`os`, `arch`),
a `host_selection` record for each artifact and `recommended_variant` (or null).
The selection record reports compatibility and its reason, backend, bundle,
native-code consent and source-build requirement using the installer's gates.
It probes GPU hardware on the execution machine without downloading packages.
Recommendations prefer prebuilt artifacts, a supported GPU backend and ordinary
runtime/library builds over SDK bundles; equivalent builds use a stable variant
order. The caller pins the explicit selected variant on installation and still
collects native/source/large-package consent. Ordinary discovery output is unchanged.

Both commands accept `--registry URL` and `--credentials FILE`, using the same
default hub, environment overrides and saved credentials as installation.

```sh
sputnik pkg install slowpilot/sputniktorch --version 0.1.2 \
  --backend cuda --bundle runtime --allow-native --allow-native-large
sputnik pkg install slowpilot/sputniktorch --version 0.1.2 \
  --variant linux-x86_64-gnu-cu130-prebuilt-source-runtime \
  --bundle runtime --allow-native --allow-native-large
sputnik pkg lock app.s --backend cuda
sputnik pkg install slowpilot/sputniktorch --locked sputnik.lock \
  --allow-native --allow-native-large
```

The default backend and bundle policy are `auto`. Installation and unlocked
imports select the most suitable compatible variant for the current host,
including CUDA, ROCm or MPS when available, with a CPU fallback. Among equivalent
targets automatic selection prefers prebuilt runtime bundles over native source
adapters and SDK bundles. `--backend cpu` explicitly retains CPU selection;
other explicit backend/bundle policies constrain the automatic choice. An explicit variant retains architecture, libc, CPU instruction,
driver, GPU capability and extension ABI checks. Native source builds additionally
require `--build-from-source`; a missing or corrupt binary never causes an
implicit compilation fallback. Equal or incomparable selectors fail with
`ARTIFACT_SELECTION_AMBIGUOUS`. An unqualified version chooses the latest stable
release with an eligible artifact, then a prerelease only if no stable qualifies.
An exact version never silently downgrades.

Linux CUDA targets declare `cuda_version`, `min_cuda_driver` and a nonempty
`cuda_architectures` list using dotted compute capabilities, for example `8.6`.
The NVIDIA host utility supplies the driver release; the system CUDA Driver API
supplies the visible devices, their compute capabilities and supported API
version. This respects `CUDA_VISIBLE_DEVICES` and `CUDA_DEVICE_ORDER` without
assuming that CUDA ordinals equal NVML indices. Every visible GPU must satisfy
the artifact declaration. CPU selection does not initialize the CUDA driver.
The CUDA toolkit compiler on the host is not used to decide whether a prebuilt
runtime is compatible. Only GPU capabilities verified by the publisher should
be advertised.

## Hard technical dependencies

A package release version and a technical provider version are separate facts.
For example, SputnikTorch `0.1.2` requires exactly LibTorch `2.14.0`; LibTorch
`2.11.0` cannot substitute for it even if its CUDA backend works on the same GPU.
The runtime/SDK requirements are typed and checked before C++ compilation or
library loading:

```yaml
native_requirements:
  - id: libtorch
    kind: runtime
    version: "2.14.0"
    abi: libtorch-2.14.0-cxx11-linux-x86_64-gnu-cu130
    build_id: "sha256:<exact-producer-identity>"
    provider: bundled
```

`version` is an exact provider version. `abi` distinguishes C++/runtime ABI
contracts, and `build_id` optionally fixes one particular producer build. An SDK
also pins its runtime's build identity through `runtime_build_id`. Wrong versions,
ABIs, producer builds and SDK/runtime pairs fail with `NATIVE_ABI_CONFLICT`.
Accepting another LibTorch version requires separate compatibility work and
verification; selection never relaxes the declared requirement.

Providers can be bundled, explicit external bindings, or exact registry package
dependencies (`provider: package`, `package: publisher/provider` plus an
exact package dependency). Registry compatibility metadata exposes native
requirements/providers before download. Multiple runtime provider libraries are
prepared in one verified directory; conflicting library bytes or singleton ABI
requirements are errors. Public package dependencies currently use exact versions.

## Store and lock

Tar distributions live at `<home>/<ref>/<version>/<variant>/`, including scoped
references. Different variants coexist; one graph activates one variant per ref. Installed
packages resolve without a lock using the current host and automatic policy.
A lock is optional and pins exact variants for reproducibility; it is not required
to import an installed GPU package.
`pkg remove ref --version V` removes all variants; adding `--variant ID` removes
one. Legacy local text-artifact layouts remain readable.

`sputnik.packages.lock.v1` records canonical reference, exact version, variant,
archive SHA-256 and source origin. The archive hash also binds its technical
requirements and bundled provider bytes. Separately packaged providers have
their own version/variant/hash lock entries. Existing registry pins are checked
on Run/build; a pinned platform mismatch fails instead of selecting another
build. `--locked FILE` restores those exact registry artifacts and rejects a
changed digest or a dependency absent from the lock. To explicitly relock among
several installed builds, use `pkg lock app.s --variant ref=variant`.

Detached publisher signatures, universal/fat binaries and Windows native loading
remain separate work. Current execution supports one prebuilt entry library per
package, with its declared dependent libraries. Source module representation and
native adapter representation remain independent.

ROCm variants declare `compute_backend: rocm` and a Linux target. Explicit
`--backend rocm` selects them explicitly; automatic selection also chooses
them on a compatible AMD host. Explicit CPU selection excludes ROCm-only runtimes.
Host checks query `hipInit`/`hipGetDeviceCount` from the installed HIP C library,
respecting HIP device visibility. A missing runtime or visible GPU rejects both
automatic and exact-variant selection. LibTorch/ROCm versions and ABI identities
remain native-provider requirements; no Python or HIP compiler is used to probe.
