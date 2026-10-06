#include "tools/iamber/project_session.h"
#include "tools/iamber/dependencies.h"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

std::vector<BundledModuleSource> load_project_bundled_sources(
    const amber::notebook::LoadedProject &project, bool defer_dependency_errors) {
  std::vector<BundledModuleSource> result;
  const std::set<std::string> selected(project.document.auto_imports.begin(),
                                        project.document.auto_imports.end());
  result.reserve(project.document.modules.size());
  // Preserve auto_imports order for deterministic initialization/conflicts.
  for (const std::string &id : project.document.auto_imports) {
    const auto module = amber::notebook::load_project_module(project, id);
    result.push_back({module.id, module.path, module.source, true});
  }
  for (const auto &entry : project.document.modules) {
    if (selected.count(entry.id) != 0U)
      continue;
    const auto module = amber::notebook::load_project_module(project, entry.id);
    result.push_back({module.id, module.path, module.source, false});
  }
  std::size_t dependency_bytes = 0;
  std::size_t dependency_modules = 0;
  for (const auto &dependency : project.document.dependencies) {
    try {
      auto modules = load_directory_dependency(project.directory, dependency);
      for (const auto &module : modules) dependency_bytes += module.source.size();
      dependency_modules += modules.size();
      if (dependency_bytes > 128 * 1024 * 1024 || dependency_modules > 4096)
        throw std::runtime_error("project dependency source budget exceeded (128 MiB / 4096 modules)");
      result.insert(result.end(), std::make_move_iterator(modules.begin()), std::make_move_iterator(modules.end()));
    } catch (const std::exception &error) {
      if (!defer_dependency_errors) throw;
      result.push_back({"", dependency.path, "", false, error.what()});
      if (dependency_bytes > 128 * 1024 * 1024 || dependency_modules > 4096) break;
    }
  }
  return result;
}

void load_project_into_session(Session *session,
                               const amber::notebook::LoadedProject &project,
                               const std::string &sheet_id) {
  if (!session)
    throw std::invalid_argument("null project session");
  if (session->evaluation_active)
    throw std::runtime_error("cannot replace a session during evaluation");
  amber::notebook::validate_project_document(project.document);
  const auto &id = sheet_id.empty() ? project.document.active_sheet : sheet_id;
  const auto found = std::find_if(
      project.document.sheets.begin(), project.document.sheets.end(),
      [&](const auto &sheet) { return sheet.id == id; });
  if (found == project.document.sheets.end()) {
    throw std::runtime_error("unknown project sheet: " + id);
  }
  Session loaded;
  loaded.project_inputs = amber::notebook::project_input_defaults(project.document);
  loaded.project_sheet_id = id;
  loaded.project_label = project.document.title + "/" + found->title;
  loaded.bundled_modules = load_project_bundled_sources(project, true);
  for (const auto &sheet : project.document.sheets) {
    for (const auto &cell : sheet.cells)
      amber::notebook::reserve_cell_id(cell.id);
  }
  loaded.cells.reserve(found->cells.size());
  for (const auto &saved : found->cells) {
    Cell cell;
    cell.id = saved.id;
    cell.kind = saved.kind;
    cell.formatting = saved.formatting;
    if (saved.kind == "code") {
      cell.source = saved.source;
      cell.watch = saved.mode == amber::notebook::CellMode::Watch;
    } else {
      if (saved.kind == "text")
        cell.source = saved.source;
      cell.watch = false;
      cell.dirty = false;
      cell.result = "preserved (read-only " + saved.kind + ")";
    }
    loaded.cells.push_back(std::move(cell));
  }
  loaded.status = "project opened; not evaluated";
  *session = std::move(loaded);
}

bool apply_project_environment(
    Session *session, const amber::notebook::LoadedProject &project,
    bool force_all, bool show_running, const EvaluationProgress &progress) {
  if (session == nullptr || session->module_editor ||
      session->project_sheet_id.empty())
    throw std::invalid_argument("environment Apply requires a project sheet");
  return apply_bundled_environment(session, load_project_bundled_sources(project),
                                    force_all, show_running, progress);
}

