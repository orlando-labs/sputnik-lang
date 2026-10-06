#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "runtime/system.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <fcntl.h>
#include <future>
#include <limits>
#include <mutex>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char **environ;

namespace sputnik::runtime {
namespace {
thread_local std::shared_ptr<std::recursive_mutex> callback_gate;
thread_local unsigned callback_depth = 0;
thread_local const std::atomic<bool> *callback_cancel = nullptr;
} // namespace
const std::atomic<bool> *system_callback_cancel_flag() {
  return callback_cancel;
}
SystemCallbackScope::SystemCallbackScope(
    std::shared_ptr<std::recursive_mutex> gate, const std::atomic<bool> *cancel)
    : previous_(callback_gate), gate_(std::move(gate)),
      previous_depth_(callback_depth), previous_cancel_(callback_cancel) {
  if (cancel)
    callback_cancel = cancel;
  if (gate_) {
    gate_->lock();
    callback_depth = gate_ == previous_ ? previous_depth_ + 1 : 1;
    callback_gate = gate_;
  }
}
SystemCallbackScope::~SystemCallbackScope() {
  callback_cancel = previous_cancel_;
  if (gate_) {
    callback_gate = previous_;
    callback_depth = previous_depth_;
    gate_->unlock();
  }
}
SystemCallbackSuspension::SystemCallbackSuspension(bool enabled) {
  if (enabled && callback_gate) {
    gate_ = callback_gate;
    depth_ = callback_depth;
    callback_depth = 0;
    for (unsigned i = 0; i < depth_; ++i)
      gate_->unlock();
  }
}
SystemCallbackSuspension::~SystemCallbackSuspension() {
  for (unsigned i = 0; i < depth_; ++i)
    gate_->lock();
  if (gate_)
    callback_depth = depth_;
}
SystemValue SystemValue::str(std::string text, bool bytes) {
  SystemValue v;
  v.kind = bytes ? Kind::Bytes : Kind::String;
  v.text = std::move(text);
  return v;
}
SystemValue SystemValue::number(std::int64_t n) {
  SystemValue v;
  v.kind = Kind::Int;
  v.integer = n;
  return v;
}
SystemValue SystemValue::boolean_value(bool b) {
  SystemValue v;
  v.kind = Kind::Bool;
  v.boolean = b;
  return v;
}
SystemValue SystemValue::resource(std::shared_ptr<RuntimeIoValue> p) {
  SystemValue v;
  v.kind = Kind::Object;
  v.object = std::move(p);
  return v;
}
SystemValue SystemValue::list(std::vector<SystemValue> items) {
  SystemValue v;
  v.kind = Kind::List;
  v.items = std::move(items);
  return v;
}

namespace {
using Clock = std::chrono::steady_clock;
using Kind = SystemValue::Kind;
using Keywords = std::map<std::string, SystemValue>;
[[noreturn]] void fail(const std::string &name, const std::string &message) {
  throw SystemError(name, message);
}
void check(bool condition, const std::string &message) {
  if (!condition)
    fail("ArgumentError", message);
}
void os_check(int error, const std::string &operation) {
  if (error)
    fail("ProcessSpawnError", operation + ": " + std::strerror(error));
}
void no_nul(const std::string &s) {
  check(s.find('\0') == std::string::npos,
        "process strings cannot contain NUL");
}
const SystemValue &kw(const Keywords &kwargs, const std::string &name) {
  static const SystemValue null;
  auto it = kwargs.find(name);
  return it == kwargs.end() ? null : it->second;
}
void allowed(const Keywords &kwargs,
             std::initializer_list<const char *> names) {
  for (const auto &entry : kwargs) {
    bool found = false;
    for (auto name : names)
      found |= entry.first == name;
    check(found, "unknown system keyword: " + entry.first);
  }
}
bool boolean(const Keywords &kwargs, const std::string &name, bool fallback) {
  const auto &v = kw(kwargs, name);
  if (v.kind == Kind::Null)
    return fallback;
  check(v.kind == Kind::Bool, name + " must be Bool");
  return v.boolean;
}
std::string string(const SystemValue &v) {
  check(v.kind == Kind::String, "expected Str");
  no_nul(v.text);
  return v.text;
}
std::string data(const SystemValue &v) {
  check(v.kind == Kind::String || v.kind == Kind::Bytes,
        "expected Str or Bytes");
  return v.text;
}
bool valid_utf8(const std::string &text) {
  for (std::size_t i = 0; i < text.size();) {
    auto c = static_cast<unsigned char>(text[i++]);
    if (c < 0x80)
      continue;
    unsigned count, value, minimum;
    if (c >= 0xc2 && c <= 0xdf) {
      count = 1;
      value = c & 0x1f;
      minimum = 0x80;
    } else if (c >= 0xe0 && c <= 0xef) {
      count = 2;
      value = c & 0x0f;
      minimum = 0x800;
    } else if (c >= 0xf0 && c <= 0xf4) {
      count = 3;
      value = c & 7;
      minimum = 0x10000;
    } else
      return false;
    if (i + count > text.size())
      return false;
    while (count--) {
      c = static_cast<unsigned char>(text[i++]);
      if ((c & 0xc0) != 0x80)
        return false;
      value = (value << 6) | (c & 0x3f);
    }
    if (value < minimum || value > 0x10ffff ||
        (value >= 0xd800 && value <= 0xdfff))
      return false;
  }
  return true;
}
double seconds(const SystemValue &v, double fallback) {
  if (v.kind == Kind::Null)
    return fallback;
  check(v.kind == Kind::Float || v.kind == Kind::Int,
        "timeout must be Int milliseconds or Float seconds");
  double n = v.kind == Kind::Int ? v.integer / 1000.0 : v.floating;
  check(std::isfinite(n) && n >= 0, "timeout must be finite and nonnegative");
  return n;
}
std::size_t size_option(const SystemValue &v, std::size_t fallback) {
  if (v.kind == Kind::Null)
    return fallback;
  check(v.kind == Kind::Int && v.integer > 0,
        "size/limit must be a positive Int");
  return static_cast<std::size_t>(v.integer);
}

struct Command final : RuntimeIoValue {
  std::vector<std::string> argv;
  std::string cwd;
  std::map<std::string, std::string> env;
  std::vector<std::string> unset_env;
  bool clear_env = false;
  bool group = false;
  const char *type_name() const override { return "system.Command"; }
  bool shareable() const override { return true; }
};
struct Status final : RuntimeIoValue {
  int pid = 0, raw = 0;
  const char *type_name() const override { return "system.Status"; }
  bool shareable() const override { return true; }
  bool success() const { return WIFEXITED(raw) && WEXITSTATUS(raw) == 0; }
};
struct Result final : RuntimeIoValue {
  std::shared_ptr<Status> status;
  SystemValue input, output, errors, input_value, output_value, error_value;
  const char *type_name() const override { return "system.Result"; }
};

struct ChildState {
  std::mutex mutex;
  std::condition_variable cv;
  int pid = -1;
  bool group = false, reaped = false;
  int raw = 0;
  Clock::time_point exited;
  std::string error;
  std::atomic<bool> stop{false};
  double kill_after = 1, drain_timeout = 1;
  void signal(int sig) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!reaped && pid > 0 && ::kill(group ? -pid : pid, sig) != 0 &&
        errno != ESRCH)
      fail("ProcessError", std::strerror(errno));
  }
  void abort(const std::string &name) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (error.empty())
        error = name;
    }
    stop = true;
  }
  void check_io(bool draining) {
    std::lock_guard<std::mutex> lock(mutex);
    if (stop)
      fail(error.empty() ? "ProcessClosedError" : error,
           "process operation interrupted");
    if (draining && reaped &&
        std::chrono::duration<double>(Clock::now() - exited).count() >=
            drain_timeout)
      fail("ProcessDrainTimeoutError",
           "subprocess exited but a descendant still holds a pipe open");
  }
};

