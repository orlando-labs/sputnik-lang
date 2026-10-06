#pragma once

#include "notebook/model.h"
#include "runtime/notebook_inputs.h"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace amber::notebook {

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
  // Portable, project-relative .am path; never an absolute path or traversal.
  std::string path;
  ProjectExtraFields extra;
};

struct ProjectInput {
  std::string id, title;
  std::string type = "number"; // number, integer, boolean, string
  runtime::NotebookInputValue initial = 0.0;
  std::optional<double> minimum, maximum;
  ProjectExtraFields extra;
};

// An explicit local package root, resolved relative to the .amberbook
// directory (or absolute). Never copied into the project's editable modules.
struct ProjectDependency {
  std::string path;
  bool auto_import = false;
  ProjectExtraFields extra;
};

struct ProjectComponent {
  std::string id, kind, title;
  std::string input; // input component -> project input
  CellId cell = 0;   // text/plot/run component -> controller sheet cell
  std::string binding; // empty text binding -> cell expression result
  ProjectExtraFields extra;
};

struct ProjectBoard {
  std::string id, title, sheet;
  unsigned columns = 2;
  std::vector<ProjectComponent> components;
  ProjectExtraFields extra;
};

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
ProjectInput parse_project_input(const std::string &json);
std::string serialize_project_input_value(const runtime::NotebookInputValue &value);
void validate_project_input_value(const ProjectInput &input, const runtime::NotebookInputValue &value);
std::shared_ptr<const runtime::NotebookInputSnapshot> project_input_defaults(const ProjectDocument &document);
ProjectBoard parse_project_board(const std::string &json);
std::string serialize_project_board(const ProjectBoard &board);

// Schema v1/v2/v3: a single atomic project.json holds all sheet documents and the
// module manifest; module sources remain ordinary .am files beside it.
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
// before the manifest. The entry must name a new modules/<id>.am path (nested
// directories below modules/ are also accepted); source is persisted as-is
// and is never executed or added to auto_imports. On success project and the
// returned module carry the new baselines. If source publication succeeds but
// the manifest cannot be committed, the source is deliberately left at its
// path, unreferenced, and the exception identifies that recoverable state.
LoadedProjectModule create_project_module(LoadedProject *project,
                                          const ProjectModule &entry,
                                          const std::string &source);

} // namespace amber::notebook
