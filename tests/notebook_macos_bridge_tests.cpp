#include "notebook/project.h"
#include "tools/notebook-macos/NotebookBridge.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unistd.h>

namespace {

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "notebook macOS bridge test failed: " << message << "\n";
    std::exit(1);
  }
}

class TempDirectory {
public:
  TempDirectory() {
    char pattern[] = "/tmp/amber_notebook_macos_bridge_XXXXXX";
    const char *created = ::mkdtemp(pattern);
    expect(created != nullptr, "temporary directory should be created");
    path = created;
  }
  ~TempDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }
  std::filesystem::path path;
};

std::string snapshot(AmberNotebook *notebook) {
  char *error = nullptr;
  char *value = amber_notebook_snapshot(notebook, &error);
  expect(error == nullptr, error == nullptr ? "snapshot should succeed" : error);
  expect(value != nullptr, "snapshot should return JSON");
  std::string result(value);
  amber_notebook_string_free(value);
  return result;
}

void command(AmberNotebook *notebook, const char *action, const char *tab = nullptr,
             const char *cell = nullptr, const char *text = nullptr) {
  char *error = nullptr;
  const int ok = amber_notebook_command(notebook, action, tab, cell, text,
                                        &error);
  if (!ok) {
    const std::string detail = error == nullptr ? "no error detail" : error;
    amber_notebook_string_free(error);
    expect(false, std::string("command ") + action + " should succeed: " +
                       detail);
  }
  expect(error == nullptr, "successful command should not allocate an error");
}

bool command_fails(AmberNotebook *notebook, const char *action,
                   const char *tab, const char *cell, const char *text,
                   std::string *message = nullptr) {
  char *error = nullptr;
  const int ok = amber_notebook_command(notebook, action, tab, cell, text,
                                        &error);
  if (ok) {
    amber_notebook_string_free(error);
    return false;
  }
  if (message != nullptr && error != nullptr)
    *message = error;
  amber_notebook_string_free(error);
  return true;
}

AmberNotebook *open_new(const std::filesystem::path &path) {
  char *error = nullptr;
  AmberNotebook *notebook =
      amber_notebook_open(path.c_str(), 1, "Bridge Test", &error);
  if (notebook == nullptr) {
    const std::string detail = error == nullptr ? "no error detail" : error;
    amber_notebook_string_free(error);
    expect(false, "new project should open: " + detail);
  }
  expect(error == nullptr, "successful create should not allocate an error");
  return notebook;
}

AmberNotebook *open_existing(const std::filesystem::path &path) {
  char *error = nullptr;
  AmberNotebook *notebook = amber_notebook_open(path.c_str(), 0, nullptr, &error);
  if (notebook == nullptr) {
    const std::string detail = error == nullptr ? "no error detail" : error;
    amber_notebook_string_free(error);
    expect(false, "existing project should open: " + detail);
  }
  expect(error == nullptr, "successful open should not allocate an error");
  return notebook;
}

std::string first_cell_id(const std::string &json, const std::string &tab_id) {
  const std::string tab_marker = "\"id\":\"" + tab_id + "\"";
  const std::size_t tab = json.find(tab_marker);
  expect(tab != std::string::npos, "requested tab should appear in snapshot");
  const std::size_t cells = json.find("\"cells\":[{\"id\":\"", tab);
  expect(cells != std::string::npos,
         "requested tab should contain a first cell in snapshot");
  const std::size_t begin = cells + std::string("\"cells\":[{\"id\":\"").size();
  const std::size_t end = json.find('"', begin);
  expect(end != std::string::npos, "cell ID should be quoted");
  return json.substr(begin, end - begin);
}

