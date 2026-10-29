#include "package/store.h"
#include "tools/isputnik/dependencies.h"
#include "tools/isputnik/project_session.h"
#include "tools/isputnik/tabs.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>

namespace fs = std::filesystem;
namespace {
void expect(bool value, const std::string &message) {
  if (!value) {
    std::cerr << "package store test failed: " << message << '\n';
    std::exit(1);
  }
}
void write(const fs::path &path, const std::string &text) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path);
  out << text;
  expect(out.good(), "write fixture");
}
void install(const fs::path &root, const fs::path &home,
             const std::string &name, const std::string &version, int answer) {
  const auto compiled =
      compile_source_text("package shared.calc\nexport answer\ndef answer(): " +
                              std::to_string(answer) + "\n",
                          "calc.s", "shared.calc");
  expect(compiled.ok, compiled.error);
  sputnik::pkg::PackageManifest manifest;
  manifest.name = name;
  manifest.version = version;
  manifest.root_module = "shared.calc";
  manifest.modules.push_back({"shared.calc", "src/calc.s"});
  const auto built = sputnik::pkg::build_package_artifact(
      manifest, {{"shared.calc",
                  "src/calc.s",
                  {},
                  sputnik::bytecode::serialize_module(compiled.module)}});
  expect(built.ok, "build artifact");
  const auto path = root / (name + "-" + version + ".sputnikpkg");
  write(path, built.serialized);
  (void)sputnik::pkg::install_in_home(path, home);
}

void path_dependencies(const fs::path &root) {
  const auto app = root / "path-consumer", a = root / "local-a", b = root / "local-b";
  fs::create_directories(app);
  const auto manifest = [](const std::string &name, const std::string &dependencies = "") {
    return "name: \"" + name + "\"\nversion: \"1.0.0\"\nsource_root: src\nfiles: [\"src/main.s\"]\nmodule_names: {\"src/main.s\": \"" + name + "\"}\n" + dependencies;
  };
  write(a / "sputnik.build.yaml", manifest("local.a", "dependencies:\n  local.b: {path: \"../local-b\"}\n"));
  write(a / "src/main.s", "package local.a\nimport local.b\n");
  write(b / "sputnik.build.yaml", manifest("local.b"));
  write(b / "src/main.s", "package local.b\nexport answer\ndef answer(): 1\n");
  write(app / "sputnik.build.yaml", manifest("consumer", "dependencies:\n  local.a: \"1.0.0\"\n"));
  const std::vector<sputnik::pkg::PackageDependency> dependencies = {{"local.a", {}, "path", {}, "../local-a"}};
  sputnik::pkg::PackageResolver first(app, root / "empty-home");
  first.require(dependencies);
  expect(first.resolve("local.b").has_value() && first.selected().size() == 2, "transitive YAML path resolves relative to its declaring package");
  const auto lock = first.lockfile(), fingerprint = first.selected().back().digest;
  write(app / "sputnik.lock", lock);
  write(b / "src/main.s", "package local.b\nexport answer\ndef answer(): 2\n");
  sputnik::pkg::PackageResolver changed(app, root / "empty-home");
  changed.require(dependencies);
  expect(changed.lockfile() == lock && changed.selected().back().digest != fingerprint, "local edits preserve graph lock but change content fingerprint");
  expect(changed.resolve("local.b")->source.find("2") != std::string::npos, "locked local source remains live");
  sputnik::pkg::PackageResolver imports_only(app, root / "empty-home");
  expect(imports_only.resolve("local.a").has_value(), "lock supplies previously resolved local sources without repeating dependency declarations");
  bool rejected = false;
  try { changed.require({{"local.b", "2.0.0", "registry", {}}}); }
  catch (const std::exception &) { rejected = true; }
  expect(rejected, "transitive exact-version conflict is rejected");
  write(root / "other-b/sputnik.build.yaml", manifest("local.b"));
  write(root / "other-b/src/main.s", "package local.b\n");
  rejected = false;
  try {
    sputnik::pkg::PackageResolver conflict(app, root / "empty-home");
    conflict.require({dependencies.front(), {"local.b", {}, "path", {}, "../other-b"}});
  } catch (const std::exception &) { rejected = true; }
  expect(rejected, "two canonical roots cannot provide the same package identity");
  rejected = false;
  try {
    sputnik::pkg::PackageResolver mismatch(app, root / "empty-home");
    mismatch.require({{"wrong.name", {}, "path", {}, "../local-a"}});
  } catch (const std::exception &) { rejected = true; }
  expect(rejected, "dependency key must match the package manifest name");
  write(a / "sputnik.build.yaml", manifest("local.a"));
  rejected = false;
  try {
    sputnik::pkg::PackageResolver altered(app, root / "empty-home");
    altered.require(dependencies);
  } catch (const std::exception &) { rejected = true; }
  expect(rejected, "changing local dependency edges requires a lock update");
}
} // namespace