// Each endpoint has one logical reader/writer. Nonblocking descriptors plus a
// bounded poll let cancellation and deadlines interrupt every transfer.
struct Stream final : RuntimeIoValue {
  int fd;
  bool writing;
  std::shared_ptr<ChildState> child;
  std::mutex mutex;
  bool record = false;
  std::size_t record_limit = 16 * 1024 * 1024;
  std::string recorded;
  explicit Stream(int fd, bool writing, std::shared_ptr<ChildState> child)
      : fd(fd), writing(writing), child(std::move(child)) {}
  ~Stream() override {
    if (fd >= 0)
      ::close(fd);
  }
  const char *type_name() const override {
    return writing ? "system.Stdin" : "system.Reader";
  }
  void close() {
    std::lock_guard<std::mutex> lock(mutex);
    if (fd >= 0) {
      ::close(fd);
      fd = -1;
    }
  }
  std::string read(std::size_t count, const std::function<bool()> &cancelled) {
    std::lock_guard<std::mutex> lock(mutex);
    check(!writing, "stdin is not readable");
    if (fd < 0)
      return {};
    std::string bytes(std::min<std::size_t>(count, 1024 * 1024), '\0');
    for (;;) {
      if (cancelled && cancelled()) {
        child->abort("CancelledError");
        fail("CancelledError", "process read cancelled");
      }
      // Try read first: buffered bytes and EOF remain valid after child exit.
      auto n = ::read(fd, bytes.data(), bytes.size());
      if (n > 0) {
        bytes.resize(static_cast<std::size_t>(n));
        return bytes;
      }
      if (n == 0) {
        ::close(fd);
        fd = -1;
        return {};
      }
      if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
        fail("IOError", std::strerror(errno));
      child->check_io(true);
      pollfd event{fd, POLLIN, 0};
      ::poll(&event, 1, 20);
    }
  }
  std::size_t write(const std::string &bytes,
                    const std::function<bool()> &cancelled) {
    std::lock_guard<std::mutex> lock(mutex);
    check(writing, "stdout/stderr are not writable");
    if (fd < 0)
      fail("ProcessClosedError", "stdin is closed");
    // Suppress only this write's SIGPIPE, preserving the caller's signal mask.
    struct PipeSignalScope {
      sigset_t set, previous;
      bool pending;
      PipeSignalScope() {
        sigemptyset(&set);
        sigaddset(&set, SIGPIPE);
        pthread_sigmask(SIG_BLOCK, &set, &previous);
        sigset_t before;
        sigpending(&before);
        pending = sigismember(&before, SIGPIPE);
      }
      ~PipeSignalScope() {
        sigset_t after;
        sigpending(&after);
        if (!pending && sigismember(&after, SIGPIPE)) {
          int signal;
          sigwait(&set, &signal);
        }
        pthread_sigmask(SIG_SETMASK, &previous, nullptr);
      }
    } signal_scope;
    std::size_t offset = 0;
    while (offset < bytes.size()) {
      if (cancelled && cancelled()) {
        child->abort("CancelledError");
        fail("CancelledError", "process write cancelled");
      }
      child->check_io(false);
      auto n = ::write(fd, bytes.data() + offset, bytes.size() - offset);
      if (n > 0) {
        if (record) {
          if (static_cast<std::size_t>(n) > record_limit - recorded.size())
            fail("ProcessOutputLimitError", "recorded stdin exceeds limit");
          recorded.append(bytes, offset, static_cast<std::size_t>(n));
        }
        offset += static_cast<std::size_t>(n);
        continue;
      }
      if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
        fail(errno == EPIPE ? "BrokenPipeError" : "IOError",
             std::strerror(errno));
      pollfd event{fd, POLLOUT, 0};
      ::poll(&event, 1, 20);
    }
    return offset;
  }
};