void test_container_result_formats() {
  TempDirectory temporary;
  auto *notebook = open_new(temporary.path / "result-viewer");
  const auto id = first_cell_id(snapshot(notebook), "sheet:main");
  command(notebook, "source", "sheet:main", id.c_str(), "{:loss: [0.2, 0.1], :device: \"mps\"}\n");
  command(notebook, "run_all", "sheet:main");
  const auto json = snapshot(notebook);
  expect(json.find("\"result_format\":{\"pretty\":\"{\\n  :loss: [\\n") != std::string::npos &&
         json.find("\"is_container\":true,\"truncated\":false,\"pretty_truncated\":false") != std::string::npos,
         "structured result metadata should survive the JSON bridge");
  amber_notebook_destroy(notebook);
}

void test_open_is_source_only_and_pump_idles() {
  TempDirectory temporary;
  const auto path = temporary.path / "empty-project";
  AmberNotebook *notebook = open_new(path);
  const std::string before = snapshot(notebook);
  expect(before.find("\"result\":\"not evaluated\"") != std::string::npos,
         "opening a project should leave code unevaluated");
  expect(before.find("\"environment_generation\":1") != std::string::npos,
         "snapshot should expose the initial environment generation");
  const int descriptor = amber_notebook_activity_descriptor(notebook, nullptr);
  expect(descriptor >= 0, "activity descriptor should be a borrowed POSIX fd");

  int delay = 42;
  char *error = nullptr;
  expect(amber_notebook_pump(notebook, &delay, &error) == 1,
         "idle pump should succeed");
  expect(error == nullptr && delay == -1,
         "pump should report no retry when every tab is idle");
  expect(amber_notebook_pump(notebook, &delay, &error) == 1 && delay == -1,
         "repeated idle pump should remain idle without a timer");
  expect(error == nullptr, "idle pump should not allocate an error");
  amber_notebook_destroy(notebook);
}

void test_empty_sheet_accepts_cell_creation_without_an_anchor() {
  TempDirectory temporary;
  const auto path = temporary.path / "empty-sheet-project";
  auto document = amber::notebook::make_project_document("Empty Sheet");
  document.sheets.front().cells.clear();
  (void)amber::notebook::create_project(path, document);
  AmberNotebook *notebook = open_existing(path);
  expect(snapshot(notebook).find("\"cells\":[]") != std::string::npos,
         "zero-cell sheet should load as an empty editor");
  command(notebook, "add_cell", "sheet:main", nullptr, nullptr);
  expect(snapshot(notebook).find("\"cells\":[{\"id\":\"") !=
             std::string::npos,
         "add_cell should accept an empty sheet without an anchor");
  command(notebook, "save", "sheet:main");
  amber_notebook_destroy(notebook);
  expect(amber::notebook::load_project(path).document.sheets.front().cells.size() ==
             1U,
         "new code cell should persist in a formerly empty sheet");
}

void test_interpolation_display_values() {
  TempDirectory temporary;
  AmberNotebook *notebook = open_new(temporary.path / "interpolation-project");
  const auto cell = first_cell_id(snapshot(notebook), "sheet:main");
  command(notebook, "source", "sheet:main", cell.c_str(),
          "amount = 42\ngreeting = \"Привет, Amber\"\namount\n");
  command(notebook, "run_all", "sheet:main");
  const auto state = snapshot(notebook);
  expect(state.find("\"text_value\":\"Привет, Amber\"") != std::string::npos,
         "interpolation string display must omit debug quotation marks");
  expect(state.find("\"text_value\":\"42\"") != std::string::npos,
         "interpolation preserves numeric display");
  expect(state.find("\"role\":\"module_cell\"") != std::string::npos,
         "notebook assignment providers must be identifiable in the snapshot");
  amber_notebook_destroy(notebook);
}

