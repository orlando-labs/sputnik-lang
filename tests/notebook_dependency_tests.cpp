#include "tools/isputnik/dependencies.h"
#include "tools/isputnik/tabs.h"
#include "frontend/lexer/token.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace sputnik::notebook;
static void require(bool ok, const std::string &message) {
  if (!ok) { std::cerr << "dependency test failed: " << message << '\n'; std::exit(1); }
}
template<class F> static void rejects(F fn, const std::string &message) {
  try { fn(); } catch (const std::exception &) { return; }
  require(false, message);
}
static void write(const fs::path &path, const std::string &text) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path); out << text;
  require(out.good(), "write fixture");
}
static const std::string manifest =
    "schema: sputnik.build.v1\nname: calc\nroot: calc\nmodules:\n"
    "  - name: calc\n    path: src/calc.s\n"
    "  - name: helper\n    path: src/helper.s\n";
static std::string body(int answer) {
  return "package calc\nfrom helper import increment\nexport answer as value, Box\n"
         "def answer(): increment(" + std::to_string(answer - 1) + ")\n"
         "class Box:\n  def init(@value)\n  def value(): @value\n";
}
static void result(Session &session, const std::string &source, const std::string &expected) {
  session.cells[0].source = source;
  session.cells[0].dirty = true;
  evaluate_from(&session, 0, true, false, false);
  require(session.cells[0].ok && session.cells[0].result == expected,
          source + " => " + session.cells[0].result + ": " + session.cells[0].error + session.environment_error);
}
int main() {
  char pattern[] = "/tmp/sputnik-notebook-dependencies-XXXXXX";
  const auto created = mkdtemp(pattern);
  require(created != nullptr, "temporary directory");
  const fs::path root(created), package = root / "local package";
  write(package / "sputnik.build.yaml", manifest);
  write(package / "src/calc.s", body(42));
  write(package / "src/helper.s", "package helper\nexport increment\ndef increment(n): n + 1\n");
  auto doc = make_project_document("Linked packages");
  doc.version = 3;
  doc.dependencies.push_back({"../local package", false, {{"editor", "{\"pinned\":true}"}}});
  doc.sheets[0].cells.resize(1);
  doc.sheets[0].cells[0].source = "import calc\nx = calc.value()\nx\n";
  auto project = create_project(root / "Linked.sputnikbook", doc);
  ProjectTabs tabs(project);
  auto &session = tabs.active().session;
  require(!session.backend && !session.cells[0].ok, "opening does not execute");
  require(project.document.modules.empty() && !fs::exists(project.directory / "modules"), "no copied modules");
  require(tabs.apply_active(true), "apply directory dependency: " + session.environment_error);
  require(session.cells[0].ok && session.cells[0].result == "42", "namespace import: " + session.cells[0].error);
  result(session, "import calc as c\nc.value()\n", "42");
  result(session, "from calc import value as answer\nanswer()\n", "42");
  result(session, "import calc\ncalc.Box(17).value()\n", "17");
  result(session, "import calc\n[1].map |n|: calc.value() + n\n", "[43]");
  result(session, "from calc import value\n[1].map |n|: value() + n\n", "[43]");
  result(session, "import calc\nf = calc.value\nf()\n", "42");
  require(session.cells[0].locals.size() == 1 && session.cells[0].locals[0].name == "f",
          "imports and synthetic namespace aliases are not uninitialized user locals");

  session.cells[0].source = "import calc\ncalc.private_value()\n";
  const bool private_applied = tabs.apply_active(true);
  require(!private_applied && session.environment_error.find("NB1008") != std::string::npos,
          "only public exports are reachable: " + session.environment_error);
  require(session.cells[0].result == "42", "failed compilation preserves results");
  session.cells[0].source = "import calc\ncalc\n";
  require(!tabs.apply_active(true) && session.environment_error.find("namespace") != std::string::npos,
          "first-class namespace has a diagnostic");
  session.cells[0].source = "value()\n";
  require(!tabs.apply_active(true), "explicit dependency must not inject ambient exports");

  session.cells[0].source = "import calc\ncalc.value()\n";
  write(package / "src/calc.s", body(84));
  require(tabs.apply_active(true) && session.cells[0].result == "84", "Apply refreshes external source");
  const auto backend = session.backend.get();
  write(package / "src/calc.s", body(99) + "1 / 0\n");
  require(!tabs.apply_active(true) && session.backend.get() == backend && session.cells[0].result == "84",
          "failed package initialization preserves old world");
  write(package / "src/calc.s", body(99));
  fs::rename(package / "src/calc.s", package / "src/calc.saved");
  require(!tabs.apply_active(true) && session.backend.get() == backend, "missing source preserves old world");
  fs::rename(package / "src/calc.saved", package / "src/calc.s");
  session.cells[0].watch = false;
  require(tabs.apply_active(false) && !session.cells[0].ok, "Apply respects Manual barrier");
  require(tabs.apply_active(true) && session.cells[0].result == "99", "explicit force can run Manual");

  auto ambient = doc; ambient.dependencies[0].auto_import = true;
  project.document = ambient;
  Session auto_session; load_project_into_session(&auto_session, project);
  result(auto_session, "value()\n", "99");
  const auto roundtrip = parse_project_document(serialize_project_document(ambient));
  require(roundtrip.version == 3 && roundtrip.dependencies[0].auto_import &&
          roundtrip.dependencies[0].extra == ambient.dependencies[0].extra, "v3 roundtrip with extras");
  auto invalid = ambient; invalid.version = 2;
  rejects([&] { validate_project_document(invalid); }, "v2 cannot hide external dependencies");
  invalid = ambient; invalid.dependencies.push_back(invalid.dependencies[0]);
  rejects([&] { validate_project_document(invalid); }, "duplicate paths rejected");

  // Structural UI/TUI commands do not execute or copy package files.
  auto blank = create_project(root / "Blank.sputnikbook", make_project_document("Blank"));
  ProjectTabs linked(blank);
  linked.add_dependency(package.string());
  require(linked.project().document.version == 3 && !linked.active().session.backend, "link is inert");
  rejects([&] { linked.add_dependency(package.string()); }, "duplicate link rejected");
  ProjectInput input; input.id = "gain"; input.title = "Gain"; input.type = "number"; input.initial = 1.0;
  linked.create_input(input);
  ProjectBoard board; board.id = "dashboard"; board.title = "Board"; board.sheet = "main";
  linked.save_board(board);
  require(load_project(blank.directory).document.version == 3, "board/input edits preserve v3");
  linked.remove_dependency(linked.project().document.dependencies[0].path);
  require(linked.project().document.dependencies.empty() && fs::exists(package / "src/calc.s"), "unlink preserves package");

  project.document = doc; project.document.dependencies[0].path = "../missing";
  ProjectTabs missing(project);
  require(!missing.active().session.backend && !missing.apply_active(), "broken link opens inert with Apply diagnostic");
  auto sources = load_directory_dependency(project.directory, doc.dependencies[0]);
  auto duplicate = sources; duplicate.push_back(sources[0]);
  require(!prepare_bundled_environment(duplicate, {"calc"}).ok, "duplicate module identities rejected");
  auto conflict = sources; conflict[0].id = "math";
  require(!prepare_bundled_environment(conflict, {"math"}).ok, "native module names cannot be replaced");

  // Use only manifest-listed files, with traversal and escaping symlinks blocked.
  write(package / "sputnik.build.yaml", "schema: sputnik.build.v1\nname: calc\nroot: calc\nmodules:\n  - name: calc\n    path: ../outside.s\n");
  write(root / "outside.s", "package calc\n");
  rejects([&] { load_directory_dependency(project.directory, doc.dependencies[0]); }, "traversal rejected");
  write(package / "sputnik.build.yaml", manifest);
  fs::rename(package / "src/calc.s", package / "src/calc.saved");
  fs::create_symlink(root / "outside.s", package / "src/calc.s");
  rejects([&] { load_directory_dependency(project.directory, doc.dependencies[0]); }, "symlink escape rejected");
  fs::remove(package / "src/calc.s");
  fs::rename(package / "src/calc.saved", package / "src/calc.s");
  write(package / "sputnik.toml", "[package]\nname = \"calc\"\nversion = \"1.0.0\"\nroot = \"calc\"\n"
      "[[modules]]\nname = \"calc\"\npath = \"src/calc.s\"\n"
      "[[modules]]\nname = \"helper\"\npath = \"src/helper.s\"\n");
  require(load_directory_dependency(project.directory, doc.dependencies[0]).size() == 2, "sputnik.toml source map");
  // Declaring an artifact is inert. Trust and hash checks precede dlopen.
  auto native_dependency = doc.dependencies[0];
  native_dependency.extra["native_library"] = "\"fake.so\"";
  native_dependency.extra["native_sha256"] = "\"" + std::string(64, '0') + "\"";
  write(package / "fake.so", "not a dynamic library");
  require(load_directory_dependency(project.directory, native_dependency).size() == 2,
          "opening native dependency only reads declared sources");
  project.document.dependencies = {native_dependency};
  const sputnik::bytecode::BcModule empty_module;
  auto rejected_with = [&](bool trusted, const std::string &message) {
    try { load_prepared_native_dependencies(project, empty_module, trusted); }
    catch (const std::exception &error) {
      require(std::string(error.what()).find(message) != std::string::npos, error.what()); return;
    }
    require(false, "native dependency must reject: " + message);
  };
  rejected_with(false, "explicit trust");
  rejected_with(true, "digest changed");
  project.document.dependencies[0].extra["native_library"] = "\"../outside.s\"";
  rejected_with(true, "escapes its root");

  auto update_doc = doc;
  update_doc.dependencies[0] = native_dependency;
  update_doc.dependencies[0].auto_import = true;
  update_doc.extra["custom_large_number"] = "9007199254740993123";
  update_doc.sheets[0].cells[0].source = "raise ValueError(\"must not run during update\")\n";
  update_doc.sheets[0].cells[0].extra["editor"] = "{\"cursor\":7}";
  const auto update_project = create_project(root / "Update.sputnikbook", update_doc);
  write(package / "prepared/sputnik.build.yaml", manifest);
  // Use root-relative sources for this prepared manifest.
  write(package / "prepared/src/calc.s", body(42));
  write(package / "prepared/src/helper.s", "package helper\nexport increment\ndef increment(n): n + 1\n");
  write(package / "prepared/native.so", "new artifact\n");
  require(update_project_native_dependency(update_project.directory, package,
      "prepared/sputnik.build.yaml", "prepared/native.so"), "native link updated without executing/loading code");
  auto updated_project = load_project(update_project.directory);
  auto expected_doc = update_doc;
  expected_doc.dependencies[0].extra["manifest"] = "\"prepared/sputnik.build.yaml\"";
  expected_doc.dependencies[0].extra["native_library"] = "\"prepared/native.so\"";
  expected_doc.dependencies[0].extra["native_sha256"] = "\"" + sputnik::lexer::sha256_hex("new artifact\n") + "\"";
  require(serialize_project_document(updated_project.document) == serialize_project_document(expected_doc),
          "native update preserves cells, inputs, boards, auto-import and unknown metadata/number spellings");
  const auto update_baseline = updated_project.baseline;
  require(!update_project_native_dependency(update_project.directory, package,
      "prepared/sputnik.build.yaml", "prepared/native.so") &&
      load_project(update_project.directory).baseline == update_baseline, "idempotent update does not rewrite project");
  rejects([&] { update_project_native_dependency(update_project.directory, package,
      "prepared/sputnik.build.yaml", "../outside.s"); }, "update rejects path escape");
  write(package / "prepared/invalid.yaml", "schema: sputnik.build.v1\nname: calc\nroot: calc\nmodules:\n  - name: calc\n    path: missing.s\n");
  rejects([&] { update_project_native_dependency(update_project.directory, package,
      "prepared/invalid.yaml", "prepared/native.so"); }, "missing prepared source rejects update");
  require(load_project(update_project.directory).baseline == update_baseline,
          "failed updates preserve previous project");
  const auto source_project = create_project(root / "SourceOnly.sputnikbook", doc);
  rejects([&] { update_project_native_dependency(source_project.directory, package,
      "prepared/sputnik.build.yaml", "prepared/native.so"); }, "source link cannot silently become native");
  const auto unrelated = create_project(root / "Unrelated.sputnikbook", make_project_document("Unrelated"));
  rejects([&] { update_project_native_dependency(unrelated.directory, package,
      "prepared/sputnik.build.yaml", "prepared/native.so"); }, "unrelated project is not relinked");
  write(package / "build/notebook/current.json", "{\"path\":\"../local package\",\"manifest\":\"prepared/sputnik.build.yaml\","
      "\"native_library\":\"prepared/native.so\",\"native_sha256\":\"" + sputnik::lexer::sha256_hex("new artifact\n") + "\"}");
  require(prepared_native_dependency(package).extra.at("native_sha256") == expected_doc.dependencies[0].extra.at("native_sha256"),
      "prepared descriptor reads verified artifacts without loading native code");
  register_project_native_updates(package, update_project.directory);
  const auto registration = package / "build/notebook/projects" /
      (sputnik::lexer::sha256_hex(update_project.directory.string()) + ".json");
  require(fs::is_regular_file(registration), "new prepared projects register automatic native updates");
  register_project_native_updates(package, update_project.directory);
  rejects([&] { register_project_native_updates(package, unrelated.directory); }, "unrelated project cannot register package updates");
  write(package / "prepared/native.so", "tampered artifact\n");
  rejects([&] { prepared_native_dependency(package); }, "prepared descriptor rejects tampered library");
  rejects([&] { parse_project_dependency("{\"path\":\"x\",\"auto_import\":1}"); }, "dependency auto-import must be boolean");
  rejects([&] { parse_project_dependency("{\"path\":\"\"}"); }, "empty prepared dependency path rejected");
  fs::remove_all(root); // Only this test's mkdtemp directory.
  std::cout << "notebook dependency tests passed\n";
}
