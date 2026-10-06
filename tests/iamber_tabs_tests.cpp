#include "tools/iamber/tabs.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <unistd.h>

namespace {

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "iamber tabs test failed: " << message << "\n";
    std::exit(1);
  }
}

template <typename Function>
void expect_throws(Function &&function, const std::string &message) {
  bool threw = false;
  try {
    function();
  } catch (const std::exception &) {
    threw = true;
  }
  expect(threw, message);
}

class TempDirectory {
public:
  TempDirectory() {
    char pattern[] = "/tmp/amber_iamber_tabs_XXXXXX";
    const char *created = ::mkdtemp(pattern);
    expect(created != nullptr, "temporary project directory should exist");
    path = created;
  }

  ~TempDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }

  std::filesystem::path path;
};

void write_text(const std::filesystem::path &path, const std::string &source) {
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  expect(!error, "module parent directory should be created");
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  expect(output.good(), "module source should be writable");
  output.write(source.data(), static_cast<std::streamsize>(source.size()));
  expect(output.good(), "module source should be written");
}

std::string read_text(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  expect(input.good(), "saved project member should be readable");
  std::ostringstream result;
  result << input.rdbuf();
  return result.str();
}

amber::notebook::ProjectDocument make_document() {
  using amber::notebook::CellMode;
  using amber::notebook::ProjectCell;
  using amber::notebook::ProjectDocument;
  using amber::notebook::ProjectSheet;

  ProjectDocument document;
  document.title = "tabs";
  document.active_sheet = "main";
  ProjectSheet main;
  main.id = "main";
  main.title = "Main";
  ProjectCell main_cell;
  main_cell.id = amber::notebook::allocate_cell_id();
  main_cell.source = "value()\n";
  main_cell.mode = CellMode::Watch;
  main.cells.push_back(main_cell);
  document.sheets.push_back(main);

  ProjectSheet other;
  other.id = "other";
  other.title = "Other";
  ProjectCell other_cell;
  other_cell.id = amber::notebook::allocate_cell_id();
  other_cell.source = "2\n";
  other_cell.mode = CellMode::Watch;
  other.cells.push_back(other_cell);
  document.sheets.push_back(other);
  return document;
}

amber::notebook::LoadedProject make_project(const std::filesystem::path &path) {
  amber::notebook::LoadedProject project =
      amber::notebook::create_project(path, make_document());
  const std::string source = "package main\n"
                             "def value():\n"
                             "  1\n"
                             "export value\n";
  write_text(path / "lib/main.am", source);
  amber::notebook::ProjectDocument manifest = project.document;
  manifest.modules.push_back({"main", "lib/main.am", {}});
  manifest.auto_imports.push_back("main");
  amber::notebook::save_project(&project, manifest);
  return project;
}

std::size_t find_sheet_tab(const ProjectTabs &tabs, const std::string &id) {
  for (std::size_t index = 0; index < tabs.tabs().size(); ++index) {
    const ProjectTab &tab = *tabs.tabs()[index];
    if (!tab.module.has_value() && tab.session.project_sheet_id == id)
      return index;
  }
  expect(false, "expected sheet tab should exist");
  return 0U;
}

std::size_t find_module_tab(const ProjectTabs &tabs, const std::string &id) {
  for (std::size_t index = 0; index < tabs.tabs().size(); ++index) {
    const ProjectTab &tab = *tabs.tabs()[index];
    if (tab.module.has_value() && tab.module->id == id)
      return index;
  }
  expect(false, "expected module tab should exist");
  return 0U;
}

void select_tab(ProjectTabs *tabs, std::size_t index) {
  expect(tabs != nullptr, "tab host should be present");
  if (tabs->selected() == index)
    return;
  const auto count = tabs->tabs().size();
  const auto delta = index >= tabs->selected()
                         ? index - tabs->selected()
                         : count - tabs->selected() + index;
  expect(delta <= static_cast<std::size_t>(std::numeric_limits<int>::max()),
         "test tab delta should fit in int");
  expect(tabs->switch_by(static_cast<int>(delta)),
         "requested project tab should be selectable");
}