void test_source_watch_run_and_stable_ids_roundtrip() {
  TempDirectory temporary;
  const auto path = temporary.path / "run-project";
  AmberNotebook *notebook = open_new(path);
  std::string state = snapshot(notebook);
  const std::string original_cell = first_cell_id(state, "sheet:main");

  command(notebook, "source", "sheet:main", original_cell.c_str(),
          "base = 40\n");
  expect(snapshot(notebook).find("\"selected_tab\":\"sheet:main\"") !=
             std::string::npos,
         "source edits should not switch the selected tab");
  command(notebook, "add_cell", "sheet:main", original_cell.c_str(), nullptr);
  state = snapshot(notebook);
  const std::string second_marker = "\"source\":\"\",\"formatting\":\"\",\"watch\":true";
  expect(state.find(second_marker) != std::string::npos,
         "new code cell should have an empty Watch source");

  // The inserted cell is before any trailing cells; fetch its stable ID from
  // selected_cell, which now points to the newly allocated cell.
  const std::string selected_marker = "\"selected_cell\":\"";
  const std::size_t selected = state.find(selected_marker,
      state.find("\"id\":\"sheet:main\""));
  expect(selected != std::string::npos, "selected cell should be present");
  const std::size_t selected_begin = selected + selected_marker.size();
  const std::size_t selected_end = state.find('"', selected_begin);
  const std::string inserted_cell =
      state.substr(selected_begin, selected_end - selected_begin);
  expect(inserted_cell != original_cell,
         "new cells should receive a distinct stable ID");

  command(notebook, "source", "sheet:main", inserted_cell.c_str(),
          "base + 2\n");
  command(notebook, "watch", "sheet:main", inserted_cell.c_str(), "off");
  command(notebook, "run", "sheet:main", original_cell.c_str(), nullptr);
  command(notebook, "run", "sheet:main", inserted_cell.c_str(), nullptr);
  state = snapshot(notebook);
  expect(state.find("\"result\":\"42\"") != std::string::npos,
         "running a Manual cell should evaluate its dependency context");
  expect(state.find("\"watch\":false") != std::string::npos,
         "watch mode should be persisted in the snapshot");
  command(notebook, "save_all");
  expect(snapshot(notebook).find("\"modified_count\":0") !=
             std::string::npos,
         "save_all should persist all dirty tab sources");
  amber_notebook_destroy(notebook);

  notebook = open_existing(path);
  state = snapshot(notebook);
  expect(state.find("\"id\":\"" + original_cell + "\"") !=
             std::string::npos &&
             state.find("\"id\":\"" + inserted_cell + "\"") !=
                 std::string::npos,
         "cell identities should survive save and reopen");
  expect(state.find("base + 2\\n") != std::string::npos,
         "edited source should survive save and reopen");
  amber_notebook_destroy(notebook);
}

void test_bundled_module_save_auto_import_and_apply() {
  TempDirectory temporary;
  const auto path = temporary.path / "module-project";
  AmberNotebook *notebook = open_new(path);
  const std::string main_cell = first_cell_id(snapshot(notebook), "sheet:main");
  command(notebook, "new_module", nullptr, nullptr, "triple");
  std::string state = snapshot(notebook);
  const std::string module_cell = first_cell_id(state, "module:triple");
  expect(state.find("\"selected_tab\":\"module:triple\"") !=
             std::string::npos,
         "creating a module should select its source tab");

  const char *module_source = "package triple\ndef triple(x):\n  x * 3\nexport triple\n";
  command(notebook, "source", "module:triple", module_cell.c_str(),
          module_source);
  command(notebook, "source", "sheet:main", main_cell.c_str(),
          "triple(14)\n");
  state = snapshot(notebook);
  expect(state.find("\"selected_tab\":\"module:triple\"") !=
             std::string::npos,
         "editing a different tab should preserve the current selection");
  command(notebook, "save_all");
  command(notebook, "auto_import", "module:triple", nullptr, "on");
  command(notebook, "apply", "sheet:main");
  state = snapshot(notebook);
  expect(state.find("\"result\":\"42\"") != std::string::npos,
         "Apply should load and evaluate the explicitly auto-imported module");
  expect(state.find("\"selected_tab\":\"module:triple\"") !=
             std::string::npos,
         "Apply should restore the tab selection after targeting a sheet");
  expect(state.find("\"auto_import\":true") != std::string::npos,
         "snapshot should expose the module auto-import setting");

  command(notebook, "save", "sheet:main");
  amber_notebook_destroy(notebook);
  notebook = open_existing(path);
  state = snapshot(notebook);
  const std::string module_source_json =
      "package triple\\ndef triple(x):\\n  x * 3\\nexport triple\\n";
  expect(state.find("\"id\":\"module:triple\"") != std::string::npos &&
             state.find(module_source_json) != std::string::npos &&
             state.find("\"id\":\"" + main_cell + "\"") !=
                 std::string::npos &&
             state.find("triple(14)\\n") != std::string::npos,
         "module and sheet source should reopen with their persisted identity and bytes");
  amber_notebook_destroy(notebook);
}

