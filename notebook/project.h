#pragma once

#include "notebook/model.h"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace amber::notebook {

// Unknown members are retained as validated JSON, including exact number
// spellings. Terminal clients must not discard native-UI metadata.
using ProjectExtraFields = std::map<std::string, std::string>;

struct ProjectCell {
  CellId id = 0;
  std::string kind = "code";
  std::string source;
  CellMode mode = CellMode::Watch;
  ProjectExtraFields extra;
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

struct ProjectDocument {
  std::string title;
  std::string active_sheet;
  std::vector<ProjectSheet> sheets;
  std::vector<ProjectModule> modules;
  // Explicit opt-in module IDs, not a directory scan. Loading never executes
  // them. Runtime module environments are a separate implementation layer.
  std::vector<std::string> auto_imports;
  ProjectExtraFields extra;
};

// Schema v1: a single atomic project.json holds all sheet documents and the
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

} // namespace amber::notebook