struct Process final : RuntimeIoValue {
  std::shared_ptr<ChildState> state;
  std::shared_ptr<Stream> input, output, errors;
  std::thread monitor;
  std::atomic<bool> communicated{false};
  std::mutex close_mutex;
  const char *type_name() const override { return "system.Process"; }
  ~Process() override { close(); }
  void close() {
    std::lock_guard<std::mutex> lock(close_mutex);
    if (!state)
      return;
    state->stop = true;
    if (monitor.joinable())
      monitor.join();
    if (input)
      input->close();
    if (output)
      output->close();
    if (errors)
      errors->close();
  }
  std::shared_ptr<Status> wait(const std::function<bool()> &cancelled,
                               double timeout = -1) {
    const auto start = Clock::now();
    std::unique_lock<std::mutex> lock(state->mutex);
    while (!state->reaped) {
      if (cancelled && cancelled())
        fail("CancelledError", "process wait cancelled");
      if (timeout >= 0 &&
          std::chrono::duration<double>(Clock::now() - start).count() >=
              timeout)
        fail("ProcessTimeoutError", "process wait timed out");
      state->cv.wait_for(lock, std::chrono::milliseconds(20));
    }
    auto status = std::make_shared<Status>();
    status->pid = state->pid;
    status->raw = state->raw;
    return status;
  }
};

void command_options(Command &command, const Keywords &kwargs) {
  allowed(kwargs, {"cwd", "env", "clear_env", "group"});
  if (kwargs.count("cwd"))
    command.cwd =
        kw(kwargs, "cwd").kind == Kind::Null ? "" : string(kw(kwargs, "cwd"));
  command.clear_env = boolean(kwargs, "clear_env", command.clear_env);
  if (kwargs.count("group")) {
    const auto value = string(kw(kwargs, "group"));
    check(value == "new" || value == "inherit", "group must be new or inherit");
    command.group = value == "new";
  }
  const auto &env = kw(kwargs, "env");
  if (env.kind != Kind::Null) {
    check(env.kind == Kind::Map, "env must be a Map");
    for (const auto &entry : env.entries) {
      no_nul(entry.first);
      check(!entry.first.empty() && entry.first.find('=') == std::string::npos,
            "invalid environment name");
      command.unset_env.erase(std::remove(command.unset_env.begin(),
                                          command.unset_env.end(), entry.first),
                              command.unset_env.end());
      if (entry.second.kind == Kind::Null) {
        command.env.erase(entry.first);
        command.unset_env.push_back(entry.first);
      } else
        command.env[entry.first] = string(entry.second);
    }
  }
}

