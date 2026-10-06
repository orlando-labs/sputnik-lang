#include "tools/notebook-worker/process.h"
#include "tools/notebook-worker/presentation.h"
#include "notebook/renderers.h"
#include "tools/iamber/project_session.h"
#include "tools/iamber/dependencies.h"
#include "runtime/context.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace amber::notebook;
using namespace amber::notebook::worker;
using namespace std::chrono_literals;

namespace {
// The control receiver touches only this small run ledger, never Session,
// RuntimeWorld or heap locks. Remember early Stop across channel reordering.
class Control {
public:
  explicit Control(std::uint64_t generation) : generation_(generation) {
    configure_socket(kControlFd);
    thread_ = std::thread([this] { listen(); });
  }
  ~Control() {
    done_.store(true);
    thread_.join();
    ::close(kControlFd);
  }
  amber::runtime::RuntimeRunCancellation begin(std::uint64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    active_id_ = id;
    active_ = std::make_shared<amber::runtime::RuntimeRunState>();
    if (id <= cancelled_through_) active_->request_cancel();
    return active_;
  }
  bool finish() {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool cancelled = active_ && active_->cancelled();
    active_.reset(); active_id_ = 0;
    return cancelled;
  }
private:
  void listen() noexcept {
    while (!done_.load()) {
      pollfd fds[2]{{kControlFd, POLLIN, 0}, {kDataFd, 0, 0}};
      const int rc = ::poll(fds, 2, 50);
      // Parent disconnect is not a graceful Stop. No host remains to own the
      // world; terminate even if the execution thread is stuck in native code.
      if (rc > 0 && (fds[1].revents & (POLLHUP | POLLERR | POLLNVAL))) std::_Exit(0);
      if (rc <= 0 || !(fds[0].revents & POLLIN)) continue;
      char bytes[64];
      const auto n = ::recv(kControlFd, bytes, sizeof(bytes), 0);
      if (n <= 0) continue;
      try {
        const auto message = decode(std::string(bytes, static_cast<std::size_t>(n)));
        if (message.kind != Kind::Stop || message.generation != generation_ ||
            message.request == 0 || !message.fields.empty()) continue;
        amber::runtime::RuntimeRunCancellation active;
        {
          std::lock_guard<std::mutex> lock(mutex_);
          cancelled_through_ = std::max(cancelled_through_, message.request);
          if (active_ && active_id_ <= cancelled_through_) active = active_;
        }
        if (active) active->request_cancel();
      } catch (...) { /* Malformed control cannot affect an active run. */ }
    }
  }
  const std::uint64_t generation_;
  std::atomic<bool> done_{false};
  std::mutex mutex_;
  std::uint64_t active_id_ = 0, cancelled_through_ = 0;
  amber::runtime::RuntimeRunCancellation active_;
  std::thread thread_;
};
bool boolean(const std::string &value) {
  if (value == "1") return true;
  if (value == "0") return false;
  throw ProtocolError("expected worker boolean 0 or 1");
}
void arity(const Message &message, std::size_t count) {
  if (message.fields.size() != count) throw ProtocolError("invalid worker command arguments");
}
// Producers only replace immutable entries in a bounded store. One reporter
// sends coalesced snapshots; neither the VM nor its callbacks write to IPC.
// Stop has its independent channel even if the UI stops draining image data.
class LiveReporter {
public:
  LiveReporter(Channel &channel, std::mutex &send_mutex, std::uint64_t generation,
               std::uint64_t request, const std::map<CellId, std::uint64_t> &revisions,
               amber::runtime::RuntimeRunCancellation token)
      : store(std::make_shared<amber::runtime::NotebookLiveStore>()), channel_(channel),
        send_mutex_(send_mutex), generation_(generation), request_(request),
        revisions_(revisions), token_(std::move(token)) {
    thread_ = std::thread([this] {
      try {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!done_) {
          wake_.wait_for(lock, 100ms, [&] { return done_; });
          lock.unlock(); flush(); lock.lock();
        }
      } catch (...) { failure_ = std::current_exception(); token_->request_cancel(); }
    });
  }
  ~LiveReporter() { join(); store->close(); }
  void finish(Session &session) {
    join();
    if (failure_) std::rethrow_exception(failure_);
    // Even a run that finishes before the reporter thread starts must emit
    // its terminal telemetry. Producers have drained and join owns flush now.
    flush();
    for (const auto &cell : latest_.cells)
      for (auto &target : session.cells) if (target.id == cell.id) {
        target.progress = cell.progress;
        // Live output is explicitly nontransactional. On cancellation retain
        // the last accepted frame, labelled stale by the cell outcome in UI.
        if (!target.ok && !cell.displays.empty()) target.displays = cell.displays;
        break;
      }
  }
  std::shared_ptr<amber::runtime::NotebookLiveStore> store;
private:
  void join() noexcept {
    { std::lock_guard<std::mutex> lock(mutex_); done_ = true; }
    wake_.notify_one();
    if (thread_.joinable()) thread_.join();
  }
  void flush() {
    std::vector<amber::runtime::NotebookLiveEvent> events;
    auto sequence = sent_;
    if (!store->snapshot_if_changed(&sequence, &events)) return;
    Presentation next;
    for (const auto &event : events) {
      const auto revision = revisions_.find(event.cell_id);
      if (revision == revisions_.end()) continue; // environment is not a cell
      auto found = std::find_if(next.cells.begin(), next.cells.end(),
          [&](const auto &c) { return c.id == event.cell_id; });
      if (found == next.cells.end()) {
        CellPresentation cell; cell.id = event.cell_id; cell.source_revision = revision->second;
        next.cells.push_back(std::move(cell)); found = std::prev(next.cells.end());
      }
      if (event.kind == amber::runtime::NotebookLiveEventKind::Progress) found->progress.push_back(event);
      else found->displays.push_back(event.display);
    }
    if (!next.cells.empty() || !latest_.cells.empty()) {
      std::lock_guard<std::mutex> send(send_mutex_);
      send_presentation(channel_, generation_, request_, next, 2s, true);
    }
    latest_ = std::move(next); sent_ = sequence;
  }
  Channel &channel_;
  std::mutex &send_mutex_;
  std::uint64_t generation_, request_, sent_ = 0;
  std::map<CellId, std::uint64_t> revisions_;
  amber::runtime::RuntimeRunCancellation token_;
  Presentation latest_;
  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable wake_;
  bool done_ = false;
  std::exception_ptr failure_;
};
std::size_t cell_index(const Session &session, const std::string &text) {
  const auto id = parse_id(text);
  for (std::size_t i = 0; i < session.cells.size(); ++i)
    if (session.cells[i].id == id) return i;
  throw ProtocolError("unknown worker cell");
}
int serve(std::uint64_t generation) {
  Channel data(kDataFd);
  Control control(generation);
  std::optional<LoadedProject> project;
  Session session;
  std::map<CellId, std::uint64_t> source_revisions;
  register_builtin_renderers();
  data.send({Kind::Ready, generation, 0, {}}, 2s);
  std::uint64_t previous = 0;
  while (true) {
    const auto message = data.receive(1s);
    if (!message) continue;
    if (message->generation != generation || message->request <= previous)
      throw ProtocolError("stale worker command");
    previous = message->request;
    try {
      if (message->kind == Kind::Shutdown) {
        arity(*message, 0);
        data.send({Kind::Result, generation, previous, {}}, 2s);
        return 0;
      }
      if (message->kind == Kind::Load) {
        if (message->fields.size() != 2 && message->fields.size() != 3)
          throw ProtocolError("Load expects path, sheet and optional explicit native trust");
        const bool trusted = message->fields.size() == 3 && boolean(message->fields[2]);
        // Read-only bootstrap. Reload in a fresh worker instead of destroying
        // a possibly live world under an inert document-open operation.
        if (project) throw ProtocolError("worker already has a sheet");
        auto candidate = load_project(message->fields[0]);
        load_project_into_session(&session, candidate, message->fields[1]);
        for (const auto &cell : session.cells) source_revisions[cell.id] = 0;
        project = std::move(candidate);
        if (trusted) for (const auto *name : {"ffi.call", "ffi.load", "fs.read"}) {
          amber::capability::CapabilityRequest grant;
          if (!amber::capability::parse_cli_grant(name, &grant)) throw ProtocolError("invalid native host grant");
          session.runtime_capability_grants.push_back(std::move(grant));
        }
        session.prepare_runtime = [&, trusted](const auto &module) {
          load_prepared_native_dependencies(*project, module, trusted);
        };
      } else {
        if (!project) throw ProtocolError("load a sheet before executing");
        if (message->kind == Kind::Sync) {
          arity(*message, 2);
          // Inert host draft/structure synchronization. Never save these bytes
          // or replace the runtime; evaluation reconciles the kernel graph.
          auto document = parse_project_document(message->fields[0]);
          const bool stale = boolean(message->fields[1]);
          const auto sheet = std::find_if(document.sheets.begin(), document.sheets.end(),
              [&](const auto &s) { return s.id == session.project_sheet_id; });
          if (sheet == document.sheets.end()) throw ProtocolError("missing synchronized sheet");
          std::vector<Cell> cells;
          std::map<CellId, std::uint64_t> revisions;
          for (const auto &source : sheet->cells) {
            const auto old = std::find_if(session.cells.begin(), session.cells.end(),
                [&](const auto &c) { return c.id == source.id && c.kind == source.kind; });
            Cell cell = old == session.cells.end() ? Cell{} : *old;
            const bool watch = source.kind == "code" && source.mode == CellMode::Watch;
            if (cell.source != source.source || cell.watch != watch) cell.dirty = true;
            cell.id = source.id; cell.kind = source.kind;
            cell.source = source.source; cell.formatting = source.formatting; cell.watch = watch;
            if (cell.kind != "code") cell.dirty = false;
            cells.push_back(std::move(cell)); revisions.emplace(source.id, previous);
            reserve_cell_id(source.id);
          }
          project->document = std::move(document);
          session.cells.swap(cells); source_revisions.swap(revisions);
          session.environment_stale = stale;
          session.selected = std::min(session.selected, session.cells.empty() ? 0U : session.cells.size() - 1);
        } else if (message->kind == Kind::Source) {
          arity(*message, 3);
          const auto index = cell_index(session, message->fields[0]);
          const bool watch = boolean(message->fields[2]);
          session.cells[index].source = message->fields[1];
          session.cells[index].watch = watch;
          session.cells[index].dirty = true;
          source_revisions[session.cells[index].id] = previous;
        } else if (message->kind == Kind::Run || message->kind == Kind::Apply) {
          arity(*message, message->kind == Kind::Run ? 2 : 1);
          const auto index = message->kind == Kind::Run ? cell_index(session, message->fields[0]) : 0;
          const bool force = boolean(message->fields.back());
          RunOutputCapture output(session);
          auto token = control.begin(previous);
          std::mutex send_mutex;
          data.send({Kind::Started, generation, previous, {}}, 2s);
          LiveReporter live(data, send_mutex, generation, previous, source_revisions, token);
          bool cancelled = false;
          std::exception_ptr failure;
          try {
            amber::runtime::RuntimeRunCancellationScope scope(token);
            amber::runtime::NotebookLiveScope live_scope(live.store);
            if (message->kind == Kind::Run)
              evaluate_from(&session, index, force, false, false);
            else
              apply_project_environment(&session, *project, force, false);
          } catch (...) {
            failure = std::current_exception();
            try { token->request_cancel(); } catch (...) { /* Still drain, or Force Stop. */ }
          }
          // Keep Stop addressing this run throughout draining. No new source
          // command, Apply or world destruction may race its live descendants.
          token->close_root();
          if (!token->wait_for_idle(0ms)) {
            {
              std::lock_guard<std::mutex> send(send_mutex);
              data.send({Kind::Draining, generation, previous,
                         {std::to_string(token->active_tasks())}}, 2s);
            }
            while (!token->wait_for_idle(100ms)) {}
          }
          cancelled = control.finish();
          output.finish();
          live.finish(session);
          if (failure) std::rethrow_exception(failure);
          auto presentation = presentation_from_session(session, source_revisions, cancelled);
          presentation.unobserved_task_failures = token->unobserved_failures();
          if (!presentation.unobserved_task_failures.empty())
            presentation.status = std::to_string(presentation.unobserved_task_failures.size()) +
                " unobserved child task failure(s)" + (cancelled ? " (run cancelled)" : "");
          send_presentation(data, generation, previous, presentation, 2s);
          continue;
        } else throw ProtocolError("unsupported worker command");
      }
      send_presentation(data, generation, previous,
                        presentation_from_session(session, source_revisions, false), 2s);
    } catch (const std::exception &error) {
      data.send({Kind::Error, generation, previous, {error.what()}}, 2s);
    }
  }
}
} // namespace
int main(int argc, char **argv) {
  if (argc != 3 || std::string(argv[1]) != "--notebook-worker") {
    std::cerr << "Internal notebook worker: start through its host supervisor.\n";
    return 2;
  }
  try { return serve(parse_id(argv[2])); }
  catch (const std::exception &error) {
    std::cerr << "notebook worker: " << error.what() << '\n';
    return 1;
  }
}