bool same_bundled_sources_for_test(
    const std::vector<BundledModuleSource> &left,
    const std::vector<BundledModuleSource> &right) {
  if (left.size() != right.size())
    return false;
  for (std::size_t index = 0; index < left.size(); ++index) {
    if (left[index].id != right[index].id ||
        left[index].path != right[index].path ||
        left[index].source != right[index].source ||
        left[index].auto_import != right[index].auto_import)
      return false;
  }
  return true;
}

void test_constructs_all_tabs_without_execution_and_keeps_selection_identity() {
  TempDirectory temporary;
  amber::notebook::LoadedProject project = make_project(temporary.path / "p");
  ProjectTabs tabs(project);

  expect(tabs.tabs().size() == 3U, "sheets must precede modules in tab order");
  expect(tabs.selected() == 0U && tabs.active().session.project_sheet_id == "main",
         "manifest active sheet should be selected by default");
  expect(tabs.tabs()[0]->session.backend == nullptr &&
             tabs.tabs()[1]->session.backend == nullptr &&
             tabs.tabs()[2]->session.backend == nullptr,
         "constructing tabs must not create a runtime backend");
  expect(tabs.tab_bar() ==
             "tabs 1/3 [sheet:main] sheet:other module:main | env:1",
         "initial tab strip should expose stable labels and generation");

  Session *const main_session = &tabs.tabs()[0]->session;
  const int descriptor = tabs.activity_waiter().poll_descriptor();
  expect(descriptor >= 0 && descriptor == tabs.activity_waiter().poll_descriptor(),
         "the host activity descriptor must remain stable");
  expect(tabs.switch_by(1) && tabs.switch_by(-1) &&
             &tabs.active().session == main_session,
         "switching must select existing Session objects without executing");
  expect(tabs.switch_by(std::numeric_limits<int>::min()),
         "signed switch deltas must wrap without integer overflow");
  expect(tabs.active().session.backend == nullptr,
         "switching must not implicitly apply an environment");

  ProjectTabs selected_sheet(project, "main");
  ProjectTabs selected_module(project, {}, "main");
  expect(selected_sheet.tabs()[selected_sheet.selected()]->module ==
             std::nullopt,
         "same-id sheet selection must remain a sheet");
  expect(selected_module.tabs()[selected_module.selected()]->module.has_value(),
         "same-id module selection must remain a module");
  expect_throws([&] { ProjectTabs invalid(project, "main", "main"); },
                "sheet and module initial selections are mutually exclusive");
  expect_throws([&] { ProjectTabs invalid(project, "missing"); },
                "unknown initial tab should be rejected");
}

void test_opaque_cell_does_not_mark_a_clean_tab_modified() {
  TempDirectory temporary;
  auto document = make_document();
  amber::notebook::ProjectCell opaque;
  opaque.id = amber::notebook::allocate_cell_id();
  opaque.kind = "markdown";
  opaque.extra["source"] = R"({"markdown":"# Retained"})";
  document.sheets[0].cells.push_back(opaque);
  auto project = amber::notebook::create_project(temporary.path / "p", document);
  ProjectTabs tabs(project);
  expect(!tabs.modified(0U) && tabs.modified_count() == 0U &&
             tabs.tab_bar().find("sheet:main*") == std::string::npos,
         "read-only opaque cells must not show spurious unsaved edits");
  tabs.save_active();
  expect(amber::notebook::load_project(project.directory)
                 .document.sheets[0].cells.back().extra == opaque.extra,
         "saving a tab must preserve opaque cell metadata");
}

