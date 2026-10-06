#include "tools/iamber/project_session.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <unistd.h>

namespace {

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << "\n";
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
    char pattern[] = "/tmp/amber_iamber_project_XXXXXX";
    const char *created = ::mkdtemp(pattern);
    expect(created != nullptr, "mkdtemp should create an isolated fixture");
    path = created;
  }

  ~TempDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }

  std::filesystem::path path;
};

std::string read_text(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  expect(input.good(), "project fixture should be readable");
  std::ostringstream contents;
  contents << input.rdbuf();
  expect(input.good() || input.eof(), "project fixture should be read");
  return contents.str();
}

void write_text(const std::filesystem::path &path, const std::string &text) {
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  expect(!error, "project fixture parent should be creatable");
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  expect(output.good(), "project fixture should be writable");
  output.write(text.data(), static_cast<std::streamsize>(text.size()));
  expect(output.good(), "project fixture should be written");
}

amber::notebook::ProjectDocument project_document() {
  using amber::notebook::CellMode;
  using amber::notebook::ProjectCell;
  using amber::notebook::ProjectDocument;
  using amber::notebook::ProjectSheet;

  ProjectDocument document;
  document.title = "Notebook \xCE\x94";
  document.active_sheet = "main";
  document.extra["ui"] = R"({"theme":"dark","zoom":1.2500})";

  ProjectSheet main;
  main.id = "main";
  main.title = "Main sheet";
  main.extra["layout"] = R"({"split":true,"ratio":0.5000})";

  ProjectCell first;
  first.id = amber::notebook::allocate_cell_id();
  first.source = "x = 1\n";
  first.mode = CellMode::Watch;
  first.extra["editor"] = R"({"cursor":2,"folded":[1,3]})";
  main.cells.push_back(first);

  ProjectCell second;
  second.id = amber::notebook::allocate_cell_id();
  second.source = "x + 1\n";
  second.mode = CellMode::Manual;
  second.extra["metadata"] = R"({"color":"amber"})";
  main.cells.push_back(second);
  document.sheets.push_back(main);

  ProjectSheet unrelated;
  unrelated.id = "scratch";
  unrelated.title = "Scratch";
  unrelated.extra["native"] = R"({"collapsed":false})";
  ProjectCell scratch;
  scratch.id = amber::notebook::allocate_cell_id();
  scratch.source = "99\n";
  scratch.mode = CellMode::Watch;
  unrelated.cells.push_back(scratch);
  document.sheets.push_back(unrelated);
  return document;
}

void expect_documents_equal(const amber::notebook::ProjectDocument &left,
                            const amber::notebook::ProjectDocument &right,
                            const std::string &message) {
  using amber::notebook::ProjectCell;
  expect(left.title == right.title, message + ": title");
  expect(left.active_sheet == right.active_sheet, message + ": active sheet");
  expect(left.extra == right.extra, message + ": document metadata");
  expect(left.auto_imports == right.auto_imports, message + ": imports");
  expect(left.modules.size() == right.modules.size(), message + ": modules");
  for (std::size_t index = 0; index < left.modules.size(); ++index) {
    expect(left.modules[index].id == right.modules[index].id,
           message + ": module id");
    expect(left.modules[index].path == right.modules[index].path,
           message + ": module path");
    expect(left.modules[index].extra == right.modules[index].extra,
           message + ": module metadata");
  }
  expect(left.sheets.size() == right.sheets.size(), message + ": sheets");
  for (std::size_t sheet_index = 0; sheet_index < left.sheets.size();
       ++sheet_index) {
    const auto &left_sheet = left.sheets[sheet_index];
    const auto &right_sheet = right.sheets[sheet_index];
    expect(left_sheet.id == right_sheet.id, message + ": sheet id");
    expect(left_sheet.title == right_sheet.title, message + ": sheet title");
    expect(left_sheet.extra == right_sheet.extra, message + ": sheet metadata");
    expect(left_sheet.cells.size() == right_sheet.cells.size(),
           message + ": cells");
    for (std::size_t cell_index = 0; cell_index < left_sheet.cells.size();
         ++cell_index) {
      const ProjectCell &left_cell = left_sheet.cells[cell_index];
      const ProjectCell &right_cell = right_sheet.cells[cell_index];
      expect(left_cell.id == right_cell.id, message + ": cell id");
      expect(left_cell.kind == right_cell.kind, message + ": cell kind");
      expect(left_cell.source == right_cell.source, message + ": cell source");
      expect(left_cell.formatting == right_cell.formatting,
             message + ": cell formatting");
      expect(left_cell.mode == right_cell.mode, message + ": cell mode");
      expect(left_cell.extra == right_cell.extra, message + ": cell metadata");
    }
  }
}

void test_load_is_document_only_and_roundtrip_preserves_unrelated_sheets() {
  const amber::notebook::ProjectDocument original = project_document();
  amber::notebook::LoadedProject project;
  project.document = original;

  Session session;
  load_project_into_session(&session, project);
  expect(session.backend == nullptr, "project load must not create a backend");
  expect(session.project_sheet_id == "main", "active sheet should be loaded");
  expect(session.cells.size() == original.sheets[0].cells.size(),
         "active sheet cells should be loaded");

  const amber::notebook::CellId allocated = amber::notebook::allocate_cell_id();
  expect(allocated > original.sheets[1].cells[0].id,
         "load should reserve IDs from unrelated sheets too");

  // Evaluation state is deliberately richer than the project schema. None of
  // it may leak into the document conversion.
  session.cells[0].result = "runtime-only result";
  session.cells[0].error = "runtime-only error";
  session.cells[0].ok = true;
  session.cells[0].dirty = true;
  session.cells[0].cursor = session.cells[0].source.size();
  session.document_dirty = true;
  const auto roundtrip = project_document_from_session(session, project);
  expect_documents_equal(original, roundtrip, "session/document roundtrip");
}

