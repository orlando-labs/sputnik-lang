# HTTP TLS

`net.http` accepts `http://` and `https://`. HTTPS defaults to port 443;
HTTP defaults to port 80. URL canonicalization, Host headers, streaming response
bodies, pooling, and redirects retain the scheme. Different schemes have
separate pool origins; redirects between origins remove credentials and rebuild Host.

HTTPS requires TLS 1.2 or newer, verifies the peer's certificate chain and the
URL hostname, and sends SNI for DNS hosts. IP literals use IP subject alternative
names instead of DNS matching or SNI. ALPN advertises HTTP/1.1. There is no
option to disable certificate verification. Unexpected TLS EOF is an error,
including for HTTP responses whose body is delimited by connection close.

```amber
from net.http import Client

client = Client(timeout: 10)
response = client.get("https://example.com/")
text = response.body_text()
client.close!()
```

The client uses OpenSSL's default trust store. `SSL_CERT_FILE` and
`SSL_CERT_DIR` can select another default store. Explicit client keywords are:

| Keyword | Meaning |
| --- | --- |
| `tls_ca_file` | PEM file containing trusted CA certificates |
| `tls_ca_path` | OpenSSL hashed CA directory |
| `tls_cert_file` | PEM client certificate chain for mutual TLS |
| `tls_key_file` | Corresponding unencrypted PEM private key |

Paths must be nonempty strings or null. Certificate and private key must be
provided together. Explicit CA paths replace the default trust store. Trust
is configured per client, so connections cannot be reused across clients with
different TLS settings.

```amber
from net.http import Server, ServerResponse

server = Server(
  host: "127.0.0.1", port: 8443,
  tls_cert_file: "server-chain.pem",
  tls_key_file: "server-key.pem")
server.serve() |request|:
  ServerResponse(status: 200, body: "hello over TLS")
```

Private keys must be unencrypted PEM files. The server loads the certificate
chain and checks the private key before
listening. Each admitted connection performs its handshake through the existing
reactor, using the read deadline and task cancellation. Handshake parking retains
its original deadline. Invalid TLS input fails the connection, not the listener.
Without certificate/key options the server continues to accept plaintext HTTP.

`TlsCertificateError < TlsError < HttpError` reports trust, hostname, or
certificate expiry failures. `TlsError` reports configuration and TLS protocol
failures. Socket connection failures and IO timeouts retain `ConnectionError`.
The existing `timeout` includes TCP connect and the TLS handshake, then applies
to each client read/write operation; DNS resolution is still synchronous.

## Native builds

Ordinary `Client` requests (static Str/Bytes bodies), response streams, and
`Server` use direct native dispatch. For a package exporting `main`:

```sh
build/amberc build client.am --target native --entry main-only \
  --require-full-native --grant net.connect -o client
```

Server programs use `--grant net.listen`. Request producers, Request snapshots,
and JSON/form client helpers retain their existing VM stdlib bridge in native
builds; those programs support HTTPS, but do not satisfy `--require-full-native`.

## Build and tests

Install OpenSSL development headers and libraries. The Makefile finds them
through `pkg-config`; `OPENSSL_PREFIX`, `OPENSSL_INCLUDE_DIR`, and
`OPENSSL_LIB_DIR` provide overrides. `amberc` propagates the discovered include
and library locations into native runtime compilation and linking.

```sh
make test-http-tls
```

The offline test creates a local CA and checks HTTPS clients and servers in VM
and `--require-full-native` builds, TLS 1.2/1.3, SNI, IP SANs, certificate expiry,
hostname mismatch, untrusted and missing CAs, mutual TLS, pooling, chunked bodies,
HTTP-to-HTTPS redirects, handshake timeout, cooperative server handshakes,
close_notify and truncated TLS input.
It requires Python 3 and the `openssl` CLI. The TLS 1.3 fixture is skipped
if Python's SSL backend lacks TLS 1.3 support.