void test_save_active_only_persists_selected_sheet_and_modified_count_is_exact() {
  TempDirectory temporary;
  amber::notebook::LoadedProject project = make_project(temporary.path / "p");
  ProjectTabs tabs(project);

  tabs.tabs()[0]->session.cells[0].source = "main_edit = 9\n";
  tabs.tabs()[0]->session.document_dirty = true;
  expect(tabs.modified(0U) && tabs.modified_count() == 1U,
         "edited sheet should be counted as modified");

  expect(tabs.switch_by(1), "second sheet should be selectable");
  tabs.active().session.cells[0].source = "other_edit = 7\n";
  tabs.active().session.document_dirty = true;
  tabs.save_active();
  const auto after_other_save = amber::notebook::load_project(project.directory);
  expect(after_other_save.document.sheets[0].cells[0].source == "value()\n" &&
             after_other_save.document.sheets[1].cells[0].source ==
                 "other_edit = 7\n",
         "saving one sheet must preserve an inactive sheet edit and disk source");
  expect(tabs.modified(0U) && !tabs.modified(1U) && tabs.modified_count() == 1U,
         "inactive dirty sheet must remain dirty after another tab saves");

  expect(tabs.switch_by(-1), "main sheet should be selectable again");
  tabs.save_active();
  const auto after_main_save = amber::notebook::load_project(project.directory);
  expect(after_main_save.document.sheets[0].cells[0].source == "main_edit = 9\n" &&
             after_main_save.document.sheets[1].cells[0].source ==
                 "other_edit = 7\n" && tabs.modified_count() == 0U,
         "subsequent sheet saves should merge with the centralized baseline");

  expect(tabs.switch_by(2), "module tab should be available after a sheet save");
  tabs.active().session.cells[0].source = "package main\n"
                                          "def value():\n"
                                          "  3\n"
                                          "export value\n";
  tabs.active().session.document_dirty = true;
  tabs.save_active();
  expect(!tabs.modified(2U) &&
             read_text(temporary.path / "p/lib/main.am").find("  3") !=
                 std::string::npos,
         "module save should work after another sheet advances the baseline");
  expect(tabs.switch_by(-2), "main sheet should be selectable after module save");

  tabs.active().session.cells[0].source =
      std::string(1, static_cast<char>(0xc3));
  expect(tabs.modified(0U) && tabs.modified_count() == 1U,
         "unserializable source edits must still count as modified");
  expect_throws([&] { tabs.save_active(); },
                "invalid UTF-8 sheet source must be rejected by Save");
}