void test_code_edit_reorder_delete_and_new_ids_are_persisted() {
  amber::notebook::ProjectDocument original = project_document();
  original.sheets[1].cells.clear();
  Session session;
  amber::notebook::LoadedProject project;
  project.document = original;
  load_project_into_session(&session, project);

  const auto deleted_id = session.cells[1].id;
  const auto kept_id = session.cells[0].id;
  session.cells[0].source = "x = 42\n";
  session.cells[0].watch = false;

  Cell added;
  added.id = amber::notebook::allocate_cell_id();
  added.kind = "code";
  added.source = "new_value = 7\n";
  added.watch = true;

  std::vector<Cell> reordered;
  reordered.push_back(added);
  reordered.push_back(session.cells[0]);
  session.cells = std::move(reordered);

  const auto edited = project_document_from_session(session, project);
  const auto &cells = edited.sheets[0].cells;
  expect(cells.size() == 2U, "deleted code cell should stay deleted");
  expect(cells[0].id == added.id && cells[0].source == added.source &&
             cells[0].mode == amber::notebook::CellMode::Watch,
         "new code cell should retain its allocated ID and mode");
  expect(cells[1].id == kept_id && cells[1].source == "x = 42\n" &&
             cells[1].mode == amber::notebook::CellMode::Manual,
         "edited code cell should retain identity and changed source/mode");
  expect(cells[0].id != deleted_id && cells[1].id != deleted_id,
         "deleted cell ID must not reappear in the saved order");
  amber::notebook::validate_project_document(edited);
}

void test_opaque_cells_are_read_only_and_block_evaluation() {
  amber::notebook::ProjectDocument original = project_document();
  amber::notebook::ProjectCell opaque;
  opaque.id = amber::notebook::allocate_cell_id();
  opaque.kind = "markdown";
  opaque.extra["source"] = R"({"markdown":"# Keep me"})";
  opaque.extra["native"] = R"({"number":9007199254740993})";
  original.sheets[0].cells.push_back(opaque);

  amber::notebook::LoadedProject project;
  project.document = original;
  Session session;
  load_project_into_session(&session, project);
  expect(session.cells.back().kind == "markdown" &&
             session.cells.back().result.find("read-only markdown") !=
                 std::string::npos &&
             !session.cells.back().dirty,
         "opaque cell should load as a read-only placeholder");

  evaluate_from(&session, 0, true, false, false);
  expect(session.backend == nullptr &&
             session.status.find("evaluation blocked") != std::string::npos,
         "evaluation must be blocked when an opaque cell is present");

  session.cells.back().kind = "code";
  expect_throws([&] { (void)project_document_from_session(session, project); },
                "changing an opaque cell kind must be rejected");

  load_project_into_session(&session, project);
  session.cells.pop_back();
  expect_throws([&] { (void)project_document_from_session(session, project); },
                "discarding an opaque cell must be rejected");

  load_project_into_session(&session, project);
  const auto roundtrip = project_document_from_session(session, project);
  expect_documents_equal(original, roundtrip, "opaque-cell metadata roundtrip");
}

void test_text_cells_roundtrip_and_project_edits() {
  amber::notebook::ProjectDocument original = project_document();
  amber::notebook::ProjectCell text;
  text.id = amber::notebook::allocate_cell_id();
  text.kind = "text";
  text.source = "🙂note";
  text.formatting =
      R"({"version":1,"runs":[{"start":0,"length":2,"bold":true}]})";
  text.extra["native"] = R"({"selection":2})";
  original.sheets[0].cells.push_back(text);

  amber::notebook::LoadedProject project;
  project.document = original;
  Session session;
  load_project_into_session(&session, project);
  expect(session.cells.back().kind == "text" &&
             session.cells.back().source == text.source &&
             session.cells.back().formatting == text.formatting &&
             !session.cells.back().dirty,
         "text cells should load as inert read-only session cells");

  session.cells.back().source = "edited 🙂";
  session.cells.back().formatting =
      R"({"version":1,"runs":[{"start":0,"length":6,"italic":true}]})";
  auto edited = project_document_from_session(session, project);
  expect(edited.sheets[0].cells.back().source == "edited 🙂" &&
             edited.sheets[0].cells.back().formatting.find("italic") !=
                 std::string::npos &&
             edited.sheets[0].cells.back().extra == text.extra,
         "text source/formatting edits should persist with unknown metadata");

  session.cells.pop_back();
  edited = project_document_from_session(session, project);
  expect(edited.sheets[0].cells.size() + 1U ==
             original.sheets[0].cells.size(),
         "text deletion should be representable in project sessions");

  Cell added;
  added.id = amber::notebook::allocate_cell_id();
  added.kind = "text";
  added.source = "new note";
  added.formatting = R"({"version":1,"runs":[]})";
  session.cells.push_back(added);
  edited = project_document_from_session(session, project);
  expect(edited.sheets[0].cells.back().kind == "text" &&
             edited.sheets[0].cells.back().source == added.source,
         "new text cells should be serializable by the shared session layer");
}