void test_invalid_targets_opaque_cells_and_external_save_conflict() {
  TempDirectory temporary;
  const auto path = temporary.path / "opaque-project";
  auto document = amber::notebook::make_project_document("Opaque Test");
  auto &sheet = document.sheets.front();
  sheet.cells.front().id = 5001U;
  sheet.cells.front().source = "first = 1\n";
  amber::notebook::ProjectCell markdown;
  markdown.id = 5002U;
  markdown.kind = "markdown";
  markdown.extra["source"] = R"({"text":"keep me"})";
  sheet.cells.push_back(markdown);
  amber::notebook::ProjectCell second;
  second.id = 5003U;
  second.source = "second = 2\n";
  sheet.cells.push_back(second);
  amber::notebook::ProjectSheet other;
  other.id = "other";
  other.title = "Other";
  amber::notebook::ProjectCell other_cell;
  other_cell.id = 5004U;
  other.cells.push_back(other_cell);
  document.sheets.push_back(other);
  (void)amber::notebook::create_project(path, document);

  AmberNotebook *notebook = open_existing(path);
  std::string state = snapshot(notebook);
  expect(state.find("preserved (read-only markdown)") != std::string::npos &&
             state.find("\"result\":\"not evaluated\"") !=
                 std::string::npos,
         "opening should preserve opaque cells without executing source");
  expect(command_fails(notebook, "source", "sheet:main", "999999", "bad", nullptr),
         "stale cell IDs should be rejected");
  expect(command_fails(notebook, "source", "sheet:main", "5002", "bad", nullptr),
         "opaque cells must reject source editing");
  expect(command_fails(notebook, "delete_cell", "sheet:main", "5002", nullptr),
         "opaque cells must reject deletion");
  expect(command_fails(notebook, "source", "sheet:missing", "5001", "bad", nullptr),
         "unknown tabs should be rejected");
  expect(command_fails(notebook, "select", "sheet:other", "999999", nullptr,
                       nullptr),
         "select should reject a stale cell ID");
  state = snapshot(notebook);
  expect(state.find("\"selected_tab\":\"sheet:main\"") !=
             std::string::npos,
         "failed select should not switch the active tab");
  expect(command_fails(notebook, "move_up", "sheet:main", "5002", nullptr,
                       nullptr),
         "opaque cells cannot be moved directly");

  command(notebook, "source", "sheet:main", "5001", "first = 7\n");
  amber::notebook::LoadedProject external =
      amber::notebook::load_project(path);
  auto externally_changed = external.document;
  externally_changed.title = "External edit";
  amber::notebook::save_project(&external, externally_changed);
  std::string conflict;
  expect(command_fails(notebook, "save", "sheet:main", nullptr, nullptr,
                       &conflict),
         "save should detect an external manifest edit");
  expect(conflict.find("changed externally") != std::string::npos,
         "save error should explain the external conflict");
  state = snapshot(notebook);
  expect(state.find("\"modified\":true") != std::string::npos,
         "failed save should retain unsaved source state");
  amber_notebook_destroy(notebook);

  const auto reopened = amber::notebook::load_project(path);
  expect(reopened.document.title == "External edit" &&
             reopened.document.sheets.front().cells[1].extra == markdown.extra,
         "failed save should keep external data and opaque metadata intact");
}

