#include "runtime/tls.h"

#include "runtime/context.h"

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <sys/socket.h>
#include <vector>

namespace sputnik::runtime {
namespace {
RuntimeIoStatus ok(std::size_t count = 0) {
  RuntimeIoStatus status;
  status.ok = true;
  status.count = count;
  return status;
}

RuntimeIoStatus tls_error(const std::string &operation, SSL *ssl = nullptr) {
  RuntimeIoStatus status;
  status.error_name = "TlsError";
  status.message = operation;
  if (ssl != nullptr && SSL_get_verify_result(ssl) != X509_V_OK) {
    status.error_name = "TlsCertificateError";
    status.message += ": ";
    status.message += X509_verify_cert_error_string(SSL_get_verify_result(ssl));
  }
  unsigned long code;
  while ((code = ERR_get_error()) != 0) {
    char detail[256];
    ERR_error_string_n(code, detail, sizeof(detail));
    status.message += ": " + std::string(detail);
  }
  return status;
}

// OpenSSL's default socket BIO does not use MSG_NOSIGNAL on Linux. Keep
// SIGPIPE handling local to the socket rather than changing process signals.
BIO_METHOD *socket_bio_method() {
  static BIO_METHOD *method = [] {
    BIO_METHOD *result = BIO_meth_new(
        BIO_TYPE_SOURCE_SINK | BIO_get_new_index(), "Sputnik TLS socket");
    if (result == nullptr)
      return result;
    BIO_meth_set_create(result, [](BIO *bio) {
      BIO_set_init(bio, 1);
      return 1;
    });
    BIO_meth_set_destroy(result, [](BIO *) { return 1; });
    BIO_meth_set_read(result, [](BIO *bio, char *bytes, int size) {
      BIO_clear_retry_flags(bio);
      const int fd =
          static_cast<int>(reinterpret_cast<std::intptr_t>(BIO_get_data(bio)));
      const int count = static_cast<int>(::recv(fd, bytes, size, 0));
      if (count < 0 &&
          (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
        BIO_set_retry_read(bio);
      return count;
    });
    BIO_meth_set_write(result, [](BIO *bio, const char *bytes, int size) {
      BIO_clear_retry_flags(bio);
      const int fd =
          static_cast<int>(reinterpret_cast<std::intptr_t>(BIO_get_data(bio)));
#ifdef MSG_NOSIGNAL
      const int flags = MSG_NOSIGNAL;
#else
      const int flags = 0; // macOS streams already set SO_NOSIGPIPE.
#endif
      const int count = static_cast<int>(::send(fd, bytes, size, flags));
      if (count < 0 &&
          (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
        BIO_set_retry_write(bio);
      return count;
    });
    BIO_meth_set_ctrl(result, [](BIO *, int command, long, void *) -> long {
      return command == BIO_CTRL_FLUSH ? 1 : 0;
    });
    return result;
  }();
  return method;
}

int select_http_alpn(SSL *, const unsigned char **out, unsigned char *outlen,
                     const unsigned char *in, unsigned int inlen, void *) {
  static const unsigned char protocols[] = {8,   'h', 't', 't', 'p',
                                            '/', '1', '.', '1'};
  unsigned char *selected = nullptr;
  if (SSL_select_next_proto(&selected, outlen, protocols, sizeof(protocols), in,
                            inlen) != OPENSSL_NPN_NEGOTIATED) {
    return SSL_TLSEXT_ERR_ALERT_FATAL;
  }
  *out = selected;
  return SSL_TLSEXT_ERR_OK;
}
} // namespace

struct RuntimeTlsContext::Impl {
  SSL_CTX *ctx = nullptr;
  bool server = false;
  ~Impl() { SSL_CTX_free(ctx); }
};

RuntimeTlsContext::RuntimeTlsContext(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}
RuntimeTlsContext::~RuntimeTlsContext() = default;

std::shared_ptr<RuntimeTlsContext>
RuntimeTlsContext::create(const RuntimeTlsOptions &options, bool server,
                          RuntimeIoStatus *status) {
  ERR_clear_error();
  auto impl = std::make_unique<Impl>();
  impl->server = server;
  impl->ctx = SSL_CTX_new(server ? TLS_server_method() : TLS_client_method());
  if (impl->ctx == nullptr ||
      SSL_CTX_set_min_proto_version(impl->ctx, TLS1_2_VERSION) != 1) {
    *status = tls_error("TLS context initialization failed");
    return nullptr;
  }
  SSL_CTX_set_options(impl->ctx, SSL_OP_NO_COMPRESSION);
  // Configured keys must be unencrypted; never prompt on a runtime worker.
  SSL_CTX_set_default_passwd_cb(impl->ctx,
                                [](char *, int, int, void *) { return 0; });
  SSL_CTX_set_mode(impl->ctx, SSL_MODE_ENABLE_PARTIAL_WRITE |
                                  SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
  if (!server) {
    SSL_CTX_set_verify(impl->ctx, SSL_VERIFY_PEER, nullptr);
    const bool custom_ca = !options.ca_file.empty() || !options.ca_path.empty();
    if ((custom_ca
             ? SSL_CTX_load_verify_locations(
                   impl->ctx,
                   options.ca_file.empty() ? nullptr : options.ca_file.c_str(),
                   options.ca_path.empty() ? nullptr : options.ca_path.c_str())
             : SSL_CTX_set_default_verify_paths(impl->ctx)) != 1) {
      *status = tls_error("TLS trust store loading failed");
      return nullptr;
    }
  } else {
    SSL_CTX_set_alpn_select_cb(impl->ctx, select_http_alpn, nullptr);
  }
  if (options.cert_file.empty() != options.key_file.empty() ||
      (server && options.cert_file.empty())) {
    *status = tls_error("TLS requires both tls_cert_file and tls_key_file");
    return nullptr;
  }
  if (!options.cert_file.empty() &&
      (SSL_CTX_use_certificate_chain_file(impl->ctx,
                                          options.cert_file.c_str()) != 1 ||
       SSL_CTX_use_PrivateKey_file(impl->ctx, options.key_file.c_str(),
                                   SSL_FILETYPE_PEM) != 1 ||
       SSL_CTX_check_private_key(impl->ctx) != 1)) {
    *status = tls_error("TLS certificate/private key loading failed");
    return nullptr;
  }
  *status = ok();
  return std::shared_ptr<RuntimeTlsContext>(
      new RuntimeTlsContext(std::move(impl)));
}

struct RuntimeTlsSession::Impl {
  std::shared_ptr<RuntimeTlsContext> context;
  SSL *ssl = nullptr;
  std::mutex mutex;
  bool ready = false;
  bool failed = false;
  Deadline handshake_deadline;
  bool handshake_started = false;
  ~Impl() { SSL_free(ssl); }

  RuntimeIoStatus retry(int result, bool nonblocking, Deadline deadline,
                        const Wait &wait, const char *operation,
                        std::unique_lock<std::mutex> &lock) {
    // SSL_get_error must immediately follow the SSL operation on this thread.
    const int error = SSL_get_error(ssl, result);
    if (error == SSL_ERROR_ZERO_RETURN) {
      RuntimeIoStatus status = ok();
      status.eof = true;
      return status;
    }
    if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
      if (nonblocking) {
        RuntimeIoStatus status;
        status.would_block = true;
        return status;
      }
      lock.unlock();
      return wait(error == SSL_ERROR_WANT_WRITE, deadline);
    }
    failed = true;
    RuntimeIoStatus status = tls_error(operation, ssl);
    if (error == SSL_ERROR_SYSCALL && errno != 0) {
      status.message += ": " + std::string(std::strerror(errno));
    }
    return status;
  }
};

RuntimeTlsSession::RuntimeTlsSession(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}
RuntimeTlsSession::~RuntimeTlsSession() = default;

std::unique_ptr<RuntimeTlsSession>
RuntimeTlsSession::create(int fd, std::shared_ptr<RuntimeTlsContext> context,
                          const std::string &host, RuntimeIoStatus *status) {
  ERR_clear_error();
  auto impl = std::make_unique<Impl>();
  impl->context = std::move(context);
  impl->ssl = SSL_new(impl->context->impl_->ctx);
  BIO *bio =
      socket_bio_method() == nullptr ? nullptr : BIO_new(socket_bio_method());
  if (impl->ssl == nullptr || bio == nullptr) {
    BIO_free(bio);
    *status = tls_error("TLS session initialization failed");
    return nullptr;
  }
  BIO_set_data(bio, reinterpret_cast<void *>(static_cast<std::intptr_t>(fd)));
  SSL_set_bio(impl->ssl, bio, bio);
  if (impl->context->impl_->server) {
    SSL_set_accept_state(impl->ssl);
  } else {
    SSL_set_connect_state(impl->ssl);
    std::string peer = host;
    if (peer.size() > 2 && peer.front() == '[' && peer.back() == ']') {
      peer = peer.substr(1, peer.size() - 2);
    }
    if (peer.empty() || peer.find('\0') != std::string::npos) {
      *status = tls_error("TLS peer name is invalid");
      return nullptr;
    }
    X509_VERIFY_PARAM *param = SSL_get0_param(impl->ssl);
    X509_VERIFY_PARAM_set_hostflags(param,
                                    X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
    // IP literals are checked against iPAddress SANs and never sent as SNI.
    const bool ip = X509_VERIFY_PARAM_set1_ip_asc(param, peer.c_str()) == 1;
    if (!ip && (SSL_set1_host(impl->ssl, peer.c_str()) != 1 ||
                SSL_set_tlsext_host_name(impl->ssl, peer.c_str()) != 1)) {
      *status = tls_error("TLS peer name configuration failed");
      return nullptr;
    }
    static const unsigned char protocols[] = {8,   'h', 't', 't', 'p',
                                              '/', '1', '.', '1'};
    if (SSL_set_alpn_protos(impl->ssl, protocols, sizeof(protocols)) != 0) {
      *status = tls_error("TLS ALPN configuration failed");
      return nullptr;
    }
  }
  *status = ok();
  return std::unique_ptr<RuntimeTlsSession>(
      new RuntimeTlsSession(std::move(impl)));
}

RuntimeIoStatus RuntimeTlsSession::handshake(Deadline deadline,
                                             const Wait &wait) {
  std::unique_lock<std::mutex> lock(impl_->mutex);
  if (impl_->ready && !impl_->failed)
    return ok();
  if (impl_->failed)
    return tls_error("TLS session has failed");
  if (!impl_->handshake_started) {
    impl_->handshake_started = true;
    impl_->handshake_deadline = deadline;
  }
  for (;;) {
    if (!lock.owns_lock())
      lock.lock();
    if (impl_->failed)
      return tls_error("TLS session has failed");
    ERR_clear_error();
    errno = 0;
    const int result = SSL_do_handshake(impl_->ssl);
    if (result == 1) {
      impl_->ready = true;
      return ok();
    }
    RuntimeIoStatus status =
        impl_->retry(result, false, impl_->handshake_deadline, wait,
                     "TLS handshake failed", lock);
    if (status.eof) {
      impl_->failed = true;
      return tls_error("peer closed during TLS handshake");
    }
    if (!status.ok)
      return status;
  }
}

RuntimeIoStatus RuntimeTlsSession::read(RuntimeByteBuffer &buffer,
                                        bool nonblocking, Deadline deadline,
                                        const Wait &wait) {
  if (buffer.read_mode() || buffer.remaining() == 0) {
    RuntimeIoStatus status;
    status.error_name = "ArgumentError";
    status.message = "read buffer has no remaining capacity";
    return status;
  }
  RuntimeIoStatus access = buffer.access_status();
  if (!access.ok)
    return access;
  Wait handshake_wait = nonblocking ? Wait([](bool, Deadline) {
    RuntimeIoStatus status;
    status.would_block = true;
    return status;
  })
                                    : wait;
  RuntimeIoStatus status = handshake(deadline, handshake_wait);
  if (!status.ok || status.eof)
    return status;
  std::vector<unsigned char> bytes(
      std::min<std::size_t>(buffer.remaining(), 65536));
  for (;;) {
    std::unique_lock<std::mutex> lock(impl_->mutex);
    if (impl_->failed)
      return tls_error("TLS session has failed");
    ERR_clear_error();
    errno = 0;
    std::size_t count = 0;
    const int result =
        SSL_read_ex(impl_->ssl, bytes.data(), bytes.size(), &count);
    if (result == 1) {
      buffer.append(bytes.data(), count);
      return ok(count);
    }
    status = impl_->retry(result, nonblocking, deadline, wait,
                          "TLS read failed", lock);
    if (!status.ok || status.eof)
      return status;
  }
}

RuntimeIoStatus RuntimeTlsSession::write(const std::string &bytes,
                                         bool nonblocking, Deadline deadline,
                                         const Wait &wait) {
  Wait handshake_wait = nonblocking ? Wait([](bool, Deadline) {
    RuntimeIoStatus status;
    status.would_block = true;
    return status;
  })
                                    : wait;
  RuntimeIoStatus status = handshake(deadline, handshake_wait);
  if (!status.ok || status.eof)
    return status;
  if (bytes.empty())
    return ok();
  for (;;) {
    std::unique_lock<std::mutex> lock(impl_->mutex);
    if (impl_->failed)
      return tls_error("TLS session has failed");
    ERR_clear_error();
    errno = 0;
    std::size_t count = 0;
    const int result =
        SSL_write_ex(impl_->ssl, bytes.data(), bytes.size(), &count);
    if (result == 1)
      return ok(count);
    status = impl_->retry(result, nonblocking, deadline, wait,
                          "TLS write failed", lock);
    if (!status.ok || status.eof)
      return status;
  }
}

void RuntimeTlsSession::close() {
  // Best-effort close_notify on the nonblocking socket; never wait for the peer
  // during resource teardown. Unclean peer EOF remains an error on reads.
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->ready && !impl_->failed) {
    ERR_clear_error();
    (void)SSL_shutdown(impl_->ssl);
  }
  impl_->failed = true;
}
} // namespace sputnik::runtime
