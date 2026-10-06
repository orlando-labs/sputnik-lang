#include "tools/notebook-worker/process.h"
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
extern char **environ;

namespace amber::notebook::worker {
namespace {
std::atomic<std::uint64_t> generations{1};
struct Fds {
  int values[2]{-1, -1};
  ~Fds() { for (int fd : values) if (fd >= 0) ::close(fd); }
  void make(int type) {
    if (::socketpair(AF_UNIX, type, 0, values) < 0) throw ProtocolError("worker socketpair failed");
    for (int &fd : values) {
      // Child dup destinations cannot alias either endpoint, even in a host
      // with hundreds of open descriptors. All originals are close-on-exec.
      const int moved = ::fcntl(fd, F_DUPFD_CLOEXEC, 200);
      if (moved < 0) throw ProtocolError("worker descriptor allocation failed");
      ::close(fd); fd = moved;
      configure_socket(fd);
    }
  }
};
} // namespace
Process::~Process() {
  request_force_stop(generation_);
  // A stuck OS process must not freeze UI teardown. Retain exact child
  // ownership in a reaper until waitpid confirms death, without retaining UI.
  try { (void)poll_exit(); } catch (...) { /* Already relinquished PID ownership. */ }
  if (pid_ > 0) {
    const auto child = pid_;
    std::thread([child] { int status; while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {} }).detach();
  }
  if (control_ >= 0) ::close(control_);
}
void Process::start(const std::string &executable) {
  std::lock_guard<std::mutex> lock(control_mutex_);
  if (pid_ > 0) throw ProtocolError("worker has not exited; confirm exit before restarting");
  if (executable.empty() || executable[0] != '/') throw ProtocolError("worker executable must be absolute");
  Fds data, control;
  data.make(SOCK_STREAM); control.make(SOCK_DGRAM);
  const int data_fd = data.values[0]; data.values[0] = -1;
  auto next_channel = std::make_unique<Channel>(data_fd);
  const auto next_generation = generations.fetch_add(1);
  if (!next_generation) throw ProtocolError("worker generation exhausted");
  std::string generation = std::to_string(next_generation);
  const char *args[]{executable.c_str(), "--notebook-worker", generation.c_str(), nullptr};
  posix_spawn_file_actions_t actions;
  if (posix_spawn_file_actions_init(&actions) != 0) throw ProtocolError("spawn actions init failed");
  int rc = posix_spawn_file_actions_adddup2(&actions, data.values[1], kDataFd);
  if (!rc) rc = posix_spawn_file_actions_adddup2(&actions, control.values[1], kControlFd);
  if (!rc) rc = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
  if (!rc) rc = posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
  pid_t child = -1;
  if (!rc) rc = ::posix_spawn(&child, executable.c_str(), &actions, nullptr, const_cast<char **>(args), environ);
  posix_spawn_file_actions_destroy(&actions);
  if (rc) throw ProtocolError(std::string("start notebook worker: ") + std::strerror(rc));
  channel_ = std::move(next_channel);
  control_ = control.values[0]; control.values[0] = -1;
  pid_ = child; generation_ = next_generation;
  in_flight_ = 0; next_request_ = 1; exit_.reset();
  last_sent_.store(0);
}
RequestId Process::send(Kind kind, std::vector<std::string> fields) {
  if (!channel_ || exit_) throw ProtocolError("worker is not running");
  if (in_flight_) throw ProtocolError("worker request already in flight");
  if (next_request_ == std::numeric_limits<std::uint64_t>::max()) throw ProtocolError("worker request id exhausted");
  const auto id = next_request_++;
  channel_->send({kind, generation_, id, std::move(fields)}, std::chrono::seconds(2));
  in_flight_ = id;
  last_sent_.store(id);
  return {generation_, id};
}
std::optional<Message> Process::receive(std::chrono::milliseconds timeout) {
  if (!channel_) throw ProtocolError("worker is not started");
  auto message = channel_->receive(timeout);
  if (message) {
    if (message->generation != generation_) throw ProtocolError("stale worker generation");
    if (message->kind == Kind::Ready) {
      if (message->request != 0) throw ProtocolError("invalid worker handshake");
    } else {
      if (!in_flight_ || message->request != in_flight_) throw ProtocolError("stale worker reply");
      if (message->kind == Kind::Result || message->kind == Kind::Error) in_flight_ = 0;
      else if (message->kind != Kind::Started && message->kind != Kind::Draining &&
               message->kind != Kind::SnapshotBegin && message->kind != Kind::SnapshotChunk &&
               message->kind != Kind::LiveBegin && message->kind != Kind::LiveChunk &&
               message->kind != Kind::LiveEnd)
        throw ProtocolError("unexpected worker reply");
    }
  }
  return message;
}
bool Process::request_stop(RequestId request) {
  std::lock_guard<std::mutex> lock(control_mutex_);
  if (pid_ <= 0 || !request.request || control_ < 0 ||
      request.generation != generation_ || request.request > last_sent_.load()) return false;
  const auto bytes = encode({Kind::Stop, generation_, request.request, {}});
#ifdef MSG_NOSIGNAL
  constexpr int flags = MSG_NOSIGNAL;
#else
  constexpr int flags = 0;
#endif
  return ::send(control_, bytes.data(), bytes.size(), flags) == static_cast<ssize_t>(bytes.size());
}
bool Process::request_force_stop(std::uint64_t expected_generation) {
  std::lock_guard<std::mutex> lock(control_mutex_);
  // Never signal a process group or a PID that we have reaped/relinquished.
  return pid_ > 0 && expected_generation == generation_ &&
         (::kill(pid_, SIGKILL) == 0 || errno == ESRCH);
}
std::optional<int> Process::poll_exit() {
  std::lock_guard<std::mutex> lock(control_mutex_);
  if (exit_ || pid_ <= 0) return exit_;
  int status = 0;
  const auto result = ::waitpid(pid_, &status, WNOHANG);
  if (result == pid_) {
    exit_ = status; pid_ = -1;
    if (control_ >= 0) ::close(control_);
    control_ = -1;
  } else if (result < 0 && errno != EINTR) {
    // Losing ownership is not proof of a successful stop. Invalidate the PID
    // before reporting the error so a later Force Stop cannot hit a reused PID.
    pid_ = -1;
    if (control_ >= 0) ::close(control_);
    control_ = -1;
    throw ProtocolError("notebook worker waitpid failed");
  }
  return exit_;
}
} // namespace amber::notebook::worker
