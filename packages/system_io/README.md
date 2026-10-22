# System IO SDK modules

Bundled Sputnik library modules over small native leaves:

- sdk.fs.Root: descriptor-relative filesystem operations. Member paths cannot
  be absolute or contain dot/dot-dot components; directory walks and file opens
  reject symlinks. Files must be regular. Open supports exclusive creation and
  private modes; link never replaces a target and returns false on conflict.
  Mutations sync their containing directories; callers explicitly sync file data.
- sdk.fs.File: bounded reads, complete writes, descriptor metadata, truncate,
  sync, and offset-preserving SHA-256. Close files, locks and roots explicitly.
- sdk.digest.SHA256: incremental digest with explicit finalization and cleanup.
- sdk.process: environment, executable path, effective uid, umask, termination
  flag, resource limits, and bounded stdin. Limits affect the current process.
  Address-space limits apply on Linux; macOS does not provide the same guarantee.
- sdk.pkg.inspect(file, required_profile: ""): verified distribution/manifest facts for an open file. A supplied profile bounds inspection and must match the manifest before payload inspection. The manifest includes `native_payload` for extensions, providers, or requirements.
  No compilation, extraction, loading or execution of uploaded code occurs.
  Target facts include CPU features and compute backends. `compatibility` exports
  verified bytecode format versions, profile flag masks, ABI hashes and native
  extension ABI versions from the inventory-checked build metadata. Inspection
  preserves the file offset, including when reading build metadata.

This is a bundled SDK package, not a new VM builtin or registry-specific extension.
Its manifest can be merged into application builds; native leaves require ffi.
That grant is a trusted native-code boundary: the calling application controls
which filesystem roots and process operations to expose. These APIs do not
provide a sandbox for untrusted Sputnik code. The library contains no storage
layout, CAS policy, event spool, package admission or HTTP service logic.

Build the SDK first. OpenSSL, libarchive and zlib are required. The manifest
includes common Homebrew prefixes; installations elsewhere should supply their
include/library directories in the consuming build manifest.

    python3 packages/system_io/tests/run.py
    python3 packages/system_io/tests/run.py --native

The Python file is only an external development test harness. It creates
symlink/traversal fixtures and invokes the Sputnik selftest in both backends.
