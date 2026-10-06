#pragma once

#include "bytecode/format.h"
#include "notebook/notebook.h"
#include "runtime/module_loader.h"
#include "runtime/text.h"
#include "runtime/notebook_live.h"
#include "tools/iamber/activity.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// The state and evaluation adapter used by iamber's UI.  This header is
// deliberately independent of curses so non-interactive clients can evaluate
// notebook cells without linking the terminal frontend.

struct LocalView {
  std::string name;
  std::string role;
  std::string binding_kind;
  std::string value;
  // Plain display for notebook prose; strings have no debug quotation marks.
  // Materialized with the successful execution, never evaluated by the UI.
  std::string text_value;
  bool initialized = false;
  bool watched = false;
  std::uint64_t watch_cell_id = 0;
  std::uint64_t watch_revision = 0;
};

struct CodeErrorRange {
  int start_line = 0;
  int start_column = 0;
  int end_line = 0;
  int end_column = 0;
  bool whole_line = false;
};

struct SourceErrorRange {
  std::string file;
  std::size_t start_line = 0;
  std::size_t start_column = 0;
  std::size_t end_line = 0;
  std::size_t end_column = 0;
  std::size_t start_offset = 0;
  std::size_t end_offset = 0;
  bool has_offsets = false;
  bool whole_line = false;
};

struct CellErrorRange {
  std::size_t cell_index = 0;
  CodeErrorRange range;
};

struct CellErrorView {
  std::string message;
  CodeErrorRange range;
  bool has_range = false;
};

struct ResultFormat {
  std::string pretty;
  bool is_container = false;
  bool truncated = false;
  bool pretty_truncated = false;
};

struct Cell {
  amber::notebook::CellId id = 0;
  // Non-code project cells are opaque, read-only placeholders in iamber.
  std::string kind = "code";
  std::string source;
  std::size_t cursor = 0;
  bool watch = true;
  // An edit/invalidation still needs an evaluation attempt. False also
  // covers a current diagnostic; it does not imply the source was installed.
  // Backend/source equality controls the runtime-event barrier separately.
  bool dirty = true;
  bool running = false;
  bool ok = false;
  std::string result = "not evaluated";
  ResultFormat result_format;
  std::string error;
  std::vector<LocalView> locals;
  std::vector<amber::runtime::RuntimeTextOutputEvent> output_events;
  std::uint64_t watch_epoch = 0;
  std::size_t watch_event_count = 0;
  std::vector<CodeErrorRange> error_ranges;
  std::vector<CellErrorView> errors;
  std::size_t selected_error = 0;
  // Inert rich-text payload for kind == "text"; empty means plain text.
  std::string formatting;
  std::vector<amber::runtime::NotebookDisplay> displays;
  std::vector<amber::runtime::NotebookLiveEvent> progress;
};

// Exact bundled sources selected by a project's ordered auto_imports list.
// Opening a sheet only reads these bytes; compilation, module initialization,
// and publication belong to an explicit environment Apply/evaluation step.
struct BundledModuleSource {
  std::string id;
  std::string path;
  std::string source;
  // Dependency-only modules are available to explicit imports but do not
  // contribute names to the sheet's implicit environment.
  bool auto_import = true;
  // Read failures are inert on project open, but diagnosed by explicit Apply.
  std::string load_error = {};
};

struct BundledEnvironmentDiagnostic {
  std::string module_id;
  std::string path;
  std::string message;
  std::vector<SourceErrorRange> source_ranges;
};

struct CompiledBundledModule {
  std::string id;
  std::string path;
  amber::bytecode::BcModule module;
};

struct BundledExportBinding {
  std::string name;
  std::string qualified_path;
  std::string module_id;
  std::string kind;
  std::uint32_t target_index = 0;
};

// Compile/link preparation is deliberately separate from project loading and
// runtime publication. This operation executes no Amber code. On any error
// the ambient binding map is empty, so a caller cannot publish a partial
// environment accidentally.
struct BundledEnvironmentPreparation {
  bool ok = false;
  std::vector<CompiledBundledModule> modules;
  std::vector<BundledExportBinding> exports;
  std::map<std::string, std::string> ambient_constant_paths;
  std::vector<BundledEnvironmentDiagnostic> diagnostics;
  std::shared_ptr<const amber::bytecode::BcModule> image;
  std::map<std::string, std::map<std::string, std::string>> import_paths;
};

// Opaque persistent backend owned by one Session.  Keeping this out of the
// public UI model lets curses continue to treat Session as its state bag while
// the VM/kernel/image lifetimes remain explicit in session.cpp.
struct SessionBackend;

// Owner-thread hook invoked before execution, not from an output-writing
// thread. A host may retain these thread-safe sinks until a run drains. Cell 0
// denotes environment initialization. The callback must not mutate Session.
using SessionOutputObserver = std::function<void(
    amber::notebook::CellId,
    std::shared_ptr<amber::runtime::RuntimeTextWriter>,
    std::shared_ptr<amber::runtime::RuntimeTextWriter>)>;

