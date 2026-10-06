#include "tools/iamber/tabs.h"
#include "tools/iamber/dependencies.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

void validate_new_tab_id(const std::string &id) {
  const auto start = [](char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_';
  };
  if (id.empty() || !start(id.front()) ||
      !std::all_of(id.begin(), id.end(), [&](char ch) {
        return start(ch) || (ch >= '0' && ch <= '9');
      })) {
    throw std::invalid_argument("new tab id must match [A-Za-z_][A-Za-z0-9_]*");
  }
}

bool same_bundled_sources(const std::vector<BundledModuleSource> &left,
                          const std::vector<BundledModuleSource> &right) {
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.size(); ++index) {
    const BundledModuleSource &a = left[index];
    const BundledModuleSource &b = right[index];
    if (a.id != b.id || a.path != b.path || a.source != b.source ||
        a.auto_import != b.auto_import || a.load_error != b.load_error) {
      return false;
    }
  }
  return true;
}

// The terminal marks edits while it changes buffers, but this cheap check
// keeps the tab strip correct for owner-thread clients that edit a Session
// directly. The exact helpers remain authoritative for save/apply decisions.
bool sheet_modified_for_display(
    const ProjectTab &tab, const amber::notebook::LoadedProject &project) {
  if (tab.session.document_dirty) {
    return true;
  }
  const auto found = std::find_if(
      project.document.sheets.begin(), project.document.sheets.end(),
      [&](const auto &sheet) { return sheet.id == tab.session.project_sheet_id; });
  if (found == project.document.sheets.end() ||
      found->cells.size() != tab.session.cells.size()) {
    return true;
  }
  for (std::size_t index = 0; index < found->cells.size(); ++index) {
    const auto &saved = found->cells[index];
    const Cell &current = tab.session.cells[index];
    const auto mode = current.watch ? amber::notebook::CellMode::Watch
                                    : amber::notebook::CellMode::Manual;
    if (saved.id != current.id || saved.kind != current.kind) {
      return true;
    }
    // Other cell kinds are read-only placeholders. Their opaque source/mode
    // metadata is not represented by the terminal Cell's execution fields.
    if (current.kind == "code" &&
        (saved.source != current.source || saved.mode != mode)) {
      return true;
    }
    if (current.kind == "text" &&
        (saved.source != current.source ||
         saved.formatting != current.formatting)) {
      return true;
    }
  }
  return false;
}

bool module_modified_for_display(const ProjectTab &tab) {
  if (tab.module.has_value() && tab.session.document_dirty) {
    return true;
  }
  return tab.module.has_value() &&
         (tab.session.cells.size() != 1U ||
          tab.session.cells.front().source != tab.module->source);
}

} // namespace

