#pragma once

#include "runtime/io.h"

#include <functional>

namespace amber::runtime {

// PEM files. Empty CA paths use OpenSSL's default trust store (including
// SSL_CERT_FILE/SSL_CERT_DIR). Peer verification is always enabled for clients.
struct RuntimeTlsOptions {
  std::string ca_file;
  std::string ca_path;
  std::string cert_file;
  std::string key_file;
};

class RuntimeTlsContext {
public:
  static std::shared_ptr<RuntimeTlsContext>
  create(const RuntimeTlsOptions &options, bool server,
         RuntimeIoStatus *status);
  ~RuntimeTlsContext();

private:
  struct Impl;
  explicit RuntimeTlsContext(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
  friend class RuntimeTlsSession;
};

// Sessions are confined with their owning TCP stream. The wait callback uses
// the IO reactor, including cooperative parking, cancellation and deadlines.
class RuntimeTlsSession {
public:
  using Deadline = std::optional<std::chrono::steady_clock::time_point>;
  using Wait = std::function<RuntimeIoStatus(bool write, Deadline deadline)>;
  static std::unique_ptr<RuntimeTlsSession>
  create(int fd, std::shared_ptr<RuntimeTlsContext> context,
         const std::string &host, RuntimeIoStatus *status);
  ~RuntimeTlsSession();
  RuntimeIoStatus handshake(Deadline deadline, const Wait &wait);
  RuntimeIoStatus read(RuntimeByteBuffer &buffer, bool nonblocking,
                       Deadline deadline, const Wait &wait);
  RuntimeIoStatus write(const std::string &bytes, bool nonblocking,
                        Deadline deadline, const Wait &wait);
  void close();

private:
  struct Impl;
  explicit RuntimeTlsSession(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

} // namespace amber::runtime