void test_mixed_text_execution_and_diagnostic_identity() {
  amber::notebook::LoadedProject project;
  project.document = project_document();
  auto &cells = project.document.sheets.front().cells;
  cells[0].source = "x = 21\nx\n";
  cells[1].source = "x * 2\n";
  amber::notebook::ProjectCell note;
  note.id = amber::notebook::allocate_cell_id();
  note.kind = "text";
  note.source = "x = 999\nclass ThisIsNotCode:\n  ((\n";
  cells.insert(cells.begin(), note);
  note.id = amber::notebook::allocate_cell_id();
  cells.insert(cells.begin() + 2, note);
  Session session;
  load_project_into_session(&session, project);
  evaluate_from(&session, 0, true, false, false);
  expect(session.cells[1].ok && session.cells[1].result == "21" &&
             session.cells[3].ok && session.cells[3].result == "42",
         "leading and interspersed text must not compile or hide code outputs");
  const auto *backend = session.backend.get();
  const auto sources = notebook_sources(session.cells);
  expect(sources.size() == 2, "the dependency graph must exclude inert text");
  evaluate_from(&session, 0, true, false, false);
  expect(session.backend.get() == backend && session.cells[3].result == "42",
         "Run All from leading text must reuse the backend and run Manual code");
  session.cells[0].source = "a different note\nwith more lines\n";
  std::swap(session.cells[0], session.cells[1]);
  const auto after_move = notebook_sources(session.cells);
  expect(after_move.size() == sources.size(), "moving text cannot add graph inputs");
  for (std::size_t i = 0; i < sources.size(); ++i) {
    expect(after_move[i].id == sources[i].id && after_move[i].source == sources[i].source &&
               after_move[i].file == sources[i].file && after_move[i].mode == sources[i].mode,
           "text edits/moves must leave exact compiler inputs stable");
  }
  expect(pump_runtime_events_detailed(&session, false).disposition !=
             RuntimeEventPumpDisposition::DeferredByEdits,
         "text edits must not suspend runtime event processing");
  evaluate_from(&session, 3, false, false, false);
  expect(session.backend.get() == backend && session.cells[3].result == "42",
         "selected Manual code must run by CellId, not filtered source position");

  // A compiler failure after leading/interspersed notes belongs to the code
  // cell, never to the text that occupied its filtered compiler index.
  load_project_into_session(&session, project);
  session.cells[3].source = "broken = (\n";
  evaluate_from(&session, 3, false, false, false);
  expect(!session.cells[3].ok && !session.cells[3].error.empty() &&
             !session.cells[3].error_ranges.empty(),
         "mixed-sheet diagnostics must target the failing display code cell");
  expect(session.cells[0].error.empty() && session.cells[2].error.empty() &&
             session.cells[0].error_ranges.empty() && session.cells[2].error_ranges.empty(),
         "compiler ranges must never land on an inert text block");
}

struct BundledModuleFixture {
  std::string id;
  std::string path;
  std::string source;
};

amber::notebook::LoadedProject make_bundled_project(
    const std::filesystem::path &directory,
    const std::vector<BundledModuleFixture> &modules,
    const std::vector<std::string> &auto_imports) {
  // create_project intentionally accepts only a manifest with no members.
  // Add the files first, then publish the module manifest through save_project.
  amber::notebook::ProjectDocument empty = project_document();
  empty.modules.clear();
  empty.auto_imports.clear();
  amber::notebook::LoadedProject project =
      amber::notebook::create_project(directory, empty);
  amber::notebook::ProjectDocument manifest = project.document;
  for (const BundledModuleFixture &module : modules) {
    write_text(directory / module.path, module.source);
    manifest.modules.push_back({module.id, module.path, {}});
  }
  manifest.auto_imports = auto_imports;
  amber::notebook::save_project(&project, manifest);
  return project;
}

void test_text_edits_preserve_bundled_environment() {
  TempDirectory temporary;
  auto project = make_bundled_project(temporary.path / "text-environment",
      {{"maths", "lib/maths.am", "package maths\ndef twice(x):\n  x * 2\nexport twice\n"}},
      {"maths"});
  auto &cells = project.document.sheets.front().cells;
  cells[0].source = "x = 21\nx\n";
  cells[1].source = "twice(x)\n";
  amber::notebook::ProjectCell note;
  note.id = amber::notebook::allocate_cell_id();
  note.kind = "text";
  note.source = "A bundled notebook";
  cells.insert(cells.begin(), note);
  amber::notebook::save_project(&project, project.document);
  Session session;
  load_project_into_session(&session, project);
  expect(apply_project_environment(&session, project, true, false, {}),
         "Apply must support a leading text block");
  expect(session.cells[2].ok && session.cells[2].result == "42",
         "bundled exports must remain available past inert text");
  const auto *backend = session.backend.get();
  session.cells[0].source = "Edited note";
  std::swap(session.cells[0], session.cells[1]);
  evaluate_from(&session, 0, true, false, false);
  expect(session.backend.get() == backend && session.cells[2].result == "42",
         "text-only edits/moves must not rebuild or reinitialize bundled code");
}

void test_bundled_environment_runs_exports_and_reachable_dependencies() {
  TempDirectory temporary;
  const std::string models = "package models\n"
                             "offset = 40\n"
                             "\n"
                             "class Box:\n"
                             "  def init(@value):\n"
                             "    @value\n"
                             "\n"
                             "def add(delta):\n"
                             "  offset + delta\n"
                             "\n"
                             "export Box, add\n";
  const auto project = make_bundled_project(
      temporary.path / "bundled-happy",
      {{"models", "lib/models.am", models}}, {"models"});
  Session session;
  load_project_into_session(&session, project);
  session.cells[0].source = "answer = add(2)\n";
  session.cells[0].watch = true;
  session.cells[1].source = "Box(7)\n";
  session.cells[1].watch = true;

  expect(apply_project_environment(&session, project, true, false),
         "bundled exports should apply to a project sheet");
  expect(session.backend != nullptr && session.cells[0].ok &&
             session.cells[0].result == "42" && session.cells[1].ok &&
             session.cells[1].result == "<instance models.Box>",
         "a bundled class and captured function should run from notebook cells");

  TempDirectory dependency_temporary;
  const std::string util = "package util\n"
                           "def plus_one(value):\n"
                           "  value + 1\n"
                           "export plus_one\n";
  const std::string app = "package app\n"
                          "from util import plus_one\n"
                          "def answer(value):\n"
                          "  plus_one(value)\n"
                          "export answer\n";
  const auto dependency_project = make_bundled_project(
      dependency_temporary.path / "bundled-dependency",
      {{"util", "lib/util.am", util}, {"app", "lib/app.am", app}},
      {"app"});
  Session dependency_session;
  load_project_into_session(&dependency_session, dependency_project);
  dependency_session.cells[0].source = "answer(41)\n";
  dependency_session.cells[0].watch = true;
  dependency_session.cells[1].source = "0\n";
  dependency_session.cells[1].watch = true;
  expect(apply_project_environment(&dependency_session, dependency_project,
                                    true, false),
         "an auto-import root should pull in a reachable non-autoimport module");
  expect(dependency_session.cells[0].ok &&
             dependency_session.cells[0].result == "42",
         "a notebook should call an export from a root's bundled dependency");
}

