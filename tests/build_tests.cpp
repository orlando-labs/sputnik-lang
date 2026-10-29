#include "buildsys/build.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "build test failed: " << message << "\n";
    std::exit(1);
  }
}

void test_manifest_parses_and_normalizes() {
  const std::string source =
      "{\n"
      "  \"schema\": \"sputnik.build.v1\",\n"
      "  \"name\": \"demo\",\n"
      "  \"root\": \"demo.main\",\n"
      "  \"profiles\": {\n"
      "    \"required\": [\"effects.v1\", \"core.v1\", \"core.v1\"],\n"
      "    \"optional\": [\"typed.v1\"],\n"
      "    \"forbidden\": [\"ffi.v1\"]\n"
      "  },\n"
      "  \"stdlib\": [\n"
      "    {\"name\": \"sputnik.core\", \"path\": \"stdlib/core.s\"}\n"
      "  ],\n"
      "  \"modules\": [\n"
      "    {\"name\": \"demo.main\", \"path\": \"src/main.s\"},\n"
      "    {\"name\": \"demo.util\", \"path\": \"src/util.s\"}\n"
      "  ]\n"
      "}\n";
  const sputnik::build::BuildManifestResult parsed =
      sputnik::build::parse_build_manifest(source, "sputnik.build.json");
  expect(parsed.ok(), sputnik::build::diagnostics_to_string(parsed.diagnostics));
  expect(parsed.manifest.profiles.required_features.size() == 2,
         "required features should be unique");
  expect(parsed.manifest.profiles.required_features[0] == "core.v1",
         "required features should be sorted");
  expect(parsed.manifest.stdlib_modules.size() == 1,
         "stdlib module should parse");
  expect(parsed.manifest.stdlib_modules[0].bootstrap_layer == "B2",
         "stdlib bootstrap should default to B2");
  expect(parsed.manifest.modules[0].name == "demo.main",
         "modules should sort by name");

  const std::uint32_t flags =
      sputnik::build::profile_flags_for(parsed.manifest.profiles);
  expect((flags & (1U << 0U)) != 0U, "core profile flag should be set");
  expect((flags & (1U << 1U)) != 0U, "typed profile flag should be set");
  expect((flags & (1U << 3U)) != 0U, "effects profile flag should be set");
}

void test_yaml_manifest_parses_and_normalizes() {
  const std::string source =
      "schema: sputnik.build.v1\n"
      "name: demo.yaml\n"
      "root: demo.main\n"
      "profiles:\n"
      "  required: [effects.v1, core.v1, core.v1, ffi.v1]\n"
      "  optional:\n"
      "    - typed.v1\n"
      "  forbidden: []\n"
      "  numeric:\n"
      "    int: Int64\n"
      "    overflow: checked\n"
      "stdlib:\n"
      "  - name: sputnik.core\n"
      "    path: stdlib/core.s\n"
      "modules:\n"
      "  - name: demo.util\n"
      "    path: src/util.s\n"
      "  - name: demo.main\n"
      "    path: src/main.s\n"
      "native_extensions:\n"
      "  - name: tiny\n"
      "    language: c\n"
      "    sources: [native/tiny.c]\n"
      "    library_dirs: [vendor/lib]\n"
      "    runtime_library_dirs: [\"@loader_path/lib\"]\n"
      "    blocking_symbols: [tiny.wait]\n"
      "    symbols:\n"
      "      - logical: tiny.answer\n"
      "        symbol: tiny_answer\n";
  const sputnik::build::BuildManifestResult parsed =
      sputnik::build::parse_build_manifest(source, "sputnik.build.yaml");
  expect(parsed.ok(), sputnik::build::diagnostics_to_string(parsed.diagnostics));
  expect(parsed.manifest.profiles.required_features.size() == 3,
         "YAML required features should be unique");
  expect(parsed.manifest.profiles.required_features[0] == "core.v1",
         "YAML required features should be sorted");
  expect(parsed.manifest.profiles.numeric_int == "Int64" &&
             parsed.manifest.profiles.numeric_overflow == "checked",
         "YAML numeric profile should parse");
  expect(parsed.manifest.modules[0].name == "demo.main",
         "YAML modules should sort by name");
  expect(parsed.manifest.native_extensions.size() == 1,
         "YAML native extension should parse");
  const sputnik::pkg::PackageNativeExtension &extension =
      parsed.manifest.native_extensions[0];
  expect(extension.sources.size() == 1 &&
             extension.sources[0] == "native/tiny.c",
         "YAML native sources should parse");
  expect(extension.library_dirs == std::vector<std::string>{"vendor/lib"} &&
             extension.runtime_library_dirs == std::vector<std::string>{"@loader_path/lib"},
         "native link and runtime directories");
  expect(extension.symbols.size() == 1 &&
             extension.symbols[0].logical == "tiny.answer" &&
             extension.symbols[0].symbol == "tiny_answer",
         "YAML native symbols should parse");
  expect(extension.blocking_symbols.size() == 1 &&
             extension.blocking_symbols[0] == "tiny.wait",
         "YAML blocking native symbols should parse");

  const std::string obsolete =
      "schema: sputnik.build.v1\n"
      "name: obsolete\n"
      "root: obsolete.main\n"
      "profiles:\n"
      "  required: [core.v1, ffi.v1]\n"
      "modules:\n"
      "  - name: obsolete.main\n"
      "    path: main.s\n"
      "native_extensions:\n"
      "  - name: tiny\n"
      "    language: c\n"
      "    capabilities: [ffi]\n";
  const sputnik::build::BuildManifestResult rejected =
      sputnik::build::parse_build_manifest_yaml(obsolete, "sputnik.build.yaml");
  expect(!rejected.ok(),
         "YAML native extension capabilities should be rejected");
}