std::shared_ptr<Process> spawn(const Command &command, bool pipes,
                               const SystemCall &call, double timeout = -1,
                               double kill_after = 1,
                               double drain_timeout = 1) {
  check(!command.argv.empty() && !command.argv[0].empty(),
        "executable must not be empty");
  if (call.cancelled && call.cancelled())
    fail("CancelledError", "process spawn cancelled");
  std::map<std::string, std::string> env;
  if (!command.clear_env)
    for (char **p = environ; p && *p; ++p) {
      std::string entry(*p);
      auto equals = entry.find('=');
      if (equals != std::string::npos)
        env[entry.substr(0, equals)] = entry.substr(equals + 1);
    }
  for (const auto &name : command.unset_env)
    env.erase(name);
  for (const auto &entry : command.env)
    env[entry.first] = entry.second;
  std::string executable = command.argv[0];
  std::string cwd = command.cwd;
  if (cwd.empty() || cwd[0] != '/') {
    char *current = ::getcwd(nullptr, 0);
    os_check(current ? 0 : errno, "getcwd");
    std::string absolute(current);
    free(current);
    cwd = cwd.empty() ? absolute : absolute + "/" + cwd;
  }
  if (executable.find('/') == std::string::npos) {
    const std::string path = env.count("PATH") ? env["PATH"] : "/usr/bin:/bin";
    bool found = false;
    for (std::size_t begin = 0; begin <= path.size();) {
      auto end = path.find(':', begin);
      if (end == std::string::npos)
        end = path.size();
      std::string dir = path.substr(begin, end - begin);
      if (dir.empty() || dir[0] != '/')
        dir = cwd + "/" + dir;
      std::string candidate = dir + "/" + executable;
      if (::access(candidate.c_str(), X_OK) == 0) {
        executable = candidate;
        found = true;
        break;
      }
      begin = end + 1;
    }
    if (!found)
      fail("ProcessSpawnError", "executable not found: " + executable);
  } else if (executable[0] != '/')
    executable = cwd + "/" + executable;
  if (call.authorize)
    call.authorize("process.spawn", executable);

  struct SpawnSetup {
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attrs;
    std::vector<int> fds;
    SpawnSetup() {
      os_check(posix_spawn_file_actions_init(&actions), "spawn actions");
      const int error = posix_spawnattr_init(&attrs);
      if (error) {
        posix_spawn_file_actions_destroy(&actions);
        os_check(error, "spawn attributes");
      }
    }
    ~SpawnSetup() {
      for (int fd : fds)
        if (fd >= 0)
          ::close(fd);
      posix_spawn_file_actions_destroy(&actions);
      posix_spawnattr_destroy(&attrs);
    }
    void release(int fd) {
      for (int &owned : fds)
        if (owned == fd)
          owned = -1;
    }
  } setup;
  std::shared_ptr<Process> process = std::make_shared<Process>();
  auto state = std::make_shared<ChildState>();
  state->group = command.group;
  state->kill_after = kill_after;
  state->drain_timeout = drain_timeout;
  process->state = state;
  if (!pipes)
    os_check(posix_spawn_file_actions_addopen(&setup.actions, STDIN_FILENO,
                                              "/dev/null", O_RDONLY, 0),
             "null stdin");
  for (int stream = 0; stream < 3; ++stream) {
    if (!pipes)
      continue;
    int fds[2];
#if defined(__linux__)
    os_check(::pipe2(fds, O_CLOEXEC) == 0 ? 0 : errno, "pipe");
#else
    os_check(::pipe(fds) == 0 ? 0 : errno, "pipe");
#endif
    setup.fds.push_back(fds[0]);
    setup.fds.push_back(fds[1]);
    // Keep child-action sources above the stdio range, including when the
    // embedding application started with a closed standard descriptor.
    for (int &fd : fds) {
      int high = fcntl(fd, F_DUPFD_CLOEXEC, 3);
      int error = errno;
      ::close(fd);
      setup.release(fd);
      os_check(high < 0 ? error : 0, "pipe descriptor");
      fd = high;
      setup.fds.push_back(fd);
    }
    int parent = stream == 0 ? fds[1] : fds[0];
    int child = stream == 0 ? fds[0] : fds[1];
    os_check(posix_spawn_file_actions_adddup2(&setup.actions, child, stream),
             "stdio dup");
    os_check(posix_spawn_file_actions_addclose(&setup.actions, fds[0]),
             "pipe close");
    os_check(posix_spawn_file_actions_addclose(&setup.actions, fds[1]),
             "pipe close");
    os_check(fcntl(parent, F_SETFL, fcntl(parent, F_GETFL) | O_NONBLOCK) < 0
                 ? errno
                 : 0,
             "nonblocking pipe");
    auto endpoint = std::make_shared<Stream>(parent, stream == 0, state);
    setup.release(parent);
    if (stream == 0)
      process->input = endpoint;
    else if (stream == 1)
      process->output = endpoint;
    else
      process->errors = endpoint;
  }
  if (!command.cwd.empty())
    os_check(posix_spawn_file_actions_addchdir_np(&setup.actions, cwd.c_str()),
             "spawn cwd");
  short flags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
#if defined(__APPLE__)
  flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
  if (!pipes)
    for (int fd = 1; fd < 3; ++fd)
      if (fcntl(fd, F_GETFD) >= 0)
        os_check(posix_spawn_file_actions_adddup2(&setup.actions, fd, fd),
                 "inherit stdio");
#elif defined(__GLIBC__)
#if __GLIBC_PREREQ(2, 34)
  os_check(posix_spawn_file_actions_addclosefrom_np(&setup.actions, 3),
           "close inherited descriptors");
#endif
#endif
  sigset_t empty, defaults;
  sigemptyset(&empty);
  sigemptyset(&defaults);
  sigaddset(&defaults, SIGPIPE);
  os_check(posix_spawnattr_setsigmask(&setup.attrs, &empty),
           "spawn signal mask");
  os_check(posix_spawnattr_setsigdefault(&setup.attrs, &defaults),
           "spawn signal defaults");
  if (command.group) {
    flags |= POSIX_SPAWN_SETPGROUP;
    os_check(posix_spawnattr_setpgroup(&setup.attrs, 0), "spawn process group");
  }
  os_check(posix_spawnattr_setflags(&setup.attrs, flags), "spawn flags");
  std::vector<char *> argv, envp;
  std::vector<std::string> env_strings;
  for (const auto &arg : command.argv)
    argv.push_back(const_cast<char *>(arg.c_str()));
  argv.push_back(nullptr);
  for (const auto &entry : env)
    env_strings.push_back(entry.first + "=" + entry.second);
  for (auto &entry : env_strings)
    envp.push_back(entry.data());
  envp.push_back(nullptr);
  pid_t pid;
  os_check(posix_spawn(&pid, executable.c_str(), &setup.actions, &setup.attrs,
                       argv.data(), envp.data()),
           "spawn " + executable);
  state->pid = pid;
  try {
    const bool scoped =
        call.selector != "spawn" || call.block.kind == Kind::Callback;
    process->monitor = std::thread(
        [state, timeout, scoped,
         cancelled = scoped ? call.cancelled : std::function<bool()>{}] {
          const auto start = Clock::now();
          auto terminating = start;
          bool sent_term = false;
          for (;;) {
            if (cancelled && cancelled())
              state->abort("CancelledError");
            if (timeout >= 0 &&
                std::chrono::duration<double>(Clock::now() - start).count() >=
                    timeout)
              state->abort("ProcessTimeoutError");
            {
              std::lock_guard<std::mutex> lock(state->mutex);
              if (state->reaped) {
                if (!scoped || state->stop)
                  break;
              } else {
                int status;
                auto result = ::waitpid(state->pid, &status, WNOHANG);
                if (result == state->pid || (result < 0 && errno == ECHILD)) {
                  state->raw = result == state->pid ? status : 0;
                  state->reaped = true;
                  state->exited = Clock::now();
                  state->cv.notify_all();
                  if (!scoped || state->stop)
                    break;
                }
                if (state->stop && !state->reaped) {
                  const int target = state->group ? -state->pid : state->pid;
                  if (!sent_term) {
                    ::kill(target, SIGTERM);
                    terminating = Clock::now();
                    sent_term = true;
                  }
                  if (std::chrono::duration<double>(Clock::now() - terminating)
                          .count() >= state->kill_after)
                    ::kill(target, SIGKILL);
                }
              }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
          }
        });
  } catch (...) {
    ::kill(pid, SIGKILL);
    while (::waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {
    }
    throw;
  }
  return process;
}

SystemValue invoke(const SystemValue &callback, const SystemValue &arg) {
  check(callback.kind == Kind::Callback, "stream handler must be callable");
  SystemCallbackScope scope(callback_gate);
  return callback.callback({arg});
}

std::shared_ptr<Result> communicate(const std::shared_ptr<Process> &process,
                                    const SystemCall &call) {
  check(call.args.empty() && call.block.kind == Kind::Null,
        "communicate takes named arguments and named blocks");
  check(process->input && process->output && process->errors,
        "communicate requires piped stdio");
  const auto input = kw(call.kwargs, "input");
  const auto stdin_block = kw(call.kwargs, "stdin"),
             stdout_block = kw(call.kwargs, "stdout"),
             stderr_block = kw(call.kwargs, "stderr");
  auto callback_gate = std::make_shared<std::recursive_mutex>();
  check(input.kind == Kind::Null || stdin_block.kind == Kind::Null,
        "input and stdin handler are mutually exclusive");
  for (const auto &handler : {stdin_block, stdout_block, stderr_block})
    check(handler.kind == Kind::Null || handler.kind == Kind::Callback,
          "stream handler must be callable");
  if (input.kind != Kind::Null)
    (void)data(input);
  const bool record_input = boolean(call.kwargs, "record_input", false);
  (void)boolean(call.kwargs, "check", true);
  const auto limit = size_option(kw(call.kwargs, "limit"), 16 * 1024 * 1024);
  check(!process->communicated.exchange(true),
        "process communicate may only be called once");
  auto result = std::make_shared<Result>();
  process->input->record = record_input;
  process->input->record_limit = limit;
  std::atomic<std::size_t> captured{0};
  std::mutex failure_mutex;
  std::exception_ptr failure;
  auto read_stream = [&](const std::shared_ptr<Stream> &stream,
                         const SystemValue &handler, SystemValue &bytes,
                         SystemValue &value) {
    if (handler.kind != Kind::Null) {
      SystemCallbackScope scope(callback_gate, &process->state->stop);
      value = invoke(handler, SystemValue::resource(stream));
      stream->close();
      return;
    }
    std::string collected;
    for (;;) {
      auto chunk = stream->read(65536, call.cancelled);
      if (chunk.empty())
        break;
      if (captured.fetch_add(chunk.size()) + chunk.size() > limit)
        fail("ProcessOutputLimitError",
             "captured stdout and stderr exceed limit");
      collected += chunk;
    }
    bytes = SystemValue::str(std::move(collected), true);
  };
  auto guarded = [&](auto function) {
    return std::async(std::launch::async, [&, function] {
      try {
        function();
      } catch (...) {
        {
          std::lock_guard<std::mutex> lock(failure_mutex);
          if (!failure)
            failure = std::current_exception();
        }
        process->state->abort("ProcessError");
      }
    });
  };
  std::future<void> writer, reader, errors;
  try {
    writer = guarded([&] {
      if (stdin_block.kind != Kind::Null) {
        SystemCallbackScope scope(callback_gate, &process->state->stop);
        result->input_value =
            invoke(stdin_block, SystemValue::resource(process->input));
      } else if (input.kind != Kind::Null)
        process->input->write(data(input), call.cancelled);
      process->input->close();
      if (process->input->record)
        result->input = SystemValue::str(process->input->recorded, true);
    });
    reader = guarded([&] {
      read_stream(process->output, stdout_block, result->output,
                  result->output_value);
    });
    errors = guarded([&] {
      read_stream(process->errors, stderr_block, result->errors,
                  result->error_value);
    });
  } catch (...) {
    // Interrupt started transfers before future destructors join them if a
    // later host thread could not be created.
    process->state->abort("ProcessError");
    throw;
  }
  for (auto *future : {&writer, &reader, &errors}) {
    future->get();
  }
  result->status = process->wait({});
  std::string error;
  {
    std::lock_guard<std::mutex> lock(process->state->mutex);
    error = process->state->error;
  }
  if (!error.empty() && error != "ProcessError")
    fail(error, "subprocess operation failed");
  if (failure)
    std::rethrow_exception(failure);
  if (boolean(call.kwargs, "check", true) && !result->status->success())
    fail("ProcessExitError", "subprocess exited unsuccessfully (status " +
                                 std::to_string(result->status->raw) + ")");
  return result;
}

// Template fragments alternate static command text and opaque interpolants.
// Only static text participates in lexing; holes can never introduce syntax.
std::vector<std::string> template_argv(const std::vector<SystemValue> &parts) {
  std::vector<std::string> argv;
  std::string word;
  bool active = false, escape = false;
  char quote = 0;
  auto finish = [&] {
    if (active) {
      argv.push_back(std::move(word));
      word.clear();
      active = false;
    }
  };
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i % 2) {
      check(!escape, "interpolation cannot follow a command escape");
      word += string(parts[i]);
      active = true;
      continue;
    }
    const auto text = string(parts[i]);
    for (char c : text) {
      if (escape) {
        word += c;
        active = true;
        escape = false;
        continue;
      }
      if (c == '\\' && quote != '\'') {
        escape = true;
        active = true;
        continue;
      }
      if (quote) {
        if (c == quote)
          quote = 0;
        else
          word += c;
        active = true;
        continue;
      }
      if (c == '\'' || c == '"') {
        quote = c;
        active = true;
        continue;
      }
      if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
        finish();
        continue;
      }
      check(std::string("|&;<>$`() *?[]~").find(c) == std::string::npos,
            "shell syntax requires system.shell");
      word += c;
      active = true;
    }
  }
  check(!quote && !escape, "unterminated command quote or escape");
  finish();
  check(!argv.empty() && !argv[0].empty(), "command is empty");
  return argv;
}
} // namespace