void test_bundled_environment_failures_preserve_previous_backend_and_results() {
  TempDirectory temporary;
  const std::string good = "package models\n"
                           "offset = 40\n"
                           "def add(delta):\n"
                           "  offset + delta\n"
                           "export add\n";
  const auto project = make_bundled_project(
      temporary.path / "bundled-failures",
      {{"models", "lib/models.am", good}}, {"models"});
  Session session;
  load_project_into_session(&session, project);
  session.cells[0].source = "x = add(2)\n";
  session.cells[0].watch = true;
  const std::string previous_result = "42";
  expect(apply_project_environment(&session, project, true, false) &&
             session.backend != nullptr && session.cells[0].ok &&
             session.cells[0].result == previous_result,
         "a valid environment should establish the baseline backend");
  SessionBackend *const previous_backend = session.backend.get();

  const std::vector<BundledModuleSource> duplicate_exports{
      {"left", "lib/left.am", "package left\nclass A:\nexport A\n"},
      {"right", "lib/right.am", "package right\nclass A:\nexport A\n"}};
  expect(!apply_bundled_environment(&session, duplicate_exports, true, false) &&
             session.backend.get() == previous_backend && session.cells[0].ok &&
             session.cells[0].result == previous_result,
         "duplicate exports must preserve the active backend and results");

  const std::vector<BundledModuleSource> compile_failure{
      {"models", "lib/models.am", "package models\ndef add(value):\n  (\n"
                                    "export add\n"}};
  expect(!apply_bundled_environment(&session, compile_failure, true, false) &&
             session.backend.get() == previous_backend && session.cells[0].ok &&
             session.cells[0].result == previous_result,
         "module compile failure must preserve the active backend and results");

  const std::vector<BundledModuleSource> init_failure{
      {"models", "lib/models.am", "package models\n"
                                    "offset = 99\n"
                                    "def add(value):\n"
                                    "  offset + value\n"
                                    "export add\n"
                                    "raise \"init failed\"\n"}};
  expect(!apply_bundled_environment(&session, init_failure, true, false) &&
             session.backend.get() == previous_backend && session.cells[0].ok &&
             session.cells[0].result == previous_result,
         "module init failure must preserve the active backend and results");
}

void test_bundled_aliases_and_same_named_classes_share_one_world() {
  const std::vector<BundledModuleSource> modules{
      {"left", "left.am", "offset = 40\n"
                             "class Shared:\n"
                             "  def init(@v)\n"
                             "  def value(): @v\n"
                             "def add(v): offset + v\n"
                             "export Shared as Left, add as bump\n"},
      {"right", "right.am", "class Shared:\n"
                               "  def init(@v)\n"
                               "  def value(): @v + 100\n"
                               "export Shared as Right\n"},
      {"app", "app.am", "from left import Left as Parent, bump as plus\n"
                           "class Derived < Parent\n"
                           "def via_alias(v): plus(v)\n"
                           "export Derived, via_alias\n"}};
  Session session;
  Cell first;
  first.source = "l = Left(7)\nr = Right(9)\nc = Derived(3)\nvia_alias(1)\n";
  first.watch = true;
  session.cells.push_back(first);
  Cell second;
  second.source = "l.value() + r.value() + c.value()\n";
  second.watch = true;
  session.cells.push_back(second);
  expect(apply_bundled_environment(&session, modules, true, false) &&
             session.cells[0].ok && session.cells[0].result == "41" &&
             session.cells[1].ok && session.cells[1].result == "119",
         "aliases, captured functions and imported superclasses must retain "
         "their provider identity across cells: " + session.environment_error);
}

void test_session_execution_rejects_reentrant_replacement() {
  TempDirectory temporary;
  const auto project = amber::notebook::create_project(
      temporary.path / "reentry", project_document());
  Session session;
  load_project_into_session(&session, project);
  unsigned callbacks = 0;
  const EvaluationProgress check = [&](Session *running, std::size_t) {
    ++callbacks;
    SessionBackend *const active = running->backend.get();
    expect(running->evaluation_active, "execution guard must cover progress");
    expect(!apply_bundled_environment(running, {}, true, false),
           "recursive Apply must defer while a plan is active");
    evaluate_from(running, 0U, true, false, false);
    expect(pump_runtime_events_detailed(running, false).disposition ==
               RuntimeEventPumpDisposition::Busy,
           "a progress callback must not recursively drain the world");
    expect_throws([&] { load_project_into_session(running, project); },
                  "project load must not destroy an active backend");
    expect(running->backend.get() == active,
           "reentrant calls must retain the active backend");
  };
  evaluate_from_with_progress(&session, 0U, true, true, check);
  expect(callbacks == 2U && !session.evaluation_active &&
             session.cells[0].ok && session.cells[1].result == "2",
         "a guarded initial source plan should finish normally");
  session.cells[0].source = "x = 2\n";
  evaluate_from_with_progress(&session, 0U, true, true, check);
  expect(callbacks == 4U && session.cells[1].result == "3",
         "source replacement must retain the same reentrancy barrier");
  expect(apply_bundled_environment(&session, {}, true, true, check) &&
             callbacks == 6U && session.cells[1].result == "3",
         "Apply must guard its newly published world during automatic runs");

  SessionBackend *const previous = session.backend.get();
  expect(apply_bundled_environment(
             &session, {}, true, true,
             [](Session *, std::size_t) { throw std::runtime_error("host stopped"); }),
         "module initialization commits before automatic cell execution");
  expect(session.backend.get() != previous && !session.evaluation_active &&
             !session.environment_apply_active && !session.cells[0].running &&
             session.cells[0].dirty && !session.cells[0].ok &&
             session.status.find("evaluation interrupted: host stopped") !=
                 std::string::npos,
         "host cancellation after Apply must leave an explicit recoverable state");
  evaluate_from(&session, 0U, true, false, false);
  expect(session.cells[0].ok && session.cells[1].result == "3",
         "an interrupted automatic plan must permit a later explicit run");

  session.cells[0].source = "x =\n";
  session.cells[0].dirty = true;
  evaluate_from(&session, 0U, true, false, false);
  expect(!session.cells[0].ok && !session.cells[0].dirty &&
             !session.cells[0].error.empty() && !session.evaluation_active &&
             pump_runtime_events_detailed(&session, false).disposition ==
                 RuntimeEventPumpDisposition::DeferredByEdits,
         "a malformed edit must expose current diagnostics while the runtime "
         "retains its uninstalled-source barrier");
}

