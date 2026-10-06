#include "tools/notebook-worker/protocol.h"
#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace amber::notebook::worker {
namespace {
using Clock = std::chrono::steady_clock;
void append(std::string &bytes, std::uint64_t value, unsigned count) {
  for (unsigned i = count; i > 0; --i)
    bytes.push_back(static_cast<char>(value >> ((i - 1U) * 8U)));
}
std::uint64_t read(const std::string &bytes, std::size_t &offset, unsigned count) {
  if (count > bytes.size() - offset) throw ProtocolError("truncated worker frame");
  std::uint64_t value = 0;
  for (unsigned i = 0; i < count; ++i)
    value = (value << 8U) | static_cast<unsigned char>(bytes[offset++]);
  return value;
}
std::size_t frame_size(const std::string &bytes) {
  std::size_t offset = 0;
  const auto length = read(bytes, offset, 4);
  if (length < 24 || length > kMaxMessageBytes)
    throw ProtocolError("invalid worker frame size");
  return static_cast<std::size_t>(length) + 4U;
}
bool wait_fd(int fd, short events, Clock::time_point deadline) {
  while (true) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
    pollfd item{fd, events, 0};
    const int rc = ::poll(&item, 1, static_cast<int>(std::clamp<std::int64_t>(left.count(), 0, 60000)));
    if (rc > 0) {
      if (item.revents & POLLNVAL) throw ProtocolError("invalid worker channel");
      return true; // recv/send reports EOF/error, including HUP with data.
    }
    if (rc < 0 && errno != EINTR) throw ProtocolError(std::strerror(errno));
    if (Clock::now() >= deadline) return false;
  }
}
} // namespace

std::uint64_t parse_id(const std::string &text) {
  std::uint64_t value = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value == 0)
    throw ProtocolError("invalid worker identity");
  return value;
}
std::string encode(const Message &message) {
  if (message.fields.size() > 65536) throw ProtocolError("too many worker fields");
  std::size_t size = 24;
  for (const auto &field : message.fields) {
    if (field.size() > kMaxMessageBytes - 4 || size > kMaxMessageBytes - 4 - field.size())
      throw ProtocolError("worker message exceeds size limit");
    size += field.size() + 4;
  }
  std::string bytes;
  bytes.reserve(size + 4);
  append(bytes, size, 4);
  append(bytes, kProtocolVersion, 2);
  append(bytes, static_cast<std::uint16_t>(message.kind), 2);
  append(bytes, message.generation, 8);
  append(bytes, message.request, 8);
  append(bytes, message.fields.size(), 4);
  for (const auto &field : message.fields) {
    append(bytes, field.size(), 4);
    bytes += field;
  }
  return bytes;
}
Message decode(const std::string &bytes) {
  if (bytes.size() < 4 || frame_size(bytes) != bytes.size())
    throw ProtocolError("truncated or trailing worker frame");
  std::size_t offset = 4;
  if (read(bytes, offset, 2) != kProtocolVersion)
    throw ProtocolError("unsupported worker protocol version");
  const auto kind = read(bytes, offset, 2);
  if (kind < 1 || kind > static_cast<unsigned>(Kind::LiveEnd)) throw ProtocolError("unknown worker message");
  Message result;
  result.kind = static_cast<Kind>(kind);
  result.generation = read(bytes, offset, 8);
  result.request = read(bytes, offset, 8);
  const auto count = read(bytes, offset, 4);
  if (count > 65536 || count > (bytes.size() - offset) / 4)
    throw ProtocolError("invalid worker field count");
  for (std::uint64_t i = 0; i < count; ++i) {
    const auto length = read(bytes, offset, 4);
    if (length > bytes.size() - offset) throw ProtocolError("truncated worker field");
    result.fields.push_back(bytes.substr(offset, length));
    offset += length;
  }
  if (offset != bytes.size()) throw ProtocolError("trailing worker fields");
  return result;
}
void configure_socket(int fd) {
  const int flags = ::fcntl(fd, F_GETFL);
  if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ||
      ::fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) throw ProtocolError("configure worker socket failed");
#ifdef SO_NOSIGPIPE
  int yes = 1;
  if (::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes)) < 0)
    throw ProtocolError("configure worker SIGPIPE protection failed");
#endif
}
Channel::Channel(int fd) : fd_(fd) {
  try { configure_socket(fd); } catch (...) { ::close(fd_); throw; }
}
Channel::~Channel() { ::close(fd_); }
void Channel::send(const Message &message, std::chrono::milliseconds timeout) {
  if (poisoned_) throw ProtocolError("worker channel is unusable");
  const auto bytes = encode(message);
  const auto deadline = Clock::now() + timeout;
  std::size_t offset = 0;
  try {
    while (offset < bytes.size()) {
      if (!wait_fd(fd_, POLLOUT, deadline)) throw ProtocolError("worker send timed out");
#ifdef MSG_NOSIGNAL
      constexpr int flags = MSG_NOSIGNAL;
#else
      constexpr int flags = 0;
#endif
      const auto n = ::send(fd_, bytes.data() + offset, bytes.size() - offset, flags);
      if (n > 0) offset += static_cast<std::size_t>(n);
      else if (n == 0 || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK))
        throw ProtocolError("worker channel closed while sending");
    }
  } catch (...) { poisoned_ = true; throw; }
}
std::optional<Message> Channel::receive(std::chrono::milliseconds timeout) {
  if (poisoned_) throw ProtocolError("worker channel is unusable");
  const auto deadline = Clock::now() + timeout;
  try {
    while (true) {
      const std::size_t wanted = pending_.size() < 4 ? 4 : frame_size(pending_);
      if (pending_.size() == wanted && wanted > 4) {
        auto message = decode(pending_);
        pending_.clear();
        return message;
      }
      if (!wait_fd(fd_, POLLIN, deadline)) return std::nullopt;
      char buffer[16384];
      const auto n = ::recv(fd_, buffer, std::min(sizeof(buffer), wanted - pending_.size()), 0);
      if (n > 0) pending_.append(buffer, static_cast<std::size_t>(n));
      else if (n == 0) throw ProtocolError(pending_.empty() ? "worker channel closed" : "truncated worker frame at EOF");
      else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
        throw ProtocolError("worker receive failed");
    }
  } catch (...) { poisoned_ = true; throw; }
}
} // namespace amber::notebook::worker
