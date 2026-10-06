# sputnik.package.v1

Status: `W9.4` package/registry/signing/lockfile tooling is implemented, and
`W9.5` hot reload consumes whole package artifacts through the runtime
package-swap API.

Implemented surface:

- restricted `sputnik.toml` parsing through
  [package/package.h](../../package/package.h:1);
- supported manifest sections: `[package]`, repeated `[[modules]]`, and
  `[dependencies]` with string versions;
- deterministic `sputnik.lock` rendering with sorted dependencies and stable
  SHA-256 checksums;
- reproducible text `.sputnikpkg` artifacts containing manifest identity,
  lockfile bytes, module `.sputnikbc` bytes as hex, module digests, and optional
  dev signatures;
- signature algorithm `sputnik-sha256-dev-v1`, intended only for deterministic
  local smoke coverage until real key management is introduced;
- artifact parse/inspect/verify APIs and JSON renderers;
- filesystem registry helpers for install/publish under
  `<registry>/<package>/<version>/`;
- `sputnik package-manifest`, `package-lock`, `package-build`,
  `package-inspect`, `package-verify`, `package-install`, and
  `package-publish`;
- `runtime::RuntimeWorld::reload_package_artifact` accepts a parsed package
  artifact as the reload unit, predecodes all embedded module bytes, enforces
  manifest identity plus public export, ABI/profile, and selector/arity
  compatibility, rejects frozen worlds, and publishes the replacement root
  module atomically.

`package-build` compiles each manifest module through the existing
lexer/parser/binder/HIR/bytecode path, verifies the serialized `.sputnikbc`, and
then packages those bytes. Source package names must match manifest module
names.

Current limits:

- the TOML parser intentionally accepts only the W9.4 manifest subset;
- signatures are deterministic SHA-256 developer signatures, not a public-key
  trust model;
- registry install/publish is filesystem-only and has no network protocol;
- package artifacts are not loaded directly by `RuntimeModuleLoader`; callers
  still unpack module bytes and map them by logical module id;
- hot reload is currently a focused runtime API path, not an `sputniktest`
  source-level package fixture phase.