void test_open_rejects_non_text_module_sources() {
  TempDirectory temporary;
  const amber::notebook::ProjectModule entry{"binary", "modules/binary.am", {}};

  const auto invalid_path = temporary.path / "invalid-utf8-project";
  auto invalid_project = amber::notebook::create_project(
      invalid_path, amber::notebook::make_project_document("Invalid UTF-8"));
  (void)amber::notebook::create_project_module(
      &invalid_project, entry,
      std::string("package binary\n") + static_cast<char>(0xff));
  char *error = nullptr;
  AmberNotebook *notebook =
      amber_notebook_open(invalid_path.c_str(), 0, nullptr, &error);
  expect(notebook == nullptr,
         "opening a module with invalid UTF-8 should fail before snapshot");
  expect(error != nullptr && std::string(error).find("valid UTF-8") !=
                                 std::string::npos,
         "invalid UTF-8 open error should identify the source encoding");
  amber_notebook_string_free(error);

  const auto nul_path = temporary.path / "nul-module-project";
  auto nul_project = amber::notebook::create_project(
      nul_path, amber::notebook::make_project_document("NUL Module"));
  (void)amber::notebook::create_project_module(
      &nul_project, entry, std::string("package binary\n\0tail", 20U));
  error = nullptr;
  notebook = amber_notebook_open(nul_path.c_str(), 0, nullptr, &error);
  expect(notebook == nullptr,
         "opening a module with embedded NUL should fail before snapshot");
  expect(error != nullptr && std::string(error).find("embedded NUL") !=
                                 std::string::npos,
         "embedded NUL open error should identify the source byte");
  amber_notebook_string_free(error);
}

std::string selected_cell_id(const std::string &json) {
  const std::string marker = "\"selected_cell\":\"";
  const auto start = json.find(marker);
  expect(start != std::string::npos, "snapshot has selected cell");
  const auto first = start + marker.size();
  return json.substr(first, json.find('"', first) - first);
}

void test_native_text_blocks_roundtrip_without_execution() {
  TempDirectory temporary;
  const auto path = temporary.path / "rich-text.amberbook";
  auto *notebook = open_new(path);
  const auto code = first_cell_id(snapshot(notebook), "sheet:main");
  command(notebook, "source", "sheet:main", code.c_str(), "x = 21\nx\n");
  command(notebook, "add_text", "sheet:main", code.c_str());
  const auto text = selected_cell_id(snapshot(notebook));
  const char *payload = R"({"source":"x = 999\nЗаметка 🦊","formatting":{"version":1,"runs":[{"start":0,"length":7,"bold":true}]}})";
  command(notebook, "rich_source", "sheet:main", text.c_str(), payload);
  expect(command_fails(notebook, "source", "sheet:main", text.c_str(), "discard formatting"),
         "plain source update must not discard formatting");
  expect(command_fails(notebook, "watch", "sheet:main", text.c_str(), "on"),
         "text must not become a Watch cell");
  expect(command_fails(notebook, "run", "sheet:main", text.c_str(), nullptr),
         "text cannot be explicitly run");
  command(notebook, "add_cell", "sheet:main", text.c_str());
  const auto dependent = selected_cell_id(snapshot(notebook));
  command(notebook, "source", "sheet:main", dependent.c_str(), "x * 2\n");
  command(notebook, "run_all", "sheet:main");
  auto state = snapshot(notebook);
  expect(state.find("\"result\":\"42\"") != std::string::npos,
         "text resembling code must not redefine x or block consumers");
  command(notebook, "move_up", "sheet:main", text.c_str());
  command(notebook, "rich_source", "sheet:main", text.c_str(),
          R"({"source":"x = 777\nЗаметка 🦊","formatting":{"version":1,"runs":[{"start":0,"length":7,"italic":true}]}})");
  state = snapshot(notebook);
  expect(state.find("\"dirty\":true") == std::string::npos &&
             state.find("\"result\":\"42\"") != std::string::npos,
         "text edit/move must keep code results current");
  const auto before_invalid = state;
  expect(command_fails(notebook, "rich_source", "sheet:main", text.c_str(),
                       R"({"source":"🦊","formatting":{"version":1,"runs":[{"start":1,"length":1,"bold":true}]}})"),
         "text must reject ranges splitting a surrogate pair");
  expect(snapshot(notebook) == before_invalid, "invalid rich edit must be atomic");
  command(notebook, "save_all");
  amber_notebook_destroy(notebook);
  const auto saved = amber::notebook::load_project(path);
  const auto &saved_text = saved.document.sheets.front().cells.front();
  expect(saved_text.kind == "text" && saved_text.source == "x = 777\nЗаметка 🦊" &&
             saved_text.formatting.find("italic") != std::string::npos,
         "rich text should persist plain source and styles in sheet order");
  notebook = open_existing(path);
  state = snapshot(notebook);
  expect(state.find("x = 777\\nЗаметка 🦊") != std::string::npos &&
             state.find("italic") != std::string::npos &&
             state.find("\"modified_count\":0") != std::string::npos,
         "reopening text must restore its native source, styles and clean state");
  command(notebook, "save_all");
  expect(amber::notebook::load_project(path).document.sheets.front().cells.front().source == saved_text.source,
         "Save after reopen must never replace text with an empty source");
  command(notebook, "run_all", "sheet:main");
  expect(snapshot(notebook).find("\"result\":\"42\"") != std::string::npos,
         "reopened mixed sheet should evaluate code correctly");
  command(notebook, "delete_cell", "sheet:main", text.c_str());
  command(notebook, "save_all");
  amber_notebook_destroy(notebook);
  expect(amber::notebook::load_project(path).document.sheets.front().cells.size() == 2U,
         "text deletion should not delete or replace code cells");
}

