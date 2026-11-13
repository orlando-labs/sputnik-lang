#include "runtime/audio_system.h"
#include "notebook/audio_components.h"
#include "tools/isputnik/tabs.h"
#include "tools/isputnik/dependencies.h"
#include "frontend/lexer/lexer.h"

#include <algorithm>
#include <charconv>
#include <functional>
#include <exception>
#include <iterator>
#include <cstdint>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

constexpr const char *CellIDHighWater = "cell_id_high_water";

sputnik::notebook::CellId cell_id_high_water(const sputnik::notebook::ProjectDocument &document) {
  const auto saved = document.extra.find(CellIDHighWater);
  if (saved == document.extra.end()) return 0;
  const auto &text = saved->second;
  sputnik::notebook::CellId id = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), id);
  return result.ec == std::errc{} && result.ptr == text.data() + text.size() &&
         id != std::numeric_limits<sputnik::notebook::CellId>::max() ? id : 0;
}

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
    const ProjectTab &tab, const sputnik::notebook::LoadedProject &project) {
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
    const auto mode = current.watch ? sputnik::notebook::CellMode::Watch
                                    : sputnik::notebook::CellMode::Manual;
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

ProjectTabs::ProjectTabs(sputnik::notebook::LoadedProject project,
                         const std::string &initial_sheet,
                         const std::string &initial_module)
    : project_(std::move(project)),
      environment_sources_(load_project_bundled_sources(project_, true)) {
  if (!initial_sheet.empty() && !initial_module.empty()) {
    throw std::invalid_argument(
        "initial_sheet and initial_module are mutually exclusive");
  }
  sputnik::notebook::validate_project_document(project_.document);
  if (const auto id = cell_id_high_water(project_.document)) sputnik::notebook::reserve_cell_id(id);
  inputs_ = sputnik::notebook::project_input_defaults(project_.document);

  tabs_.reserve(project_.document.sheets.size() +
                project_.document.modules.size());
  for (const auto &sheet : project_.document.sheets) {
    auto tab = std::make_unique<ProjectTab>();
    load_project_into_session(&tab->session, project_, sheet.id);
    assign_inputs(*tab);
    // Every sheet starts from the same immutable source snapshot. Loading a
    // sheet is intentionally source-only; it does not build or run a VM.
    tab->session.bundled_modules = environment_sources_;
    tab->session.set_activity_wakeup(activity_.notifier());
    tab->activity = tab->session.activity_waiter();
    tabs_.push_back(std::move(tab));
  }
  for (const auto &manifest_module : project_.document.modules) {
    auto tab = std::make_unique<ProjectTab>();
    sputnik::notebook::LoadedProjectModule module =
        sputnik::notebook::load_project_module(project_, manifest_module.id);
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
  assign_inputs(*tab);
  // Carry host policy, not another tab's runtime or editor state. All these
  // potentially allocating operations happen before disk publication.
  tab->session.runtime_capability_grants =
      active().session.runtime_capability_grants;
  tab->session.runtime_watch_event_capacity =
      active().session.runtime_watch_event_capacity;
  tab->session.console_definitions = active().session.console_definitions;
  tab->session.set_activity_wakeup(activity_.notifier());
  tab->activity = tab->session.activity_waiter();
  tab->environment_generation = environment_generation_;
}

bool ProjectTabs::set_input(const std::string &id, const std::string &json) {
  return set_inputs({{id, json}});
}

bool ProjectTabs::set_inputs(const std::map<std::string, std::string> &values, bool evaluate) {
  require_idle();
  const auto &definitions = project_.document.inputs;
  auto next = std::make_shared<sputnik::runtime::NotebookInputSnapshot>(*inputs_);
  std::set<std::string> changed;
  for (const auto &[id, json] : values) {
    const auto found = std::find_if(definitions.begin(), definitions.end(), [&](const auto &input) { return input.id == id; });
    if (found == definitions.end()) throw std::runtime_error("unknown project input: " + id);
    auto value = sputnik::notebook::parse_project_input_value(json);
    sputnik::notebook::validate_project_input_value(*found, value);
    if (found->type == "number" && std::holds_alternative<std::int64_t>(value))
      value = static_cast<double>(std::get<std::int64_t>(value));
    if (next->at(id) != value) { next->at(id) = std::move(value); changed.insert(id); }
  }
  sputnik::notebook::validate_project_control_values(project_.document, *next);
  if (changed.empty()) return false;
  inputs_ = next;
  // Publish to every sheet before executing any: callbacks never see a
  // partially distributed snapshot. Each sheet owns its own runtime Values.
  for (const auto &tab : tabs_) assign_inputs(*tab);
  if (evaluate) for (const auto &tab : tabs_) apply_project_input_changes(&tab->session, changed);
  return true;
}

bool ProjectTabs::set_cell_inputs(sputnik::notebook::CellId owner, const std::map<std::string, std::string> &values, bool evaluate) {
  require_idle();
  const sputnik::notebook::ProjectCell *target = nullptr;
  for (const auto &sheet : project_.document.sheets) for (const auto &cell : sheet.cells)
    if (cell.id == owner) target = &cell;
  if (!target || target->kind != "code" || !target->extra.count("cell_ui"))
    throw std::runtime_error("cell inputs unavailable");
  const auto ui = sputnik::notebook::parse_project_cell_ui(target->extra.at("cell_ui"));
  auto next = std::make_shared<sputnik::runtime::NotebookInputSnapshot>(*inputs_);
  std::set<std::string> changed;
  for (const auto &edit : values) {
    const auto &id = edit.first;
    const auto &json = edit.second;
    const auto definition = std::find_if(ui.inputs.begin(), ui.inputs.end(), [&](const auto &input) { return input.id == id; });
    if (definition == ui.inputs.end()) throw std::runtime_error("unknown cell input: " + id);
    auto value = sputnik::notebook::parse_project_input_value(json);
    sputnik::notebook::validate_project_input_value(*definition, value);
    if (definition->type == "number" && std::holds_alternative<std::int64_t>(value)) value = static_cast<double>(std::get<std::int64_t>(value));
    const auto key = sputnik::runtime::notebook_cell_input_key(owner, id);
    if (next->at(key) != value) { next->at(key) = std::move(value); changed.insert(key); }
  }
  sputnik::notebook::validate_project_control_values(project_.document, *next);
  if (changed.empty()) return false;
  inputs_ = next;
  for (const auto &tab : tabs_) assign_inputs(*tab);
  if (evaluate) for (const auto &tab : tabs_) apply_project_input_changes(&tab->session, changed);
  return true;
}

void ProjectTabs::set_cell_id(sputnik::notebook::CellId owner, sputnik::notebook::CellId replacement) {
  require_idle();
  if (!replacement || replacement == std::numeric_limits<sputnik::notebook::CellId>::max())
    throw std::invalid_argument("Hard ID must be an integer from 1 to 18446744073709551614.");
  ProjectTab *target_tab = nullptr;
  Cell *target_cell = nullptr;
  for (const auto &tab : tabs_) for (auto &cell : tab->session.cells) {
    if (cell.id == owner) { target_tab = tab.get(); target_cell = &cell; }
    else if (cell.id == replacement) throw std::invalid_argument("Hard ID is already used by another cell.");
  }
  if (!target_cell || target_tab->module || (target_cell->kind != "code" && target_cell->kind != "text"))
    throw std::invalid_argument("Hard ID editing requires a code or text cell in a sheet.");
  if (owner == replacement) return;
  auto next = project_.document;
  for (const auto &sheet : next.sheets) for (const auto &cell : sheet.cells)
    if (cell.id == replacement) throw std::invalid_argument("Hard ID is already used by another saved cell.");
  auto sheet = std::find_if(next.sheets.begin(), next.sheets.end(), [&](const auto &item) {
    return item.id == target_tab->session.project_sheet_id;
  });
  if (sheet == next.sheets.end()) throw std::runtime_error("Cell sheet is unavailable.");
  auto saved = std::find_if(sheet->cells.begin(), sheet->cells.end(), [&](const auto &cell) { return cell.id == owner; });
  if (saved == sheet->cells.end()) {
    sputnik::notebook::ProjectCell cell;
    cell.id = owner; cell.kind = target_cell->kind; cell.source = target_cell->source;
    cell.formatting = target_cell->formatting;
    cell.mode = target_cell->watch ? sputnik::notebook::CellMode::Watch : sputnik::notebook::CellMode::Manual;
    sheet->cells.push_back(std::move(cell)); saved = std::prev(sheet->cells.end());
  }
  saved->id = replacement;
  for (auto &consumer : next.sheets)
    for (auto &link : consumer.imports)
      if (link.sheet == sheet->id && link.cell == owner) link.cell = replacement;
  std::function<void(std::vector<sputnik::notebook::ProjectComponent> &)> rebind = [&](auto &items) {
    for (auto &component : items) {
      if (component.cell == owner) component.cell = replacement;
      if (component.panels) for (auto &panel : *component.panels) rebind(panel.components);
    }
  };
  for (auto &board : next.boards) if (board.sheet == sheet->id) rebind(board.components);
  auto values = std::make_shared<sputnik::runtime::NotebookInputSnapshot>(*inputs_);
  if (auto entry = saved->extra.find("cell_ui"); entry != saved->extra.end()) {
    auto ui = sputnik::notebook::parse_project_cell_ui(entry->second);
    rebind(ui.pre); rebind(ui.post);
    for (const auto &input : ui.inputs) {
      const auto old_key = sputnik::runtime::notebook_cell_input_key(owner, input.id);
      const auto old = values->find(old_key);
      if (old != values->end()) {
        values->emplace(sputnik::runtime::notebook_cell_input_key(replacement, input.id), old->second);
        values->erase(old);
      }
    }
    entry->second = sputnik::notebook::serialize_project_cell_ui(ui);
  }
  sputnik::notebook::validate_project_document(next);
  sputnik::notebook::reserve_cell_id(replacement);
  std::exception_ptr failure;
  try { sputnik::notebook::save_project(&project_, next);
  for (const auto &tab : tabs_) if (!tab->module) tab->session.project_audio = sputnik::notebook::project_audio_components(project_.document, tab->session.project_sheet_id); }
  catch (...) {
    // A directory fsync may fail after the manifest was already committed.
    // Publish the matching in-memory identity in that case as well.
    const auto &committed = project_.document.sheets;
    const bool published = std::any_of(committed.begin(), committed.end(), [&](const auto &item) {
      return item.id == sheet->id && std::any_of(item.cells.begin(), item.cells.end(), [&](const auto &cell) { return cell.id == replacement; });
    });
    if (!published) throw;
    failure = std::current_exception();
  }
  target_cell->id = replacement;
  inputs_ = std::move(values);
  for (const auto &tab : tabs_) assign_inputs(*tab);
  if (target_cell->kind == "code") for (auto &cell : target_tab->session.cells)
    if (cell.kind == "code") cell.dirty = true;
  target_tab->session.status = "cell Hard ID updated";
  if (failure) std::rethrow_exception(failure);
}

void ProjectTabs::save_cell_ui(sputnik::notebook::CellId owner, const std::string &json) {
  require_idle();
  auto ui = sputnik::notebook::parse_project_cell_ui(json);
  sputnik::notebook::validate_project_cell_ui(ui, owner);
  auto next = project_.document;
  sputnik::notebook::ProjectCell *target = nullptr;
  for (auto &sheet : next.sheets) for (auto &cell : sheet.cells) if (cell.id == owner) target = &cell;
  if (!target) {
    for (const auto &tab : tabs_) {
      const auto source = std::find_if(tab->session.cells.begin(), tab->session.cells.end(), [&](const auto &cell) { return cell.id == owner; });
      if (source == tab->session.cells.end() || source->kind != "code" || tab->module) continue;
      auto sheet = std::find_if(next.sheets.begin(), next.sheets.end(), [&](const auto &item) { return item.id == tab->session.project_sheet_id; });
      if (sheet != next.sheets.end()) {
        sputnik::notebook::ProjectCell cell; cell.id = owner; cell.source = source->source;
        cell.mode = source->watch ? sputnik::notebook::CellMode::Watch : sputnik::notebook::CellMode::Manual;
        sheet->cells.push_back(std::move(cell)); target = &sheet->cells.back();
      }
      break;
    }
  }
  if (!target || target->kind != "code") throw std::runtime_error("cell UI requires a code cell");
  next.version = std::max(next.version, 4U);
  // Preserve opaque root metadata when editing the supported properties.
  if (target->extra.count("cell_ui")) {
    const auto previous = sputnik::notebook::parse_project_cell_ui(target->extra.at("cell_ui"));
    ui.extra.insert(previous.extra.begin(), previous.extra.end());
    for (auto &input : ui.inputs) for (const auto &old : previous.inputs)
      if (input.id == old.id) input.extra.insert(old.extra.begin(), old.extra.end());
    const std::set<std::string> editable = {"audio_id", "show_waveform", "chart_id", "table_id", "image_id", "image_mode", "caption", "image_allow_save",
      "image_allow_load", "image_allow_read", "x_label", "y_label", "stroke", "x_min", "x_max", "y_min", "y_max", "control", "fields", "p1"};
    for (auto *zone : {&ui.pre, &ui.post}) for (auto &component : *zone)
      for (const auto *old_zone : {&previous.pre, &previous.post}) for (const auto &old : *old_zone)
        if (component.id == old.id && component.kind == old.kind)
          for (const auto &[name, value] : old.extra) if (!editable.count(name)) component.extra.emplace(name, value);
  }
  target->extra["cell_ui"] = sputnik::notebook::serialize_project_cell_ui(ui);
  auto values = std::make_shared<sputnik::runtime::NotebookInputSnapshot>(*sputnik::notebook::project_input_defaults(next));
  std::set<std::string> changed;
  for (auto &[key, value] : *values) {
    const auto old = inputs_->find(key);
    if (old != inputs_->end()) {
      try {
        auto accepted = old->second;
        if (!key.empty() && key[0] == '\0') {
          // A changed type/bound may invalidate a previously accepted value.
          for (const auto &input : ui.inputs) if (key == sputnik::runtime::notebook_cell_input_key(owner, input.id)) {
            sputnik::notebook::validate_project_input_value(input, accepted);
            if (input.type == "number" && std::holds_alternative<std::int64_t>(accepted))
              accepted = static_cast<double>(std::get<std::int64_t>(accepted));
          }
        }
        value = std::move(accepted);
        if (value != old->second) changed.insert(key);
      } catch (const std::exception &) { changed.insert(key); }
    } else changed.insert(key);
  }
  for (const auto &[key, value] : *inputs_) if (!values->count(key)) changed.insert(key);
  sputnik::notebook::validate_project_control_values(next, *values);
  sputnik::notebook::save_project(&project_, next);
  for (const auto &tab : tabs_) if (!tab->module) tab->session.project_audio = sputnik::notebook::project_audio_components(project_.document, tab->session.project_sheet_id);
  inputs_ = values;
  for (const auto &tab : tabs_) {
    assign_inputs(*tab);
    // Structure editing stays inert; defer Watch until an explicit run or input edit.
    invalidate_project_input_changes(&tab->session, changed);
  }
}

void ProjectTabs::create_input(const sputnik::notebook::ProjectInput &input) {
  require_idle();
  auto next = project_.document;
  next.version = std::max(next.version, 2U);
  next.inputs.push_back(input);
  sputnik::notebook::validate_project_document(next);
  auto values = std::make_shared<sputnik::runtime::NotebookInputSnapshot>(*inputs_);
  auto value = input.initial;
  if (input.type == "number" && std::holds_alternative<std::int64_t>(value))
    value = static_cast<double>(std::get<std::int64_t>(value));
  values->emplace(input.id, std::move(value));
  sputnik::notebook::save_project(&project_, next);
  for (const auto &tab : tabs_) if (!tab->module) tab->session.project_audio = sputnik::notebook::project_audio_components(project_.document, tab->session.project_sheet_id);
  inputs_ = values;
  for (const auto &tab : tabs_) assign_inputs(*tab);
  // Defining project structure never executes user code.
}

void ProjectTabs::delete_input(const std::string &id) {
  require_idle();
  auto next = project_.document;
  const auto found = std::find_if(next.inputs.begin(), next.inputs.end(), [&](const auto &input) { return input.id == id; });
  if (found == next.inputs.end()) throw std::runtime_error("unknown project input: " + id);
  next.inputs.erase(found);
  // Validate every binding, including nested panels, forms, actions and presets,
  // before changing the manifest or publishing a new input snapshot.
  try {
    sputnik::notebook::validate_project_document(next);
  } catch (const std::exception &error) {
    throw std::runtime_error("Cannot delete project input '" + id + "'. Remove its board bindings in Design first. " + error.what());
  }
  auto values = std::make_shared<sputnik::runtime::NotebookInputSnapshot>(*inputs_);
  values->erase(id);
  sputnik::notebook::save_project(&project_, next);
  inputs_ = values;
  for (const auto &tab : tabs_) {
    assign_inputs(*tab);
    // Removing a definition is inert. Reads become stale until the next Run.
    invalidate_project_input_changes(&tab->session, {id});
  }
}

void ProjectTabs::set_resource_policy(const std::string &json) {
  require_idle();
  auto next = project_.document;
  next.extra["resource_policy"] = json;
  next = sputnik::notebook::parse_project_document(sputnik::notebook::serialize_project_document(next));
  sputnik::notebook::validate_project_document(next);
  sputnik::notebook::save_project(&project_, next);
}

void ProjectTabs::set_retain_cell_results(bool enabled) {
  require_idle();
  auto next = project_.document;
  next.extra["retain_cell_results"] = enabled ? "true" : "false";
  // Save the manifest option independently from unsaved source buffers.
  sputnik::notebook::save_project(&project_, next);
  for (const auto &tab : tabs_) if (!tab->module) tab->session.project_audio = sputnik::notebook::project_audio_components(project_.document, tab->session.project_sheet_id);
}

void ProjectTabs::save_board(const sputnik::notebook::ProjectBoard &board) {
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
    const auto merge_rows = [](auto &rows, const auto &previous) {
      for (auto &row : rows) {
        const auto old = std::find_if(previous.begin(), previous.end(),
            [&](const auto &item) { return item.id == row.id; });
        if (old != previous.end()) row.extra.insert(old->extra.begin(), old->extra.end());
      }
    };
    std::function<void(std::vector<sputnik::notebook::ProjectComponent> &,
                       const std::vector<sputnik::notebook::ProjectComponent> &, unsigned)> merge_components;
    merge_components = [&](auto &items, const auto &previous, unsigned depth) {
      if (depth > 8) throw std::runtime_error("tabs nesting exceeds 8 levels");
      for (auto &component : items) {
        const auto old = std::find_if(previous.begin(), previous.end(),
            [&](const auto &item) { return item.id == component.id; });
        if (old == previous.end()) continue;
        if (component.panels && old->panels) for (auto &panel : *component.panels) {
          const auto old_panel = std::find_if(old->panels->begin(), old->panels->end(),
              [&](const auto &item) { return item.id == panel.id; });
          if (old_panel == old->panels->end()) continue;
          panel.extra.insert(old_panel->extra.begin(), old_panel->extra.end());
          merge_rows(panel.rows, old_panel->rows);
          merge_components(panel.components, old_panel->components, depth + 1);
        }
        for (const auto &entry : old->extra) {
          if ((component.kind == "audio_annotator" || component.kind == "image" || component.kind == "image_carousel") &&
              (entry.first == "annotation_classes" || entry.first == "audio_transcript_allowed" || entry.first == "audio_transcript_required" ||
               entry.first == "audio_segment_transcript_allowed" || entry.first == "audio_segment_transcript_required")) continue;
          if (entry.first == "p1" || (component.kind == "action" && (entry.first == "module_id" || entry.first == "function"))) continue;
          if (component.kind == "input" && entry.first == "control") continue;
          if (component.kind == "parameter_form" && entry.first == "fields") continue;
          if (component.kind == "rich_text" && (entry.first == "text_content" || entry.first == "text_mode")) continue;
          if (component.kind == "progress" && entry.first == "progress_id") continue;
          if (component.kind == "table" && entry.first == "table_id") continue;
          if (component.kind == "audio" && (entry.first == "audio_id" || entry.first == "show_waveform")) continue;
          if (component.kind == "periodic_work" &&
              (entry.first == "module_id" || entry.first == "function" || entry.first == "period_seconds" ||
               entry.first == "timing" || entry.first == "retention" || entry.first == "tail_limit" ||
               entry.first == "store_values" || entry.first == "store_errors" || entry.first == "run_immediately" ||
               entry.first == "stop_on_error" || entry.first == "pause_when_hidden")) continue;
          // These chart properties are understood by the board editor. Omitting
          // an optional bound means returning that axis to its automatic range.
          if (component.kind == "live_chart" &&
              (entry.first == "chart_id" || entry.first == "x_label" || entry.first == "y_label" ||
               entry.first == "stroke" || entry.first == "y_min" || entry.first == "y_max" ||
               entry.first == "x_min" || entry.first == "x_max" || entry.first == "caption")) continue;
          if ((component.kind == "audio_input" || component.kind == "audio_annotator") && (entry.first == "audio_id" || entry.first == "audio_recording" || entry.first == "audio_source" || entry.first == "audio_view" || entry.first == "audio_fft_size" || entry.first == "annotation_id")) continue;
          if ((component.kind == "image" || component.kind == "image_carousel") &&
              (entry.first == "image_id" || entry.first == "image_mode" || entry.first == "caption" ||
               entry.first == "image_allow_save" || entry.first == "image_allow_load" || entry.first == "image_allow_read" ||
               entry.first == "image_annotator" || entry.first == "annotation_id" || entry.first == "image_items" || entry.first == "image_series_caption")) continue;
          component.extra.insert(entry);
        }
      }
    };
    if (replacement.rows && found->rows) merge_rows(*replacement.rows, *found->rows);
    merge_components(replacement.components, found->components, 0);
    *found = std::move(replacement);
  }
  std::map<std::string, std::shared_ptr<const sputnik::runtime::NotebookImageInputs>> staged;
  for (const auto &tab : tabs_) if (!tab->module) {
    auto images = std::make_shared<sputnik::runtime::NotebookImageInputs>(*sputnik::notebook::load_project_image_inputs(next, project_.directory, tab->session.project_sheet_id));
    if (tab->session.project_images) for (auto &[id, value] : *images) {
      const auto old = tab->session.project_images->find(id);
      if (old != tab->session.project_images->end()) value.selected = old->second.selected;
    }
    staged.emplace(tab->session.project_sheet_id, images);
  }
  std::map<std::string, std::shared_ptr<const sputnik::runtime::NotebookAudioInputs>> staged_audio;
  for (const auto &tab : tabs_) if (!tab->module) staged_audio[tab->session.project_sheet_id] = sputnik::notebook::load_project_audio_inputs(next, project_.directory, tab->session.project_sheet_id);
  auto input_values = std::make_shared<sputnik::runtime::NotebookInputSnapshot>(*sputnik::notebook::project_input_defaults(next));
  for (auto &[key, value] : *input_values) if (key.rfind(std::string(1, '\0') + "audio:", 0) != 0 && key.rfind(std::string(1, '\0') + "selection:", 0) != 0 && key.rfind(std::string(1, '\0') + "annotations:", 0) != 0 && key.rfind(std::string(1, '\0') + "audio_annotations:", 0) != 0) {
    const auto old = inputs_->find(key); if (old != inputs_->end()) value = old->second;
  }
  sputnik::notebook::validate_project_control_values(next, *input_values);
  std::set<std::string> changed_annotations;
  auto annotation_key = [](const std::string &key) { return key.rfind(std::string(1, '\0') + "annotations:", 0) == 0 || key.rfind(std::string(1, '\0') + "audio_annotations:", 0) == 0; };
  for (const auto &[key, value] : *input_values) if (annotation_key(key)) {
    const auto old = inputs_->find(key); if (old == inputs_->end() || old->second != value) changed_annotations.insert(key);
  }
  for (const auto &[key, value] : *inputs_) if (annotation_key(key) && !input_values->count(key)) changed_annotations.insert(key);
  sputnik::notebook::save_project(&project_, next);
  inputs_ = input_values;
  for (const auto &tab : tabs_) if (!tab->module) {
    assign_inputs(*tab);
    tab->session.project_audio_inputs = staged_audio.at(tab->session.project_sheet_id);
    invalidate_project_input_changes(&tab->session, changed_annotations);
  }
  for (const auto &tab : tabs_) if (!tab->module) tab->session.project_audio = sputnik::notebook::project_audio_components(project_.document, tab->session.project_sheet_id);
  for (const auto &tab : tabs_) if (!tab->module) {
    const auto images = staged.at(tab->session.project_sheet_id);
    for (auto &cell : tab->session.cells) {
      for (const auto &[id, input] : *images) {
        bool changed = !tab->session.project_images || !tab->session.project_images->count(id);
        if (!changed) {
          const auto &old = tab->session.project_images->at(id);
          changed = input.images.size() != old.images.size() || input.caption != old.caption;
          for (std::size_t i = 0; !changed && i < input.images.size(); ++i)
            changed = input.images[i].bytes != old.images[i].bytes || input.images[i].caption != old.images[i].caption;
        }
        if (changed) {
          const auto channel = id;
          cell.images.erase(std::remove_if(cell.images.begin(), cell.images.end(), [&](const auto &event) { return event.id == channel; }), cell.images.end());
        }
        const bool permission_changed = !tab->session.project_images || !tab->session.project_images->count(id) ||
            tab->session.project_images->at(id).readable != input.readable;
        if ((changed || permission_changed) && cell.kind == "code") cell.dirty = true;
      }
    }
    tab->session.project_images = images;
  }
}

void ProjectTabs::select_image(const std::string &sheet, const std::string &channel, std::size_t index) {
  require_idle();
  for (const auto &tab : tabs_) if (!tab->module && tab->session.project_sheet_id == sheet) {
    if (!tab->session.project_images || !tab->session.project_images->count(channel)) throw std::runtime_error("unknown image channel");
    auto inputs = std::make_shared<sputnik::runtime::NotebookImageInputs>(*tab->session.project_images);
    std::size_t count = inputs->at(channel).images.size();
    for (const auto &cell : tab->session.cells) for (const auto &images : cell.images) if (images.id == channel) count = images.images.size();
    if (index >= std::max<std::size_t>(1, count)) throw std::runtime_error("image selection is out of range");
    inputs->at(channel).selected = index;
    tab->session.project_images = inputs;
    // Image input changes are inert. Consumers are rerun explicitly.
    return;
  }
  throw std::runtime_error("unknown image sheet");
}

void ProjectTabs::edit_images(const sputnik::notebook::ProjectImageEdit &edit) {
  require_idle();
  auto board = std::find_if(project_.document.boards.begin(), project_.document.boards.end(), [&](const auto &value) { return value.id == edit.board; });
  if (board == project_.document.boards.end()) throw std::runtime_error("unknown image board");
  auto replacement = *board;
  std::function<sputnik::notebook::ProjectComponent *(std::vector<sputnik::notebook::ProjectComponent> &)> find = [&](auto &items) -> sputnik::notebook::ProjectComponent * {
    for (auto &item : items) {
      if (item.id == edit.component) return &item;
      if (item.panels) for (auto &panel : *item.panels) if (auto found = find(panel.components)) return found;
    }
    return nullptr;
  };
  auto component = find(replacement.components);
  if (!component || (component->kind != "image" && component->kind != "image_carousel") ||
      !sputnik::notebook::project_image_option(*component, "image_allow_load")) throw std::runtime_error("image editing is disabled");
  if (component->kind == "image" && edit.images.size() > 1) throw std::runtime_error("Image accepts one image");
  if (edit.images.size() > 64 || edit.caption.size() > 65536 || edit.selected >= std::max<std::size_t>(1, edit.images.size()))
    throw std::runtime_error("invalid image collection size or selection");
  std::size_t bytes = 0, text = edit.caption.size();
  for (const auto &image : edit.images) {
    sputnik::runtime::notebook_validate_image(image);
    if (!image.plot_scene.empty()) throw std::runtime_error("image collections require raster data");
    bytes += image.bytes.size(); text += image.caption.size();
    if (bytes > 64U * 1024U * 1024U || text > 256U * 1024U) throw std::runtime_error("image collection exceeds limits");
  }
  const auto channel = sputnik::notebook::project_image_channel(*component);
  const auto old_assets = sputnik::notebook::project_image_assets(*component);
  const auto old_inputs = sputnik::notebook::load_project_image_inputs(project_.document, project_.directory, replacement.sheet);
  std::vector<sputnik::notebook::ProjectImageAsset> assets;
  for (const auto &image : edit.images) {
    std::string path;
    if (const auto old = old_inputs->find(channel); old != old_inputs->end())
      for (std::size_t i = 0; i < std::min(old_assets.size(), old->second.images.size()); ++i)
        if (old->second.images[i].bytes == image.bytes) { path = old_assets[i].path; break; }
    if (path.empty()) path = sputnik::notebook::store_project_image(project_, image);
    assets.push_back({path, image.caption});
  }
  if (const auto annotations = component->extra.find("annotations"); annotations != component->extra.end() && annotations->second != "null") {
    const auto source = sputnik::notebook::project_component_data_source("annotations", annotations->second);
    if (std::none_of(edit.images.begin(), edit.images.end(), [&](const auto &image) { return sputnik::runtime::notebook_asset_id(image.bytes) == source.identity; })) component->extra["annotations"] = "null";
  }
  component->extra["image_items"] = sputnik::notebook::serialize_project_image_assets(assets);
  component->extra["image_series_caption"] = sputnik::notebook::serialize_project_input_value(edit.caption);
  static std::uint64_t image_revision = 0;
  component->extra["image_revision"] = sputnik::notebook::serialize_project_input_value(
      std::to_string(sputnik::runtime::NotebookLiveStore::monotonic_millis()) + "_" + std::to_string(++image_revision));
  component->extra["image_selection"] = std::to_string(edit.selected);
  save_board(replacement);
  // An explicit edit takes ownership even when the persisted fallback was identical.
  for (const auto &tab : tabs_) if (tab->session.project_sheet_id == replacement.sheet)
    for (auto &cell : tab->session.cells) cell.images.erase(std::remove_if(cell.images.begin(), cell.images.end(), [&](const auto &event) { return event.id == channel; }), cell.images.end());
  select_image(replacement.sheet, channel, edit.selected);
}

bool ProjectTabs::restore_inputs(const std::map<std::string, std::string> &values) {
  require_idle();
  auto next = std::make_shared<sputnik::runtime::NotebookInputSnapshot>(*inputs_);
  std::set<std::string> changed;
  for (const auto &[name, json] : values) {
    const sputnik::notebook::ProjectInput *definition = nullptr;
    std::string key; sputnik::notebook::ProjectCellUI ui;
    if (name.rfind("project:", 0) == 0) {
      key = name.substr(8);
      for (const auto &input : project_.document.inputs) if (input.id == key) definition = &input;
    } else if (name.rfind("cell:", 0) == 0) {
      const auto separator = name.find(':', 5);
      if (separator == std::string::npos) throw std::runtime_error("invalid saved cell input key");
      const auto owner = name.substr(5, separator - 5), id = name.substr(separator + 1);
      for (const auto &sheet : project_.document.sheets) for (const auto &cell : sheet.cells)
        if (std::to_string(cell.id) == owner && cell.extra.count("cell_ui")) {
          ui = sputnik::notebook::parse_project_cell_ui(cell.extra.at("cell_ui"));
          for (const auto &input : ui.inputs) if (input.id == id) { definition = &input; break; }
          key = sputnik::runtime::notebook_cell_input_key(cell.id, id);
        }
    }
    if (!definition || !next->count(key)) throw std::runtime_error("saved input no longer exists: " + name);
    auto value = sputnik::notebook::parse_project_input_value(json);
    sputnik::notebook::validate_project_input_value(*definition, value);
    if (definition->type == "number" && std::holds_alternative<std::int64_t>(value)) value = static_cast<double>(std::get<std::int64_t>(value));
    if (next->at(key) != value) { next->at(key) = std::move(value); changed.insert(key); }
  }
  sputnik::notebook::validate_project_control_values(project_.document, *next);
  if (changed.empty()) return false;
  inputs_ = next;
  for (const auto &tab : tabs_) { assign_inputs(*tab); invalidate_project_input_changes(&tab->session, changed); }
  return true;
}

void ProjectTabs::edit_component_data(const sputnik::notebook::ProjectComponentDataEdit &edit) {
  require_idle(); sputnik::notebook::validate_project_component_data(edit.kind, edit.json);
  const auto original = std::find_if(project_.document.boards.begin(), project_.document.boards.end(), [&](const auto &b) { return b.id == edit.board; });
  if (original == project_.document.boards.end()) throw std::runtime_error("unknown component data board");
  auto board = *original;
  std::function<sputnik::notebook::ProjectComponent *(std::vector<sputnik::notebook::ProjectComponent> &)> find = [&](auto &items) -> sputnik::notebook::ProjectComponent * {
    for (auto &c : items) { if (c.id == edit.component) return &c; if (c.panels) for (auto &panel : *c.panels) if (auto found = find(panel.components)) return found; } return nullptr;
  };
  auto component = find(board.components);
  if (!component || (edit.kind == "selection" ? component->kind != "table" : edit.kind == "audio_annotations" ? component->kind != "audio_annotator" : ((component->kind != "image" && component->kind != "image_carousel") || !sputnik::notebook::project_image_option(*component, "image_annotator"))))
    throw std::runtime_error("component does not own this editable data");
  const auto channel = sputnik::notebook::project_component_data_channel(*component, edit.kind);
  if (channel.empty()) throw std::runtime_error("configure a data channel in the designer");
  Session *session = nullptr;
  for (const auto &tab : tabs_) if (!tab->module && tab->session.project_sheet_id == board.sheet) session = &tab->session;
  if (!session) throw std::runtime_error("component controller sheet unavailable");
  if (edit.json != "null") {
    sputnik::notebook::validate_project_annotation_data(*component, edit.kind, edit.json);
    const auto source = sputnik::notebook::project_component_data_source(edit.kind, edit.json);
    bool current = false;
    if (edit.kind == "selection") {
      const auto channel_it = component->extra.find("table_id");
      const auto table_channel = channel_it == component->extra.end() ? std::string{} : std::get<std::string>(sputnik::notebook::parse_project_input_value(channel_it->second));
      for (const auto &cell : session->cells) if (cell.id == component->cell) {
        for (const auto &display : cell.displays)
          if (display.mime == "application/vnd.sputnik.table+json" && (table_channel.empty() || display.id == table_channel) && sputnik::runtime::notebook_asset_id(display.bytes) == source.identity) current = true;
        if (table_channel.empty() && !cell.result_format.table_json.empty() && sputnik::runtime::notebook_asset_id(cell.result_format.table_json) == source.identity) current = true;
      }
    } else if (edit.kind == "audio_annotations") {
      const bool from_cell = sputnik::notebook::project_audio_source(*component) == "cell";
      if (source.channel != sputnik::notebook::project_audio_channel(*component) || source.source != (from_cell ? "cell" : "input") || source.cell != (from_cell ? component->cell : 0)) throw std::runtime_error("audio annotation binding changed");
      std::optional<sputnik::runtime::AudioDisplay> audio;
      if (!from_cell && session->project_audio_inputs) {
        const auto input = session->project_audio_inputs->find(source.channel);
        if (input != session->project_audio_inputs->end() && !input->second.audio.bytes.empty()) audio = input->second.audio;
      }
      if (from_cell) for (const auto &cell : session->cells) if (cell.id == component->cell) for (const auto &display : cell.displays)
        if (display.mime == sputnik::runtime::kAudioMime && display.id == source.channel) audio = sputnik::runtime::audio_decode(display.bytes);
      if (audio && sputnik::runtime::notebook_asset_id(audio->bytes) == source.identity) {
        const auto decoded = sputnik::runtime::audio_system_decode(audio->bytes, audio->format);
        current = decoded.rate == source.rate && decoded.channels == source.channels && decoded.bytes.size() / (4 * decoded.channels) == source.frames;
      }
    } else {
      if (source.channel != sputnik::notebook::project_image_channel(*component)) throw std::runtime_error("annotation image channel changed");
      auto matches = [&](const auto &image) { return image.width == source.width && image.height == source.height && sputnik::runtime::notebook_asset_id(image.bytes) == source.identity; };
      if (session->project_images) {
        const auto input = session->project_images->find(source.channel);
        if (input != session->project_images->end()) current = std::any_of(input->second.images.begin(), input->second.images.end(), matches);
      }
      for (auto cell = session->cells.rbegin(); cell != session->cells.rend(); ++cell) {
        const auto images = std::find_if(cell->images.begin(), cell->images.end(), [&](const auto &series) { return series.id == source.channel; });
        if (images != cell->images.end()) { current = std::any_of(images->images.begin(), images->images.end(), matches); break; }
      }
    }
    if (!current) throw std::runtime_error("source changed; select or annotate the current snapshot again");
  }
  component->extra[edit.kind] = edit.json;
  const auto key = sputnik::runtime::notebook_component_data_key(edit.kind, channel);
  save_board(board);
  for (const auto &tab : tabs_) invalidate_project_input_changes(&tab->session, {key});
}

void ProjectTabs::edit_audio(const sputnik::notebook::ProjectAudioEdit &edit) {
  require_idle();
  auto next = project_.document;
  const auto board = std::find_if(next.boards.begin(), next.boards.end(), [&](const auto &b) { return b.id == edit.board; });
  if (board == next.boards.end()) throw std::runtime_error("unknown Audio Input board");
  std::function<sputnik::notebook::ProjectComponent *(std::vector<sputnik::notebook::ProjectComponent> &)> find = [&](auto &items) -> sputnik::notebook::ProjectComponent * {
    for (auto &c : items) { if (c.id == edit.component) return &c; if (c.panels) for (auto &panel : *c.panels) if (auto found = find(panel.components)) return found; }
    return nullptr;
  };
  auto component = find(board->components);
  if (!component || !sputnik::notebook::project_owns_audio_input(*component)) throw std::runtime_error("unknown Audio Input component");
  if (edit.name.size() > 1024 || edit.name.find('\0') != std::string::npos) throw std::runtime_error("invalid audio input name");
  const auto path = edit.audio ? sputnik::notebook::store_project_audio(project_, *edit.audio) : std::string{};
  component->extra["audio_asset"] = sputnik::notebook::serialize_project_input_value(path);
  component->extra["audio_name"] = sputnik::notebook::serialize_project_input_value(edit.name);
  // Preload every new frozen input map before committing the manifest.
  std::map<std::string, std::shared_ptr<const sputnik::runtime::NotebookAudioInputs>> staged;
  for (const auto &tab : tabs_) if (!tab->module) staged[tab->session.project_sheet_id] = sputnik::notebook::load_project_audio_inputs(next, project_.directory, tab->session.project_sheet_id);
  auto values = std::make_shared<sputnik::runtime::NotebookInputSnapshot>(*inputs_);
  const auto defaults = sputnik::notebook::project_input_defaults(next);
  const auto key = sputnik::runtime::audio_input_key(sputnik::notebook::project_audio_channel(*component));
  (*values)[key] = defaults->at(key);
  sputnik::notebook::save_project(&project_, next); inputs_ = values;
  for (const auto &tab : tabs_) if (!tab->module) {
    assign_inputs(*tab); tab->session.project_audio_inputs = staged.at(tab->session.project_sheet_id);
    invalidate_project_input_changes(&tab->session, {key});
  }
}

void ProjectTabs::add_dependency(const std::string &directory) {
  require_idle();
  const auto root = std::filesystem::canonical(project_.directory / directory);
  sputnik::notebook::ProjectDependency dependency;
  dependency.path = root.lexically_relative(project_.directory).generic_string();
  if (dependency.path.empty()) dependency.path = root.string();
  (void)load_directory_dependency(project_.directory, dependency);
  auto next = project_.document;
  next.version = std::max(next.version, 3U);
  next.dependencies.push_back(dependency);
  sputnik::notebook::save_project(&project_, next);
  for (const auto &tab : tabs_) if (!tab->module) tab->session.project_audio = sputnik::notebook::project_audio_components(project_.document, tab->session.project_sheet_id);
  for (const auto &tab : tabs_) if (!tab->module) tab->session.environment_stale = true;
  active().session.status = "dependency linked; structure saved; Apply to load it (no code copied or executed)";
}

void ProjectTabs::remove_dependency(const std::string &path) {
  require_idle();
  auto next = project_.document;
  const auto found = std::find_if(next.dependencies.begin(), next.dependencies.end(), [&](const auto &d) { return d.path == path; });
  if (found == next.dependencies.end()) throw std::invalid_argument("unknown dependency path: " + path);
  next.dependencies.erase(found);
  sputnik::notebook::save_project(&project_, next);
  for (const auto &tab : tabs_) if (!tab->module) tab->session.project_audio = sputnik::notebook::project_audio_components(project_.document, tab->session.project_sheet_id);
  for (const auto &tab : tabs_) if (!tab->module) tab->session.environment_stale = true;
  active().session.status = "dependency unlinked; package files untouched; Apply to load the new environment";
}

void ProjectTabs::rename_sheet(const std::string &id, const std::string &title) {
  require_idle();
  const auto characters = std::count_if(title.begin(), title.end(), [](unsigned char c) { return (c & 0xc0) != 0x80; });
  if (title.empty() || characters > 200 || title.find_first_not_of(" \t\r\n") == std::string::npos)
    throw std::invalid_argument("Enter a sheet name (up to 200 characters).");
  auto next = project_.document;
  const auto found = std::find_if(next.sheets.begin(), next.sheets.end(), [&](const auto &s) { return s.id == id; });
  if (found == next.sheets.end()) throw std::invalid_argument("unknown sheet: " + id);
  found->title = title;
  sputnik::notebook::save_project(&project_, next);
  for (const auto &tab : tabs_) if (!tab->module && tab->session.project_sheet_id == id)
    tab->session.project_label = next.title + "/" + title;
}

std::unique_ptr<ProjectTab> ProjectTabs::make_sheet_tab(const sputnik::notebook::ProjectSheet &sheet) {
  auto tab = std::make_unique<ProjectTab>();
  tab->session.project_sheet_id = sheet.id;
  tab->session.project_label = project_.document.title + "/" + sheet.title;
  // A new sheet starts from the last applied environment without initializing a VM.
  tab->session.bundled_modules = environment_sources_;
  Cell cell;
  cell.id = sheet.cells.front().id;
  tab->session.cells.push_back(std::move(cell));
  tab->session.status = "sheet created; structure saved; not evaluated";
  attach_new_tab(tab.get());
  return tab;
}

void ProjectTabs::create_sheet(const std::string &id, const std::string &title) {
  require_idle();
  validate_new_tab_id(id);
  auto next = project_.document;
  sputnik::notebook::ProjectSheet sheet;
  sheet.id = id;
  sheet.title = title.empty() ? id : title;
  sputnik::notebook::ProjectCell saved_cell;
  saved_cell.id = sputnik::notebook::allocate_cell_id();
  sheet.cells.push_back(saved_cell);
  next.sheets.push_back(sheet);
  sputnik::notebook::validate_project_document(next);

  auto tab = make_sheet_tab(sheet);
  tabs_.reserve(tabs_.size() + 1U);
  const std::size_t index = project_.document.sheets.size();
  const auto publish = [&] {
    tabs_.insert(tabs_.begin() + index, std::move(tab));
    selected_ = index;
  };
  try {
    // Merge with the saved document only, not other tabs' unsaved contents.
    sputnik::notebook::save_project(&project_, next);
  for (const auto &tab : tabs_) if (!tab->module) tab->session.project_audio = sputnik::notebook::project_audio_components(project_.document, tab->session.project_sheet_id);
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

void ProjectTabs::delete_sheet(const std::string &id, const std::string &blank_id) {
  require_idle();
  auto next = project_.document;
  const auto found = std::find_if(next.sheets.begin(), next.sheets.end(),
                                  [&](const auto &sheet) { return sheet.id == id; });
  if (found == next.sheets.end()) throw std::invalid_argument("unknown sheet: " + id);
  for (const auto &consumer : next.sheets)
    for (const auto &link : consumer.imports)
      if (link.sheet == id)
        throw std::invalid_argument("Sheet '" + consumer.title + "' imports '" + link.name +
                                    "' from this sheet. Remove or change that import first.");
  const auto index = static_cast<std::size_t>(found - next.sheets.begin());
  // Keep future native allocations above removed cells even after reopening.
  // A remote notebook retains tombstones for these identities.
  auto high_water = cell_id_high_water(next);
  for (const auto &cell : found->cells) high_water = std::max(high_water, cell.id);
  for (const auto &cell : tabs_[index]->session.cells) high_water = std::max(high_water, cell.id);
  if (high_water) next.extra[CellIDHighWater] = std::to_string(high_water);
  next.sheets.erase(found);
  const bool replacing_last = next.sheets.empty();

  std::unique_ptr<ProjectTab> blank;
  if (replacing_last) {
    validate_new_tab_id(blank_id);
    if (blank_id == id) throw std::invalid_argument("A replacement sheet needs a new ID.");
    auto sheet = sputnik::notebook::make_project_document(next.title).sheets.front();
    sheet.id = blank_id;
    sheet.title = "Sheet 1";
    blank = make_sheet_tab(sheet);
    next.sheets.push_back(std::move(sheet));
  }
  const auto replacement = std::min(index, next.sheets.size() - 1);
  const auto &replacement_id = next.sheets[replacement].id;
  if (next.active_sheet == id) next.active_sheet = replacement_id;
  for (auto &board : next.boards) if (board.sheet == id) board.sheet = replacement_id;

  const auto publish = [&] {
    tabs_[index]->session.close_activity();
    if (blank) tabs_[index] = std::move(blank);
    else tabs_.erase(tabs_.begin() + index);
    if (selected_ == index) selected_ = replacement;
    else if (selected_ > index && !replacing_last) --selected_;
    for (const auto &tab : tabs_) if (!tab->module)
      tab->session.project_audio = sputnik::notebook::project_audio_components(project_.document, tab->session.project_sheet_id);
  };
  try {
    sputnik::notebook::save_project(&project_, next);
  } catch (...) {
    const auto &saved = project_.document.sheets;
    if (std::none_of(saved.begin(), saved.end(), [&](const auto &sheet) { return sheet.id == id; })) publish();
    throw;
  }
  publish();
}

void ProjectTabs::save_sheet_imports(const std::string &id, const std::string &json) {
  require_idle();
  auto next = project_.document;
  const auto found = std::find_if(next.sheets.begin(), next.sheets.end(),
      [&](const auto &sheet) { return sheet.id == id; });
  if (found == next.sheets.end()) throw std::invalid_argument("unknown sheet: " + id);
  const auto index = static_cast<std::size_t>(found - next.sheets.begin());
  found->imports = sputnik::notebook::parse_project_sheet_imports(json);
  if (!found->imports.empty()) next.version = 5;
  std::set<std::string> names;
  for (const auto &link : found->imports) names.insert(link.name);
  sputnik::notebook::save_project(&project_, next);
  auto &session = tabs_[index]->session;
  session.sheet_import_names = std::move(names);
  for (auto &cell : session.cells) if (cell.kind == "code") cell.dirty = true;
}

const sputnik::notebook::ProjectSheet *ProjectTabs::sheet_for(const ProjectTab &tab) const {
  const auto &sheets = project_.document.sheets;
  const auto found = std::find_if(sheets.begin(), sheets.end(),
      [&](const auto &sheet) { return sheet.id == tab.session.project_sheet_id; });
  return found == sheets.end() ? nullptr : &*found;
}

void ProjectTabs::assign_inputs(ProjectTab &tab) {
  if (tab.imported_values.empty()) {
    tab.session.project_inputs = inputs_;
    return;
  }
  auto values = std::make_shared<sputnik::runtime::NotebookInputSnapshot>(*inputs_);
  for (const auto &[name, value] : tab.imported_values)
    values->emplace(sputnik::runtime::notebook_sheet_import_key(name), value);
  tab.session.project_inputs = std::move(values);
}

std::set<std::string> ProjectTabs::refresh_sheet_imports(ProjectTab &tab) {
  const auto *sheet = sheet_for(tab);
  if (!sheet || (sheet->imports.empty() && tab.imported_values.empty())) return {};
  using sputnik::runtime::notebook_sheet_import_key;
  sputnik::runtime::NotebookExports imports;
  for (const auto &link : sheet->imports) {
    for (const auto &provider : tabs_) {
      if (provider->session.project_sheet_id != link.sheet) continue;
      const auto &cells = provider->session.cells;
      const auto source = std::find_if(cells.begin(), cells.end(),
          [&](const auto &cell) { return cell.id == link.cell; });
      if (source == cells.end() || source->dirty || !source->ok) break;
      const auto exported = source->exports.find(link.export_name);
      if (exported != source->exports.end()) imports.emplace(link.name, exported->second);
      break;
    }
  }
  std::set<std::string> changed;
  std::size_t bytes = 0;
  for (const auto &[name, value] : imports) {
    bytes += name.size() + value.size();
    if (bytes > 7U * 1024U * 1024U)
      throw std::runtime_error("sheet import snapshot exceeds the 7 MiB worker transfer limit");
    auto key = notebook_sheet_import_key(name);
    const auto previous = tab.imported_values.find(name);
    if (previous == tab.imported_values.end() || previous->second != value) changed.insert(key);
  }
  for (const auto &[name, _] : tab.imported_values)
    if (!imports.count(name)) changed.insert(notebook_sheet_import_key(name));
  tab.imported_values.swap(imports);
  assign_inputs(tab);
  return changed;
}

bool ProjectTabs::sheet_has_cycle(const std::string &id) const {
  std::set<std::string> visited;
  std::function<bool(const std::string &)> reaches = [&](const auto &current) {
    if (!visited.insert(current).second) return false;
    const auto &sheets = project_.document.sheets;
    const auto found = std::find_if(sheets.begin(), sheets.end(),
        [&](const auto &sheet) { return sheet.id == current; });
    if (found == sheets.end()) return false;
    for (const auto &link : found->imports)
      if (link.sheet == id || reaches(link.sheet)) return true;
    return false;
  };
  return reaches(id);
}

bool ProjectTabs::run_linked(ProjectTab &tab, const std::string &action,
                             std::size_t index, const SheetRun &run) {
  if (tab.module || project_.document.version < 5)
    return run(tab, action, index, {});
  using sputnik::notebook::CellId;
  std::map<CellId, std::uint64_t> before;
  for (const auto &entry : tabs_)
    for (const auto &cell : entry->session.cells) before.emplace(cell.id, cell.evaluations);
  if (action == "run" || action == "run_all" || action == "apply")
    tab.iteration_ready.clear();
  std::set<CellId> preparing;
  std::function<bool(ProjectTab &, const std::string &, std::size_t)> evaluate;
  evaluate = [&](ProjectTab &target, const std::string &command, std::size_t start) {
    const auto *sheet = sheet_for(target);
    const auto &cells = target.session.cells;
    if (!sheet || cells.empty()) return run(target, command, start, {});
    const auto root = cells.at(start).id;
    if (!preparing.insert(root).second) return false;
    const auto sources = notebook_sources(target.session);
    const sputnik::notebook::DependencyGraph graph(sources);
    std::set<CellId> relevant{root};
    if (command == "run_all" || command == "apply" || !cells.at(start).ok)
      for (const auto &source : sources)
        if (command == "run_all" || source.watch_enabled()) relevant.insert(source.id);
    const auto downstream = graph.invalidation_plan(root);
    relevant.insert(downstream.begin(), downstream.end());
    for (auto edge = graph.edges().rbegin(); edge != graph.edges().rend(); ++edge)
      if (relevant.count(edge->consumer)) relevant.insert(edge->provider.cell_id);
    for (const auto &link : sheet->imports) {
      if (std::none_of(relevant.begin(), relevant.end(), [&](auto id) {
            const auto *analysis = graph.analysis(id);
            return analysis && analysis->reads.count(link.name);
          })) continue;
      for (const auto &provider : tabs_) {
        if (provider->session.project_sheet_id != link.sheet) continue;
        const auto &upstream = provider->session.cells;
        const auto source = std::find_if(upstream.begin(), upstream.end(),
            [&](const auto &cell) { return cell.id == link.cell; });
        if (source == upstream.end() || !source->watch ||
            (source->ok && !source->dirty && source->exports.count(link.export_name))) break;
        if (!preparing.count(source->id) &&
            !evaluate(*provider, "run", static_cast<std::size_t>(source - upstream.begin()))) {
          preparing.erase(root);
          return false;
        }
        break;
      }
    }
    const auto changed = refresh_sheet_imports(target);
    const bool completed = run(target, command, start, changed);
    preparing.erase(root);
    return completed;
  };
  const bool completed = action == "watch" || action == "inputs"
      ? run(tab, action, index, refresh_sheet_imports(tab))
      : evaluate(tab, action, index);
  if (completed) propagate_exports(run, before);
  return completed;
}

void ProjectTabs::propagate_exports(const SheetRun &run,
    const std::map<sputnik::notebook::CellId, std::uint64_t> &before) {
  std::map<ProjectTab *, std::set<sputnik::notebook::CellId>> feedback;
  std::vector<ProjectTab *> ordered;
  std::set<std::string> visited;
  std::function<void(ProjectTab &)> visit = [&](ProjectTab &tab) {
    const auto *sheet = sheet_for(tab);
    if (!sheet || !visited.insert(sheet->id).second) return;
    for (const auto &link : sheet->imports)
      for (const auto &provider : tabs_)
        if (!provider->module && provider->session.project_sheet_id == link.sheet) visit(*provider);
    ordered.push_back(&tab);
  };
  for (const auto &tab : tabs_) if (!tab->module) visit(*tab);
  std::size_t passes = before.size() + 1;
  bool complete = true;
  while (passes--) {
    bool ran = false;
    for (auto *tab : ordered) {
      const auto *sheet = sheet_for(*tab);
      if (!sheet || sheet->imports.empty()) continue;
      const bool started = std::any_of(tab->session.cells.begin(), tab->session.cells.end(),
          [](const auto &cell) { return cell.kind == "code" && (cell.ok || !cell.error.empty()); });
      if (!started) continue;
      const auto changed = refresh_sheet_imports(*tab);
      if (changed.empty()) continue;
      tab->iteration_deferred = tab->iteration_ready;
      if (sheet_has_cycle(sheet->id)) {
        for (const auto &source : notebook_sources(tab->session)) {
          const auto cell = std::find_if(tab->session.cells.begin(), tab->session.cells.end(),
              [&](const auto &entry) { return entry.id == source.id; });
          if (before.at(source.id) == cell->evaluations || !cell->ok) continue;
          tab->iteration_deferred.insert(source.id);
          const auto analysis = sputnik::notebook::analyze_cell(source);
          if (std::any_of(analysis.reads.begin(), analysis.reads.end(), [&](const auto &name) {
                return changed.count(sputnik::runtime::notebook_sheet_import_key(name));
              })) feedback[tab].insert(source.id);
        }
      }
      struct ClearDeferred {
        ProjectTab &tab;
        ~ClearDeferred() { tab.iteration_deferred.clear(); }
      } clear{*tab};
      if (!run(*tab, "inputs", 0, changed)) return;
      if (tab->session.status == "project input evaluation failed") complete = false;
      const auto sources = notebook_sources(tab->session);
      const sputnik::notebook::DependencyGraph graph(sources);
      std::set<sputnik::notebook::CellId> roots;
      for (const auto &source : sources) {
        const auto *analysis = graph.analysis(source.id);
        if (analysis && std::any_of(analysis->reads.begin(), analysis->reads.end(), [&](const auto &name) {
              return changed.count(sputnik::runtime::notebook_sheet_import_key(name));
            })) roots.insert(source.id);
      }
      for (const auto id : graph.invalidation_plan(roots)) {
        if (tab->iteration_deferred.count(id)) continue;
        const auto cell = std::find_if(tab->session.cells.begin(), tab->session.cells.end(),
            [&](const auto &value) { return value.id == id; });
        if (cell != tab->session.cells.end() && (cell->dirty || !cell->ok)) complete = false;
      }
      ran = true;
    }
    if (!ran) break;
  }
  if (passes == static_cast<std::size_t>(-1))
    throw std::runtime_error("sheet dependency propagation did not settle");
  if (complete) for (const auto &[tab, cells] : feedback) {
    tab->iteration_ready.insert(cells.begin(), cells.end());
    tab->session.status = "Cycle completed · ready for the next manual iteration";
  }
}

void ProjectTabs::create_module(const std::string &id) {
  require_idle();
  validate_new_tab_id(id);
  const sputnik::notebook::ProjectModule entry{id, "modules/" + id + ".s", {}};
  const std::string source = "package " + id + "\n";
  auto preview = project_;
  preview.document.modules.push_back(entry);
  preview.baseline = sputnik::notebook::serialize_project_document(preview.document);
  auto tab = std::make_unique<ProjectTab>();
  tab->module = sputnik::notebook::LoadedProjectModule{id, entry.path, source, source};
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
    (void)sputnik::notebook::create_project_module(&project_, entry, source);
  } catch (...) {
    if (project_.document.modules.size() == old_count + 1U &&
        project_.document.modules.back().id == id)
      publish();
    throw;
  }
  publish();
}

void ProjectTabs::rename_module(const std::string &id, const std::string &name) {
  require_idle();
  const auto found = std::find_if(tabs_.begin(), tabs_.end(), [&](const auto &tab) {
    return tab->module && tab->module->id == id;
  });
  if (found == tabs_.end()) throw std::invalid_argument("unknown module: " + id);
  const auto start = name.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) throw std::invalid_argument("Enter a module name.");
  const auto end = name.find_last_not_of(" \t\r\n");
  std::string replacement;
  bool separator = false;
  // Keep the same safe identifier and filename in every notebook host.
  for (auto i = start; i <= end; ++i) {
    const auto ch = name[i];
    const bool next_separator = ch == ' ' || ch == '-' || ch == '\t' || ch == '\r' || ch == '\n';
    if (!separator || !next_separator) replacement.push_back(next_separator ? '_' : ch);
    separator = next_separator;
  }
  if (replacement.size() > 64U) throw std::invalid_argument("Use a module name of up to 64 characters.");
  validate_new_tab_id(replacement);
  auto &tab = **found;
  if (tab.session.cells.size() != 1U)
    throw std::runtime_error("module editor requires one source buffer");
  auto source = tab.session.cells.front().source;
  const auto lexed = sputnik::lexer::Lexer(source, tab.module->path).lex();
  for (std::size_t i = 0; i + 1 < lexed.tokens.size(); ++i) {
    if (lexed.tokens[i].kind != sputnik::lexer::TokenKind::KeywordPackage) continue;
    const auto &token = lexed.tokens[i + 1];
    if (token.kind == sputnik::lexer::TokenKind::Identifier && token.lexeme == id)
      source.replace(token.span.start.offset, token.span.end.offset - token.span.start.offset, replacement);
    break;
  }
  auto module = *tab.module;
  // Allocate the editor state before crossing the manifest commit boundary.
  auto label = project_.document.title + "/module:" + replacement;
  auto module_path = (std::filesystem::path(module.path).parent_path() /
      (replacement + std::filesystem::path(module.path).extension().string())).generic_string();
  auto editor_id = replacement;
  const auto publish = [&] {
    tab.module = std::move(module);
    tab.session.project_module_id.swap(editor_id);
    tab.session.project_module_path.swap(module_path);
    tab.session.project_label.swap(label);
    tab.session.cells.front().source.swap(source);
    tab.session.cells.front().dirty = true;
    tab.session.document_dirty = false;
    for (const auto &sheet : tabs_) if (!sheet->module) sheet->session.environment_stale = true;
  };
  try {
    sputnik::notebook::rename_project_module(&project_, &module, replacement, source);
  } catch (...) {
    if (module.id == replacement && module.path == module_path && module.baseline == source)
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
  sputnik::notebook::save_project(&project_, next);
  for (const auto &tab : tabs_) if (!tab->module) tab->session.project_audio = sputnik::notebook::project_audio_components(project_.document, tab->session.project_sheet_id);
  active().session.status.swap(status);
  return present != enabled;
}

bool ProjectTabs::apply_active(bool force_all, bool show_running,
                               const EvaluationProgress &progress) {
  return run_linked(active(), "apply", 0,
      [&](ProjectTab &tab, const auto &command, auto start, const auto &changed) {
        if (command == "inputs") {
          apply_project_input_changes(&tab.session, changed, tab.iteration_deferred);
          return true;
        }
        invalidate_project_input_changes(&tab.session, changed);
        if (command == "apply") return apply_tab(tab, force_all, show_running, progress);
        evaluate_from_with_progress(&tab.session, start, false, show_running, progress);
        return !sputnik::runtime::runtime_run_cancel_requested();
      });
}

bool ProjectTabs::apply_tab(ProjectTab &tab, bool force_all, bool show_running,
                            const EvaluationProgress &progress) {
  require_idle();
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

const sputnik::notebook::LoadedProject &ProjectTabs::project() const noexcept {
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