int main(int argc, char **argv) {
  if (argc == 3 && std::string(argv[1]) == "--import-replacements") {
    const fs::path home = fs::canonical(argv[2]);
    ::setenv("SPUTNIK_PKG_HOME", home.c_str(), 1);
    for (const auto &alias : {std::string("orm"), std::string("vendor.orm")}) {
      auto document = sputnik::notebook::make_project_document("Fork imports");
      document.sheets[0].cells.resize(2);
      for (auto &cell : document.sheets[0].cells)
        cell.id = sputnik::notebook::allocate_cell_id();
      document.sheets[0].cells[0].source = "import my.orm as " + alias + "\n";
      document.sheets[0].cells[1].source =
          "import " + alias + "\nfrom " + alias + " import Box\n"
          "from client" + std::string(alias == "orm" ? "0" : "1") +
          " import through_dependency, Box as ClientBox\n"
          "[orm.answer(), through_dependency(), Box == ClientBox]\n";
      auto project = sputnik::notebook::create_project(
          home.parent_path() / (alias + ".sputnikbook"), document);
      ProjectTabs tabs(project);
      auto &session = tabs.active().session;
      expect(tabs.apply_active(true), "installed fork applies without dependency links: " + session.environment_error);
      expect(session.cells[1].ok && session.cells[1].result == "[42, 91, true]",
             "notebook redirects installed bytecode dependencies: " + session.cells[1].error);
      expect(project.document.dependencies.empty(), "fork imports need no notebook dependency metadata");
    }
    std::cout << "import_replacement_notebook: PASS\n";
    return 0;
  }
  if (argc == 3 && std::string(argv[1]) == "--native-registry") {
    const fs::path home = fs::canonical(argv[2]);
    ::setenv("SPUTNIK_PKG_HOME",home.c_str(),1);
    auto document = sputnik::notebook::make_project_document("Native registry variant");
    document.sheets[0].cells.resize(1);
    document.sheets[0].cells[0].source = "from test.native_test import answer\nanswer()\n";
    auto project = sputnik::notebook::create_project(home.parent_path()/"NativeVariant.sputnikbook",document);
    ProjectTabs tabs(project);
    auto &session = tabs.active().session;
    session.prepare_runtime = [&](const auto &module) { load_prepared_native_dependencies(project,module,true); };
    for (const auto *name : {"ffi.load","ffi.call"}) session.runtime_capability_grants.push_back(
      sputnik::capability::make_capability(name,"*","native registry test",sputnik::capability::kCapabilityFlagWildcardTarget));
    expect(tabs.apply_active(true),"native source distribution resolves in notebook: "+session.environment_error);
    expect(session.cells[0].ok && session.cells[0].result=="42","notebook executes prebuilt source distribution: "+session.cells[0].error);
    std::cout << "package_native_registry_notebook: PASS\n";
    return 0;
  }
  if (argc == 3 && std::string(argv[1]) == "--native") {
    const fs::path home = fs::canonical(argv[2]);
    ::setenv("SPUTNIK_PKG_HOME", home.c_str(), 1);
    auto document =
        sputnik::notebook::make_project_document("Installed native package");
    document.sheets[0].cells.resize(1);
    document.sheets[0].cells[0].source =
        "from native_calc import answer\n"
        "from native_calc.facade import answer as facade_answer\n"
        "from nat.box import main as box_main\n"
        "from nat.demo import main as demo_main\n"
        "facade_answer() + box_main() + demo_main()\n";
    auto project = sputnik::notebook::create_project(
        home.parent_path() / "Native.sputnikbook", document);
    ProjectTabs tabs(project);
    auto &session = tabs.active().session;
    bool trusted = false;
    session.prepare_runtime = [&](const auto &module) {
      load_prepared_native_dependencies(project, module, trusted);
    };
    for (const auto *name : {"ffi.load", "ffi.call"})
      session.runtime_capability_grants.push_back(
          sputnik::capability::make_capability(
              name, "*", "test host",
              sputnik::capability::kCapabilityFlagWildcardTarget));
    expect(!tabs.apply_active(true),
           "native packages retain execution-time trust");
    expect(session.environment_error.find("explicit trust") !=
               std::string::npos,
           "native trust diagnostic");
    trusted = true;
    expect(tabs.apply_active(true),
           "installed native package runs in trusted notebook: " +
               session.environment_error);
    expect(session.cells[0].ok && session.cells[0].result == "108",
           "notebook reuses prepared native library: " +
               session.cells[0].error);
    std::cout << "package_store_native_notebook ok\n";
    return 0;
  }
  char pattern[] = "/tmp/sputnik-package-store-XXXXXX";
  const auto created = ::mkdtemp(pattern);
  expect(created != nullptr, "temporary directory");
  const fs::path root(created), home = root / "home";
  path_dependencies(root);
  const auto *previous = std::getenv("SPUTNIK_PKG_HOME");
  const std::string saved = previous ? previous : "";
  const bool had_previous = previous != nullptr;
  ::setenv("SPUTNIK_PKG_HOME", home.c_str(), 1);
  install(root, home, "unrelated-package-name", "1.9.0", 41);
  install(root, home, "unrelated-package-name", "1.10.0", 42);
  expect(sputnik::pkg::version_less("1.9.0", "1.10.0"),
         "numeric version ordering");
  expect(sputnik::pkg::version_less("1.0.0-preview.9", "1.0.0-preview.10"),
         "numeric prerelease ordering");
  expect(sputnik::pkg::version_less("1.0.0-preview.10", "1.0.0"),
         "stable release ordering");

  auto document = sputnik::notebook::make_project_document("Shared packages");
  document.sheets[0].cells.resize(1);
  document.sheets[0].cells[0].source =
      "from shared.calc import answer\nanswer()\n";
  auto project =
      sputnik::notebook::create_project(root / "Example.sputnikbook", document);
  ProjectTabs tabs(project);
  auto &session = tabs.active().session;
  expect(!session.backend, "opening does not compile or initialize packages");
  expect(tabs.apply_active(true),
         "notebook resolves installed bytecode: " + session.environment_error);
  expect(session.cells[0].ok && session.cells[0].result == "42",
         "installed module runs in notebook: " + session.cells[0].error);
  expect(project.document.dependencies.empty(),
         "automatic imports do not rewrite project dependencies");

  sputnik::pkg::PackageResolver resolver(project.directory);
  expect(resolver.resolve("shared.calc").has_value(),
         "module name need not match package name");
  write(project.directory / "sputnik.lock", resolver.lockfile());
  install(root, home, "unrelated-package-name", "1.11.0", 43);
  expect(tabs.apply_active(true), "apply retains pinned version");
  expect(session.cells[0].result == "42",
         "lock pin survives newer installation");
  fs::remove(project.directory / "sputnik.lock");
  expect(tabs.apply_active(true), "apply resolves updated version");
  expect(session.cells[0].result == "43", "apply refreshes installed snapshot");

  const auto local = project.directory / "packages" / "custom";
  write(local / "sputnik.build.json",
        "{\"schema\":\"sputnik.build.v1\",\"name\":\"unrelated-package-name\","
        "\"root\":\"shared.calc\",\"modules\":[{\"name\":\"shared.calc\","
        "\"path\":\"calc.s\"}]}");
  write(local / "calc.s",
        "package shared.calc\nexport answer\ndef answer(): 7\n");
  expect(tabs.apply_active(true), "apply discovers local override");
  expect(session.cells[0].result == "7",
         "notebook packages/ is anchored to document directory");
  write(local / "calc.s",
        "package shared.calc\nexport answer\ndef answer(): 8\n");
  evaluate_from(&session, 0, true, false, false);
  expect(session.cells[0].result == "7",
         "ordinary Run retains loaded snapshot");
  expect(tabs.apply_active(true) && session.cells[0].result == "8",
         "Apply refreshes source override");

  // A package lookup collision is diagnosed rather than selecting by directory
  // order.
  fs::remove_all(project.directory / "packages");
  install(root, home, "second-provider", "1.0.0", 99);
  expect(!tabs.apply_active(true), "ambiguous providers fail Apply");
  expect(session.environment_error.find("ambiguous module") !=
             std::string::npos,
         "conflict identifies module providers");

  if (had_previous)
    ::setenv("SPUTNIK_PKG_HOME", saved.c_str(), 1);
  else
    ::unsetenv("SPUTNIK_PKG_HOME");
  fs::remove_all(root);
  std::cout << "package_store_tests ok\n";
}