void test_module_save_apply_generation_stale_barrier_and_failure_rollback() {
  TempDirectory temporary;
  amber::notebook::LoadedProject project = make_project(temporary.path / "p");
  ProjectTabs tabs(project);
  expect(tabs.apply_active(true, false), "initial sheet Apply should succeed");
  expect(tabs.environment_generation() == 1U &&
             tabs.active().session.cells[0].ok &&
             tabs.active().session.cells[0].result == "1",
         "initial Apply should initialize only the active sheet");
  tabs.active().edit_mode = true;
  tabs.active().session.cells[0].cursor = 2U;
  SessionBackend *const old_backend = tabs.active().session.backend.get();
  const std::string main_result = tabs.active().session.cells[0].result;
  expect(tabs.switch_by(1), "other sheet should be selectable before reloading");
  expect(tabs.apply_active(true, false),
         "the second sheet should be explicitly initialized");
  expect(tabs.tabs()[1]->session.backend != nullptr &&
             tabs.tabs()[1]->session.cells[0].result == "2",
         "initializing another sheet should not replace the first runtime");
  expect(tabs.switch_by(-1), "main sheet should be selected for module update");
  expect(tabs.active().session.backend.get() == old_backend &&
             tabs.active().session.cells[0].result == main_result &&
             tabs.active().session.cells[0].cursor == 2U &&
             tabs.active().edit_mode,
         "switching must preserve backend, result, cursor and edit mode");

  expect(tabs.switch_by(2), "module tab should be selectable");
  tabs.active().session.cells[0].source = "package main\n"
                                          "def value():\n"
                                          "  2\n"
                                          "export value\n";
  tabs.active().session.document_dirty = true;
  tabs.save_active();
  expect(read_text(temporary.path / "p/lib/main.am").find("  2") !=
             std::string::npos,
         "module save should persist its source without running it");
  expect(tabs.switch_by(1), "switching from module should wrap to main sheet");

  expect(tabs.apply_active(true, false), "changed module Apply should succeed");
  expect(tabs.environment_generation() == 2U &&
             tabs.active().environment_generation == 2U &&
             !tabs.active().session.environment_stale &&
             tabs.active().session.cells[0].result == "2",
         "successful changed Apply should publish the next active generation");
  expect(tabs.tabs()[1]->session.environment_stale &&
             tabs.tabs()[1]->session.backend != nullptr,
         "other sheets must become stale without implicit evaluation");
  expect(pump_runtime_events_detailed(&tabs.tabs()[1]->session, false).disposition ==
             RuntimeEventPumpDisposition::DeferredByEdits,
         "an initialized stale sheet must defer runtime watch events");
  evaluate_from(&tabs.tabs()[1]->session, 0U, true, false, false);
  expect(tabs.tabs()[1]->session.status.find("F6 Apply") != std::string::npos,
         "stale sheets must reject normal evaluation");
  expect(tabs.tabs()[1]->session.environment_stale,
         "rejected stale evaluation must preserve the stale barrier");

  expect(tabs.switch_by(1), "stale second sheet should be selected for catch-up");
  expect(tabs.apply_active(true, false),
         "a stale sheet must be explicitly catch-up applied");
  expect(tabs.environment_generation() == 2U &&
             tabs.active().environment_generation == 2U &&
             !tabs.active().session.environment_stale &&
             !tabs.tabs()[0]->session.environment_stale &&
             tabs.active().session.cells[0].result == "2",
         "catch-up Apply should clear only the current stale sheet without a new generation");
  expect(tabs.switch_by(-1), "main sheet should be selected after catch-up");

  bool rejected_reentrant_switch = false;
  const bool applied = tabs.apply_active(
      true, true, [&](Session *, std::size_t) {
        try {
          (void)tabs.switch_by(1);
        } catch (const std::exception &) {
          rejected_reentrant_switch = true;
        }
      });
  expect(applied && rejected_reentrant_switch,
         "progress callbacks must not replace tabs during evaluation");

  expect(tabs.switch_by(2), "module tab should be selectable for bad edit");
  tabs.active().session.cells[0].source = "package main\ndef value(:\n";
  tabs.active().session.document_dirty = true;
  tabs.save_active();
  expect(tabs.switch_by(1), "main sheet should be selectable after bad edit");
  const std::uint64_t generation_before_failure = tabs.environment_generation();
  SessionBackend *const backend_before_failure =
      tabs.active().session.backend.get();
  const std::string result_before_failure = tabs.active().session.cells[0].result;
  expect(!tabs.apply_active(true, false), "invalid module Apply should fail");
  expect(tabs.environment_generation() == generation_before_failure &&
             tabs.active().session.backend.get() == backend_before_failure &&
             tabs.active().session.cells[0].result == result_before_failure,
         "failed Apply must preserve the host generation and old runtime");
  expect(old_backend != nullptr && backend_before_failure != nullptr,
         "successful Apply should have installed runtime backends");

  expect(tabs.switch_by(2), "module tab should be selectable for Apply rejection");
  expect(!tabs.apply_active(false, false),
         "Apply from a module tab should be rejected with a status");
  expect(tabs.active().session.status ==
             "switch to a sheet to Apply bundled modules",
         "module Apply rejection should explain how to continue");
  expect(tabs.switch_by(1), "sheet should be selectable after module rejection");
}

void test_modified_module_blocks_apply_and_activity_closes_before_sessions() {
  TempDirectory temporary;
  amber::notebook::LoadedProject project = make_project(temporary.path / "p");
  SessionActivityWaiter retained_host_waiter;
  SessionActivityWaiter retained_local_waiter;
  SessionActivityNotifier retained_local_notifier;
  {
    ProjectTabs tabs(project);
    retained_host_waiter = tabs.activity_waiter();
    retained_local_waiter = tabs.tabs()[0]->activity;
    retained_local_notifier = tabs.tabs()[0]->session.activity_notifier();
    expect(retained_local_notifier.notify_runtime(),
           "a sheet notification should wake its local waiter");
    expect(retained_local_waiter.wait(std::chrono::milliseconds(0)).runtime_ready,
           "local activity waiter should receive its own notification");
    expect(tabs.switch_by(1), "another sheet should be selected");
    expect(retained_local_notifier.notify_runtime(),
           "a retained sheet notifier should remain live after switching");
    expect(retained_host_waiter.wait(std::chrono::milliseconds(0)).runtime_ready,
           "host activity should receive notifications independent of selection");
    expect(tabs.switch_by(1), "module tab should be selected");
    tabs.active().session.cells[0].source += "\n";
    tabs.active().session.document_dirty = true;
    expect(tabs.switch_by(1), "sheet tab should be selected");
    expect(!tabs.apply_active(), "modified module tabs must block Apply");
    expect(tabs.active().session.status ==
               "save modified module tabs before Apply",
           "Apply should require modified module tabs to be saved first");
  }
  const SessionActivityResult closed = retained_host_waiter.wait(
      std::chrono::milliseconds(0));
  expect(closed.shutdown, "destroying ProjectTabs should close host activity");
  expect(retained_local_waiter.wait(std::chrono::milliseconds(0)).shutdown &&
             !retained_local_notifier.notify_runtime(),
         "destroying ProjectTabs should close retained local activity too");
}