void test_bundled_environment_reload_preserves_watch_manual_policy() {
  TempDirectory temporary;
  const std::string initial = "package values\n"
                              "def value(): 1\n"
                              "export value\n";
  const auto project = make_bundled_project(
      temporary.path / "bundled-reload",
      {{"values", "lib/values.am", initial}}, {"values"});
  Session session;
  load_project_into_session(&session, project);
  session.cells[0].source = "value()\n";
  session.cells[0].watch = true;
  session.cells[1].source = "manual = value()\n";
  session.cells[1].watch = false;
  Cell downstream;
  downstream.source = "manual + 1\n";
  downstream.watch = true;
  session.cells.push_back(downstream);
  expect(apply_project_environment(&session, project, true, false) &&
             session.cells[0].result == "1" && session.cells[1].result == "1" &&
             session.cells[2].result == "2",
         "initial environment should evaluate both cells with force_all");

  amber::notebook::LoadedProjectModule module =
      amber::notebook::load_project_module(project, "values");
  amber::notebook::save_project_module(
      project, &module, "package values\ndef value(): 2\nexport value\n");
  expect(apply_project_environment(&session, project, false, false),
         "a changed bundled module should reload successfully");
  expect(session.cells[0].ok && session.cells[0].result == "2" &&
             !session.cells[1].ok && session.cells[1].result == "error" &&
             session.cells[1].error.find("manual cell is stale") !=
                 std::string::npos && !session.cells[2].ok &&
             session.cells[2].error.find("stale manual dependency") !=
                 std::string::npos,
         "reload should rerun Watch cells while keeping Manual cells stale");

  SessionBackend *const applied = session.backend.get();
  evaluate_from(&session, 1U, false, false, false);
  expect(session.backend.get() == applied && session.cells[1].ok &&
             session.cells[1].result == "2" && session.cells[2].ok &&
             session.cells[2].result == "3",
         "an explicit Manual run must unblock Watch consumers in the applied world");

  expect(apply_project_environment(&session, project, true, false) &&
             session.cells[0].ok && session.cells[0].result == "2" &&
             session.cells[1].ok && session.cells[1].result == "2" &&
             session.cells[2].ok && session.cells[2].result == "3",
         "force_all should rerun the stale Manual cell after reload");
}

void test_explicit_empty_bundled_environment_is_runnable() {
  TempDirectory temporary;
  amber::notebook::ProjectDocument document = project_document();
  document.modules.clear();
  document.auto_imports.clear();
  amber::notebook::LoadedProject project =
      amber::notebook::create_project(temporary.path / "bundled-empty",
                                      document);
  Session session;
  load_project_into_session(&session, project);
  session.cells[0].source = "x = 7\n";
  session.cells[0].watch = true;
  expect(apply_project_environment(&session, project, true, false) &&
             session.backend != nullptr && session.cells[0].ok &&
             session.cells[0].result == "7",
         "an explicitly empty environment should still run notebook cells");
}

void test_invalid_sheet_or_document_keeps_old_session_and_mailbox() {
  Session session;
  Cell old;
  old.id = amber::notebook::allocate_cell_id();
  old.source = "old = 1\n";
  session.cells.push_back(old);
  session.project_sheet_id = "old-session";
  const auto waiter = session.activity_waiter();
  const auto notifier = session.activity_notifier();

  amber::notebook::LoadedProject project;
  project.document = project_document();
  (void)notifier.notify_input();
  expect_throws(
      [&] { load_project_into_session(&session, project, "missing"); },
      "an invalid --sheet selection must throw");
  expect(session.cells.size() == 1U && session.cells[0].source == old.source &&
             session.project_sheet_id == "old-session",
         "invalid sheet load must preserve the existing session");
  expect(waiter.wait(std::chrono::milliseconds(0)).input_ready,
         "invalid sheet load must not close the old activity mailbox");

  (void)notifier.notify_input();
  amber::notebook::LoadedProject malformed = project;
  malformed.document.sheets.clear();
  expect_throws([&] { load_project_into_session(&session, malformed); },
                "an invalid project document must throw");
  expect(session.cells.size() == 1U && session.cells[0].source == old.source,
         "invalid document load must preserve the old cells");
  expect(waiter.wait(std::chrono::milliseconds(0)).input_ready,
         "invalid document load must preserve the old mailbox");
  expect(notifier.notify_runtime(),
         "the old notifier must remain usable after rejected loads");
  expect(waiter.wait(std::chrono::milliseconds(0)).runtime_ready,
         "the old mailbox must still deliver runtime notifications");
}

void test_empty_sheet_stays_empty_on_noop_save() {
  TempDirectory temporary;
  amber::notebook::ProjectDocument empty;
  empty.title = "Empty";
  empty.active_sheet = "main";
  amber::notebook::ProjectSheet sheet;
  sheet.id = "main";
  sheet.title = "Main";
  empty.sheets.push_back(sheet);

  amber::notebook::LoadedProject project =
      amber::notebook::create_project(temporary.path / "empty", empty);
  Session session;
  load_project_into_session(&session, project);
  expect(session.cells.empty(), "an empty project sheet must remain empty");
  expect(!project_session_modified(session, project),
         "a no-op empty sheet should not be considered modified");
  save_project_session(&session, &project);
  expect(!session.document_dirty,
         "a successful no-op save clears document dirty");

  const auto reopened = amber::notebook::load_project(project.directory);
  expect(reopened.document.sheets.size() == 1U &&
             reopened.document.sheets[0].cells.empty(),
         "no-op save must not synthesize a replacement cell");
}