void test_path_dependencies() {
  const auto settings = sputnik::build::parse_build_configuration(R"JSON({"dependencies":{"local":{"path":"../local"}}})JSON");
  expect(settings.ok() && settings.manifest.modules.empty(), "source configuration needs no source inventory or entry");
  const auto entry = sputnik::build::parse_build_manifest_json(R"JSON({"entry":"backend/main.s"})JSON");
  expect(entry.ok() && entry.manifest.entry == "backend/main.s", "explicit build entry needs no root or name");
  expect(sputnik::build::parse_build_manifest_json(sputnik::build::manifest_to_json(entry.manifest)).ok(), "entry JSON roundtrips");
  for (const auto *value : {"null", "false", "[]", "\"\""})
    expect(!sputnik::build::parse_build_configuration(std::string("{\"entry\":") + value + "}").ok(), "invalid entry rejected");
  const auto parsed = sputnik::build::parse_build_manifest_json(R"JSON({
    "schema":"sputnik.build.v1", "name":"app", "root":"app",
    "modules":[{"name":"app","path":"backend/app.s"}],
    "dependencies":{"local":{"path":"../local checkout","version":"1.2.3"},"installed":"2.0.0"}
  })JSON");
  expect(parsed.ok(), "path dependency parses beside an exact installed version");
  const auto again = sputnik::build::parse_build_manifest_json(sputnik::build::manifest_to_json(parsed.manifest));
  expect(again.ok(), "path dependency JSON roundtrip");
  bool local = false;
  for (const auto &dependency : again.manifest.dependencies)
    if (dependency.name == "local") local = dependency.source == "path" && dependency.path == "../local checkout" && dependency.version == "1.2.3";
  expect(local, "roundtrip preserves path and optional version");
  const auto yaml = sputnik::build::parse_build_manifest_yaml(
      "schema: sputnik.build.v1\nname: app\nroot: app\nmodules:\n  - name: app\n    path: app.s\ndependencies:\n  local: {path: ../local}\n");
  expect(yaml.ok() && yaml.manifest.dependencies.front().version.empty(), "YAML path needs no version");
  for (const auto &value : {"{}", "{\"path\":\"\"}", "{\"path\":42}", "{\"path\":\".\",\"typo\":true}"}) {
    const auto invalid = sputnik::build::parse_build_manifest_json(
        std::string("{\"schema\":\"sputnik.build.v1\",\"name\":\"app\",\"root\":\"app\",\"modules\":[{\"name\":\"app\",\"path\":\"app.s\"}],\"dependencies\":{\"local\":") + value + "}}");
    expect(!invalid.ok(), "malformed path dependency rejected");
  }
}

void test_manifest_rejects_profile_conflict() {
  const std::string source =
      "{\"schema\":\"sputnik.build.v1\",\"name\":\"bad\",\"root\":\"bad.main\","
      "\"profiles\":{\"required\":[\"core.v1\"],"
      "\"forbidden\":[\"core.v1\"]},"
      "\"modules\":[{\"name\":\"bad.main\",\"path\":\"main.s\"}]}";
  const sputnik::build::BuildManifestResult parsed =
      sputnik::build::parse_build_manifest_json(source, "bad.json");
  expect(!parsed.ok(), "conflicting profile manifest should fail");
  expect(parsed.diagnostics[0].message.find("both required and forbidden") !=
             std::string::npos,
         "conflict diagnostic should name profile conflict");
}