ProjectTabs::ProjectTabs(amber::notebook::LoadedProject project,
                         const std::string &initial_sheet,
                         const std::string &initial_module)
    : project_(std::move(project)),
      environment_sources_(load_project_bundled_sources(project_, true)) {
  if (!initial_sheet.empty() && !initial_module.empty()) {
    throw std::invalid_argument(
        "initial_sheet and initial_module are mutually exclusive");
  }
  amber::notebook::validate_project_document(project_.document);
  inputs_ = amber::notebook::project_input_defaults(project_.document);

  tabs_.reserve(project_.document.sheets.size() +
                project_.document.modules.size());
  for (const auto &sheet : project_.document.sheets) {
    auto tab = std::make_unique<ProjectTab>();
    load_project_into_session(&tab->session, project_, sheet.id);
    tab->session.project_inputs = inputs_;
    // Every sheet starts from the same immutable source snapshot. Loading a
    // sheet is intentionally source-only; it does not build or run a VM.
    tab->session.bundled_modules = environment_sources_;
    tab->session.set_activity_wakeup(activity_.notifier());
    tab->activity = tab->session.activity_waiter();
    tabs_.push_back(std::move(tab));
  }
  for (const auto &manifest_module : project_.document.modules) {
    auto tab = std::make_unique<ProjectTab>();
    amber::notebook::LoadedProjectModule module =
        amber::notebook::load_project_module(project_, manifest_module.id);
    load_project_module_into_session(&tab->session, project_, module);
    tab->module = std::move(module);
    tab->session.set_activity_wakeup(activity_.notifier());
    tab->activity = tab->session.activity_waiter();
    tabs_.push_back(std::move(tab));
  }
  if (tabs_.empty()) {
    throw std::runtime_error("project has no tabs");
  }

  if (!initial_sheet.empty() || !initial_module.empty()) {
    const std::string &wanted = initial_sheet.empty() ? initial_module
                                                       : initial_sheet;
    const bool want_module = !initial_module.empty();
    const auto found = std::find_if(
        tabs_.begin(), tabs_.end(), [&](const auto &tab) {
          if (tab->module.has_value() != want_module) {
            return false;
          }
          return want_module ? tab->module->id == wanted
                             : tab->session.project_sheet_id == wanted;
        });
    if (found == tabs_.end()) {
      throw std::invalid_argument("unknown initial project tab: " + wanted);
    }
    selected_ = static_cast<std::size_t>(found - tabs_.begin());
  } else {
    const auto found = std::find_if(
        tabs_.begin(), tabs_.end(), [&](const auto &tab) {
          return !tab->module.has_value() &&
                 tab->session.project_sheet_id == project_.document.active_sheet;
        });
    if (found == tabs_.end()) {
      throw std::runtime_error("project active sheet is not a tab");
    }
    selected_ = static_cast<std::size_t>(found - tabs_.begin());
  }
}

ProjectTabs::~ProjectTabs() {
  // Close the stable host mailbox first, then every session-local channel,
  // while all backends are still owned by their Sessions. The waiter copies
  // in ProjectTab are destroyed only after this body returns.
  activity_.close();
  for (const auto &tab : tabs_) {
    if (tab != nullptr) {
      tab->session.close_activity();
    }
  }
}

ProjectTab &ProjectTabs::active() noexcept { return *tabs_[selected_]; }

const ProjectTab &ProjectTabs::active() const noexcept {
  return *tabs_[selected_];
}

const std::vector<std::unique_ptr<ProjectTab>> &ProjectTabs::tabs() const
    noexcept {
  return tabs_;
}

std::size_t ProjectTabs::selected() const noexcept { return selected_; }

void ProjectTabs::require_idle() const {
  for (const auto &tab : tabs_) {
    if (tab != nullptr && tab->session.evaluation_active) {
      throw std::runtime_error("project tabs are busy evaluating");
    }
  }
}

bool ProjectTabs::switch_by(int delta) {
  require_idle();
  if (tabs_.empty()) {
    return false;
  }
  const std::int64_t count = static_cast<std::int64_t>(tabs_.size());
  const std::int64_t wrapped = static_cast<std::int64_t>(delta) % count;
  std::int64_t next = static_cast<std::int64_t>(selected_) + wrapped;
  next %= count;
  if (next < 0) {
    next += count;
  }
  const std::size_t next_index = static_cast<std::size_t>(next);
  if (next_index == selected_) {
    return false;
  }
  selected_ = next_index;
  return true;
}

bool ProjectTabs::modified(std::size_t index) const {
  if (index >= tabs_.size()) {
    throw std::out_of_range("project tab index out of range");
  }
  const ProjectTab &tab = *tabs_[index];
  if (tab.module.has_value()) {
    return project_module_session_modified(tab.session, *tab.module);
  }
  return project_session_modified(tab.session, project_);
}

std::size_t ProjectTabs::modified_count() const {
  std::size_t count = 0;
  for (std::size_t index = 0; index < tabs_.size(); ++index) {
    if (modified(index)) {
      ++count;
    }
  }
  return count;
}

void ProjectTabs::save_active() {
  require_idle();
  ProjectTab &tab = active();
  if (tab.module.has_value()) {
    save_project_module_session(&tab.session, project_, &*tab.module);
  } else {
    save_project_session(&tab.session, &project_);
  }
}

