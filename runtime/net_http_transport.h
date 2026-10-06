#pragma once

// HTTP(S) connector for the VM-independent exchange/streaming transport seam.
// TLS sessions live on RuntimeTcpStream, so HTTP framing and pooling are
// shared.

#include "runtime/io.h"
#include "runtime/net_http.h"
#include "runtime/tls.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace sputnik::runtime::http {

// HttpTransport backed by a connected RuntimeTcpStream. Reads use a reusable
// internal ByteBuffer; the per-operation timeout applies to each read/write.
class TcpHttpTransport : public HttpTransport {
public:
  explicit TcpHttpTransport(
      std::shared_ptr<RuntimeTcpStream> stream,
      std::chrono::milliseconds timeout = std::chrono::milliseconds::max());

  bool write_all(const std::string &data, std::string *error) override;
  long read_some(std::string *chunk, std::string *error) override;
  void close() override;
  HttpErrorKind error_kind() const override { return error_kind_; }

  const std::shared_ptr<RuntimeTcpStream> &stream() const { return stream_; }

private:
  std::shared_ptr<RuntimeTcpStream> stream_;
  std::chrono::milliseconds timeout_;
  RuntimeByteBuffer read_buffer_;
  HttpErrorKind error_kind_ = HttpErrorKind::Connection;
};

// Connect a plaintext TCP transport to host:port (the connect phase of an
// exchange). On failure returns nullptr and sets *kind (Connection) / *error
// with the underlying transport detail. DNS happens inside connect; cancellable
// DNS / open_timeout is deferred per §30.4.
std::unique_ptr<TcpHttpTransport>
http_tcp_connect(const std::string &host, std::uint16_t port,
                 std::chrono::milliseconds timeout, HttpErrorKind *kind,
                 std::string *error);

// HTTPS uses the same transport seam, streaming codec and connection leases.
std::unique_ptr<HttpTransport> http_connect(const HttpRequest &request,
                                            std::chrono::milliseconds timeout,
                                            const RuntimeTlsOptions &tls,
                                            HttpErrorKind *kind,
                                            std::string *error);

} // namespace sputnik::runtime::http
