#include "tools/notebook-macos/NotebookExecution.h"
#include "tools/notebook-worker/presentation.h"
#include <algorithm>
#include <stdexcept>
#include <thread>

using namespace amber::notebook;
using namespace amber::notebook::worker;
using namespace std::chrono_literals;

int NotebookExecutionControl::phase() const { std::lock_guard<std::mutex> lock(mutex_); return phase_; }
bool NotebookExecutionControl::forced() const { std::lock_guard<std::mutex> lock(mutex_); return force_; }
void NotebookExecutionControl::begin() {
  std::lock_guard<std::mutex> lock(mutex_);
  phase_ = 1; stop_ = force_ = false; request_ = {}; generation_ = 0; process_.reset();
  live_.reset();
}
void NotebookExecutionControl::publish_live(Presentation value) {
  auto snapshot = std::make_shared<const Presentation>(std::move(value));
  std::lock_guard<std::mutex> lock(mutex_);
  if (phase_ && !force_) live_ = std::move(snapshot);
}
std::shared_ptr<const Presentation> NotebookExecutionControl::take_live() {
  std::lock_guard<std::mutex> lock(mutex_);
  return std::exchange(live_, {});
}
void NotebookExecutionControl::bind(std::shared_ptr<Process> process) {
  std::lock_guard<std::mutex> lock(mutex_);
  process_ = std::move(process); generation_ = process_->generation();
  if (force_) process_->request_force_stop(generation_);
}
void NotebookExecutionControl::running(RequestId request) {
  std::lock_guard<std::mutex> lock(mutex_);
  request_ = request;
  if (!stop_ && !force_) phase_ = 2;
  if (stop_) process_->request_stop(request_);
}
void NotebookExecutionControl::draining() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!stop_ && !force_) phase_ = 3;
}
bool NotebookExecutionControl::stop(bool force) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!phase_) return false;
  if (force) {
    force_ = true; phase_ = 5;
    return !process_ || process_->request_force_stop(generation_);
  }
  if (force_) return false;
  stop_ = true; phase_ = 4;
  return !process_ || !request_.request || process_->request_stop(request_);
}
void NotebookExecutionControl::finish() {
  std::lock_guard<std::mutex> lock(mutex_);
  phase_ = 0; process_.reset(); request_ = {}; generation_ = 0;
}
bool NotebookExecutionControl::commit() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (force_) return false;
  phase_ = 0; process_.reset(); request_ = {}; generation_ = 0;
  return true;
}

namespace {
Presentation receive_snapshot(Process &process, RequestId request,
                              NotebookExecutionControl &control, bool execution,
                              const Session *session = nullptr, std::uint64_t revision = 0) {
  PresentationAssembler assembler(request.generation, request.request);
  std::optional<PresentationAssembler> live;
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  bool started = false, draining = false;
  while (true) {
    if (control.forced()) throw ProtocolError("worker terminated by user");
    const auto message = process.receive(100ms);
    if (!message) {
      if (!execution && std::chrono::steady_clock::now() >= deadline)
        throw ProtocolError("worker synchronization timed out");
      continue;
    }
    if (message->kind == Kind::Error)
      throw ProtocolError(message->fields.empty() ? "worker command failed" : message->fields[0]);
    if (message->kind == Kind::Started) {
      if (!execution || started) throw ProtocolError("unexpected worker Started");
      started = true; continue;
    }
    if (message->kind == Kind::Draining) {
      if (!started || draining) throw ProtocolError("unexpected worker Draining");
      draining = true; control.draining(); continue;
    }
    if (execution && !started) throw ProtocolError("missing worker Started");
    if (message->kind == Kind::LiveBegin || message->kind == Kind::LiveChunk || message->kind == Kind::LiveEnd) {
      if (!execution || !session) throw ProtocolError("unexpected live worker output");
      if (!live) live.emplace(request.generation, request.request, true);
      if (auto snapshot = live->accept(*message)) {
        for (const auto &cell : snapshot->cells) {
          const auto found = std::find_if(session->cells.begin(), session->cells.end(),
              [&](const auto &c) { return c.id == cell.id; });
          if (found == session->cells.end() || cell.source_revision != revision)
            throw ProtocolError("stale live cell output");
        }
        control.publish_live(std::move(*snapshot));
        live.reset();
      }
      continue;
    }
    if (live) throw ProtocolError("incomplete live worker output");
    if (auto snapshot = assembler.accept(*message)) return std::move(*snapshot);
  }
}
void terminate_and_reap(Process &process) {
  // SIGKILL acknowledgement is not completion. Remain on the owner queue until
  // waitpid confirms exit; main-thread controls/status stay available.
  process.request_force_stop(process.generation());
  while (!process.poll_exit()) std::this_thread::sleep_for(10ms);
}
void invalidate(Session &session) {
  for (auto &cell : session.cells) if (cell.kind == "code") cell.dirty = true;
}
void merge(Session &session, const Presentation &result, std::uint64_t revision,
           NotebookExecutionControl &control) {
  // All-or-nothing metadata validation before any publication. Session source,
  // selection, formatting, Watch mode and document dirtiness are host-owned.
  if (result.cells.size() != session.cells.size()) throw ProtocolError("worker sheet shape mismatch");
  auto cells = session.cells;
  for (std::size_t i = 0; i < cells.size(); ++i) {
    auto &cell = cells[i]; const auto &value = result.cells[i];
    if (cell.id != value.id || value.source_revision != revision)
      throw ProtocolError("stale worker cell revision");
    cell.dirty = value.dirty; cell.ok = value.ok;
    cell.result = value.result; cell.error = value.error;
    cell.result_format = value.result_format;
    cell.locals = value.locals; cell.output_events = value.output; cell.displays = value.displays;
    cell.progress = value.progress;
    cell.watch_epoch = value.watch_epoch; cell.watch_event_count = value.watch_event_count;
    cell.error_ranges = value.error_ranges; cell.errors = value.errors; cell.selected_error = 0;
  }
  std::string status = result.status;
  if (result.cancelled) status = "Cancelled — " + status;
  for (const auto &failure : result.unobserved_task_failures) {
    status += "\nTask " + std::to_string(failure.task_id) + ": " + failure.error_name + ": " + failure.message;
    if (failure.spawn_source.present)
      status += " (spawned at " + failure.spawn_source.file + ":" + std::to_string(failure.spawn_source.line) + ")";
  }
  auto environment_error = result.environment_error;
  auto environment_output = result.environment_output;
  // Linearize publication against Force Stop without allocating/copying under
  // the control lock. Once committed, a late Stop cannot kill the idle worker.
  if (!control.commit()) throw ProtocolError("worker terminated by user");
  session.cells.swap(cells); session.status.swap(status);
  session.environment_error.swap(environment_error); session.environment_output.swap(environment_output);
  session.environment_stale = result.environment_stale;
}
}