void ProjectTabs::attach_new_tab(ProjectTab *tab) {
  tab->session.project_inputs = inputs_;
  // Carry host policy, not another tab's runtime or editor state. All these
  // potentially allocating operations happen before disk publication.
  tab->session.runtime_capability_grants =
      active().session.runtime_capability_grants;
  tab->session.runtime_watch_event_capacity =
      active().session.runtime_watch_event_capacity;
  tab->session.set_activity_wakeup(activity_.notifier());
  tab->activity = tab->session.activity_waiter();
  tab->environment_generation = environment_generation_;
}

bool ProjectTabs::set_input(const std::string &id, const std::string &json) {
  require_idle();
  const auto &definitions = project_.document.inputs;
  const auto found = std::find_if(definitions.begin(), definitions.end(), [&](const auto &input) { return input.id == id; });
  if (found == definitions.end()) throw std::runtime_error("unknown project input: " + id);
  auto value = amber::notebook::parse_project_input_value(json);
  amber::notebook::validate_project_input_value(*found, value);
  if (found->type == "number" && std::holds_alternative<std::int64_t>(value))
    value = static_cast<double>(std::get<std::int64_t>(value));
  if (inputs_->at(id) == value) return false;
  auto next = std::make_shared<amber::runtime::NotebookInputSnapshot>(*inputs_);
  next->at(id) = std::move(value);
  inputs_ = next;
  // Publish to every sheet before executing any: callbacks never see a
  // partially distributed snapshot. Each sheet owns its own runtime Values.
  for (const auto &tab : tabs_) tab->session.project_inputs = inputs_;
  for (const auto &tab : tabs_) apply_project_input_changes(&tab->session, {id});
  return true;
}

void ProjectTabs::create_input(const amber::notebook::ProjectInput &input) {
  require_idle();
  auto next = project_.document;
  next.version = std::max(next.version, 2U);
  next.inputs.push_back(input);
  amber::notebook::validate_project_document(next);
  auto values = std::make_shared<amber::runtime::NotebookInputSnapshot>(*inputs_);
  auto value = input.initial;
  if (input.type == "number" && std::holds_alternative<std::int64_t>(value))
    value = static_cast<double>(std::get<std::int64_t>(value));
  values->emplace(input.id, std::move(value));
  amber::notebook::save_project(&project_, next);
  inputs_ = values;
  for (const auto &tab : tabs_) tab->session.project_inputs = inputs_;
  // Defining project structure never executes user code.
}

void ProjectTabs::save_board(const amber::notebook::ProjectBoard &board) {
  require_idle();
  auto next = project_.document;
  next.version = std::max(next.version, 2U);
  const auto found = std::find_if(next.boards.begin(), next.boards.end(), [&](const auto &item) { return item.id == board.id; });
  if (found == next.boards.end()) next.boards.push_back(board);
  else {
    auto replacement = board;
    // A host editor may only understand today's component properties. Keep
    // opaque metadata for surviving identities, including exact JSON numbers.
    replacement.extra.insert(found->extra.begin(), found->extra.end());
    for (auto &component : replacement.components) {
      const auto old = std::find_if(found->components.begin(), found->components.end(),
          [&](const auto &item) { return item.id == component.id; });
      if (old != found->components.end()) component.extra.insert(old->extra.begin(), old->extra.end());
    }
    *found = std::move(replacement);
  }
  amber::notebook::save_project(&project_, next);
}

void ProjectTabs::add_dependency(const std::string &directory) {
  require_idle();
  const auto root = std::filesystem::canonical(project_.directory / directory);
  amber::notebook::ProjectDependency dependency;
  dependency.path = root.lexically_relative(project_.directory).generic_string();
  if (dependency.path.empty()) dependency.path = root.string();
  (void)load_directory_dependency(project_.directory, dependency);
  auto next = project_.document;
  next.version = 3;
  next.dependencies.push_back(dependency);
  amber::notebook::save_project(&project_, next);
  for (const auto &tab : tabs_) if (!tab->module) tab->session.environment_stale = true;
  active().session.status = "dependency linked; structure saved; Apply to load it (no code copied or executed)";
}