void test_runtime_feature_support_surface() {
  expect(sputnik::build::runtime_supports_feature("core.v1"),
         "runtime should support core.v1");
  expect(!sputnik::build::runtime_supports_feature("ffi.v1"),
         "runtime should not support ffi.v1 by default");
  // macro.v1 is an opt-in build profile (DESIGN-macro-system §14). Post-
  // expansion bytecode carries no macro nodes, so the loader must accept a
  // module that stamps macro.v1 in its PROF metadata.
  expect(sputnik::build::runtime_supports_feature("macro.v1"),
         "runtime should support macro.v1");
  const sputnik::build::BuildProfileSet macro_profiles = {{"macro.v1"}, {}, {}};
  expect((sputnik::build::profile_flags_for(macro_profiles) & (1U << 16U)) != 0U,
         "macro.v1 profile flag should be set");
}

void test_native_extensions_parse_and_gate() {
  const std::string source =
      "{\"schema\":\"sputnik.build.v1\",\"name\":\"crypto\","
      "\"root\":\"crypto.blake3\","
      "\"profiles\":{\"required\":[\"core.v1\",\"ffi.v1\"]},"
      "\"modules\":[{\"name\":\"crypto.blake3\",\"path\":\"src/blake3.s\"}],"
      "\"native_extensions\":[{"
      "\"name\":\"blake3\",\"language\":\"c\","
      "\"sources\":[\"native/blake3.c\"],"
      "\"library_dirs\":[\"vendor/lib\"],"
      "\"runtime_library_dirs\":[\"@loader_path/lib\"],"
      "\"blocking_symbols\":[\"blake3.wait\"],"
      "\"symbols\":[{\"logical\":\"blake3.hash\","
      "\"symbol\":\"sputnik_blake3_hash\"}],"
      "\"types\":[{\"sputnik\":\"crypto.blake3.Hasher\",\"tag\":\"blake3.Hasher\","
      "\"ownership\":\"owned\",\"destructor\":\"blake3.hasher_free\"}],"
      "\"errors\":[{\"name\":\"crypto.blake3.HashError\","
      "\"parent\":\"NativeError\",\"default_message\":\"hash failed\"}]"
      "}]}";
  const sputnik::build::BuildManifestResult parsed =
      sputnik::build::parse_build_manifest_json(source, "sputnik.build.json");
  expect(parsed.ok(), sputnik::build::diagnostics_to_string(parsed.diagnostics));
  expect(parsed.manifest.native_extensions.size() == 1,
         "one native extension should parse");
  const sputnik::pkg::PackageNativeExtension &extension =
      parsed.manifest.native_extensions[0];
  expect(extension.name == "blake3" && extension.language == "c",
         "native extension name/language");
  expect(extension.sources.size() == 1 &&
             extension.sources[0] == "native/blake3.c",
         "native extension sources");
  expect(extension.blocking_symbols.size() == 1 &&
             extension.blocking_symbols[0] == "blake3.wait",
         "native extension blocking symbols");
  expect(extension.library_dirs == std::vector<std::string>{"vendor/lib"} &&
             extension.runtime_library_dirs == std::vector<std::string>{"@loader_path/lib"},
         "native link and runtime directories");
  expect(extension.symbols.size() == 1 &&
             extension.symbols[0].symbol == "sputnik_blake3_hash",
         "native extension symbol map");
  expect(extension.types.size() == 1 &&
             extension.types[0].tag == "blake3.Hasher" &&
             extension.types[0].ownership == "owned",
         "native extension type tag/ownership");
  expect(extension.errors.size() == 1 &&
             extension.errors[0].name == "crypto.blake3.HashError" &&
             extension.errors[0].parent == "NativeError",
         "native extension error descriptor");

  const std::string obsolete_extension_caps =
      "{\"schema\":\"sputnik.build.v1\",\"name\":\"crypto\","
      "\"root\":\"crypto.blake3\","
      "\"profiles\":{\"required\":[\"core.v1\",\"ffi.v1\"]},"
      "\"modules\":[{\"name\":\"crypto.blake3\",\"path\":\"src/blake3.s\"}],"
      "\"native_extensions\":[{\"name\":\"blake3\",\"language\":\"c\","
      "\"capabilities\":[\"ffi\"]}]}";
  const sputnik::build::BuildManifestResult obsolete =
      sputnik::build::parse_build_manifest_json(obsolete_extension_caps,
                                              "sputnik.build.json");
  expect(!obsolete.ok(),
         "native extension capabilities should be rejected as obsolete");

  // Without ffi.v1 the same manifest is rejected.
  const std::string ungated =
      "{\"schema\":\"sputnik.build.v1\",\"name\":\"crypto\","
      "\"root\":\"crypto.blake3\","
      "\"profiles\":{\"required\":[\"core.v1\"]},"
      "\"modules\":[{\"name\":\"crypto.blake3\",\"path\":\"src/blake3.s\"}],"
      "\"native_extensions\":[{\"name\":\"blake3\",\"language\":\"c\"}]}";
  const sputnik::build::BuildManifestResult gated =
      sputnik::build::parse_build_manifest_json(ungated, "sputnik.build.json");
  expect(!gated.ok(), "native extensions without ffi.v1 should be rejected");
  bool mentions_ffi = false;
  for (const sputnik::build::BuildDiagnostic &diagnostic : gated.diagnostics) {
    mentions_ffi =
        mentions_ffi || diagnostic.message.find("ffi.v1") != std::string::npos;
  }
  expect(mentions_ffi, "gating diagnostic should name the ffi.v1 requirement");
}