void test_modification_detection_ignores_runtime_state_but_tracks_document() {
  TempDirectory temporary;
  const auto directory = temporary.path / "modified";
  const auto original = project_document();
  amber::notebook::LoadedProject project =
      amber::notebook::create_project(directory, original);
  Session session;
  load_project_into_session(&session, project);
  expect(!project_session_modified(session, project),
         "fresh project session should be unmodified");

  session.cells[0].result = "evaluated result";
  session.cells[0].error = "stale diagnostic";
  session.cells[0].ok = true;
  session.cells[0].dirty = true;
  session.cells[0].cursor = 999U;
  session.document_dirty = true;
  expect(!project_session_modified(session, project),
         "runtime result/dirty/cursor must not count as document changes");
  save_project_session(&session, &project);
  expect(
      !session.document_dirty && !project_session_modified(session, project),
      "saving runtime-only state should leave the project semantically clean");

  session.cells[0].source = "x = 2\n";
  expect(project_session_modified(session, project),
         "source edits must count as meaningful project changes");
  session.cells[0].source = original.sheets[0].cells[0].source;
  session.cells[0].watch = false;
  expect(project_session_modified(session, project),
         "watch-mode edits must count as meaningful project changes");
  session.cells[0].watch = true;
  expect(!project_session_modified(session, project),
         "restoring source and mode should restore semantic equality");
}

void test_partial_utf8_edit_remains_discardable_without_touching_disk() {
  TempDirectory temporary;
  const auto original = project_document();
  amber::notebook::LoadedProject project =
      amber::notebook::create_project(temporary.path / "partial", original);
  Session session;
  load_project_into_session(&session, project);
  const std::string disk_before = read_text(project.directory / "project.json");

  session.cells[0].source = std::string(1, static_cast<char>(0xc3));
  expect(project_session_modified(session, project),
         "an unrepresentable editor source must remain discardable");
  expect(session.backend == nullptr,
         "project load and failed save must not create a runtime backend");
  expect_throws([&] { save_project_session(&session, &project); },
                "saving a partial UTF-8 editor source must fail safely");
  expect(read_text(project.directory / "project.json") == disk_before,
         "failed partial UTF-8 save must leave project.json unchanged");
  expect(project.baseline == disk_before,
         "failed partial UTF-8 save must leave the baseline unchanged");
}

amber::notebook::LoadedProject
make_module_project(const std::filesystem::path &directory,
                    const std::string &source) {
  amber::notebook::LoadedProject project =
      amber::notebook::create_project(directory, project_document());
  write_text(directory / "lib/core.am", source);
  amber::notebook::ProjectDocument document = project.document;
  document.modules.push_back(
      amber::notebook::ProjectModule{"core", "lib/core.am", {}});
  amber::notebook::save_project(&project, document);
  return project;
}

void test_module_load_is_one_buffer_no_backend_and_does_not_execute() {
  TempDirectory temporary;
  const std::string source = "raise \"module source must not run\"\n";
  amber::notebook::LoadedProject project =
      make_module_project(temporary.path / "module-load", source);
  const amber::notebook::LoadedProjectModule module =
      amber::notebook::load_project_module(project, "core");

  Session session;
  load_project_module_into_session(&session, project, module);
  expect(session.backend == nullptr,
         "module load must not create a persistent runtime backend");
  expect(session.module_editor && session.project_module_id == "core" &&
             session.project_module_path == "lib/core.am",
         "module load should identify the selected module");
  expect(session.project_label == project.document.title + "/module:core",
         "module load should expose a useful project/module label");
  expect(!session.auto_watch && session.cells.size() == 1U &&
             !session.cells.front().watch && !session.cells.front().dirty &&
             session.cells.front().source == source,
         "module load should create one clean, non-autowatch source buffer");
  expect(session.cells.front().result == "not evaluated" &&
             session.cells.front().error.empty() &&
             session.status.find("not run") != std::string::npos,
         "module load must not execute or report a source error");
}

void test_module_run_isolated_and_failed_edit_preserves_sheet_environment() {
  TempDirectory temporary;
  const std::string source = "class A:\n"
                             "  def init(@a):\n"
                             "    @a\n"
                             "export A\n"
                             "A(7)\n";
  amber::notebook::LoadedProject project =
      make_module_project(temporary.path / "module-run", source);
  const amber::notebook::LoadedProjectModule module =
      amber::notebook::load_project_module(project, "core");
  Session session;
  load_project_module_into_session(&session, project, module);

  evaluate_from(&session, 0, true, false, false);
  expect(session.backend == nullptr && session.cells.front().ok &&
             session.cells.front().result == "<instance A>",
         "explicit module evaluation should construct A(7) in isolation");
  expect(session.status.find("isolated runtime") != std::string::npos &&
             session.status.find("not installed") != std::string::npos,
         "module evaluation should describe its isolated sheet behavior");

  const std::string previous_label = session.project_label;
  const std::string previous_module_id = session.project_module_id;
  session.cells.front().source = "class A(\n";
  evaluate_from(&session, 0, true, false, false);
  expect(session.backend == nullptr && !session.cells.front().ok &&
             session.cells.front().result == "error",
         "a malformed module edit should fail without creating a backend");
  expect(session.status.find("previous sheet environment unchanged") !=
             std::string::npos,
         "a failed module run should preserve the prior sheet environment");
  expect(session.project_label == previous_label &&
             session.project_module_id == previous_module_id &&
             session.module_editor,
         "a failed module run should preserve module/session identity");
}