void ProjectTabs::remove_dependency(const std::string &path) {
  require_idle();
  auto next = project_.document;
  const auto found = std::find_if(next.dependencies.begin(), next.dependencies.end(), [&](const auto &d) { return d.path == path; });
  if (found == next.dependencies.end()) throw std::invalid_argument("unknown dependency path: " + path);
  next.dependencies.erase(found);
  amber::notebook::save_project(&project_, next);
  for (const auto &tab : tabs_) if (!tab->module) tab->session.environment_stale = true;
  active().session.status = "dependency unlinked; package files untouched; Apply to load the new environment";
}

void ProjectTabs::create_sheet(const std::string &id, const std::string &title) {
  require_idle();
  validate_new_tab_id(id);
  auto next = project_.document;
  amber::notebook::ProjectSheet sheet;
  sheet.id = id;
  sheet.title = title.empty() ? id : title;
  amber::notebook::ProjectCell saved_cell;
  saved_cell.id = amber::notebook::allocate_cell_id();
  sheet.cells.push_back(saved_cell);
  next.sheets.push_back(sheet);
  amber::notebook::validate_project_document(next);

  auto tab = std::make_unique<ProjectTab>();
  tab->session.project_sheet_id = id;
  tab->session.project_label = next.title + "/" + sheet.title;
  // New sheets start at the project's last applied source snapshot. Structure
  // edits (including auto-import settings) never initialize a candidate world.
  tab->session.bundled_modules = environment_sources_;
  Cell cell;
  cell.id = saved_cell.id;
  tab->session.cells.push_back(std::move(cell));
  tab->session.status = "sheet created; structure saved; not evaluated";
  attach_new_tab(tab.get());
  tabs_.reserve(tabs_.size() + 1U);
  const std::size_t index = project_.document.sheets.size();
  const auto publish = [&] {
    tabs_.insert(tabs_.begin() + index, std::move(tab));
    selected_ = index;
  };
  try {
    // Merge with the saved document only, not other tabs' unsaved contents.
    amber::notebook::save_project(&project_, next);
  } catch (...) {
    // A directory-sync error can follow a successful manifest rename. The
    // persistence layer advances its baseline at that boundary; reflect it in
    // the tab host too, then propagate the durability warning to the user.
    if (project_.document.sheets.size() == index + 1U &&
        project_.document.sheets.back().id == id)
      publish();
    throw;
  }
  publish();
}

void ProjectTabs::create_module(const std::string &id) {
  require_idle();
  validate_new_tab_id(id);
  const amber::notebook::ProjectModule entry{id, "modules/" + id + ".am", {}};
  const std::string source = "package " + id + "\n";
  auto preview = project_;
  preview.document.modules.push_back(entry);
  preview.baseline = amber::notebook::serialize_project_document(preview.document);
  auto tab = std::make_unique<ProjectTab>();
  tab->module = amber::notebook::LoadedProjectModule{id, entry.path, source, source};
  // This adapter only checks the staged manifest and builds an editor buffer;
  // it neither follows the new path nor executes its source.
  load_project_module_into_session(&tab->session, preview, *tab->module);
  tab->session.cells.front().cursor = source.size();
  tab->session.status = "module created; structure saved; not run";
  attach_new_tab(tab.get());
  tabs_.reserve(tabs_.size() + 1U);
  const std::size_t old_count = project_.document.modules.size();
  const auto publish = [&] {
    tabs_.push_back(std::move(tab));
    selected_ = tabs_.size() - 1U;
  };
  try {
    (void)amber::notebook::create_project_module(&project_, entry, source);
  } catch (...) {
    if (project_.document.modules.size() == old_count + 1U &&
        project_.document.modules.back().id == id)
      publish();
    throw;
  }
  publish();
}

bool ProjectTabs::set_auto_import(const std::string &module_id, bool enabled) {
  require_idle();
  const auto &modules = project_.document.modules;
  if (std::none_of(modules.begin(), modules.end(), [&](const auto &module) {
        return module.id == module_id;
      }))
    throw std::invalid_argument("unknown module: " + module_id);

  auto next = project_.document;
  const auto found = std::find(next.auto_imports.begin(), next.auto_imports.end(),
                              module_id);
  const bool present = found != next.auto_imports.end();
  if (enabled && !present)
    next.auto_imports.push_back(module_id);
  else if (!enabled && present)
    next.auto_imports.erase(found);
  std::string status = present == enabled
      ? "auto-import unchanged; F6 Apply to use saved settings"
      : std::string("auto-import ") + (enabled ? "enabled" : "disabled") +
            "; structure saved; F6 Apply to use changes";
  // Even a no-op validates the external manifest baseline. Applied source
  // snapshots, generation and staleness change only after successful Apply.
  amber::notebook::save_project(&project_, next);
  active().session.status.swap(status);
  return present != enabled;
}