bool system_resource(const std::shared_ptr<RuntimeIoValue> &value) {
  return value && std::string(value->type_name()).rfind("system.", 0) == 0;
}
bool system_blocking_selector(const std::string &selector) {
  return selector == "capture" || selector == "output" || selector == "run" ||
         selector == "communicate" || selector == "wait" ||
         selector == "close" || selector == "read!" ||
         selector == "read_all!" || selector == "each_chunk" ||
         selector == "write_all!" || selector == "write!" ||
         selector == "spawn";
}

SystemValue system_dispatch(const SystemCall &call) {
  SystemCallbackSuspension callback_wait(
      system_blocking_selector(call.selector));
  const auto &name = call.selector;
  const auto &args = call.args;
  const auto &kwargs = call.kwargs;
  if (call.receiver.kind == Kind::Module) {
    if (name == "command" || name == "shell" || name == "__template") {
      check(call.block.kind == Kind::Null,
            "command construction does not accept a block");
      auto command = std::make_shared<Command>();
      if (name == "shell") {
        allowed(kwargs,
                {"executable", "args", "cwd", "env", "clear_env", "group"});
        check(args.size() == 1, "shell requires one script");
        command->argv = {kw(kwargs, "executable").kind == Kind::Null
                             ? "/bin/sh"
                             : string(kw(kwargs, "executable")),
                         "-c", string(args[0]), "sputnik-shell"};
        const auto &extra = kw(kwargs, "args");
        if (extra.kind != Kind::Null) {
          check(extra.kind == Kind::List, "args must be an Array");
          for (const auto &arg : extra.items)
            command->argv.push_back(string(arg));
        }
      } else if (name == "__template") {
        check(args.size() == 1 && args[0].kind == Kind::List,
              "invalid command template");
        command->argv = template_argv(args[0].items);
      } else {
        check(!args.empty(), "command requires executable");
        for (const auto &arg : args)
          command->argv.push_back(string(arg));
      }
      Keywords options = kwargs;
      if (name == "shell") {
        options.erase("args");
        options.erase("executable");
      }
      command_options(*command, options);
      check(!command->argv[0].empty(), "executable must not be empty");
      return SystemValue::resource(command);
    }
  }
  auto command = std::dynamic_pointer_cast<Command>(call.receiver.object);
  if (command) {
    if (name == "with") {
      check(args.empty() && call.block.kind == Kind::Null,
            "with accepts only options");
      auto copy = std::make_shared<Command>(*command);
      command_options(*copy, kwargs);
      return SystemValue::resource(copy);
    }
    if (name == "argv" || name == "program" || name == "cwd") {
      check(args.empty() && kwargs.empty() && call.block.kind == Kind::Null,
            "property takes no arguments");
      if (name == "program")
        return SystemValue::str(command->argv[0]);
      if (name == "cwd")
        return command->cwd.empty() ? SystemValue{}
                                    : SystemValue::str(command->cwd);
      std::vector<SystemValue> result;
      for (const auto &arg : command->argv)
        result.push_back(SystemValue::str(arg));
      return SystemValue::list(std::move(result));
    }
    if (name == "capture" || name == "output" || name == "run" ||
        name == "spawn") {
      check(args.empty(), "process execution takes keyword arguments");
      allowed(kwargs,
              {"input", "stdin", "stdout", "stderr", "timeout", "kill_after",
               "drain_timeout", "check", "limit", "record_input"});
      if (name == "run" || name == "spawn")
        allowed(kwargs, {"timeout", "kill_after", "drain_timeout", "check"});
      if (name == "spawn")
        allowed(kwargs, {"timeout", "kill_after", "drain_timeout"});
      if (name == "output")
        check(kw(kwargs, "stdout").kind == Kind::Null,
              "output does not accept a stdout handler");
      (void)boolean(kwargs, "check", true);
      (void)boolean(kwargs, "record_input", false);
      (void)size_option(kw(kwargs, "limit"), 16 * 1024 * 1024);
      check(kw(kwargs, "input").kind == Kind::Null ||
                kw(kwargs, "stdin").kind == Kind::Null,
            "input and stdin handler are mutually exclusive");
      if (kw(kwargs, "input").kind != Kind::Null)
        (void)data(kw(kwargs, "input"));
      for (const char *handler : {"stdin", "stdout", "stderr"})
        check(kw(kwargs, handler).kind == Kind::Null ||
                  kw(kwargs, handler).kind == Kind::Callback,
              "stream handler must be callable");
      check(call.block.kind == Kind::Null || name == "spawn",
            "capture uses named stdin/stdout/stderr blocks");
      const double timeout = seconds(kw(kwargs, "timeout"), -1);
      auto process = spawn(*command, name != "run", call, timeout,
                           seconds(kw(kwargs, "kill_after"), 1),
                           seconds(kw(kwargs, "drain_timeout"), 1));
      if (name == "spawn") {
        if (call.block.kind != Kind::Null) {
          try {
            SystemCallbackScope scope(callback_gate, &process->state->stop);
            auto result = invoke(call.block, SystemValue::resource(process));
            process->close();
            return result;
          } catch (...) {
            process->close();
            throw;
          }
        }
        return SystemValue::resource(process);
      }
      if (name == "run") {
        auto status = process->wait(call.cancelled);
        std::string error;
        {
          std::lock_guard<std::mutex> lock(process->state->mutex);
          error = process->state->error;
        }
        if (!error.empty())
          fail(error, "subprocess operation failed");
        if (boolean(kwargs, "check", true) && !status->success())
          fail("ProcessExitError", "subprocess exited unsuccessfully");
        return SystemValue::resource(status);
      }
      auto result = communicate(process, call);
      if (name == "output") {
        if (!valid_utf8(result->output.text))
          fail("ProcessDecodeError", "process stdout is not valid UTF-8; use "
                                     "capture for binary output");
        return SystemValue::str(result->output.text);
      }
      return SystemValue::resource(result);
    }
  }
  auto process = std::dynamic_pointer_cast<Process>(call.receiver.object);
  if (process) {
    if (name == "communicate") {
      allowed(kwargs, {"input", "stdin", "stdout", "stderr", "check", "limit",
                       "record_input"});
      return SystemValue::resource(communicate(process, call));
    }
    if (name == "wait") {
      check(args.empty() && call.block.kind == Kind::Null,
            "wait takes no positional arguments");
      allowed(kwargs, {"timeout"});
      return SystemValue::resource(
          process->wait(call.cancelled, seconds(kw(kwargs, "timeout"), -1)));
    }
    check(kwargs.empty() && call.block.kind == Kind::Null,
          "process method does not accept keywords or blocks");
    if (name == "signal" || name == "terminate" || name == "kill") {
      check(args.size() == (name == "signal" ? 1 : 0),
            "invalid signal arguments");
      int signal = name == "kill" ? SIGKILL : SIGTERM;
      if (name == "signal") {
        check(args[0].kind == Kind::Int && args[0].integer > 0 &&
                  args[0].integer < NSIG,
              "signal requires a valid positive signal number");
        signal = static_cast<int>(args[0].integer);
      }
      if (call.authorize)
        call.authorize("process.signal", std::to_string(process->state->pid));
      process->state->signal(signal);
      return {};
    }
    check(args.empty(), "property takes no arguments");
    if (name == "pid")
      return SystemValue::number(process->state->pid);
    if (name == "stdin")
      return process->input ? SystemValue::resource(process->input)
                            : SystemValue{};
    if (name == "stdout")
      return process->output ? SystemValue::resource(process->output)
                             : SystemValue{};
    if (name == "stderr")
      return process->errors ? SystemValue::resource(process->errors)
                             : SystemValue{};
    if (name == "close") {
      process->close();
      return {};
    }
    if (name == "running?") {
      std::lock_guard<std::mutex> lock(process->state->mutex);
      return SystemValue::boolean_value(!process->state->reaped);
    }
  }
  auto stream = std::dynamic_pointer_cast<Stream>(call.receiver.object);
  if (stream) {
    if (name == "close") {
      check(args.empty() && kwargs.empty(), "close takes no arguments");
      stream->close();
      return {};
    }
    if (name == "write_all!" || name == "write!") {
      check(args.size() == 1 && kwargs.empty() && call.block.kind == Kind::Null,
            "write requires bytes");
      return SystemValue::number(stream->write(data(args[0]), call.cancelled));
    }
    if (name == "read!" || name == "read_all!" || name == "each_chunk") {
      allowed(kwargs, {"size", "limit"});
      check(args.empty(), "read takes keyword arguments");
      const auto size = size_option(kw(kwargs, "size"), 65536),
                 limit = size_option(kw(kwargs, "limit"), 16 * 1024 * 1024);
      check((name == "each_chunk") == (call.block.kind == Kind::Callback),
            "each_chunk requires a block; read does not");
      std::string collected;
      do {
        auto chunk = stream->read(size, call.cancelled);
        if (chunk.empty()) {
          if (name == "read!")
            return {};
          break;
        }
        if (name == "each_chunk")
          invoke(call.block, SystemValue::str(std::move(chunk), true));
        else {
          check(collected.size() + chunk.size() <= limit, "read exceeds limit");
          collected += chunk;
        }
      } while (name != "read!");
      return name == "each_chunk"
                 ? SystemValue{}
                 : SystemValue::str(std::move(collected), true);
    }
  }
  auto result = std::dynamic_pointer_cast<Result>(call.receiver.object);
  if (result) {
    check(args.empty() && kwargs.empty() && call.block.kind == Kind::Null,
          "property takes no arguments");
    if (name == "status")
      return SystemValue::resource(result->status);
    if (name == "stdin")
      return result->input;
    if (name == "stdout")
      return result->output;
    if (name == "stderr")
      return result->errors;
    if (name == "stdin_value")
      return result->input_value;
    if (name == "stdout_value")
      return result->output_value;
    if (name == "stderr_value")
      return result->error_value;
  }
  auto status = result
                    ? result->status
                    : std::dynamic_pointer_cast<Status>(call.receiver.object);
  if (status) {
    check(args.empty() && kwargs.empty() && call.block.kind == Kind::Null,
          "property takes no arguments");
    if (name == "success?")
      return SystemValue::boolean_value(status->success());
    if (name == "exit_code")
      return WIFEXITED(status->raw)
                 ? SystemValue::number(WEXITSTATUS(status->raw))
                 : SystemValue{};
    if (name == "signal")
      return WIFSIGNALED(status->raw)
                 ? SystemValue::number(WTERMSIG(status->raw))
                 : SystemValue{};
    if (name == "pid")
      return SystemValue::number(status->pid);
    if (name == "signaled?")
      return SystemValue::boolean_value(WIFSIGNALED(status->raw));
  }
  fail("NoMethodError", "unknown system method: " + name);
}
} // namespace sputnik::runtime
