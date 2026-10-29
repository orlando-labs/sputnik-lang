# Shared package installation and import resolution

`sputnik run train.s` resolves imports against installed packages. Applications
do not need to copy dependencies or enumerate their source modules.

```sh
sputnik pkg install sputniktorch
sputnik run train.s
```

The default hub is `https://registry.sputnik-lang.online`. Override it with
`SPUTNIK_PKG_REGISTRY` or `--registry <url|directory>`; filesystem registries
populated with `sputnik package-publish` remain supported. An empty environment
value uses the default hub. `run`, `build` and import resolution stay offline.

The hub accepts scoped names such as `slowpilot/sputniktorch`. An operator may
promote a published package to its short root name, allowing
`sputnik pkg install sputniktorch`. Installation retains the canonical scoped
identity, module names and archive bytes; imports do not acquire a new prefix.

HTTP installation verifies bounded metadata, the canonical archive identity,
SHA-256, size, and download headers before installation. It selects the latest
non-yanked stable version, falling back to a prerelease when no stable release
exists; `--version` selects an exact non-yanked release. Portable `source` and
`bytecode` variants are supported. Automatic selection of platform-specific
native variants is not implemented. Private hubs accept `--credentials FILE`
or `SPUTNIK_PKG_CREDENTIALS`: a private, owned JSON file (0600) containing
`{"origin":"https://registry.sputnik-lang.online","token":"spreg_..."}`.
Credentials are origin-bound; redirects are not followed.

## Login and publication

```sh
sputnik pkg login --namespace alice
sputnik pkg whoami
sputnik pkg build ./library --out ./library.sputnikpkg
sputnik pkg publish ./library.sputnikpkg
sputnik pkg logout
```

Login displays a GitHub Device Flow code and URL. First-time users choose a free
personal namespace; subsequent logins retain it. Publication uses the scoped
name and variant from the archive, such as `alice/library`, and requires module
names prefixed with `alice.`. Native publication still needs an operator grant.
The administrator can also create accounts and issue tokens through the registry
console, independently of GitHub.

Credentials are saved with mode 0600 in
`~/.sputnik/credentials/<SHA256(origin)>.json`; install, publish and whoami use
the origin's saved token automatically. `--credentials` or
`SPUTNIK_PKG_CREDENTIALS` can select an operator-issued token file instead.
Login retains the expected account across token expiry. Logout revokes the
current token before removing credentials and reports network failures.
Publication keeps a private resumable session next to the archive; repeat the
same command after interruption, or specify `--session FILE`.

## Installation

Install a package from any existing directory, independently of the current
working directory and without configuring a registry:

```sh
sputnik pkg install /absolute/path/to/my-package
sputnik pkg install ../my-package
SPUTNIK_PKG_HOME=/path/to/shared-packages sputnik pkg install /path/to/my-package
```

If `SPUTNIK_PKG_HOME` is unset or empty, installation goes into
`~/.sputnik/packages`. Source files are read from the supplied directory; the
compiled package and native library are stored in the installation home. The
original checkout can be moved or removed after installation. Neither the
source directory nor the directory of a consuming program receives a package
copy. Paths containing spaces are supported with normal shell quoting.

`SPUTNIK_PKG_HOME` selects the shared installation directory; its default is
`$HOME/.sputnik/packages`. It contains immutable versioned artifacts using the
`<name>/<version>/<name>-<version>.sputnikpkg` layout for unscoped packages.
Scoped distributions use `<namespace>/<name>/<version>/<namespace>-<name>-<version>.sputnikpkg`.
The public tar.gz distribution is retained alongside verified extracted members;
activation checks both the archive and extracted bytes. Installation
records a small module index so looking up an import does not deserialize all
installed bytecode. Package names and imported module names can differ.

```sh
sputnik pkg env
sputnik pkg install progressbar --registry /path/to/registry
sputnik pkg install progressbar --version 1.2.0
sputnik pkg install ./progressbar-1.2.0.sputnikpkg
sputnik pkg install ./my-package
sputnik pkg install ./build-manifest-package --version 1.2.0
```

A source directory requires one `sputnik.build.yaml`, `sputnik.build.yml`, or
`sputnik.build.json`. It declares its version, or the caller supplies `--version`.
Installation builds the normalized tar source distribution and installs it in
the shared home under `<name>/<version>/<variant>/`. It preserves the checkout.
Path dependencies are installed recursively from their declaring directories;
registry dependencies are installed at their exact versions. An explicit local
source directory retains its existing native build behavior. Native registry
artifacts require `--allow-native --build-from-source`.

Exact transitive dependencies are installed from the same registry, reusing
already installed versions. Installation verifies artifact digests and bytecode;
reinstalling different bytes at an existing name/version is rejected. Installing
a package never evaluates its Sputnik module initializers.

Native extension source/header blobs are extracted within the package directory
and compiled during installation. Their shared library is cached with compiler,
host target, Sputnik extension ABI, source/header and build-option inputs.
Subsequent VM runs load the prepared library without invoking a compiler.
Library bytes and host target are checked before loading. `--grant ffi` retains
its existing meaning; without that grant the CLI uses Sputnik fallback bodies.

## Uninstalling packages

```sh
sputnik pkg uninstall my-package
sputnik pkg remove my-package
sputnik pkg uninstall my-package --version 1.2.0
```

`remove` is an alias for `uninstall`. Without `--version`, both commands remove
all installed versions of the named package; with `--version`, they remove only
that version. The name is the package identity from its manifest, not an import
name or source directory. Both commands use `SPUTNIK_PKG_HOME`, falling back to
`~/.sputnik/packages`, exactly as installation does.