void test_sheet_and_module_conflicts_preserve_editor_buffers() {
  {
    TempDirectory temporary;
    amber::notebook::LoadedProject project = make_project(temporary.path / "p");
    ProjectTabs tabs(project);
    tabs.active().session.cells[0].source = "local sheet edit\n";
    tabs.active().session.document_dirty = true;

    amber::notebook::LoadedProject external =
        amber::notebook::load_project(project.directory);
    external.document.title = "external replacement";
    amber::notebook::save_project(&external, external.document);
    expect_throws([&] { tabs.save_active(); },
                  "external project manifest changes must reject sheet Save");
    expect(tabs.active().session.cells[0].source == "local sheet edit\n" &&
               tabs.modified(0U),
           "sheet conflict must preserve its local buffer and dirty state");
  }
  {
    TempDirectory temporary;
    amber::notebook::LoadedProject project = make_project(temporary.path / "p");
    ProjectTabs tabs(project);
    expect(tabs.switch_by(2), "module tab should be selected for conflict test");
    const std::string local_source = "package main\n"
                                     "def value():\n"
                                     "  8\n"
                                     "export value\n";
    tabs.active().session.cells[0].source = local_source;
    tabs.active().session.document_dirty = true;
    write_text(temporary.path / "p/lib/main.am", "external module replacement\n");
    expect_throws([&] { tabs.save_active(); },
                  "external module changes must reject module Save");
    expect(tabs.active().session.cells[0].source == local_source &&
               tabs.modified(2U),
           "module conflict must preserve its local buffer and dirty state");
  }
}

