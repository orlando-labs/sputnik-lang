# Signature

`Signature` signs and verifies binary messages. Algorithm names are `Symbol` or
`Str`; keys, messages, and signatures are explicit `Bytes` (also accepting
`ByteSlice`/`ByteBuffer` inputs). There is no implicit string encoding.

```amber
keys = Signature.generate(:ed25519)
message = Bytes.new("amber release metadata")
signature = Signature.sign(:ed25519, keys["private_key"], message)
Signature.verify(:ed25519, keys["public_key"], message, signature)
```

## API

- `Signature.available?(algorithm) -> Bool` — known algorithm with an enabled
  backend in this build.
- `Signature.generate(algorithm) -> Map` — `private_key` and `public_key` Bytes.
- `Signature.public_key(algorithm, private_key) -> Bytes` — derive the public
  key from a private key.
- `Signature.sign(algorithm, private_key, message) -> Bytes`.
- `Signature.verify(algorithm, public_key, message, signature) -> Bool` — false
  for an invalid signature or changed message; a malformed key raises an
  `ArgumentError`.

| Algorithm | Hash / curve | Private key | Public key | Signature |
| --- | --- | --- | --- | --- |
| `ed25519` | Ed25519 (pure) | 32-byte seed | 32 raw bytes | 64 raw bytes |
| `ed448` | Ed448 (pure) | 57-byte seed | 57 raw bytes | 114 raw bytes |
| `ecdsa_p256_sha256` | P-256 / SHA-256 | DER PKCS#8 | DER SPKI | DER ECDSA |
| `ecdsa_p384_sha384` | P-384 / SHA-384 | DER PKCS#8 | DER SPKI | DER ECDSA |
| `rsa_pss_sha256` | RSA-PSS / SHA-256 | DER PKCS#8 | DER SPKI | raw RSA |
| `rsa_pss_sha384` | RSA-PSS / SHA-384 | DER PKCS#8 | DER SPKI | raw RSA |
| `ml_dsa_44` | ML-DSA-44 | DER PKCS#8 | DER SPKI | raw ML-DSA |
| `ml_dsa_65` | ML-DSA-65 | DER PKCS#8 | DER SPKI | raw ML-DSA |
| `ml_dsa_87` | ML-DSA-87 | DER PKCS#8 | DER SPKI | raw ML-DSA |
| `gost2012_256` | GOST R 34.10-2012 / Streebog-256, TC26 256 paramSetB | 32-byte scalar | 32-byte X + 32-byte Y | 32-byte R + 32-byte S |
| `gost2012_512` | GOST R 34.10-2012 / Streebog-512, TC26 512 paramSetA | 64-byte scalar | 64-byte X + 64-byte Y | 64-byte R + 64-byte S |

All GOST scalar, coordinate, and signature components above are fixed-width
big-endian integers. They are **not** the X.509/PKCS#8 GOST encodings, which
use different wrapping and byte-order conventions. Conversion to those formats
is outside this initial API; do not feed Amber's raw GOST keys directly into
certificate tools.

EdDSA is pure mode (not Ed25519ph/Ed448ph); ML-DSA uses its pure mode without
a context string. RSA generation uses 3072-bit keys;
verification rejects RSA keys below 2048 bits. RSA-PSS uses MGF1 with the same
hash and a digest-length salt. `generate` and randomized `sign` operations
(ECDSA, RSA-PSS, ML-DSA, GOST) require the `random.secure` capability and are
unavailable during deterministic replay; pure EdDSA signing, `public_key`, and
`verify` do not need entropy. Keep private-key Bytes secret and
do not log them.

OpenSSL provides EdDSA, ECDSA, and RSA-PSS. ML-DSA needs OpenSSL 3.5 or newer
with a provider that implements it; `available?` reports its actual presence.
GOST is enabled when the project is
built with Nettle/hogweed and GMP detected by `pkg-config`; otherwise
`Signature.available?(:gost2012_256)` and `...512` return false, and using
those algorithms raises `ArgumentError`. The Nettle backend rejects digest
values whose integer representation is zero modulo the curve order before
signing; affected Nettle versions can otherwise expose the private key.

`amberc build --target native` compiles every `Signature` operation through
direct native code generation. The native smoke suite requires full coverage,
VM independence, and absence of bytecode fallback across every available
algorithm.

Standards: [EdDSA (RFC 8032)](https://www.rfc-editor.org/rfc/rfc8032.html),
[GOST R 34.10-2012 (RFC 7091)](https://www.rfc-editor.org/rfc/rfc7091.html),
[GOST curve/X.509 identifiers (RFC 9215)](https://www.rfc-editor.org/rfc/rfc9215.html),
and [ML-DSA (FIPS 204)](https://csrc.nist.gov/pubs/fips/204/final).
