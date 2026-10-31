# Language and VM version contract

Sputnik is **1.0.0**. `runtime/version.h` is the single source of its SemVer
major, minor and patch components. The compiler, VM, standard library contract
and native extension interface share this release identity. A breaking change
to any of them requires a major release; compatible additions use minor releases,
and compatible fixes use patch releases. Bytecode container versions describe
physical encoding only; they are not a second language or extension ABI version.

## Package requirements

Every published distribution contains an explicit requirement:

```yaml
requires:
  sputnik: ">=1.0.0 <2.0.0"
```

The requirement belongs to the package, including all its variants. Each
transitive package and each artifact dependency is checked against the executing
language/VM version. An application's requirement cannot override a dependency's
requirement. Exact package dependencies continue to identify package releases;
Sputnik itself is not resolved as an ordinary package.

Requirements accept full SemVer literals, comparison intersections, `^`, `~`, and
`||` unions. Each branch must have an upper bound. Partial versions and wildcards
are rejected. Prereleases require an explicit prerelease comparator for the same
major/minor/patch tuple; build metadata does not affect precedence.

Unversioned local projects retain the fixed `>=1.0.0 <2.0.0` baseline. This default
must not track the current release: an old project must not silently acquire a
new major's semantics. For an unversioned local project, the packer also sets
the publication minimum to the producing compiler's release, retaining the
next-major upper bound. An explicitly authored range is preserved. Archived manifests without a requirement are
rejected with `LANGUAGE_REQUIREMENT_MISSING`; rebuild and publish a new package
release. Existing immutable artifacts are never rewritten or silently migrated.

Selection checks requirements before compiling or loading package code. Registry
metadata carries `compatibility.sputnik_requirement`, `bytecode_languages`,
`bytecode_versions`, and `sputnik_versions` (native producers). Clients skip
incompatible releases/variants before download. Explicit incompatible versions
fail rather than selecting another release. Downloaded manifests are checked
again against both the host and advertised requirement.

## Bytecode and native code

Bytecode format 1.2 carries the patch component of the producer's language version
and dependency language bounds. The header grows from 56 to 60 bytes: after the
language major/minor pair come a u16 patch and a zero reserved u16. DEPS adds a u16
patch after each minimum/maximum language minor. Formats 1.0/1.1 decode as patch
zero. Future producer versions and different language majors are rejected. A
compatible newer VM can run older 1.x code. Module ABI hashes include the language
version; dependency language comparisons include patch.

Native extensions export `const char *sputnik_language_version(void)`, returning
`SPUTNIK_VERSION` from the header they were compiled against. Their manifest and
build metadata use the SemVer string `sputnik_version`. The loader verifies the
actual producer version after the explicitly authorized load. A producer requires
the same language major and a VM at least as new as itself. There is no independent
`SPUTNIK_EXT_ABI_VERSION`. Old extension binaries must be rebuilt with this entry
point; there is no fallback to the previous ABI handshake. Native loading still
requires the existing explicit native trust policy.

## Locks and caches

Application locks record `sputnik_version` alongside the package graph. Execution
and `pkg install --locked` require the exact recorded language/VM release. A
different or missing version produces `LOCK_LANGUAGE_MISMATCH`; `sputnik pkg lock
<source-or-manifest>` explicitly regenerates the lock, including a legacy lock.
Package digests continue to protect immutable bytes. Local path graph digests
also cover language requirements.

Module and linked-graph caches include the language version and SHA-256 of the
running compiler executable. Rebuilding a development compiler invalidates these
caches even without changing the release number. Native extension caches include
the compiler identity, shared language version and runtime headers. Publication's
`toolchain_revision` remains provenance, not a compatibility requirement.

For a release: update the numeric macros, follow SemVer for every VM/native
change, review package ranges, regenerate locks explicitly, and run
`make test-language-version` plus affected VM/native/package/registry tests.
