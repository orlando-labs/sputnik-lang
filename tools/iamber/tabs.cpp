#include "tools/iamber/tabs.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

bool same_bundled_sources(const std::vector<BundledModuleSource> &left,
                          const std::vector<BundledModuleSource> &right) {
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.size(); ++index) {
    const BundledModuleSource &a = left[index];
    const BundledModuleSource &b = right[index];
    if (a.id != b.id || a.path != b.path || a.source != b.source ||
        a.auto_import != b.auto_import) {
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
      environment_sources_(load_project_bundled_sources(project_)) {
  if (!initial_sheet.empty() && !initial_module.empty()) {
    throw std::invalid_argument(
        "initial_sheet and initial_module are mutually exclusive");
  }
  amber::notebook::validate_project_document(project_.document);

  tabs_.reserve(project_.document.sheets.size() +
                project_.document.modules.size());
  for (const auto &sheet : project_.document.sheets) {
    auto tab = std::make_unique<ProjectTab>();
    load_project_into_session(&tab->session, project_, sheet.id);
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
