#include "runtime/system.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <dirent.h>
#include <iostream>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

using namespace amber::runtime;
using K = SystemValue::Kind;

SystemValue send(SystemValue receiver, std::string selector,
                 std::vector<SystemValue> args = {},
                 std::map<std::string, SystemValue> kwargs = {},
                 SystemValue block = {}) {
  return system_dispatch({std::move(receiver),
                          std::move(selector),
                          std::move(args),
                          std::move(kwargs),
                          std::move(block),
                          {},
                          {}});
}
SystemValue command(std::vector<std::string> argv) {
  SystemValue module;
  module.kind = K::Module;
  std::vector<SystemValue> args;
  for (auto &arg : argv)
    args.push_back(SystemValue::str(arg));
  return send(module, "command", std::move(args));
}
template <class F> void raises(const std::string &name, F fn) {
  try {
    fn();
    assert(false && "expected error");
  } catch (const SystemError &error) {
    if (error.name != name) {
      std::cerr << error.name << ": " << error.what() << '\n';
      std::abort();
    }
  }
}
int open_fds() {
  DIR *dir = opendir("/dev/fd");
  assert(dir);
  int count = 0;
  while (readdir(dir))
    ++count;
  closedir(dir);
  return count;
}
int main() {
  auto echo = command({"/usr/bin/printf", "%s", "a b; $(false) ' \""});
  assert(send(echo, "output").text == "a b; $(false) ' \"");
  auto cat = command({"/bin/sh", "-c", "cat; printf diagnostic >&2"});
  const std::string body(1024 * 1024, 'x');
  auto result =
      send(cat, "capture", {}, {{"input", SystemValue::str(body, true)}});
  assert(send(result, "stdout").text == body);
  assert(send(result, "stderr").text == "diagnostic");
  assert(send(result, "success?").boolean);
  auto exit = command({"/bin/sh", "-c", "exit 17"});
  assert(send(send(exit, "capture", {},
                   {{"check", SystemValue::boolean_value(false)}}),
              "exit_code")
             .integer == 17);
  raises("ProcessExitError", [&] { send(exit, "output"); });
  raises("ProcessSpawnError",
         [&] { send(command({"/no/such/amber-process"}), "capture"); });
  auto start = std::chrono::steady_clock::now();
  raises("ProcessTimeoutError", [&] {
    send(command({"/bin/sh", "-c", "trap '' TERM; while :; do :; done"}),
         "capture", {},
         {{"timeout", SystemValue::number(30)},
          {"kill_after", SystemValue::number(30)}});
  });
  assert(std::chrono::steady_clock::now() - start < std::chrono::seconds(3));
  raises("ProcessOutputLimitError", [&] {
    send(command({"/usr/bin/printf", "abcdef"}), "capture", {},
         {{"limit", SystemValue::number(3)}});
  });
  SystemValue writer;
  writer.kind = K::Callback;
  writer.callback = [&](const std::vector<SystemValue> &args) {
    send(args[0], "write_all!", {SystemValue::str(body, true)});
    return SystemValue::number(7);
  };
  SystemValue reader;
  reader.kind = K::Callback;
  reader.callback = [&](const std::vector<SystemValue> &args) {
    auto bytes = send(args[0], "read_all!");
    assert(bytes.text == body);
    return SystemValue::number(bytes.text.size());
  };
  result = send(cat, "capture", {}, {{"stdin", writer}, {"stdout", reader}});
  assert(send(result, "stdin_value").integer == 7);
  assert(send(result, "stdout_value").integer ==
         static_cast<std::int64_t>(body.size()));
  assert(send(result, "stdout").kind == K::Null);
  auto process = send(command({"/usr/bin/printf", "manual"}), "spawn");
  result = send(process, "communicate");
  assert(send(result, "stdout").text == "manual");
  assert(send(send(process, "wait"), "success?").boolean);
  assert(send(send(process, "wait"), "success?").boolean);
  send(process, "close");
  send(process, "close");
  auto env = SystemValue{};
  env.kind = K::Map;
  env.entries["AMBER_SYSTEM_TEST"] = SystemValue::str("special value");
  auto configured = send(
      command({"/bin/sh", "-c", "printf '%s' \"$AMBER_SYSTEM_TEST\"; pwd"}),
      "with", {}, {{"env", env}, {"cwd", SystemValue::str("/tmp")}});
  const auto configured_output = send(configured, "output").text;
  assert(configured_output == "special value/tmp\n" ||
         configured_output == "special value/private/tmp\n");
  SystemValue module;
  module.kind = K::Module;
  auto template_command =
      send(module, "__template",
           {SystemValue::list({SystemValue::str("/usr/bin/printf '%s' \""),
                               SystemValue::str("a b\"; false"),
                               SystemValue::str("\"")})});
  assert(send(template_command, "output").text == "a b\"; false");
  raises("ArgumentError", [&] {
    send(module, "__template",
         {SystemValue::list({SystemValue::str("echo x | cat")})});
  });
  // Binary pipes preserve NUL and non-UTF-8 bytes; output explicitly decodes.
  const std::string binary("a\0\xffz", 4);
  result = send(command({"/bin/cat"}), "capture", {},
                {{"input", SystemValue::str(binary, true)},
                 {"record_input", SystemValue::boolean_value(true)}});
  assert(send(result, "stdout").text == binary &&
         send(result, "stdin").text == binary);
  raises("ProcessDecodeError", [&] {
    send(command({"/bin/cat"}), "output", {},
         {{"input", SystemValue::str(binary, true)}});
  });
  // Validation precedes spawn and a failed communicate validation is retryable.
  SystemCall denied;
  denied.receiver = echo;
  denied.selector = "capture";
  int authorized = 0;
  denied.authorize = [&](const std::string &, const std::string &) {
    ++authorized;
    throw SystemError("CapabilityError", "denied");
  };
  denied.kwargs["stdin"] = SystemValue::number(4);
  raises("ArgumentError", [&] { system_dispatch(denied); });
  assert(authorized == 0);
  denied.kwargs.clear();
  raises("CapabilityError", [&] { system_dispatch(denied); });
  assert(authorized == 1);
  process = send(command({"/bin/cat"}), "spawn");
  raises("ArgumentError", [&] {
    send(process, "communicate", {}, {{"stdin", SystemValue::number(4)}});
  });
  assert(send(send(process, "communicate", {},
                   {{"input", SystemValue::str("retry")}}),
              "stdout")
             .text == "retry");
  send(process, "close");
  // Scoped cleanup is guaranteed even if a failing callback retains Process.
  SystemValue retained, scope;
  scope.kind = K::Callback;
  scope.callback = [&](const std::vector<SystemValue> &args) -> SystemValue {
    retained = args[0];
    throw SystemError("ValueError", "handler failed");
  };
  raises("ValueError",
         [&] { send(command({"/bin/sleep", "5"}), "spawn", {}, {}, scope); });
  assert(!send(retained, "running?").boolean);
  errno = 0;
  assert(waitpid(send(retained, "pid").integer, nullptr, WNOHANG) == -1 &&
         errno == ECHILD);
  std::atomic<bool> cancel{false};
  SystemCall cancelled;
  cancelled.receiver = command({"/bin/sleep", "5"});
  cancelled.selector = "capture";
  cancelled.cancelled = [&] { return cancel.load(); };
  std::thread canceller([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    cancel = true;
  });
  start = std::chrono::steady_clock::now();
  raises("CancelledError", [&] { system_dispatch(cancelled); });
  canceller.join();
  assert(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
  raises("ProcessDrainTimeoutError", [&] {
    send(command({"/bin/sh", "-c", "sleep 0.2 & exit 0"}), "capture", {},
         {{"drain_timeout", SystemValue::number(20)}});
  });
  // Repeated success/failure must not accumulate pipe descriptors.
  const auto before_fds = open_fds();
  for (int i = 0; i < 12; ++i) {
    assert(send(echo, "output").text == "a b; $(false) ' \"");
    raises("ProcessSpawnError",
           [&] { send(command({"/no/such/amber-process"}), "capture"); });
  }
  assert(open_fds() == before_fds);
  std::cout << "system tests passed\n";
}