void NotebookExecution::run(ProjectTabs &tabs, ProjectTab &tab, const std::string &action, std::size_t index) {
  if (tab.module) throw std::runtime_error("Worker preview supports sheet execution only; module Run is not yet isolated.");
  if (!tabs.project().document.inputs.empty())
    throw std::runtime_error("Worker preview does not yet support project inputs. Reopen without preview for this project.");
  if (action == "apply") for (std::size_t i = 0; i < tabs.tabs().size(); ++i)
    if (tabs.tabs()[i]->module && tabs.modified(i)) throw std::runtime_error("Save modified module tabs before Apply.");
  auto &session = tab.session;
  auto &sheet = sheets_[session.project_sheet_id];
  if (sheet.interrupted) throw std::runtime_error("Worker was interrupted. Use Restart Worker before running again; no cells will be replayed automatically.");
  if (session.cells.empty() && action != "apply") return;
  auto document = tabs.project().document;
  auto target = std::find_if(document.sheets.begin(), document.sheets.end(),
      [&](const auto &s) { return s.id == session.project_sheet_id; });
  if (target == document.sheets.end()) throw ProtocolError("host sheet is missing");
  // Preserve unknown project/cell metadata, while substituting unsaved buffers
  // only for this sheet. Dependencies remain links to their original paths.
  std::vector<ProjectCell> cells;
  for (const auto &cell : session.cells) {
    const auto saved = std::find_if(target->cells.begin(), target->cells.end(),
        [&](const auto &c) { return c.id == cell.id; });
    auto value = saved == target->cells.end() ? ProjectCell{} : *saved;
    value.id = cell.id; value.kind = cell.kind; value.source = cell.source;
    value.mode = cell.watch ? CellMode::Watch : CellMode::Manual; value.formatting = cell.formatting;
    cells.push_back(std::move(value));
  }
  target->cells.swap(cells);
  const auto source = serialize_project_document(document);
  control->begin();
  struct Finish { NotebookExecutionControl &control; ~Finish() { control.finish(); } } finish{*control};
  try {
    if (!sheet.process) {
      sheet.process = std::make_shared<Process>(); sheet.process->start(executable_);
      control->bind(sheet.process);
      const auto ready = sheet.process->receive(5s);
      if (!ready || ready->kind != Kind::Ready) throw ProtocolError("worker handshake failed");
      const auto load = sheet.process->send(Kind::Load, {tabs.project().directory.string(), session.project_sheet_id,
                                                       native_trusted ? "1" : "0"});
      (void)receive_snapshot(*sheet.process, load, *control, false);
    } else control->bind(sheet.process);
    const auto sync = sheet.process->send(Kind::Sync, {source, session.environment_stale ? "1" : "0"});
    (void)receive_snapshot(*sheet.process, sync, *control, false);
    const auto request = action == "apply"
        ? sheet.process->send(Kind::Apply, {"0"})
        : sheet.process->send(Kind::Run, {std::to_string(session.cells.at(index).id), action == "run_all" ? "1" : "0"});
    control->running(request);
    auto result = receive_snapshot(*sheet.process, request, *control, true, &session, sync.request);
    // Stop/Force Stop may race the final frame. Never publish a killed world's
    // snapshot even if it was fully received before the main-thread request.
    if (control->forced()) throw ProtocolError("worker terminated by user");
    merge(session, result, sync.request, *control);
    if (action == "apply" && result.environment_error.empty() && !result.environment_stale)
      for (const auto &other : tabs.tabs()) if (!other->module && other.get() != &tab) {
        other->session.environment_stale = true;
        invalidate(other->session);
      }
  } catch (const std::exception &error) {
    sheet.interrupted = true;
    invalidate(session);
    session.status = std::string("Worker interrupted; last results retained. Restart required. ") + error.what();
    if (sheet.process && sheet.process->generation()) terminate_and_reap(*sheet.process);
    throw;
  }
}
void NotebookExecution::restart(ProjectTab &tab) {
  if (tab.module) throw std::runtime_error("Restart requires a sheet");
  const auto found = sheets_.find(tab.session.project_sheet_id);
  if (found != sheets_.end()) {
    if (found->second.process && found->second.process->generation()) terminate_and_reap(*found->second.process);
    sheets_.erase(found);
  }
  invalidate(tab.session); tab.session.environment_stale = false;
  tab.session.status = "Worker reset; next Run starts a fresh runtime. No cells replayed.";
}