void test_source_format_registry_roundtrip() {
  const auto parsed = sputnik::build::parse_build_manifest_json(R"JSON({
    "schema":"sputnik.build.v1", "name":"app", "root":"app",
    "source_formats":[{"suffix":".cmd.sv","module":"system","macro":"cmd"}],
    "modules":[{"name":"app","path":"main.s"}]
  })JSON");
  expect(parsed.ok(), "source-format registry parses");
  expect(parsed.manifest.source_formats.size() == 1, "registry retained");
  const auto again = sputnik::build::parse_build_manifest_json(
      sputnik::build::manifest_to_json(parsed.manifest));
  expect(again.ok() && again.manifest.source_formats.size() == 1 &&
      again.manifest.source_formats[0].suffix == ".cmd.sv", "source-format registry roundtrips");
}

void test_package_inventory_and_metadata() {
  const auto parsed = sputnik::build::parse_build_manifest_yaml(R"yaml(
name: demo-library
version: "1.2.3"
profiles: {numeric: {int: BigInt, overflow: checked}}
source_root: src
files: [src/demo/value.s, src/odd.s, LICENSE]
module_names: {src/odd.s: demo.extra}
dependencies: {other: "2.0.0"}
capabilities:
  fs.read: ["./data"]
  time.now: true
)yaml");
  expect(parsed.ok(), "file-based library needs no root module or entry: " + sputnik::build::diagnostics_to_string(parsed.diagnostics));
  expect(parsed.manifest.modules.size() == 2 && parsed.manifest.modules[0].name == "demo.value", "module names derive from source-root-relative paths");
  const auto m = sputnik::build::package_manifest(parsed.manifest);
  expect(m.version == "1.2.3" && m.files.size() == 3 && m.capabilities.size() == 2, "version, inventory and capability requests retained");
  const auto roundtrip = sputnik::build::parse_build_manifest_json(sputnik::build::manifest_to_json(parsed.manifest));
  expect(roundtrip.ok() && roundtrip.manifest.modules.size() == 2 && roundtrip.manifest.package.capabilities.size() == 2 && roundtrip.manifest.package.version == "1.2.3" && roundtrip.manifest.profiles.numeric_int == "BigInt", "complete package metadata roundtrips through build JSON");
  const auto absolute = sputnik::build::parse_build_manifest_json(R"JSON({"name":"custom","root":"custom","modules":[{"name":"custom","path":"/tmp/custom.s"}]})JSON");
  const auto absolute_again = sputnik::build::parse_build_manifest_json(sputnik::build::manifest_to_json(absolute.manifest));
  expect(absolute_again.ok() && absolute_again.manifest.modules[0].path == "/tmp/custom.s", "custom absolute build mappings remain supported");
  expect(!sputnik::build::parse_build_configuration("name: first\nname: second\n").ok(), "duplicate YAML metadata rejected");
  expect(!sputnik::build::parse_build_configuration("{\"name\":\"first\",\"name\":\"second\"}").ok(), "duplicate JSON metadata rejected");
  expect(!sputnik::build::parse_build_configuration("source_root: ../outside\n").ok(), "escaping source root rejected");
  expect(!sputnik::build::parse_build_configuration("files: [src/demo.s, src/demo.s]\n").ok(), "duplicate file rejected");
}

} // namespace

int main() {
  test_package_inventory_and_metadata();
  test_path_dependencies();
  test_source_format_registry_roundtrip();
  test_manifest_parses_and_normalizes();
  test_yaml_manifest_parses_and_normalizes();
  test_manifest_rejects_profile_conflict();
  test_runtime_feature_support_surface();
  test_native_extensions_parse_and_gate();
  return 0;
}