struct Session {
private:
  // First member so move-assignment closes the old mailbox before replacing
  // its backend. The destructor explicitly closes before member teardown.
  SessionActivityChannel activity_;
  SessionActivityNotifier activity_wakeup_;

public:
  std::shared_ptr<const amber::runtime::NotebookInputSnapshot> project_inputs;
  Session();
  ~Session();
  Session(const Session &) = delete;
  Session &operator=(const Session &) = delete;
  Session(Session &&) noexcept;
  Session &operator=(Session &&) noexcept;

  // Acquire these on the Session owner thread, then hand copies to workers.
  // Only the handles are thread-safe; they never retain or access Session.
  SessionActivityNotifier activity_notifier() const noexcept;
  SessionActivityWaiter activity_waiter() const noexcept;
  // Attach before creating a backend. Tab-local hints stay independent while
  // this weak host notifier also wakes the project's stable event descriptor.
  void set_activity_wakeup(const SessionActivityNotifier &notifier);
  void close_activity() noexcept;

  std::vector<Cell> cells;
  // The last planned graph inputs are retained so an edit can invalidate
  // consumers of outputs that disappeared from the rebuilt graph.
  std::vector<amber::notebook::CellSource> dependency_snapshot;
  std::size_t selected = 0;
  int editor_scroll = 0;
  int cell_scroll = 0;
  int preferred_column = 0;
  bool auto_watch = true;
  // Applied when the persistent RuntimeWorld is first built. Keeping this
  // host policy in Session makes bounded-stream recovery testable and lets a
  // future native frontend choose its memory/latency trade-off explicitly.
  std::size_t runtime_watch_event_capacity = 65536U;
  // Explicit host grants used when creating the persistent notebook world.
  std::vector<amber::runtime::RuntimeCapabilityGrant> runtime_capability_grants;
  // Execution-time host hook, never called on document open. Isolated workers
  // use this to register explicitly trusted, prepared native dependencies.
  std::function<void(const amber::bytecode::BcModule &)> prepare_runtime;
  std::string status = "iamber ready";
  std::string project_sheet_id;
  std::string project_label;
  // Display-only tab strip, refreshed by the terminal owner before drawing.
  std::string tab_bar;
  // Another sheet applied a newer bundled source snapshot. Retain this
  // sheet's values for display but require explicit Apply before execution.
  bool environment_stale = false;
  std::vector<BundledModuleSource> bundled_modules;
  std::string environment_error;
  std::vector<amber::runtime::RuntimeTextOutputEvent> environment_output;
  SessionOutputObserver output_observer;
  // Owner-thread guard: progress callbacks may pump the UI reentrantly.
  // Callbacks may inspect state/pump events, but must not directly replace
  // the Session, its cells or its backend while an evaluation is active.
  bool evaluation_active = false;
  bool environment_apply_active = false;
  // A module editor is a single full-profile Amber source buffer. Its Run is
  // isolated from the notebook RuntimeWorld. Save persists its source; the
  // sheet's explicit Apply loads it into a new environment generation.
  bool module_editor = false;
  std::string project_module_id;
  std::string project_module_path;
  // Document edits are independent of evaluation dirtiness/results.
  bool document_dirty = false;
  std::string execution_block_reason;
  std::unique_ptr<SessionBackend> backend;
};

// Called by the project owner after publishing one validated input batch.
// Never starts a sheet that has not been run; never runs unsaved code edits.
void apply_project_input_changes(Session *session, const std::set<std::string> &keys);

struct CompileResult {
  bool ok = false;
  amber::bytecode::BcModule module;
  std::string error;
  std::vector<SourceErrorRange> error_ranges;
  std::string module_name;
};

struct EvalView {
  std::vector<amber::runtime::NotebookLiveEvent> progress;
  bool ok = false;
  std::string result;
  ResultFormat result_format;
  std::string error;
  std::vector<LocalView> locals;
  std::vector<amber::runtime::RuntimeTextOutputEvent> output_events;
  std::uint64_t watch_epoch = 0;
  std::size_t watch_event_count = 0;
  std::vector<CellErrorRange> error_ranges;
  std::vector<amber::runtime::NotebookDisplay> displays;
};

CompileResult compile_source_text(const std::string &source,
                                  const std::string &source_path,
                                  const std::string &module_name_override = {});

BundledEnvironmentPreparation prepare_bundled_environment(
    const std::vector<BundledModuleSource> &sources,
    const std::vector<std::string> &explicit_imports = {});