void test_bundled_module_manifest_id_is_compile_identity() {
  const CompileResult implicit =
      compile_source_text("value = 1\n", "lib/models.am", "models");
  expect(implicit.ok && implicit.module_name == "models",
         "a bundled source without package should compile under its manifest "
         "module id");

  const CompileResult matching = compile_source_text(
      "package models\nvalue = 1\n", "lib/models.am", "models");
  expect(matching.ok && matching.module_name == "models",
         "a matching source package should preserve the manifest identity");

  const CompileResult mismatch = compile_source_text(
      "package other\nvalue = 1\n", "lib/models.am", "models");
  expect(!mismatch.ok && mismatch.error.find("IAM1001") != std::string::npos &&
             mismatch.error.find("models") != std::string::npos &&
             mismatch.error.find("other") != std::string::npos &&
             !mismatch.error_ranges.empty() &&
             mismatch.error_ranges.front().start_line == 1 &&
             mismatch.error_ranges.front().start_column == 0,
         "a bundled source package mismatch should fail at the package "
         "directive before runtime creation");
}

void test_bundled_environment_preparation_is_nonexecuting_and_transactional() {
  const std::string class_a = "class A:\n"
                              "  def init(@value):\n"
                              "    @value\n"
                              "export A\n";
  const std::string class_b = "class B:\n"
                              "  def init():\n"
                              "    null\n"
                              "export B\n"
                              "raise \"must not execute while preparing\"\n";
  const BundledEnvironmentPreparation prepared =
      prepare_bundled_environment({{"models", "lib/models.am", class_a},
                                   {"views", "lib/views.am", class_b}});
  expect(prepared.ok && prepared.diagnostics.empty() &&
             prepared.modules.size() == 2U && prepared.exports.size() == 2U &&
             prepared.ambient_constant_paths.at("A") == "models.A" &&
             prepared.ambient_constant_paths.at("B") == "views.B",
         "environment preparation should compile ordered bundled modules and "
         "build qualified ambient paths without running module init");

  const BundledEnvironmentPreparation conflict =
      prepare_bundled_environment({{"left", "lib/left.am", class_a},
                                   {"right", "lib/right.am", class_a}});
  expect(!conflict.ok && !conflict.diagnostics.empty() &&
             conflict.diagnostics.back().message.find(
                 "ambiguous auto-import export 'A'") != std::string::npos &&
             conflict.ambient_constant_paths.empty(),
         "duplicate unqualified exports should reject the whole ambient map");

  const BundledEnvironmentPreparation mismatch = prepare_bundled_environment(
      {{"models", "lib/models.am", "package other\n" + class_a}});
  expect(!mismatch.ok && !mismatch.diagnostics.empty() &&
             mismatch.diagnostics.front().module_id == "models" &&
             mismatch.diagnostics.front().message.find("IAM1001") !=
                 std::string::npos &&
             mismatch.ambient_constant_paths.empty(),
         "a package/manifest mismatch should leave no publishable ambient "
         "environment");
}

void test_module_save_is_independent_from_run_and_tracks_conflicts() {
  TempDirectory temporary;
  const std::string runnable_source = "class A:\n"
                                      "  def init(@a):\n"
                                      "    @a\n"
                                      "export A\n"
                                      "A(7)\n";
  const std::filesystem::path directory = temporary.path / "module-save";
  amber::notebook::LoadedProject project =
      make_module_project(directory, runnable_source);
  amber::notebook::LoadedProjectModule module =
      amber::notebook::load_project_module(project, "core");
  Session session;
  load_project_module_into_session(&session, project, module);
  expect(!project_module_session_modified(session, module),
         "fresh module buffer should be semantically unmodified");

  evaluate_from(&session, 0, true, false, false);
  session.cells.front().result = "runtime-only display state";
  session.cells.front().error = "stale runtime diagnostic";
  session.cells.front().cursor = session.cells.front().source.size();
  session.cells.front().dirty = true;
  expect(
      !project_module_session_modified(session, module),
      "module evaluation results and dirtiness must not count as source edits");

  std::string arbitrary_source = std::string("partial\0source", 14);
  arbitrary_source.push_back(static_cast<char>(0xc3));
  arbitrary_source.push_back(static_cast<char>(0xff));
  session.cells.front().source = arbitrary_source;
  expect(project_module_session_modified(session, module),
         "module source edits must count as meaningful changes");
  save_project_module_session(&session, project, &module);
  expect(read_text(directory / "lib/core.am") == arbitrary_source &&
             module.source == arbitrary_source &&
             module.baseline == arbitrary_source &&
             !project_module_session_modified(session, module),
         "module save should persist arbitrary bytes and advance its baseline");
  expect(session.backend == nullptr,
         "module save must remain independent from its isolated run backend");

  const std::string external_source = "external module replacement\n";
  write_text(directory / "lib/core.am", external_source);
  session.cells.front().source = "local edit after conflict\n";
  const std::string module_source_before_conflict = module.source;
  const std::string module_baseline_before_conflict = module.baseline;
  expect_throws(
      [&] { save_project_module_session(&session, project, &module); },
      "external module edits should reject module-session save");
  expect(read_text(directory / "lib/core.am") == external_source &&
             module.source == module_source_before_conflict &&
             module.baseline == module_baseline_before_conflict,
         "module-session conflict must preserve disk and editor baseline");
}

void test_module_session_and_module_mapping_mismatches_are_rejected() {
  TempDirectory temporary;
  amber::notebook::LoadedProject project =
      make_module_project(temporary.path / "module-mismatch", "value = 1\n");
  amber::notebook::LoadedProjectModule module =
      amber::notebook::load_project_module(project, "core");
  Session session;
  load_project_module_into_session(&session, project, module);

  amber::notebook::LoadedProjectModule wrong_module = module;
  wrong_module.id = "other";
  expect_throws(
      [&] {
        load_project_module_into_session(&session, project, wrong_module);
      },
      "loading a module not present in the project should be rejected");
  expect(session.project_module_id == "core" && session.cells.size() == 1U,
         "rejected module replacement must preserve the old module session");
  expect_throws(
      [&] { save_project_module_session(&session, project, &wrong_module); },
      "saving a module with a mismatched session identity should be rejected");

  session.project_module_path = "lib/renamed.am";
  expect_throws(
      [&] { save_project_module_session(&session, project, &module); },
      "saving after a session/module path mismatch should be rejected");
}

