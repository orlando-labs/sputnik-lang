#include "tools/notebook-worker/process.h"
#include "tools/notebook-worker/presentation.h"
#include "notebook/project.h"
#include "runtime/context.h"
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

using namespace amber::notebook;
using namespace amber::notebook::worker;
using namespace std::chrono_literals;
namespace fs = std::filesystem;
static void require(bool ok, const std::string &message) {
  if (!ok) { std::cerr << "worker test: " << message << '\n'; std::exit(1); }
}
template<class F> static void rejects(F f, const std::string &message) {
  try { f(); } catch (const ProtocolError &) { return; }
  require(false, message);
}
static std::optional<Message> receive_nonlive(Process &process) {
  std::optional<PresentationAssembler> live;
  while (true) {
    const auto message = process.receive(5s);
    if (!message) return std::nullopt;
    if (message->kind == Kind::LiveBegin || message->kind == Kind::LiveChunk || message->kind == Kind::LiveEnd) {
      if (!live) live.emplace(message->generation, message->request, true);
      if (live->accept(*message)) live.reset();
      continue;
    }
    require(!live, "lifecycle frame cannot interrupt an incomplete live transfer");
    return message;
  }
}
static Message reply(Process &process, Kind kind) {
  const auto message = receive_nonlive(process);
  require(message && message->kind == kind,
          "expected reply " + std::to_string(static_cast<int>(kind)) +
          (message && !message->fields.empty() ? ": " + message->fields[0] : ""));
  return *message;
}
static int exited(Process &process) {
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (std::chrono::steady_clock::now() < deadline) {
    if (const auto status = process.poll_exit()) return *status;
    std::this_thread::sleep_for(5ms);
  }
  require(false, "worker did not actually exit"); return -1;
}
static Presentation finish(Process &process) {
  // A just-completed child may still be retiring its captures when the root
  // returns. Either lifecycle is valid; explicit drain tests check it separately.
  auto begin = receive_nonlive(process);
  if (begin && begin->kind == Kind::Draining) begin = receive_nonlive(process);
  require(begin && begin->kind == Kind::SnapshotBegin, "expected presentation begin");
  PresentationAssembler assembler(begin->generation, begin->request);
  require(!assembler.accept(*begin), "begin is not a published snapshot");
  while (true) {
    const auto message = process.receive(5s);
    require(message.has_value(), "presentation completion timeout");
    if (const auto result = assembler.accept(*message)) return *result;
  }
}
static std::string output(const CellPresentation &cell) {
  std::string result;
  for (const auto &event : cell.output) result += event.text;
  return result;
}
static void protocol_tests() {
  const Message source{Kind::Source, 42, 9, {"10", std::string("x\0y", 3), "1"}};
  const auto bytes = encode(source);
  const auto decoded = decode(bytes);
  require(decoded.generation == 42 && decoded.request == 9 && decoded.fields == source.fields,
          "binary-safe framing");
  rejects([&] { decode(bytes.substr(0, bytes.size() - 1)); }, "truncation");
  rejects([&] { decode(bytes + "x"); }, "trailing bytes");
  auto wrong = bytes; wrong[5] = 99;
  rejects([&] { decode(wrong); }, "version mismatch");
  auto oversized = bytes; oversized[0] = 127;
  rejects([&] { decode(oversized); }, "oversized prefix");
  rejects([&] { encode({Kind::Source, 1, 1, {std::string(kMaxMessageBytes, 'x')}}); }, "oversized payload");
  rejects([] { parse_id("-1"); }, "negative id");
  rejects([] { parse_id("0"); }, "zero id");
  rejects([] { parse_id("1oops"); }, "trailing id junk");
  int fds[2]; require(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair");
  Channel receiver(fds[0]);
  require(::send(fds[1], bytes.data(), 3, 0) == 3, "partial header write");
  require(!receiver.receive(5ms), "partial header timeout");
  require(::send(fds[1], bytes.data() + 3, bytes.size() - 3, 0) == static_cast<ssize_t>(bytes.size() - 3), "remainder write");
  require(receiver.receive(20ms)->fields == source.fields, "partial header retained");
  require(::send(fds[1], bytes.data(), 6, 0) == 6, "truncated frame write");
  ::close(fds[1]);
  rejects([&] { receiver.receive(20ms); }, "EOF mid-frame");
}
static void presentation_tests() {
  const auto png = [](std::size_t size, std::uint32_t width, std::uint32_t height) {
    // Header fixture; the transport never decompresses image data.
    std::string bytes(size, '\0');
    bytes.replace(0, 8, "\x89PNG\r\n\x1a\n", 8); bytes.replace(12, 4, "IHDR");
    for (unsigned i = 0; i < 4; ++i) {
      bytes[16 + i] = static_cast<char>(width >> ((3 - i) * 8));
      bytes[20 + i] = static_cast<char>(height >> ((3 - i) * 8));
    }
    return bytes;
  };
  Presentation original;
  original.status = "done";
  original.environment_error = "module diagnostic";
  original.environment_stale = true;
  original.unobserved_task_failures.push_back({9007199254740995ULL, "TypeError",
      std::string("failed\0child", 12), {true, 9, 11, "<child-spawn>", 15, 4}});
  CellPresentation cell;
  cell.id = 9007199254740993ULL; cell.source_revision = 17;
  cell.ok = true; cell.dirty = false; cell.result = "значение";
  cell.result_format = {"{\n  :значение: [1, 2]\n}", true, true, true};
  cell.watch_epoch = 42; cell.watch_event_count = 3;
  LocalView local; local.name = "value"; local.value = "\"hello\"";
  local.text_value = "hello"; local.initialized = true;
  local.watched = true; local.watch_cell_id = 19; local.watch_revision = 27;
  cell.locals.push_back(local);
  amber::runtime::RuntimeTextOutputEvent event;
  event.stream = "stderr"; event.text = std::string("a\0b\n", 4); event.order = 71;
  event.source = {true, 5, 7, "<iamber-cell>", 12, 3};
  cell.output.push_back(event); original.environment_output.push_back(event);
  const CodeErrorRange range{1, 2, 3, 4, false};
  cell.error_ranges.push_back(range); cell.errors.push_back({"diagnostic", range, true});
  // Bigger than a single frame, with binary NUL bytes and preserved panel order.
  cell.displays.push_back({"image/png", png(kMaxMessageBytes + 31, 320, 240), "a) caption", 320, 240});
  cell.displays.push_back({"image/png", png(33, 64, 48), "b) caption", 64, 48});
  cell.displays[0].plot_scene = "{\"version\":1,\"bounds\":[0,0,1,1]}";
  amber::runtime::NotebookLiveEvent progress;
  progress.id = "train"; progress.current = 2.5; progress.total = 100;
  progress.description = "MNIST"; progress.elapsed_ms = 725; progress.done = false;
  cell.progress.push_back(progress);
  original.cells.push_back(cell);
  const auto bytes = encode_presentation(original);
  const auto decoded = decode_presentation(bytes);
  require(encode_presentation(decoded) == bytes, "all inert presentation fields round trip losslessly");
  auto large_scene = original;
  large_scene.cells[0].displays[0].plot_scene.assign(4 * 1024 * 1024 + 1, 'x');
  rejects([&] { encode_presentation(large_scene); }, "oversized plot scene");
  require(decoded.cells[0].result_format.pretty == cell.result_format.pretty &&
          decoded.cells[0].result_format.is_container && decoded.cells[0].result_format.truncated &&
          decoded.cells[0].result_format.pretty_truncated, "result formats and limits survive IPC");
  auto large_result = original; large_result.cells[0].result.assign(65537, 'x');
  rejects([&] { encode_presentation(large_result); }, "oversized result snapshot");
  large_result = original; large_result.cells[0].result_format.pretty.assign(65537, 'x');
  rejects([&] { encode_presentation(large_result); }, "oversized pretty snapshot");
  require(decoded.cells[0].id == cell.id && decoded.cells[0].locals[0].watch_revision == 27,
          "64-bit identities/revisions are not JSON doubles");
  rejects([&] { decode_presentation(bytes.substr(0, bytes.size() - 1)); }, "truncated presentation");
  rejects([&] { decode_presentation(bytes + "x"); }, "trailing presentation data");
  auto duplicate = original; duplicate.cells.push_back(cell);
  rejects([&] { encode_presentation(duplicate); }, "duplicate cell identity");
  duplicate = original; duplicate.unobserved_task_failures.push_back(original.unobserved_task_failures[0]);
  rejects([&] { encode_presentation(duplicate); }, "duplicate failure identity");
  duplicate = original; duplicate.unobserved_task_failures[0].task_id = 0;
  rejects([&] { encode_presentation(duplicate); }, "zero failure identity");
  auto invalid_png = original; invalid_png.cells[0].displays[0].width = 1;
  rejects([&] { encode_presentation(invalid_png); }, "mismatched PNG dimensions");
  auto malformed = bytes; malformed[8] = 'X';
  rejects([&] { decode_presentation(malformed); }, "unknown presentation schema");
  auto invalid_count = encode_presentation(Presentation{});
  invalid_count.replace(invalid_count.size() - 8, 8, std::string(8, '\xff'));
  rejects([&] { decode_presentation(invalid_count); }, "invalid collection count before allocation");
  // Header is one length-prefixed string; next field is a canonical Bool.
  malformed = bytes; malformed[8 + std::string("amber.notebook.presentation.v5").size() + 7] = 2;
  rejects([&] { decode_presentation(malformed); }, "invalid presentation bool");
  const Message begin{Kind::SnapshotBegin, 11, 12,
      {"amber.notebook.presentation.v5", std::to_string(bytes.size())}};
  const Message chunk{Kind::SnapshotChunk, 11, 12, {"0", bytes.substr(0, kPresentationChunkBytes)}};
  const Message end{Kind::Result, 11, 12, {"amber.notebook.presentation.v5"}};
  PresentationAssembler incomplete(11, 12);
  require(!incomplete.accept(begin) && !incomplete.accept(chunk), "partial snapshot stays unpublished");
  rejects([&] { incomplete.accept(end); }, "premature Result");
  rejects([&] { incomplete.accept(begin); }, "malformed stream poisons assembler");
  PresentationAssembler repeated(11, 12); repeated.accept(begin); repeated.accept(chunk);
  rejects([&] { repeated.accept(chunk); }, "duplicate/out-of-order chunk");
  PresentationAssembler stale(11, 13);
  rejects([&] { stale.accept(begin); }, "stale request");
  PresentationAssembler generation(10, 12);
  rejects([&] { generation.accept(begin); }, "stale generation");
  PresentationAssembler overflow(11, 12);
  rejects([&] { overflow.accept({Kind::SnapshotBegin, 11, 12,
      {"amber.notebook.presentation.v5", std::to_string(kMaxPresentationBytes + 1)}}); }, "oversized total before allocation");
  PresentationAssembler no_begin(11, 12);
  rejects([&] { no_begin.accept(chunk); }, "chunk without begin");
  int fds[2]; require(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "presentation socketpair");
  Channel sender(fds[0]), receiver(fds[1]);
  std::thread writer([&] { send_presentation(sender, 11, 12, original, 2s); });
  PresentationAssembler assembled(11, 12);
  std::optional<Presentation> received;
  while (!received) {
    const auto message = receiver.receive(2s);
    require(message.has_value(), "chunk transfer timeout");
    received = assembled.accept(*message);
  }
  writer.join();
  require(encode_presentation(*received) == bytes, "multi-frame binary presentation transfer");
  rejects([&] { assembled.accept(end); }, "duplicate completion rejected");
  auto invalid_progress = original;
  invalid_progress.cells[0].progress[0].current = std::numeric_limits<double>::infinity();
  rejects([&] { encode_presentation(invalid_progress); }, "nonfinite progress cannot cross IPC");
  invalid_progress = original; invalid_progress.cells[0].progress.push_back(progress);
  rejects([&] { encode_presentation(invalid_progress); }, "duplicate progress id");
  std::thread live_writer([&] { send_presentation(sender, 11, 12, original, 2s, true); });
  PresentationAssembler live(11, 12, true);
  received.reset();
  while (!received) {
    const auto message = receiver.receive(2s);
    require(message.has_value() && message->kind != Kind::Result, "live transfer does not finish run");
    received = live.accept(*message);
  }
  live_writer.join();
  require(encode_presentation(*received) == bytes, "chunked live image and progress snapshot");
  PresentationAssembler terminal(11, 12);
  auto live_begin = begin; live_begin.kind = Kind::LiveBegin;
  rejects([&] { terminal.accept(live_begin); }, "live telemetry cannot commit cell variables");
}
static void output_capture_tests() {
  Session session;
  Cell cell; cell.id = 1; cell.source = "UNSAVED SOURCE SENTINEL";
  cell.formatting = "UNSAVED FORMATTING SENTINEL"; session.cells.push_back(cell);
  auto first = amber::runtime::RuntimeTextWriter::cell_stream("stdout");
  auto second = amber::runtime::RuntimeTextWriter::cell_stream("stderr");
  unsigned observed = 0;
  session.output_observer = [&](auto, auto, auto) { ++observed; };
  {
    RunOutputCapture capture(session);
    session.output_observer(1, first, nullptr); first->write_line("superseded");
    session.output_observer(1, nullptr, second); second->write_line("latest");
    capture.finish();
    require(first->closed() && second->closed(), "all attempts' sinks close, including superseded ones");
    require(!first->write_str("late").ok, "closed runs reject writes through retained old writers");
    require(session.cells[0].output_events.size() == 2 && session.cells[0].output_events[0].text == "latest",
            "latest attempt replaces earlier output");
    capture.finish(); // Idempotent; does not clear the already published text.
  }
  session.output_observer(1, nullptr, nullptr);
  require(observed == 3, "previous host observer is restored");
  const auto bytes = encode_presentation(presentation_from_session(session, {{1, 8}}, false));
  require(bytes.find("UNSAVED") == std::string::npos, "presentation cannot overwrite source/formatting drafts");
}
static std::string read_file(const fs::path &path) {
  std::ifstream file(path);
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
static void process_tests(const std::string &worker, const std::string &fixture) {
  char pattern[] = "/tmp/amber-worker-tests-XXXXXX";
  const char *directory = mkdtemp(pattern); require(directory, "temporary project");
  const fs::path root(directory);
  auto document = make_project_document("Worker isolation");
  document.sheets[0].cells.resize(1);
  document.sheets[0].cells[0].source = "answer = 42\nanswer\n";
  auto project = create_project(root / "test.amberbook", document);
  const auto cell = std::to_string(document.sheets[0].cells[0].id);
  const auto path = project.directory.string();
  const auto original = read_file(project.directory / "project.json");
  Process process;
  process.start(worker);
  const auto generation = process.generation();
  reply(process, Kind::Ready);
  process.send(Kind::Load, {path, ""});
  auto loaded = finish(process);
  require(!loaded.cells.at(0).ok, "opening does not execute");
  process.send(Kind::Run, {cell, "1"}); reply(process, Kind::Started);
  auto evaluated = finish(process);
  require(evaluated.cells.at(0).ok && evaluated.cells.at(0).result == "42", "normal result crosses IPC");
  require(evaluated.cells[0].locals.size() == 1 && evaluated.cells[0].locals[0].name == "answer" &&
          evaluated.cells[0].locals[0].text_value == "42", "locals and interpolation values cross IPC");
  process.send(Kind::Source, {cell, "metrics = {:loss: [0.2, 0.1], :device: \"mps\"}\nmetrics\n", "1"});
  finish(process);
  process.send(Kind::Run, {cell, "1"}); reply(process, Kind::Started);
  const auto container = finish(process);
  require(container.cells[0].ok && container.cells[0].result_format.is_container &&
          container.cells[0].result_format.pretty.find("\n  :loss: [\n") != std::string::npos &&
          !container.cells[0].result_format.truncated, "real container inspection crosses worker boundary");
  process.send(Kind::Source, {cell,
      "try:\n  while true:\n    x = 1 + 2\nensure:\n  print(\"cleanup\")\n", "1"});
  finish(process);
  const auto cancelled_run = process.send(Kind::Run, {cell, "1"});
  reply(process, Kind::Started);
  bool sent = false;
  std::thread stop([&] {
    std::this_thread::sleep_for(200ms);
    sent = process.request_stop(cancelled_run);
  });
  auto cancelled = finish(process); // blocked data owner; independent control thread
  stop.join();
  require(sent && cancelled.cancelled, "CPU Stop delivered independently");
  require(!cancelled.cells.at(0).ok && cancelled.cells.at(0).error.find("CancelledError") != std::string::npos,
          "cancelled run is not successful");
  require(output(cancelled.cells.at(0)).find("cleanup") != std::string::npos, "ensure output survives cancellation");

  const auto next_run = process.send(Kind::Run, {cell, "1"});
  reply(process, Kind::Started);
  require(process.request_stop(cancelled_run), "late Stop queued");
  require(!process.receive(150ms), "late Stop must not cancel the next run");
  require(process.request_stop(next_run), "current Stop queued");
  require(finish(process).cancelled, "new run independently cancellable");
  const auto early_run = process.send(Kind::Run, {cell, "1"});
  require(process.request_stop(early_run), "Stop may precede Started on independent channels");
  reply(process, Kind::Started);
  require(finish(process).cancelled, "early Stop is retained");
  const auto revision = process.send(Kind::Source, {cell, "99\n", "1"}); finish(process);
  process.send(Kind::Run, {cell, "1"}); reply(process, Kind::Started);
  const auto updated = finish(process);
  require(updated.cells.at(0).result == "99", "new token after cancellation");
  require(updated.cells[0].source_revision == revision.request, "snapshot identifies the acknowledged source revision");
  process.send(Kind::Source, {cell,
      "import task\nready = Channel.new(capacity: 0)\nchild = task.spawn:\n  try:\n"
      "    print(\"CHILD STARTED\")\n    ready.send(1)\n    task.sleep(60000)\n"
      "  ensure:\n    task.sleep(5)\n    print(\"LATE CLEANUP\")\nready.recv()\nchild\n", "1"});
  finish(process);
  const auto child_run = process.send(Kind::Run, {cell, "1"});
  reply(process, Kind::Started);
  reply(process, Kind::Draining);
  require(!process.receive(30ms), "root result waits for a live child");
  rejects([&] { process.send(Kind::Source, {cell, "1", "1"}); }, "cannot edit a draining world");
  require(process.request_stop(child_run), "Stop remains available throughout drain");
  const auto stopped_child = finish(process);
  require(stopped_child.cancelled, "draining child actually stopped");
  require(stopped_child.unobserved_task_failures.empty(), "Stop is not a child failure");
  require(output(stopped_child.cells[0]) == "CHILD STARTED\nLATE CLEANUP\n", "late child cleanup output crosses IPC exactly once");

  process.send(Kind::Source, {cell, "import task\nchild = task.spawn:\n  task.sleep(150)\n  print(\"LATE OUTPUT\")\nchild\n", "1"});
  finish(process);
  process.send(Kind::Run, {cell, "1"}); reply(process, Kind::Started);
  reply(process, Kind::Draining);
  const auto child_finished = finish(process);
  require(!child_finished.cancelled, "finite child drains without cancellation");
  require(output(child_finished.cells[0]) == "LATE OUTPUT\n", "new run replaces old child output");
  require(!child_finished.cells[0].output.empty() && child_finished.cells[0].output[0].source.present &&
          child_finished.cells[0].output[0].stream == "stdout", "late text keeps source and stream metadata");

  process.send(Kind::Source, {cell,
      "import task\nchild = task.spawn:\n  task.sleep(150)\n  1 / 0\n42\n", "1"});
  finish(process);
  process.send(Kind::Run, {cell, "1"}); reply(process, Kind::Started); reply(process, Kind::Draining);
  const auto child_failed = finish(process);
  require(child_failed.cells[0].ok && child_failed.cells[0].result == "42" &&
          child_failed.unobserved_task_failures.size() == 1 && !child_failed.cancelled,
          "late child failure is separate from already committed root result");
  require(child_failed.unobserved_task_failures[0].spawn_source.present &&
          !child_failed.unobserved_task_failures[0].error_name.empty() &&
          child_failed.status.find("unobserved child") != std::string::npos,
          "typed child diagnostic and spawn source cross IPC");
  process.send(Kind::Source, {cell,
      "import task\nchild = task.spawn:\n  1 / 0\ntry:\n  child.wait()\nrescue:\n  77\n", "1"});
  finish(process);
  process.send(Kind::Run, {cell, "1"}); reply(process, Kind::Started);
  const auto rescued = finish(process);
  require(rescued.cells[0].ok && rescued.cells[0].result == "77" && rescued.unobserved_task_failures.empty(),
          "rescued failure not duplicated; previous run failures do not leak");
  process.send(Kind::Source, {cell,
      "import task\nchild = task.spawn:\n  1 / 0\nchild.wait()\n", "1"});
  finish(process);
  process.send(Kind::Run, {cell, "1"}); reply(process, Kind::Started);
  const auto propagated = finish(process);
  require(!propagated.cells[0].ok && !propagated.cells[0].error.empty() &&
          propagated.unobserved_task_failures.empty(), "unrescued wait fails root without duplicating its child diagnostic");
  process.send(Kind::Source, {cell, "unknown_notebook_variable\n", "1"}); finish(process);
  process.send(Kind::Run, {cell, "1"}); reply(process, Kind::Started);
  const auto failed = finish(process);
  require(!failed.cells[0].ok && !failed.cells[0].errors.empty() && !failed.cells[0].error_ranges.empty(),
          "source diagnostics/ranges cross IPC");
  require(read_file(project.directory / "project.json") == original, "worker never saves edited source");
  process.send(Kind::Shutdown); reply(process, Kind::Result);
  const int graceful_status = exited(process);
  require(WIFEXITED(graceful_status) && WEXITSTATUS(graceful_status) == 0,
          "graceful exit confirmed and reaped");
  require(!process.request_force_stop(process.generation()), "never signal a reaped PID");

  // A separate fixture executable reads Stop, but its native child ignores
  // cancellation. The root has returned; completion must still wait for it.
  process.start(fixture); reply(process, Kind::Ready);
  require(process.generation() != generation, "restart has a fresh generation");
  const auto hung_run = process.send(Kind::Run);
  reply(process, Kind::Started);
  reply(process, Kind::Draining);
  require(process.request_stop(hung_run), "native stall Stop sent");
  require(!process.receive(100ms), "acknowledging/requesting Stop is not completion");
  require(!process.request_force_stop(generation), "old-generation force cannot kill the current worker");
  require(process.request_force_stop(process.generation()), "force kill independent of native code");
  const int status = exited(process);
  require(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL, "confirmed forced exit");
  require(process.poll_exit() == status, "exit status remains stable after reaping");

  process.start(worker); reply(process, Kind::Ready);
  require(!process.request_stop(cancelled_run), "old-generation control cannot cancel a restarted worker");
  process.send(Kind::Load, {path, ""});
  require(!finish(process).cells.at(0).ok, "restart never replays cells");
  process.send(Kind::Run, {cell, "1"}); reply(process, Kind::Started);
  require(finish(process).cells.at(0).result == "42", "clean runtime uses host-saved source");
  process.request_force_stop(process.generation()); exited(process);
  fs::remove_all(root);
}
static void environment_process_tests(const std::string &worker) {
  char pattern[] = "/tmp/amber-worker-environment-XXXXXX";
  const char *directory = mkdtemp(pattern); require(directory, "temporary module project");
  const fs::path root(directory);
  auto document = make_project_document("Environment output");
  document.sheets[0].cells.resize(1);
  document.sheets[0].cells[0].source = "answer()\n";
  auto project = create_project(root / "Modules.amberbook", document);
  document.modules.push_back({"helpers", "helpers.am", {}});
  document.auto_imports = {"helpers"};
  std::ofstream(project.directory / "helpers.am") <<
      "package helpers\nexport answer\ndef answer(): 42\n"
      "child = task.spawn:\n  task.sleep(500)\n  print(\"ENV LATE\")\n";
  save_project(&project, document);
  Process process; process.start(worker); reply(process, Kind::Ready);
  process.send(Kind::Load, {project.directory.string(), ""});
  require(finish(process).environment_output.empty(), "module open does not initialize");
  process.send(Kind::Apply, {"1"}); reply(process, Kind::Started); reply(process, Kind::Draining);
  const auto result = finish(process);
  require(result.environment_error.empty() && result.cells[0].ok, "environment applied");
  std::string text;
  for (const auto &event : result.environment_output) text += event.text;
  require(text == "ENV LATE\n" && result.cells[0].output.empty(), "late module output stays in the environment panel");
  process.send(Kind::Shutdown); reply(process, Kind::Result); exited(process);

  std::ofstream(project.directory / "helpers.am") <<
      "package helpers\nexport answer\ndef answer(): 42\n"
      "child = task.spawn:\n  task.sleep(500)\n  1 / 0\n";
  process.start(worker); reply(process, Kind::Ready);
  process.send(Kind::Load, {project.directory.string(), ""}); finish(process);
  process.send(Kind::Apply, {"1"}); reply(process, Kind::Started); reply(process, Kind::Draining);
  const auto failed = finish(process);
  require(failed.environment_error.empty() && failed.cells[0].ok && failed.unobserved_task_failures.size() == 1,
          "module descendant failure reported even after successful environment Apply");
  require(failed.unobserved_task_failures[0].spawn_source.file.find("helpers.am") != std::string::npos,
          "module descendant error points to its spawn source");
  process.send(Kind::Shutdown); reply(process, Kind::Result); exited(process);
  fs::remove_all(root);
}
static void plot_process_tests(const std::string &worker, const std::string &plot_source) {
  char pattern[] = "/tmp/amber-worker-plots-XXXXXX";
  const char *directory = mkdtemp(pattern); require(directory, "temporary plot project");
  const fs::path root(directory);
  auto document = make_project_document("Worker plots");
  document.sheets[0].cells.resize(1);
  document.sheets[0].cells[0].source =
      "p = figure(width: 320, height: 240).line([[0, 0], [1, 1]])\n"
      "notebook.show(p, caption: \"second\", order: 2)\n"
      "notebook.show(p, caption: \"first\", order: 1)\n42\n";
  const auto cell = std::to_string(document.sheets[0].cells[0].id);
  document.version = 3;
  document.dependencies.push_back({fs::canonical(plot_source).parent_path().parent_path().string(), true, {}});
  auto project = create_project(root / "Plots.amberbook", document);
  Process process; process.start(worker); reply(process, Kind::Ready);
  process.send(Kind::Load, {project.directory.string(), ""});
  require(finish(process).cells[0].displays.empty(), "plot project open is inert");
  process.send(Kind::Run, {cell, "1"}); reply(process, Kind::Started);
  const auto result = finish(process);
  require(result.cells[0].ok, "plot executes inside isolated worker: " + result.environment_error + result.cells[0].error);
  const auto &panels = result.cells[0].displays;
  require(panels.size() == 2 && panels[0].caption == "first" && panels[1].caption == "second", "ordered plot captions cross IPC");
  require(panels[0].width == 320 && panels[0].height == 240 &&
          panels[0].bytes.substr(0, 8) == std::string("\x89PNG\r\n\x1a\n", 8), "real PNG bytes cross worker boundary");
  process.send(Kind::Source, {cell, "notebook.show(figure(width: 320, height: 240), caption: \"discard\")\n1 / 0\n", "1"}); finish(process);
  process.send(Kind::Run, {cell, "1"}); reply(process, Kind::Started);
  const auto failed = finish(process);
  require(!failed.cells[0].ok && failed.cells[0].displays.size() == 2 &&
          failed.cells[0].displays[0].bytes == panels[0].bytes && failed.cells[0].displays[0].caption == "first",
          "failed run preserves last successful figure across IPC");
  process.send(Kind::Shutdown); reply(process, Kind::Result); exited(process);
  fs::remove_all(root);
}
int main(int argc, char **argv) {
  if (argc == 3 && std::string(argv[1]) == "--notebook-worker") {
    alarm(15); // Bound the fixture's lifetime even if its test parent crashes.
    const auto generation = parse_id(argv[2]);
    Channel data(kDataFd);
    data.send({Kind::Ready, generation, 0, {}}, 1s);
    const auto message = data.receive(5s);
    if (!message) return 2;
    data.send({Kind::Started, generation, message->request, {}}, 1s);
    // A native child ignores the cancellation flag. The root has returned,
    // but run membership cannot retire until that foreign call exits.
    amber::runtime::RuntimeTaskModule tasks(1);
    auto run = std::make_shared<amber::runtime::RuntimeRunState>();
    std::atomic<bool> entered{false};
    {
      amber::runtime::RuntimeRunCancellationScope scope(run);
      tasks.spawn([&] {
        entered.store(true);
        while (true) ::pause();
        return amber::runtime::Value::null();
      });
    }
    while (!entered.load()) std::this_thread::yield();
    run->close_root();
    data.send({Kind::Draining, generation, message->request, {"1"}}, 1s);
    configure_socket(kControlFd);
    while (!run->wait_for_idle(10ms)) {
      char bytes[64];
      const auto count = ::recv(kControlFd, bytes, sizeof(bytes), 0);
      if (count > 0 && decode(std::string(bytes, count)).kind == Kind::Stop) run->request_cancel();
    }
    return 3; // An uncooperative child must never reach this path.
  }
  require(argc == 2 || argc == 3, "worker executable and optional plot source arguments");
  alarm(60);
  protocol_tests();
  presentation_tests();
  output_capture_tests();
  process_tests(fs::absolute(argv[1]), fs::absolute(argv[0]));
  environment_process_tests(fs::absolute(argv[1]));
  if (argc == 3) plot_process_tests(fs::absolute(argv[1]), argv[2]);
  alarm(0);
  std::cout << "notebook_worker_tests: ok\n";
}