// Cross the same bytecode serialization boundary used by persisted modules
// before an immutable notebook image is handed to RuntimeWorld.  The
// deserializer runs the complete bytecode verifier, so callers receive a
// decoded module rather than the mutable compiler snapshot.  A null result
// means that serialization or verification failed; error receives a compact
// diagnostic when supplied.  This small public seam also lets non-UI tests
// exercise the initial-image trust boundary with malformed modules.
std::shared_ptr<const amber::bytecode::BcModule>
verified_notebook_image(const amber::bytecode::BcModule &candidate,
                        std::string *error = nullptr);

std::vector<amber::notebook::CellSource>
notebook_sources(const std::vector<Cell> &cells);
amber::notebook::DependencyGraph
dependency_graph_for_cells(const std::vector<Cell> &cells);
std::optional<std::string>
cyclic_watch_error_for_cell(const std::vector<Cell> &cells, std::size_t index);

EvalView watch_cycle_eval_view(std::string message);
EvalView evaluate_prefix(const std::vector<Cell> &cells, std::size_t end_index);

void clear_error_ranges_until(Session *session, std::size_t end_index);
void apply_eval(Session *session, std::size_t index, EvalView view);
void clamp_selected_error(Cell *cell);
bool selected_cell_has_errors(const Session *session);
void focus_selected_error(Session *session);

// Owner-thread notification. It may inspect display state or pump events,
// not mutate sources/ownership. Reentrant execution/session-load APIs defer
// or reject while the outer plan holds its execution guard.
using EvaluationProgress =
    std::function<void(Session *session, std::size_t cell_index)>;

// Explicit full-runtime restart. Compile/link/init failure preserves the
// active backend and its results. Successful publication discards old runtime
// values, runs Watch cells, and leaves Manual cells stale unless force_all.
// true means the environment was applied, even if a subsequent cell faults
// or its host progress callback interrupts the automatic plan.
bool apply_bundled_environment(
    Session *session, const std::vector<BundledModuleSource> &sources,
    bool force_all = false, bool show_running = false,
    const EvaluationProgress &progress = {});

// One owner-thread pass over the RuntimeWorld watch stream. The disposition
// is intentionally richer than the legacy bool API so a native event loop can
// distinguish an idle stream from a coalesced reentrant call or a cursor that
// requires host recovery. No result exposes or publishes the private cursor.
enum class RuntimeEventPumpDisposition {
  Unavailable,
  NoWork,
  // Visible sources/policies differ from the installed backend. Runtime
  // pumping must not run old code or clear an editor's dirty state. No batch
  // has been polled/acknowledged; resume after explicit source evaluation.
  DeferredByEdits,
  Acknowledged,
  Busy,
  RetryRequired,
  InvalidCursor,
  Failed,
};

struct RuntimeEventPumpResult {
  RuntimeEventPumpDisposition disposition =
      RuntimeEventPumpDisposition::Unavailable;
  std::size_t observed_events = 0;
  std::size_t acknowledged_batches = 0;
  std::size_t resynchronized_batches = 0;
  std::size_t rounds = 0;
  // False records a cell/plan failure. Ordinary event batches are still
  // acknowledged after publishing that error barrier; failed conservative
  // resyncs retain their cursor and return RetryRequired instead.
  bool execution_complete = true;
  // A terminal error/retry left the currently observed cursor unpublished.
  // The owner must repair or back off before pumping it again.
  bool cursor_retained = false;
  // Conservative signal that the bounded drain stopped at its fairness cap;
  // a native owner should schedule another pump rather than wait indefinitely.
  bool drain_limit_reached = false;
  std::string error;

  bool handled() const noexcept { return acknowledged_batches != 0U; }
  // RetryRequired is reserved for an incomplete conservative resync. Failed
  // plus needs_recovery() requires host repair/backoff, not a tight retry.
  bool retry_required() const noexcept {
    return disposition == RuntimeEventPumpDisposition::RetryRequired;
  }
  bool needs_recovery() const noexcept { return cursor_retained; }
};

// Evaluates the graph's invalidation plan.  The public overload is UI-free;
// the progress variant lets the curses frontend redraw a running cell.
void evaluate_from(Session *session, std::size_t start, bool force_all,
                   bool edit_mode = false, bool show_running = true);
void evaluate_from_with_progress(Session *session, std::size_t start,
                                 bool force_all, bool show_running,
                                 const EvaluationProgress &progress);

// Drains bounded RuntimeWorld watch batches and evaluates their automatic
// kernel plans. This must run on the Session owner thread: producer/waiter
// threads may only wake that owner. A successor cursor is published only after
// the corresponding plan has been processed. Reentrant calls are coalesced.
RuntimeEventPumpResult
pump_runtime_events_detailed(Session *session, bool show_running = true,
                             const EvaluationProgress &progress = {});

// Compatibility wrapper. Returns true iff at least one batch advanced the
// private acknowledgement cursor, even if a later round requests recovery.
bool pump_runtime_events(Session *session, bool show_running = true,
                         const EvaluationProgress &progress = {});