void test_create_sheet_and_module_preserve_runtime_and_dirty_buffers() {
  TempDirectory temporary;
  amber::notebook::LoadedProject project = make_project(temporary.path / "p");
  ProjectTabs tabs(project);
  expect(tabs.apply_active(true, false),
         "an existing sheet should have an applied environment before creation");

  ProjectTab *const main_tab = tabs.tabs()[0].get();
  Session *const main_session = &main_tab->session;
  SessionBackend *const main_backend = main_session->backend.get();
  const std::string main_result = main_session->cells[0].result;
  const std::uint64_t applied_generation = tabs.environment_generation();
  const std::vector<BundledModuleSource> applied_sources =
      main_session->bundled_modules;
  amber::runtime::RuntimeCapabilityGrant grant;
  grant.name = "process.spawn";
  grant.target = "worker";
  grant.reason = "project command test";
  main_session->runtime_capability_grants.push_back(grant);
  main_session->runtime_watch_event_capacity = 1234U;
  SessionActivityNotifier retained_notifier = main_session->activity_notifier();
  SessionActivityWaiter retained_mailbox = main_session->activity_waiter();

  // Structure commands save only the manifest. These two buffers must survive
  // both creations and must not be silently published to their source files.
  main_session->cells[0].source = "dirty sheet buffer\n";
  main_session->document_dirty = true;
  const std::string old_module_source =
      read_text(temporary.path / "p/lib/main.am");

  std::set<amber::notebook::CellId> existing_cell_ids;
  for (const auto &tab : tabs.tabs())
    for (const auto &cell : tab->session.cells)
      existing_cell_ids.insert(cell.id);

  tabs.create_sheet("draft", "Draft sheet");
  expect(tabs.tabs().size() == 4U && tabs.selected() == 2U,
         "new sheets should be inserted after sheets and selected");
  expect(!tabs.active().module.has_value() &&
             tabs.active().session.project_sheet_id == "draft" &&
             tabs.active().session.project_label == "tabs/Draft sheet",
         "new sheet identity and title should be visible in its Session");
  expect(tabs.active().session.cells.size() == 1U &&
             tabs.active().session.cells[0].source.empty() &&
             tabs.active().session.cells[0].watch &&
             existing_cell_ids.count(tabs.active().session.cells[0].id) == 0U,
         "new sheets should contain one unique empty Watch cell");
  expect(tabs.environment_generation() == applied_generation &&
             tabs.active().environment_generation == applied_generation &&
             tabs.active().session.backend == nullptr &&
             same_bundled_sources_for_test(tabs.active().session.bundled_modules,
                                            applied_sources),
         "new sheets should inherit only the applied source snapshot and generation");
  expect(main_tab == tabs.tabs()[0].get() && &tabs.tabs()[0]->session == main_session &&
             tabs.tabs()[0]->session.backend.get() == main_backend &&
             tabs.tabs()[0]->session.cells[0].result == main_result &&
             !tabs.tabs()[0]->session.environment_stale,
         "creating a sheet must not rebuild or stale existing runtime state");
  expect(tabs.tabs()[2]->session.runtime_capability_grants.size() == 1U &&
             tabs.tabs()[2]->session.runtime_capability_grants[0].name ==
                 grant.name &&
             tabs.tabs()[2]->session.runtime_capability_grants[0].target ==
                 grant.target &&
             tabs.tabs()[2]->session.runtime_watch_event_capacity == 1234U,
         "new sheets should inherit the active Session host policy");
  expect(retained_notifier.notify_runtime() &&
             retained_mailbox.wait(std::chrono::milliseconds(0)).runtime_ready,
         "creating a sheet must retain existing Session mailbox identity");
  expect(tabs.project().document.active_sheet == "main",
         "creating a sheet must not change the project's active_sheet");

  const std::size_t old_module_index = find_module_tab(tabs, "main");
  tabs.tabs()[old_module_index]->session.cells[0].source =
      "package main\ndef value():\n  8\nexport value\n";
  tabs.tabs()[old_module_index]->session.document_dirty = true;
  tabs.create_module("draft_module");
  expect(tabs.tabs().size() == 5U && tabs.selected() == 4U &&
             tabs.active().module.has_value() &&
             tabs.active().module->id == "draft_module",
         "new modules should be appended after all sheets and selected last");
  expect(tabs.active().module->path == "modules/draft_module.am" &&
             tabs.active().session.cells.size() == 1U &&
             tabs.active().session.cells[0].source == "package draft_module\n" &&
             tabs.active().session.backend == nullptr,
         "new modules should have the expected regular source without running");
  expect(std::filesystem::is_regular_file(
             temporary.path / "p/modules/draft_module.am") &&
             read_text(temporary.path / "p/modules/draft_module.am") ==
                 "package draft_module\n",
         "new module source should be persisted as a regular file");
  expect(tabs.modified(0U) && tabs.modified(old_module_index),
         "creation must preserve dirty sheet and module buffers");
  expect(read_text(temporary.path / "p/lib/main.am") == old_module_source,
         "creating a module must not save a dirty existing module");

  const auto after_structure = amber::notebook::load_project(project.directory);
  expect(after_structure.document.active_sheet == "main" &&
             after_structure.document.sheets.size() == 3U &&
             after_structure.document.sheets.back().id == "draft" &&
             after_structure.document.sheets.back().title == "Draft sheet" &&
             after_structure.document.modules.size() == 2U &&
             after_structure.document.modules.back().id == "draft_module" &&
             after_structure.document.sheets[0].cells[0].source == "value()\n",
         "structure saves must merge with the central disk baseline only");

  // Saving either dirty tab after the structure commands must retain the
  // newly-created members, proving that the centralized baseline advanced.
  select_tab(&tabs, find_sheet_tab(tabs, "main"));
  tabs.save_active();
  select_tab(&tabs, find_module_tab(tabs, "main"));
  tabs.save_active();
  const auto after_member_saves = amber::notebook::load_project(project.directory);
  expect(after_member_saves.document.sheets.back().id == "draft" &&
             after_member_saves.document.modules.back().id == "draft_module" &&
             after_member_saves.document.sheets[0].cells[0].source ==
                 "dirty sheet buffer\n" &&
             read_text(temporary.path / "p/lib/main.am").find("  8") !=
                 std::string::npos &&
             tabs.modified_count() == 0U,
         "later member saves must preserve structural additions and clear dirtiness");
}

