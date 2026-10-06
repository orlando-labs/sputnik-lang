#pragma once
#include "tools/notebook-worker/protocol.h"
#include <memory>
#include <mutex>
#include <atomic>
#include <sys/types.h>

namespace amber::notebook::worker {
struct RequestId {
  std::uint64_t generation = 0;
  std::uint64_t request = 0;
};
// Data/lifecycle methods have one host owner. Only request_stop/force_stop may
// be called concurrently, including while send/receive/evaluation is blocked.
// No implicit restart, source replay or project writes.
class Process {
public:
  Process() = default;
  ~Process();
  Process(const Process &) = delete;
  Process &operator=(const Process &) = delete;
  void start(const std::string &executable);
  RequestId send(Kind kind, std::vector<std::string> fields = {});
  std::optional<Message> receive(std::chrono::milliseconds timeout);
  bool request_stop(RequestId request);
  bool request_force_stop(std::uint64_t expected_generation);
  // The only confirmation of termination. nullopt means still alive, even
  // after SIGKILL; the integer is the raw waitpid status.
  std::optional<int> poll_exit();
  std::uint64_t generation() const { return generation_; }
private:
  std::mutex control_mutex_;
  pid_t pid_ = -1;
  int control_ = -1;
  std::unique_ptr<Channel> channel_;
  std::uint64_t generation_ = 0;
  std::uint64_t next_request_ = 1;
  std::uint64_t in_flight_ = 0;
  std::atomic<std::uint64_t> last_sent_{0};
  std::optional<int> exit_;
};
constexpr int kDataFd = 198;
constexpr int kControlFd = 199;
} // namespace amber::notebook::worker