Removal deletes each version's artifact, module index, extracted native sources
and prepared libraries/cache. It leaves dependencies, other packages, original
checkouts, project `packages/` overrides and lockfiles untouched. After removing
one version, unconstrained imports can use a remaining version. A lock pinned
to a removed version requires reinstalling it or explicitly updating the lock.
A running notebook retains its current loaded generation until Apply/restart.

Unknown packages/versions and malformed arguments fail without creating a home
directory. Symlinked package/version directories are rejected; symlinks inside
an installed version are unlinked without deleting their external targets.

## Resolution

Source entry commands need no manifest. Local imports resolve relative to
the entry file, using dotted names as subdirectories and optionally stripping
the entry's package prefix. Only imports are followed; unrelated source files
are not scanned. Installed packages work without `dependencies`.

The package search anchor is the nearest optional `sputnik.build.json` /
`.yaml` / `.yml` above the source entry, or its directory when none exists.
The optional configuration can contain only dependency overrides or template
formats; it needs no source list or entry declaration. Explicit manifest and
notebook commands anchor to their input directory. Packages are searched at
that anchor and in the shared home, independently of the working directory.
A projectless console searches only the shared home.

1. Explicit project/build modules, reachable local source imports and explicitly linked notebook dependencies.
2. Explicit `dependencies` path bindings and local sources pinned by a graph lock.
3. Packages under `<anchor>/packages/`.
4. Packages in `SPUTNIK_PKG_HOME`.

Builtin runtime namespaces retain their reserved identity. Local package
directory names need not match imports: the manifest's module table is the
authority. Symlinks to development checkouts are supported, while source/native
members must remain within the resolved package root.

One package version/build is activated per graph. A selected local package
cannot silently acquire missing modules from its global counterpart. Ambiguous
module providers at the same priority and conflicting exact versions are
errors. Unconstrained imports prefer the newest installed stable version, with
numeric release/prerelease comparison; a prerelease is used if no stable
version is available. Profile, bytecode and dependency ABI compatibility retain
the graph linker's checks. There is no backtracking range solver.

Only reachable modules enter the executable graph and initialize. Installed
native packages collect binding metadata from their already compiled modules;
native source overrides compile their declarations to prepare bindings.
Discovery and import
resolution never download packages. A missing import reports searched paths
and suggests `sputnik pkg install`.

`sputnik run <file.s>`, manifest execution and native `sputnik build` use the same resolver.
VM graph cache keys include the resolved package fingerprints. Notebook opening
does not discover, compile or load shared packages; explicit Apply/evaluation
does. Ordinary notebook Run retains its loaded generation. Installed native
notebook packages use the host's existing explicit native trust boundary.

## Version declarations and locking

Build manifests can pin package identities independently of module names:

```json
"dependencies": {"some-library": "1.2.0"}
```

Dependencies are optional: imports discover installed providers without a
repeated package list. A declaration constrains selection rather than granting
permission to import. A build dependency may also name a live directory:

```yaml
dependencies:
  some-library: {path: ../some-library}
```

A path object accepts an optional exact `version` consistency check. Each path
is relative to its declaring manifest; conflicting identities/versions are
errors. Packing validates the local package and records its declared version
without the development path. Installing from the source directory recursively
installs those local dependencies first. Source scripts and notebooks read the
same adjacent build configuration.
Version ranges are not implemented; incompatible requirements fail explicitly.

```sh
sputnik pkg lock train.s
sputnik pkg lock sputnik.build.json
```

These commands write an adjacent `sputnik.lock` with schema
`sputnik.packages.lock.v1`, selected package identities, versions and SHA-256
artifact fingerprints. For mutable source directories, `digest` fingerprints
package identity and declared dependency edges, while `source_kind = "path"`
and `source` identify the directory relative to the lock. It does not freeze
working-tree bytes. Source/native/template contents separately invalidate build
caches. A lock can locate its previously selected local roots even when the
consumer omits `dependencies`. Later runs enforce the graph lock without
rewriting it. Running `pkg lock` again deliberately resolves current versions.
An import that introduces a package absent from the lock fails until the lock
is updated. Local overrides shadow the entire installed package, including
modules the override does not provide.
The older `sputnik.lock.v1` embedded in package artifacts remains unchanged.

Automatic imports do not modify notebook `project.json` or save dependency
copies. Explicit notebook links remain available for custom prepared packages.

## Verification

`make test-package-store` covers registry/directory installation, exact
transitive dependencies, package/module name differences, local overrides,
version ordering, locking, graph-cache invalidation, unused initializers,
malformed artifacts, prepared native reuse with an unavailable compiler, and
trusted native imports in a notebook. `package_store_tests` additionally checks
Apply/Run snapshot behavior and ambiguous providers.

`node tests/path_dependencies_test.mjs --native` checks transitive paths,
manifest-relative resolution from a different CWD, live macro/template edits,
source/identity conflicts, locked installed imports without dependencies, and
strict native execution. It uses an isolated package home and no registry
network access.

`make test-package-directory-install` specifically verifies absolute, relative
and symlink source directories, the default and overridden package homes,
installation without a registry, and source/native imports after deleting the
original checkout.

`make test-package-remove` covers both aliases, exact/all-version removal,
fallback to remaining versions, default/overridden homes, native cleanup,
malformed arguments, corrupted artifacts and preservation of unrelated files
and symlink targets.
