#pragma once

#include "notebook/model.h"
#include "runtime/notebook_inputs.h"
#include "runtime/notebook_image.h"
#include "runtime/audio.h"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace sputnik::notebook {

// Unknown members are retained as validated JSON, including exact number
// spellings. Terminal clients must not discard native-UI metadata.
using ProjectExtraFields = std::map<std::string, std::string>;

// Text cells carry plain UTF-8 source plus an optional, inert rich-text
// formatting payload.  The formatting member is kept as validated JSON so
// clients that do not render rich text can round-trip it without interpreting
// or evaluating it.
struct ProjectTextContent {
  std::string source;
  // Empty means plain text.  Otherwise this is a validated formatting object
  // of the form {"version":1,"runs":[...]}.
  std::string formatting;
};

ProjectTextContent parse_project_text_content(const std::string &json);
std::string serialize_project_text_content(const ProjectTextContent &content);
void validate_project_text_content(const ProjectTextContent &content);

struct ProjectCell {
  CellId id = 0;
  std::string kind = "code";
  std::string source;
  CellMode mode = CellMode::Watch;
  ProjectExtraFields extra;
  // Only meaningful for kind == "text"; empty means plain text.
  std::string formatting;
};

struct ProjectSheet {
  std::string id;
  std::string title;
  std::vector<ProjectCell> cells;
  ProjectExtraFields extra;
};

struct ProjectModule {
  std::string id;
  // Portable, project-relative .s path; never an absolute path or traversal.
  std::string path;
  ProjectExtraFields extra;
};

struct ProjectInput {
  std::string id, title;
  std::string type = "number"; // number, integer, boolean, string, range
  runtime::NotebookInputValue initial = 0.0;
  std::optional<double> minimum, maximum;
  ProjectExtraFields extra;
};

// An explicit local package root, resolved relative to the .sputnikbook
// directory (or absolute). Never copied into the project's editable modules.
struct ProjectDependency {
  std::string path;
  bool auto_import = false;
  ProjectExtraFields extra;
};

struct ProjectBoardPanel;

struct ProjectComponent {
  std::string id, kind, title;
  std::string input; // input component -> project input
  CellId cell = 0;   // value/result/progress/plot/run -> controller sheet code cell
  std::string binding; // empty text binding -> cell expression result
  ProjectExtraFields extra;
  std::string row = {}; // explicit row ID; empty on legacy ordered grids
  unsigned span = 12;
  std::string h_align = "stretch", v_align = "top";
  std::optional<std::vector<ProjectBoardPanel>> panels = std::nullopt; // tabs own independent row grids
};

// Authored configuration belongs to a stable code-cell identity.
struct ProjectCellUI {
  std::vector<ProjectInput> inputs;
  std::vector<ProjectComponent> pre, post;
  ProjectExtraFields extra;
};
ProjectCellUI parse_project_cell_ui(const std::string &json);
std::string serialize_project_cell_ui(const ProjectCellUI &ui);
void validate_project_cell_ui(const ProjectCellUI &ui, CellId owner);

struct ProjectBoardRow {
  std::string id;
  std::optional<double> height; // UI points; absent means content height
  ProjectExtraFields extra;
};

struct ProjectBoardPanel {
  std::string id, title;
  std::vector<ProjectBoardRow> rows;
  std::vector<ProjectComponent> components;
  ProjectExtraFields extra;
};

struct ProjectBoard {
  std::string id, title, sheet;
  unsigned columns = 2;
  std::vector<ProjectComponent> components;
  ProjectExtraFields extra;
  std::optional<std::vector<ProjectBoardRow>> rows = std::nullopt;
};

// Nonvisual board service. Settings use extension fields so older hosts retain
// their exact JSON spelling even when they do not render the service.
struct PeriodicWorkSettings {
  std::string module_id, function;
  double period_seconds = 60;
  std::string timing = "fixed_delay", retention = "tail";
  unsigned tail_limit = 100;
  bool store_values = true, store_errors = true, run_immediately = true;
  bool stop_on_error = false, pause_when_hidden = true;
};
PeriodicWorkSettings project_periodic_work_settings(const ProjectComponent &);

struct ProjectActionInvocation {
  std::string source;
  std::shared_ptr<const runtime::NotebookInputSnapshot> inputs;
};
ProjectActionInvocation project_action_invocation(const ProjectComponent &,
    const runtime::NotebookInputSnapshot &);

struct ProjectImageAsset { std::string path, caption; };
struct ProjectImageEdit {
  std::string board, component, caption;
  std::size_t selected = 0;
  std::vector<runtime::NotebookDisplay> images;
};
std::vector<ProjectImageAsset> project_image_assets(const ProjectComponent &component);
std::string serialize_project_image_assets(const std::vector<ProjectImageAsset> &assets);
ProjectImageEdit parse_project_image_edit(const std::string &json);
bool project_image_option(const ProjectComponent &component, const std::string &key);
std::string project_image_channel(const ProjectComponent &component);

struct ProjectDocument {
  std::string title;
  std::string active_sheet;
  std::vector<ProjectSheet> sheets;
  std::vector<ProjectModule> modules;
  // Explicit opt-in module IDs, not a directory scan. Loading never executes
  // them. Runtime module environments are a separate implementation layer.
  std::vector<std::string> auto_imports;
  ProjectExtraFields extra;
  unsigned version = 1;
  std::vector<ProjectInput> inputs;
  std::vector<ProjectBoard> boards;
  std::vector<ProjectDependency> dependencies;
};