void test_structure_commands_reject_invalid_collisions_and_external_changes() {
  TempDirectory temporary;
  amber::notebook::LoadedProject project = make_project(temporary.path / "p");
  ProjectTabs tabs(project);
  const std::size_t initial_count = tabs.tabs().size();
  const std::vector<std::string> invalid_ids = {"", "1starts_with_digit",
                                                 "has-dash", "has/slash",
                                                 "\xC3\xA9"};
  for (const std::string &id : invalid_ids) {
    expect_throws([&] { tabs.create_sheet(id); },
                  "invalid sheet identifiers must be rejected");
    expect_throws([&] { tabs.create_module(id); },
                  "invalid module identifiers must be rejected");
  }
  expect(tabs.tabs().size() == initial_count,
         "invalid structure commands must not append tabs");

  tabs.create_sheet("shared");
  const std::size_t after_sheet = tabs.tabs().size();
  expect_throws([&] { tabs.create_sheet("shared"); },
                "duplicate sheet identifiers must be rejected");
  expect(tabs.tabs().size() == after_sheet,
         "duplicate sheet creation must not append a tab");
  tabs.create_module("shared");
  const std::size_t after_module = tabs.tabs().size();
  expect_throws([&] { tabs.create_module("shared"); },
                "duplicate module identifiers must be rejected");
  expect(tabs.tabs().size() == after_module,
         "duplicate module creation must not append a tab");
  expect_throws([&] { (void)tabs.set_auto_import("missing", true); },
                "auto-import must validate that a module exists");

  TempDirectory external_manifest_directory;
  amber::notebook::LoadedProject external_project =
      make_project(external_manifest_directory.path / "p");
  ProjectTabs external_tabs(external_project);
  amber::notebook::LoadedProject writer =
      amber::notebook::load_project(external_project.directory);
  writer.document.title = "external manifest edit";
  amber::notebook::save_project(&writer, writer.document);
  const std::size_t before_manifest_conflict = external_tabs.tabs().size();
  const std::size_t selected_before_manifest_conflict = external_tabs.selected();
  expect_throws([&] { external_tabs.create_sheet("lost"); },
                "external manifest changes must reject structure creation");
  expect(external_tabs.tabs().size() == before_manifest_conflict &&
             external_tabs.selected() == selected_before_manifest_conflict,
         "manifest conflict must not append or select a new tab");

  TempDirectory external_module_directory;
  amber::notebook::LoadedProject module_project =
      make_project(external_module_directory.path / "p");
  ProjectTabs module_tabs(module_project);
  write_text(external_module_directory.path / "p/modules/external.am",
             "external module owner\n");
  const std::size_t before_module_conflict = module_tabs.tabs().size();
  expect_throws([&] { module_tabs.create_module("external"); },
                "an externally-created module source must not be overwritten");
  expect(module_tabs.tabs().size() == before_module_conflict &&
             read_text(external_module_directory.path / "p/modules/external.am") ==
                 "external module owner\n",
         "module source conflict must not append a tab or clobber the file");
}