void test_module_diagnostics_use_raw_source_coordinates() {
  TempDirectory temporary;
  const std::string syntax_error_source = "one = 1\n@\n";
  amber::notebook::LoadedProject syntax_project = make_module_project(
      temporary.path / "module-diagnostics-syntax", syntax_error_source);
  const amber::notebook::LoadedProjectModule syntax_module =
      amber::notebook::load_project_module(syntax_project, "core");
  Session syntax_session;
  load_project_module_into_session(&syntax_session, syntax_project,
                                   syntax_module);
  evaluate_from(&syntax_session, 0, true, false, false);
  expect(
      syntax_session.cells.size() == 1U && !syntax_session.cells.front().ok &&
          !syntax_session.cells.front().error_ranges.empty(),
      "module syntax errors should map to the sole source buffer at index 0");
  const CodeErrorRange &syntax_range =
      syntax_session.cells.front().error_ranges.front();
  expect(syntax_range.start_line == 1 && syntax_range.start_column == 1 &&
             syntax_range.end_line == 2 && syntax_range.end_column == 0,
         "module syntax diagnostics should have zero-based raw line/column");
  expect(
      !syntax_session.cells.front().errors.empty() &&
          syntax_session.cells.front().errors.front().has_range &&
          syntax_session.cells.front().errors.front().range.start_line == 1 &&
          syntax_session.cells.front().errors.front().range.start_column == 1,
      "module syntax diagnostic views should retain the mapped range");

  const std::string utf8_error_source = "x = \"é\" $\n";
  amber::notebook::LoadedProject utf8_project = make_module_project(
      temporary.path / "module-diagnostics-utf8", utf8_error_source);
  const amber::notebook::LoadedProjectModule utf8_module =
      amber::notebook::load_project_module(utf8_project, "core");
  Session utf8_session;
  load_project_module_into_session(&utf8_session, utf8_project, utf8_module);
  evaluate_from(&utf8_session, 0, true, false, false);
  expect(!utf8_session.cells.front().error_ranges.empty() &&
             utf8_session.cells.front().error_ranges.front().start_line == 0 &&
             utf8_session.cells.front().error_ranges.front().start_column == 8,
         "module diagnostics should count UTF-8 source columns as characters, "
         "not bytes");

  const std::string cr_error_source = "one = 1\r@";
  amber::notebook::LoadedProject cr_project = make_module_project(
      temporary.path / "module-diagnostics-cr", cr_error_source);
  const amber::notebook::LoadedProjectModule cr_module =
      amber::notebook::load_project_module(cr_project, "core");
  Session cr_session;
  load_project_module_into_session(&cr_session, cr_project, cr_module);
  evaluate_from(&cr_session, 0, true, false, false);
  expect(!cr_session.cells.front().error_ranges.empty() &&
             cr_session.cells.front().error_ranges.front().start_line == 1 &&
             cr_session.cells.front().error_ranges.front().start_column == 1,
         "module diagnostics should retain lexer coordinates for CR-only "
         "source");

  const std::string runtime_error_source = "first = 1\n"
                                           "raise \"boom\"\n";
  amber::notebook::LoadedProject runtime_project = make_module_project(
      temporary.path / "module-diagnostics-runtime", runtime_error_source);
  const amber::notebook::LoadedProjectModule runtime_module =
      amber::notebook::load_project_module(runtime_project, "core");
  Session runtime_session;
  load_project_module_into_session(&runtime_session, runtime_project,
                                   runtime_module);
  evaluate_from(&runtime_session, 0, true, false, false);
  expect(runtime_session.cells.size() == 1U &&
             !runtime_session.cells.front().ok &&
             runtime_session.cells.front().error.find("ModuleInitError") !=
                 std::string::npos &&
             !runtime_session.cells.front().error_ranges.empty(),
         "module init diagnostics should map to source buffer index 0");
  const CodeErrorRange &runtime_range =
      runtime_session.cells.front().error_ranges.front();
  expect(runtime_range.start_line == 1 && runtime_range.start_column == 0 &&
             runtime_range.end_line == 1 && runtime_range.end_column == 1,
         "module init diagnostics should use raw zero-based coordinates");
}

} // namespace

int main() {
  test_load_is_document_only_and_roundtrip_preserves_unrelated_sheets();
  test_code_edit_reorder_delete_and_new_ids_are_persisted();
  test_opaque_cells_are_read_only_and_block_evaluation();
  test_text_cells_roundtrip_and_project_edits();
  test_mixed_text_execution_and_diagnostic_identity();
  test_text_edits_preserve_bundled_environment();
  test_bundled_environment_runs_exports_and_reachable_dependencies();
  test_bundled_environment_failures_preserve_previous_backend_and_results();
  test_bundled_aliases_and_same_named_classes_share_one_world();
  test_session_execution_rejects_reentrant_replacement();
  test_bundled_environment_reload_preserves_watch_manual_policy();
  test_explicit_empty_bundled_environment_is_runnable();
  test_invalid_sheet_or_document_keeps_old_session_and_mailbox();
  test_empty_sheet_stays_empty_on_noop_save();
  test_modification_detection_ignores_runtime_state_but_tracks_document();
  test_partial_utf8_edit_remains_discardable_without_touching_disk();
  test_module_load_is_one_buffer_no_backend_and_does_not_execute();
  test_module_run_isolated_and_failed_edit_preserves_sheet_environment();
  test_bundled_module_manifest_id_is_compile_identity();
  test_bundled_environment_preparation_is_nonexecuting_and_transactional();
  test_module_save_is_independent_from_run_and_tracks_conflicts();
  test_module_session_and_module_mapping_mismatches_are_rejected();
  test_module_diagnostics_use_raw_source_coordinates();
  std::cout << "iamber project tests ok\n";
}