runtime::NotebookInputValue parse_project_input_value(const std::string &json);
// Inert Config Editor validation/formatting. Retains number/string lexemes,
// rejects duplicate keys and numbers outside the runtime's Int64/finite Float.
// pretty=false validates and returns the original UTF-8 source verbatim.
std::string project_config_json(const std::string &source, bool pretty = false);
std::string project_config_document(const std::string &source, const std::string &format, bool pretty = false);
std::map<std::string, std::string> parse_project_input_edits(const std::string &json);
ProjectInput parse_project_input(const std::string &json);
std::string serialize_project_input_value(const runtime::NotebookInputValue &value);
void validate_project_input_value(const ProjectInput &input, const runtime::NotebookInputValue &value);
void validate_project_control_values(const ProjectDocument &, const runtime::NotebookInputSnapshot &);
std::shared_ptr<const runtime::NotebookInputSnapshot> project_input_defaults(const ProjectDocument &document);
ProjectBoard parse_project_board(const std::string &json);
std::string serialize_project_board(const ProjectBoard &board);
ProjectDependency parse_project_dependency(const std::string &json);

// Schema v1/v2/v3: a single atomic project.json holds all sheet documents and the
// module manifest; module sources remain ordinary .s files beside it.
// Throws std::runtime_error on malformed/unsupported documents. IDs are JSON
// decimal strings to avoid loss in native or browser clients using doubles.
ProjectDocument parse_project_document(const std::string &json);
std::string serialize_project_document(const ProjectDocument &document);
void validate_project_document(const ProjectDocument &document);
ProjectDocument make_project_document(std::string title);

struct LoadedProject {
  std::filesystem::path directory;
  ProjectDocument document;
  // Exact disk contents are the optimistic-concurrency baseline for saving.
  std::string baseline;
};

std::shared_ptr<const runtime::NotebookImageInputs> load_project_image_inputs(
    const ProjectDocument &, const std::filesystem::path &, const std::string &sheet);
std::string store_project_image(const LoadedProject &, const runtime::NotebookDisplay &);

struct ProjectComponentDataSource { std::string identity, channel, source; unsigned width = 0, height = 0, rate = 0, channels = 0; std::uint64_t frames = 0, cell = 0; };
ProjectComponentDataSource project_component_data_source(const std::string &kind, const std::string &json);
struct ProjectComponentDataEdit { std::string board, component, kind, json; };
ProjectComponentDataEdit parse_project_component_data_edit(const std::string &);
std::string project_component_data_channel(const ProjectComponent &, const std::string &kind);
void validate_project_component_data(const std::string &kind, const std::string &json);
void validate_project_annotation_data(const ProjectComponent &, const std::string &kind, const std::string &json);

struct ProjectAudioEdit {
  std::string board, component, name;
  std::optional<runtime::AudioDisplay> audio;
};
ProjectAudioEdit parse_project_audio_edit(const std::string &json);
std::string project_audio_source(const ProjectComponent &component);
bool project_owns_audio_input(const ProjectComponent &component);
std::string project_audio_channel(const ProjectComponent &component);
std::string project_audio_asset(const ProjectComponent &component);
std::shared_ptr<const runtime::NotebookAudioInputs> load_project_audio_inputs(const ProjectDocument &, const std::filesystem::path &, const std::string &sheet);
std::string store_project_audio(const LoadedProject &, const runtime::AudioDisplay &);

struct LoadedProjectModule {
  std::string id;
  std::string path;
  std::string source;
  // Exact source bytes at load/last save, for conflict detection.
  std::string baseline;
};

// Does not initialize a runtime or execute module/cell source. Symlinked
// project members and paths outside the project are rejected.
LoadedProject load_project(const std::filesystem::path &directory);
// Creates a NEW directory only. Existing directories are never overwritten.
LoadedProject create_project(const std::filesystem::path &directory,
                             const ProjectDocument &document);
// Writes only project.json, never modules or resources. Conflicting external
// edits fail without replacement. Cooperating writers serialize via a lock;
// an unrelated editor racing the final replacement cannot be made atomic.
void save_project(LoadedProject *project, const ProjectDocument &document);

// Module source persistence is intentionally separate from project.json.
// Loading does not compile or execute source; saving accepts incomplete source
// so an editor can persist work independently of explicit runtime Apply.
// Access is rejected while project.document differs from its disk baseline;
// callers must save the manifest or reopen before following module paths.
LoadedProjectModule load_project_module(const LoadedProject &project,
                                        const std::string &module_id);
void save_project_module(const LoadedProject &project,
                         LoadedProjectModule *module,
                         const std::string &source);

// Stages a new module source and its manifest entry, publishing the source
// before the manifest. The entry must name a new modules/<id>.s path (nested
// directories below modules/ are also accepted); source is persisted as-is
// and is never executed or added to auto_imports. On success project and the
// returned module carry the new baselines. If source publication succeeds but
// the manifest cannot be committed, the source is deliberately left at its
// path, unreferenced, and the exception identifies that recoverable state.
LoadedProjectModule create_project_module(LoadedProject *project,
                                          const ProjectModule &entry,
                                          const std::string &source);

} // namespace sputnik::notebook
