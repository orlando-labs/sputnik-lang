#include "runtime/net_http_transport.h"

#include <algorithm>
#include <string>
#include <utility>

namespace amber::runtime::http {

namespace {

// Prefer the runtime error class name, fall back to the human message.
std::string io_detail(const RuntimeIoStatus &status, const char *fallback) {
  if (!status.error_name.empty()) {
    return status.message.empty() ? status.error_name
                                  : status.error_name + ": " + status.message;
  }
  if (!status.message.empty()) {
    return status.message;
  }
  return fallback;
}

} // namespace

TcpHttpTransport::TcpHttpTransport(std::shared_ptr<RuntimeTcpStream> stream,
                                   std::chrono::milliseconds timeout)
    : stream_(std::move(stream)), timeout_(timeout), read_buffer_(64U * 1024U) {
}

bool TcpHttpTransport::write_all(const std::string &data, std::string *error) {
  const RuntimeIoStatus status = stream_->write_all(data, timeout_);
  if (!status.ok) {
    error_kind_ = status.error_name == "TlsCertificateError"
                      ? HttpErrorKind::TlsCertificate
                  : status.error_name == "TlsError" ? HttpErrorKind::Tls
                                                    : HttpErrorKind::Connection;
    *error = io_detail(status, "failed to write request");
    return false;
  }
  return true;
}

long TcpHttpTransport::read_some(std::string *chunk, std::string *error) {
  chunk->clear();
  read_buffer_.adopt_to_current_owner();
  const RuntimeIoStatus cleared = read_buffer_.clear();
  if (!cleared.ok) {
    *error = io_detail(cleared, "read buffer reset failed");
    return -1;
  }
  const RuntimeIoStatus status = stream_->read(read_buffer_, timeout_);
  if (!status.ok && !status.eof) {
    error_kind_ = status.error_name == "TlsCertificateError"
                      ? HttpErrorKind::TlsCertificate
                  : status.error_name == "TlsError" ? HttpErrorKind::Tls
                                                    : HttpErrorKind::Connection;
    *error = io_detail(status, "failed to read response");
    return -1;
  }
  std::string data = read_buffer_.bytes();
  if (status.eof || data.empty()) {
    return 0; // peer closed
  }
  *chunk = std::move(data);
  return static_cast<long>(chunk->size());
}

void TcpHttpTransport::close() {
  if (stream_ != nullptr) {
    stream_->close();
  }
}

std::unique_ptr<TcpHttpTransport>
http_tcp_connect(const std::string &host, std::uint16_t port,
                 std::chrono::milliseconds timeout, HttpErrorKind *kind,
                 std::string *error) {
  const RuntimeEndpoint endpoint(host, port);
  RuntimeTcpConnectResult result = RuntimeTcpStream::connect(
      endpoint, timeout, RuntimeIsolationMode::Unchecked);
  if (!result.ok || result.stream == nullptr) {
    *kind = HttpErrorKind::Connection;
    *error = io_detail(result, "connection failed");
    return nullptr;
  }
  return std::make_unique<TcpHttpTransport>(std::move(result.stream), timeout);
}

std::unique_ptr<HttpTransport> http_connect(const HttpRequest &request,
                                            std::chrono::milliseconds timeout,
                                            const RuntimeTlsOptions &tls,
                                            HttpErrorKind *kind,
                                            std::string *error) {
  const auto start = std::chrono::steady_clock::now();
  auto transport =
      http_tcp_connect(request.host, request.port, timeout, kind, error);
  if (transport == nullptr || request.scheme != "https")
    return transport;
  RuntimeIoStatus status;
  auto context = RuntimeTlsContext::create(tls, false, &status);
  if (context != nullptr)
    status = transport->stream()->start_tls(context, request.host);
  if (status.ok) {
    const auto remaining =
        timeout == std::chrono::milliseconds::max()
            ? timeout
            : std::max(
                  std::chrono::milliseconds(0),
                  timeout -
                      std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - start));
    status = transport->stream()->tls_handshake(remaining);
  }
  if (!status.ok || status.eof) {
    *kind = status.error_name == "TlsCertificateError"
                ? HttpErrorKind::TlsCertificate
            : status.error_name == "TlsError" ? HttpErrorKind::Tls
                                              : HttpErrorKind::Connection;
    *error = io_detail(status, "TLS handshake failed");
    transport->close();
    return nullptr;
  }
  return transport;
}

} // namespace amber::runtime::http