bool ProjectTabs::apply_active(bool force_all, bool show_running,
                               const EvaluationProgress &progress) {
  require_idle();
  ProjectTab &tab = active();
  if (tab.module.has_value()) {
    tab.session.status = "switch to a sheet to Apply bundled modules";
    return false;
  }
  for (const auto &candidate : tabs_) {
    if (candidate->module.has_value() &&
        project_module_session_modified(candidate->session,
                                        *candidate->module)) {
      tab.session.status = "save modified module tabs before Apply";
      return false;
    }
  }

  std::vector<BundledModuleSource> staged;
  try {
    staged = load_project_bundled_sources(project_);
  } catch (const std::exception &error) {
    tab.session.environment_error = error.what();
    tab.session.status = std::string("environment Apply failed: ") +
                         tab.session.environment_error;
    return false;
  } catch (...) {
    tab.session.environment_error = "could not load bundled module sources";
    tab.session.status = "environment Apply failed: " +
                         tab.session.environment_error;
    return false;
  }
  const bool changed = !same_bundled_sources(environment_sources_, staged);
  if (changed && environment_generation_ ==
                     std::numeric_limits<std::uint64_t>::max()) {
    tab.session.status = "environment Apply failed: environment generation exhausted";
    return false;
  }

  if (!apply_bundled_environment(&tab.session, staged, force_all, show_running,
                                 progress)) {
    return false;
  }

  if (changed) {
    ++environment_generation_;
    environment_sources_.swap(staged);
    for (const auto &candidate : tabs_) {
      if (!candidate->module.has_value() && candidate.get() != &tab) {
        candidate->session.environment_stale = true;
      }
    }
  }
  tab.environment_generation = environment_generation_;
  // apply_bundled_environment clears this on its successful publication; keep
  // the assignment explicit because it is the host's stale-generation commit.
  tab.session.environment_stale = false;
  return true;
}

std::string ProjectTabs::tab_bar() const {
  std::ostringstream out;
  out << "tabs " << (selected_ + 1U) << "/" << tabs_.size() << " ";
  const auto append = [&](std::size_t index, bool selected) {
    const ProjectTab &tab = *tabs_[index];
    std::string label = tab.module.has_value()
                            ? "module:" + tab.module->id
                            : "sheet:" + tab.session.project_sheet_id;
    const bool dirty = tab.module.has_value()
                           ? module_modified_for_display(tab)
                           : sheet_modified_for_display(tab, project_);
    if (dirty) {
      label += '*';
    }
    if (!tab.module.has_value() && tab.session.environment_stale) {
      label += "!env";
    }
    if (selected) {
      out << '[' << label << ']';
    } else {
      out << label;
    }
  };
  append(selected_, true);
  for (std::size_t index = 0; index < tabs_.size(); ++index) {
    if (index == selected_) {
      continue;
    }
    out << ' ';
    append(index, false);
  }
  out << " | env:" << environment_generation_;
  if (active().module) {
    const auto &imports = project_.document.auto_imports;
    out << " auto-import:"
        << (std::find(imports.begin(), imports.end(), active().module->id) !=
                    imports.end() ? "on" : "off");
  }
  return out.str();
}

const amber::notebook::LoadedProject &ProjectTabs::project() const noexcept {
  return project_;
}

std::uint64_t ProjectTabs::environment_generation() const noexcept {
  return environment_generation_;
}

SessionActivityWaiter ProjectTabs::activity_waiter() const noexcept {
  return activity_.waiter();
}

void ProjectTabs::close_activity() noexcept {
  activity_.close();
  for (const auto &tab : tabs_) {
    if (tab != nullptr) {
      tab->session.close_activity();
    }
  }
}