void test_auto_import_commands_are_ordered_and_apply_is_explicit() {
  TempDirectory temporary;
  amber::notebook::LoadedProject project = make_project(temporary.path / "p");
  ProjectTabs tabs(project);
  expect(tabs.apply_active(true, false),
         "the initially auto-imported module should evaluate the sheet");
  const std::uint64_t generation_before_toggle = tabs.environment_generation();
  SessionBackend *const old_backend = tabs.active().session.backend.get();
  const std::string old_result = tabs.active().session.cells[0].result;
  const std::size_t other_index = find_sheet_tab(tabs, "other");

  expect(!tabs.set_auto_import("main", true),
         "enabling an already-enabled import should be idempotent");
  expect(tabs.project().document.auto_imports == std::vector<std::string>{"main"},
         "idempotent import changes must preserve ordering");
  expect(tabs.set_auto_import("main", false),
         "disabling an enabled import should report a change");
  expect(tabs.project().document.auto_imports.empty() &&
             amber::notebook::load_project(project.directory)
                     .document.auto_imports.empty(),
         "auto-import structure changes should be persisted immediately");
  expect(tabs.environment_generation() == generation_before_toggle &&
             tabs.active().session.backend.get() == old_backend &&
             tabs.active().session.cells[0].result == old_result &&
             !tabs.tabs()[other_index]->session.environment_stale,
         "changing auto-imports must not rebuild or stale existing runtimes");
  expect(!tabs.apply_active(true, false),
         "an explicit Apply should report failure after removing value's import");
  expect(tabs.environment_generation() == generation_before_toggle &&
             tabs.active().session.backend.get() == old_backend &&
             tabs.active().session.cells[0].result == old_result &&
             !tabs.tabs()[other_index]->session.environment_stale,
         "failed Apply must preserve the previous runtime and results");

  tabs.create_module("helper");
  expect(!tabs.active().module->id.empty() &&
             std::find(tabs.project().document.auto_imports.begin(),
                       tabs.project().document.auto_imports.end(), "helper") ==
                 tabs.project().document.auto_imports.end(),
         "new modules must start with auto-import disabled");
  tabs.active().session.cells[0].source =
      "package helper\ndef helper_value():\n  7\nexport helper_value\n";
  tabs.active().session.document_dirty = true;
  tabs.save_active();
  expect(tabs.set_auto_import("helper", true) &&
             tabs.project().document.auto_imports ==
                 std::vector<std::string>{"helper"},
         "enabling a new module should append it in order");
  expect(tabs.environment_generation() == generation_before_toggle,
         "auto-import structure edits must not advance generation");

  select_tab(&tabs, find_sheet_tab(tabs, "main"));
  tabs.active().session.cells[0].source = "helper_value()\n";
  tabs.active().session.document_dirty = true;
  expect(tabs.apply_active(true, false),
         "explicit Apply should publish an enabled module export");
  expect(tabs.active().session.cells[0].result == "7" &&
             tabs.environment_generation() == generation_before_toggle + 1U &&
             tabs.tabs()[other_index]->session.environment_stale,
         "Apply should export the function, advance generation, and stale other sheets");

  expect(tabs.set_auto_import("main", true) &&
             tabs.project().document.auto_imports ==
                 std::vector<std::string>{"helper", "main"},
         "enabling another module should append rather than sort imports");
  expect(!tabs.set_auto_import("main", true),
         "re-enabling an import should be idempotent");
  expect(tabs.set_auto_import("helper", false) &&
             tabs.project().document.auto_imports ==
                 std::vector<std::string>{"main"},
         "disabling an import should remove only that ordered entry");
  expect(!tabs.set_auto_import("helper", false),
         "disabling an already-disabled import should be idempotent");
  expect(tabs.set_auto_import("helper", true) &&
             tabs.project().document.auto_imports ==
                 std::vector<std::string>{"main", "helper"},
         "re-enabling an import should append it at the end");
}

} // namespace

int main() {
  test_constructs_all_tabs_without_execution_and_keeps_selection_identity();
  test_opaque_cell_does_not_mark_a_clean_tab_modified();
  test_save_active_only_persists_selected_sheet_and_modified_count_is_exact();
  test_module_save_apply_generation_stale_barrier_and_failure_rollback();
  test_modified_module_blocks_apply_and_activity_closes_before_sessions();
  test_sheet_and_module_conflicts_preserve_editor_buffers();
  test_create_sheet_and_module_preserve_runtime_and_dirty_buffers();
  test_structure_commands_reject_invalid_collisions_and_external_changes();
  test_auto_import_commands_are_ordered_and_apply_is_explicit();
  std::cout << "iamber tabs tests ok\n";
}
