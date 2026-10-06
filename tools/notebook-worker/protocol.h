#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace amber::notebook::worker {
// Private, versioned host/worker protocol. Payloads are immutable bytes, never
// runtime Values/pointers. The limit includes all fields of a single message.
constexpr std::uint16_t kProtocolVersion = 8;
constexpr std::size_t kMaxMessageBytes = 8U * 1024U * 1024U;
enum class Kind : std::uint16_t {
  Ready = 1, Load, Source, Run, Apply, Shutdown, Started, Result, Error, Stop, Draining,
  SnapshotBegin, SnapshotChunk, Sync, LiveBegin, LiveChunk, LiveEnd
};
struct Message {
  Kind kind = Kind::Error;
  std::uint64_t generation = 0;
  std::uint64_t request = 0;
  std::vector<std::string> fields;
};
class ProtocolError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};
std::string encode(const Message &message);
Message decode(const std::string &frame);
std::uint64_t parse_id(const std::string &text);

// Exclusive owner-thread data transport. Partial reads survive timeouts;
// a partial write failure poisons the channel (retrying would corrupt framing).
// This FD is independent of Stop and of process termination.
class Channel {
public:
  explicit Channel(int fd);
  ~Channel();
  Channel(const Channel &) = delete;
  Channel &operator=(const Channel &) = delete;
  void send(const Message &message, std::chrono::milliseconds timeout);
  std::optional<Message> receive(std::chrono::milliseconds timeout);
private:
  int fd_;
  std::string pending_;
  bool poisoned_ = false;
};
void configure_socket(int fd);
} // namespace amber::notebook::worker