amber::notebook::ProjectDocument
project_document_from_session(const Session &session,
                              const amber::notebook::LoadedProject &project) {
  auto document = project.document;
  const auto found = std::find_if(
      document.sheets.begin(), document.sheets.end(),
      [&](const auto &sheet) { return sheet.id == session.project_sheet_id; });
  if (found == document.sheets.end())
    throw std::runtime_error("session has no project sheet");
  std::map<amber::notebook::CellId, amber::notebook::ProjectCell> original;
  for (const auto &cell : found->cells)
    original.emplace(cell.id, cell);
  std::vector<amber::notebook::ProjectCell> cells;
  std::set<amber::notebook::CellId> kept;
  cells.reserve(session.cells.size());
  for (const auto &cell : session.cells) {
    amber::notebook::ProjectCell saved;
    const auto previous = original.find(cell.id);
    if (previous != original.end())
      saved = previous->second;
    if (previous != original.end() && cell.kind != saved.kind)
      throw std::runtime_error("changing a cell kind is not supported");
    saved.id = cell.id;
    if (cell.kind == "code") {
      saved.source = cell.source;
      saved.mode = cell.watch ? amber::notebook::CellMode::Watch
                              : amber::notebook::CellMode::Manual;
    } else if (cell.kind == "text") {
      // The terminal keyboard editor does not expose text editing, but the
      // shared project/session adapter is also used by the native bridge.
      // Keep text inert while allowing that bridge to create/edit it.
      saved.kind = "text";
      saved.source = cell.source;
      saved.formatting = cell.formatting;
    } else if (previous == original.end()) {
      throw std::runtime_error("cannot create an opaque cell in iamber");
    }
    kept.insert(cell.id);
    cells.push_back(std::move(saved));
  }
  for (const auto &[id, cell] : original) {
    if (cell.kind != "code" && cell.kind != "text" && !kept.count(id)) {
      throw std::runtime_error("cannot discard a read-only cell in iamber");
    }
  }
  found->cells = std::move(cells);
  amber::notebook::validate_project_document(document);
  return document;
}

bool project_session_modified(const Session &session,
                              const amber::notebook::LoadedProject &project) {
  try {
    return amber::notebook::serialize_project_document(
               project_document_from_session(session, project)) !=
           amber::notebook::serialize_project_document(project.document);
  } catch (const std::runtime_error &) {
    // In-progress edits can be unrepresentable (for example a partial UTF-8
    // character). Save must reject them, but Quit must still offer explicit
    // discard instead of trapping the user behind the serialization error.
    return true;
  }
}

void save_project_session(Session *session,
                          amber::notebook::LoadedProject *project) {
  if (!session || !project)
    throw std::invalid_argument("null project session");
  const auto document = project_document_from_session(*session, *project);
  try {
    amber::notebook::save_project(project, document);
  } catch (...) {
    // A directory fsync can fail after the atomic rename. The persistence
    // layer advances its baseline before reporting that state, so keep the UI
    // dirtiness aligned with the actual committed document.
    session->document_dirty = project_session_modified(*session, *project);
    throw;
  }
  session->document_dirty = false;
  session->status = "project saved";
}

void load_project_module_into_session(
    Session *session, const amber::notebook::LoadedProject &project,
    const amber::notebook::LoadedProjectModule &module) {
  if (!session)
    throw std::invalid_argument("null project module session");
  if (session->evaluation_active)
    throw std::runtime_error("cannot replace a session during evaluation");
  const amber::notebook::ProjectDocument manifest =
      amber::notebook::parse_project_document(project.baseline);
  if (amber::notebook::serialize_project_document(project.document) !=
      amber::notebook::serialize_project_document(manifest)) {
    throw std::runtime_error(
        "project manifest has unsaved changes; save or reopen before opening "
        "a module");
  }
  const auto found = std::find_if(
      manifest.modules.begin(), manifest.modules.end(), [&](const auto &entry) {
        return entry.id == module.id && entry.path == module.path;
      });
  if (found == manifest.modules.end())
    throw std::runtime_error("module is not part of the loaded project");
  for (const amber::notebook::ProjectSheet &sheet : manifest.sheets) {
    for (const amber::notebook::ProjectCell &cell : sheet.cells) {
      amber::notebook::reserve_cell_id(cell.id);
    }
  }
  Session loaded;
  loaded.module_editor = true;
  loaded.project_module_id = module.id;
  loaded.project_module_path = module.path;
  loaded.project_label = project.document.title + "/module:" + module.id;
  loaded.auto_watch = false;
  Cell source;
  source.id = amber::notebook::allocate_cell_id();
  source.source = module.source;
  source.watch = false;
  source.dirty = false;
  loaded.cells.push_back(std::move(source));
  loaded.status = "module opened; not run or installed in sheets";
  *session = std::move(loaded);
}

bool project_module_session_modified(
    const Session &session,
    const amber::notebook::LoadedProjectModule &module) {
  return !session.module_editor || session.project_module_id != module.id ||
         session.project_module_path != module.path ||
         session.cells.size() != 1U ||
         session.cells.front().source != module.source;
}

void save_project_module_session(Session *session,
                                 const amber::notebook::LoadedProject &project,
                                 amber::notebook::LoadedProjectModule *module) {
  if (!session || !module)
    throw std::invalid_argument("null project module session");
  if (!session->module_editor || session->project_module_id != module->id ||
      session->project_module_path != module->path ||
      session->cells.size() != 1U) {
    throw std::runtime_error("session is not editing this project module");
  }
  try {
    amber::notebook::save_project_module(project, module,
                                         session->cells.front().source);
  } catch (...) {
    session->document_dirty =
        project_module_session_modified(*session, *module);
    throw;
  }
  session->document_dirty = false;
  session->status =
      "module source saved; Run validates it in an isolated runtime";
}