void test_external_directory_link_import_and_unlink() {
  TempDirectory temporary;
  const auto project = temporary.path / "Linked.amberbook";
  const auto package = temporary.path / "Package with spaces";
  std::filesystem::create_directory(package);
  std::ofstream(package / "amber.toml") <<
      "[package]\nname = \"maths\"\nversion = \"1.0.0\"\nroot = \"maths\"\n"
      "[[modules]]\nname = \"maths\"\npath = \"maths.am\"\n";
  std::ofstream(package / "maths.am") << "package maths\nexport answer\ndef answer(): 42\n";
  auto *notebook = open_new(project);
  const auto id = first_cell_id(snapshot(notebook), "sheet:main");
  command(notebook, "source", "sheet:main", id.c_str(), "import maths\nmaths.answer()\n");
  command(notebook, "add_dependency", nullptr, nullptr, package.c_str());
  auto saved = amber::notebook::load_project(project);
  expect(saved.document.version == 3 && saved.document.modules.empty() && saved.document.dependencies.size() == 1,
         "dependency link saves structure without copying source");
  expect(snapshot(notebook).find("\"auto_import\":false") != std::string::npos &&
         snapshot(notebook).find("\"result\":\"not evaluated\"") != std::string::npos,
         "dependency snapshot is source-only");
  command(notebook, "apply", "sheet:main");
  expect(snapshot(notebook).find("\"result\":\"42\"") != std::string::npos,
         "native bridge can import from a directory");
  command(notebook, "remove_dependency", nullptr, nullptr, saved.document.dependencies[0].path.c_str());
  expect(amber::notebook::load_project(project).document.dependencies.empty() &&
         std::filesystem::exists(package / "maths.am"), "unlink never deletes external files");
  amber_notebook_destroy(notebook);
}

} // namespace

int main() {
  test_container_result_formats();
  test_external_directory_link_import_and_unlink();
  test_interpolation_display_values();
  test_open_is_source_only_and_pump_idles();
  test_empty_sheet_accepts_cell_creation_without_an_anchor();
  test_source_watch_run_and_stable_ids_roundtrip();
  test_bundled_module_save_auto_import_and_apply();
  test_invalid_targets_opaque_cells_and_external_save_conflict();
  test_open_rejects_non_text_module_sources();
  test_native_text_blocks_roundtrip_without_execution();
  std::cout << "notebook macOS bridge tests passed\n";
  return 0;
}
