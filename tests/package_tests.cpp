#include "package/package.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "package test failed: " << message << "\n";
    std::exit(1);
  }
}

sputnik::pkg::PackageManifest sample_manifest() {
  const std::string manifest_source = "[package]\n"
                                      "name = \"demo.pkg\"\n"
                                      "version = \"0.1.0\"\n"
                                      "root = \"demo.core\"\n"
                                      "\n"
                                      "[[modules]]\n"
                                      "name = \"demo.extra\"\n"
                                      "path = \"src/extra.s\"\n"
                                      "\n"
                                      "[[modules]]\n"
                                      "name = \"demo.core\"\n"
                                      "path = \"src/core.s\"\n"
                                      "\n"
                                      "[dependencies]\n"
                                      "zeta.lib = \"2.0.0\"\n"
                                      "alpha.lib = \"1.0.0\"\n";

  sputnik::pkg::PackageManifestResult parsed =
      sputnik::pkg::parse_manifest_toml(manifest_source, "sputnik.toml");
  expect(parsed.ok(), "sample manifest should parse");
  return parsed.manifest;
}

std::vector<sputnik::pkg::PackageModuleBlob> sample_modules_reversed() {
  sputnik::pkg::PackageModuleBlob extra;
  extra.name = "demo.extra";
  extra.path = "src/extra.s";
  extra.bytes = {0xAA, 0xBB, 0xCC};

  sputnik::pkg::PackageModuleBlob core;
  core.name = "demo.core";
  core.path = "src/core.s";
  core.bytes = {0x01, 0x02, 0x03, 0x04};
  return {extra, core};
}

std::vector<sputnik::pkg::PackageModuleBlob> sample_modules_sorted() {
  std::vector<sputnik::pkg::PackageModuleBlob> modules =
      sample_modules_reversed();
  std::swap(modules[0], modules[1]);
  return modules;
}

void test_manifest_and_lock_are_deterministic() {
  const sputnik::pkg::PackageManifest manifest = sample_manifest();
  expect(manifest.modules.size() == 2, "manifest should expose two modules");
  expect(manifest.modules[0].name == "demo.core",
         "manifest modules should be sorted by module name");
  expect(manifest.dependencies.size() == 2,
         "manifest should expose two dependencies");
  expect(manifest.dependencies[0].name == "alpha.lib",
         "dependencies should be sorted by name");

  const std::string lockfile = sputnik::pkg::render_lockfile(manifest);
  const std::size_t alpha = lockfile.find("alpha.lib");
  const std::size_t zeta = lockfile.find("zeta.lib");
  expect(alpha != std::string::npos && zeta != std::string::npos &&
             alpha < zeta,
         "lockfile dependencies should be deterministic");
  expect(lockfile.find("checksum = \"sha256:") != std::string::npos,
         "lockfile should include dependency checksums");
}

void test_capability_manifest_and_policy_resolution() {
  const std::string manifest_source = "[package]\n"
                                      "name = \"caps.pkg\"\n"
                                      "version = \"0.1.0\"\n"
                                      "root = \"caps.core\"\n"
                                      "\n"
                                      "[[modules]]\n"
                                      "name = \"caps.core\"\n"
                                      "path = \"src/core.s\"\n"
                                      "\n"
                                      "[capabilities]\n"
                                      "fs.read = [\"./data\"]\n"
                                      "net.connect = [\"api.example:443\"]\n"
                                      "time = true\n"
                                      "ffi = false\n";
  const sputnik::pkg::PackageManifestResult parsed =
      sputnik::pkg::parse_manifest_toml(manifest_source, "sputnik.toml");
  expect(parsed.ok(), "capability manifest should parse");
  expect(parsed.manifest.capabilities.size() == 4,
         "capability manifest should canonicalize aliases");

  std::vector<sputnik::capability::CapabilityRequest> grants;
  grants.push_back(sputnik::capability::make_capability("fs.read", "./data"));
  grants.push_back(
      sputnik::capability::make_capability("net.connect", "api.example:443"));
  grants.push_back(sputnik::capability::make_capability(
      "time.now", "*", "host policy",
      sputnik::capability::kCapabilityFlagWildcardTarget));
  grants.push_back(sputnik::capability::make_capability(
      "time.sleep", "*", "host policy",
      sputnik::capability::kCapabilityFlagWildcardTarget));
  const sputnik::capability::CapabilityResolutionResult allowed =
      sputnik::capability::resolve_capabilities(parsed.manifest.capabilities,
                                              grants);
  expect(allowed.ok, "matching grants should satisfy requested capabilities");
  expect(sputnik::capability::capability_set_allows(allowed.effective, "fs.read",
                                                  "./data/orders.csv"),
         "fs.read grant should allow paths under target");
  expect(!sputnik::capability::capability_set_allows(allowed.effective, "fs.read",
                                                   "./private/orders.csv"),
         "fs.read grant should deny paths outside target");

  const sputnik::capability::CapabilityResolutionResult denied =
      sputnik::capability::resolve_capabilities(parsed.manifest.capabilities, {});
  expect(!denied.ok, "deny-by-default policy should reject missing grants");
  expect(denied.denied.size() == parsed.manifest.capabilities.size(),
         "all requested capabilities should be denied without grants");
}

void test_package_artifact_is_reproducible_and_signed() {
  const sputnik::pkg::PackageManifest manifest = sample_manifest();
  sputnik::pkg::PackageBuildOptions options;
  options.key_id = "ci";
  options.signing_key = "secret";

  const sputnik::pkg::PackageBuildResult first =
      sputnik::pkg::build_package_artifact(manifest, sample_modules_reversed(),
                                         options);
  const sputnik::pkg::PackageBuildResult second =
      sputnik::pkg::build_package_artifact(manifest, sample_modules_sorted(),
                                         options);
  expect(first.ok, "first package build should succeed");
  expect(second.ok, "second package build should succeed");
  expect(first.serialized == second.serialized,
         "package artifact should be reproducible independent of input order");

  const sputnik::pkg::PackageVerifyResult verified =
      sputnik::pkg::verify_package_artifact(first.serialized, "secret");
  expect(verified.ok, "signed package should verify with the right key");
  expect(verified.signature_present, "package signature should be present");
  expect(verified.signature_checked, "package signature should be checked");
  expect(verified.signature_valid, "package signature should be valid");

  const sputnik::pkg::PackageVerifyResult wrong_key =
      sputnik::pkg::verify_package_artifact(first.serialized, "wrong");
  expect(!wrong_key.ok, "signed package should reject the wrong key");
}

void test_registry_publish_and_install_smoke() {
  const sputnik::pkg::PackageManifest manifest = sample_manifest();
  sputnik::pkg::PackageBuildOptions options;
  options.key_id = "ci";
  options.signing_key = "secret";
  const sputnik::pkg::PackageBuildResult built =
      sputnik::pkg::build_package_artifact(manifest, sample_modules_reversed(),
                                         options);
  expect(built.ok, "package build should succeed for registry smoke");

  const std::filesystem::path root =
      std::filesystem::temp_directory_path() /
      ("sputnik_pkg_tests_" + std::to_string(getpid()));
  std::error_code error;
  std::filesystem::remove_all(root, error);

  const sputnik::pkg::PackageRegistryResult published =
      sputnik::pkg::publish_package_artifact(built.serialized, root.string(),
                                           "secret");
  expect(published.ok, "first publish should succeed");
  expect(std::filesystem::exists(published.installed_path),
         "publish should write the package artifact");

  const sputnik::pkg::PackageRegistryResult duplicate =
      sputnik::pkg::publish_package_artifact(built.serialized, root.string(),
                                           "secret");
  expect(!duplicate.ok, "duplicate publish should fail");

  const sputnik::pkg::PackageRegistryResult installed =
      sputnik::pkg::install_package_artifact(built.serialized, root.string(),
                                           "secret");
  expect(installed.ok, "install should accept the already cached artifact");

  std::filesystem::remove_all(root, error);
}

} // namespace

void test_native_extension_manifest() {
  const std::string source =
      "[package]\n"
      "name = \"crypto.blake3\"\n"
      "version = \"0.1.0\"\n"
      "root = \"crypto.blake3\"\n"
      "\n"
      "[[modules]]\n"
      "name = \"crypto.blake3\"\n"
      "path = \"src/blake3.s\"\n"
      "\n"
      "[[native]]\n"
      "name = \"blake3\"\n"
      "language = \"c\"\n"
      "sources = [\"native/blake3.c\", \"native/sputnik_blake3.c\"]\n"
      "include_dirs = [\"native/include\"]\n"
      "cxxflags = [\"-O3\"]\n"
      "library_dirs = [\"vendor/lib\"]\n"
      "runtime_library_dirs = [\"@loader_path/lib\"]\n"
      "blocking_symbols = [\"blake3.wait\"]\n"
      "\n"
      "[native.symbols]\n"
      "\"blake3.hash\" = \"sputnik_blake3_hash\"\n"
      "\"blake3.hasher_free\" = \"sputnik_blake3_hasher_free\"\n"
      "\n"
      "[[native.types]]\n"
      "sputnik = \"crypto.blake3.Hasher\"\n"
      "tag = \"blake3.Hasher\"\n"
      "ownership = \"owned\"\n"
      "destructor = \"blake3.hasher_free\"\n"
      "\n"
      "[[native.errors]]\n"
      "name = \"crypto.blake3.HashError\"\n"
      "parent = \"NativeError\"\n"
      "default_message = \"hash failed\"\n";

  sputnik::pkg::PackageManifestResult parsed =
      sputnik::pkg::parse_manifest_toml(source, "sputnik.toml");
  expect(parsed.ok(), "native manifest should parse");
  expect(parsed.manifest.native_extensions.size() == 1,
         "one native extension parsed");
  const sputnik::pkg::PackageNativeExtension &ext =
      parsed.manifest.native_extensions[0];
  expect(ext.name == "blake3" && ext.language == "c", "native ext name/lang");
  expect(ext.sources.size() == 2 && ext.sources[0] == "native/blake3.c",
         "native sources string array");
  expect(ext.include_dirs.size() == 1 && ext.cxxflags.size() == 1 &&
             ext.cxxflags[0] == "-O3",
         "native include_dirs/cxxflags arrays");
  expect(ext.library_dirs == std::vector<std::string>{"vendor/lib"} &&
             ext.runtime_library_dirs == std::vector<std::string>{"@loader_path/lib"},
         "native library paths parsed");
  expect(ext.blocking_symbols.size() == 1 &&
             ext.blocking_symbols[0] == "blake3.wait",
         "native blocking symbols array");
  expect(ext.symbols.size() == 2 && ext.symbols[0].logical == "blake3.hash" &&
             ext.symbols[0].symbol == "sputnik_blake3_hash",
         "symbol map unquotes the logical key");
  expect(ext.types.size() == 1 && ext.types[0].tag == "blake3.Hasher" &&
             ext.types[0].ownership == "owned" &&
             ext.types[0].destructor == "blake3.hasher_free",
         "native type fields");
  expect(ext.errors.size() == 1 &&
             ext.errors[0].name == "crypto.blake3.HashError" &&
             ext.errors[0].parent == "NativeError" &&
             ext.errors[0].default_message == "hash failed",
         "native error fields");

  const std::string json = sputnik::pkg::manifest_to_json(parsed.manifest);
  expect(json.find("\"native_extensions\"") != std::string::npos &&
             json.find("sputnik_blake3_hash") != std::string::npos &&
             json.find("\"blocking_symbols\":[\"blake3.wait\"]") !=
                 std::string::npos &&
             json.find("\"tag\":\"blake3.Hasher\"") != std::string::npos &&
             json.find("\"name\":\"crypto.blake3.HashError\"") !=
                 std::string::npos,
         "manifest json includes native sections");

  // Native content folds deterministically into the package digest.
  sputnik::pkg::PackageModuleBlob blob;
  blob.name = "crypto.blake3";
  blob.path = "src/blake3.s";
  blob.bytes = {0x01, 0x02, 0x03};
  sputnik::pkg::PackageBuildOptions native_options;
  native_options.target_triple = "test-triple";
  native_options.native_blobs = {
      {"blake3", "source", "native/blake3.c", {}, {0x63, 0x31}},
      {"blake3", "source", "native/sputnik_blake3.c", {}, {0x63, 0x32}},
  };
  const sputnik::pkg::PackageBuildResult missing_blobs =
      sputnik::pkg::build_package_artifact(parsed.manifest, {blob});
  expect(!missing_blobs.ok, "declared native sources require package blobs");
  const sputnik::pkg::PackageBuildResult first =
      sputnik::pkg::build_package_artifact(parsed.manifest, {blob},
                                         native_options);
  const sputnik::pkg::PackageBuildResult second =
      sputnik::pkg::build_package_artifact(parsed.manifest, {blob},
                                         native_options);
  expect(first.ok && second.ok, "native package builds");
  expect(first.artifact.manifest_digest == second.artifact.manifest_digest,
         "native manifest digest is deterministic");
  expect(first.artifact.native_blobs.size() == 2,
         "native source blobs are stored in the package");
  expect(first.artifact.native_extensions.size() == 1 &&
             first.artifact.native_extensions[0].target_triple ==
                 "test-triple",
         "native extension metadata records target triple");
  const sputnik::pkg::PackageVerifyResult verified =
      sputnik::pkg::verify_package_artifact(first.serialized);
  expect(verified.ok, "native package verifies");
  const auto roundtrip = sputnik::pkg::parse_package_artifact(first.serialized);
  expect(roundtrip.ok() &&
             roundtrip.artifact.manifest.native_extensions[0].library_dirs == ext.library_dirs &&
             roundtrip.artifact.manifest.native_extensions[0].runtime_library_dirs == ext.runtime_library_dirs,
         "native library paths survive artifact roundtrip");
  auto changed_manifest = parsed.manifest;
  changed_manifest.native_extensions[0].runtime_library_dirs = {"@loader_path/other"};
  const auto changed = sputnik::pkg::build_package_artifact(changed_manifest, {blob}, native_options);
  expect(changed.ok && changed.artifact.manifest_digest != first.artifact.manifest_digest,
         "runtime library paths contribute to manifest digest");
  const std::string inspect = sputnik::pkg::artifact_to_json(first.artifact);
  expect(inspect.find("\"native_blobs\"") != std::string::npos &&
             inspect.find("\"native_source_sha256\"") != std::string::npos,
         "package json exposes native blobs and source digest");
  std::string tampered = first.serialized;
  const std::size_t native_bytes = tampered.find("bytes=6331");
  expect(native_bytes != std::string::npos,
         "serialized package should contain native source bytes");
  tampered.replace(native_bytes + std::string("bytes=").size(), 4, "6330");
  const sputnik::pkg::PackageVerifyResult tampered_verify =
      sputnik::pkg::verify_package_artifact(tampered);
  expect(!tampered_verify.ok,
         "native blob digest mismatch should be rejected");

  const std::string base =
      "[package]\nname = \"p\"\nversion = \"0.1.0\"\nroot = \"p\"\n"
      "[[modules]]\nname = \"p\"\npath = \"p.s\"\n";
  sputnik::pkg::PackageManifestResult bad_owner = sputnik::pkg::parse_manifest_toml(
      base + "[[native]]\nname = \"x\"\n[[native.types]]\nsputnik = \"P.T\"\n"
             "tag = \"x.T\"\nownership = \"weird\"\n",
      "sputnik.toml");
  expect(!bad_owner.ok(), "invalid native ownership is rejected");
  sputnik::pkg::PackageManifestResult no_dtor = sputnik::pkg::parse_manifest_toml(
      base + "[[native]]\nname = \"x\"\n[[native.types]]\nsputnik = \"P.T\"\n"
             "tag = \"x.T\"\nownership = \"owned\"\n",
      "sputnik.toml");
  expect(!no_dtor.ok(), "owned native type without destructor is rejected");
  sputnik::pkg::PackageManifestResult orphan = sputnik::pkg::parse_manifest_toml(
      base + "[native.symbols]\n\"a\" = \"b\"\n", "sputnik.toml");
  expect(!orphan.ok(), "native.symbols before [[native]] is rejected");
  sputnik::pkg::PackageManifestResult native_caps =
      sputnik::pkg::parse_manifest_toml(
          base + "[[native]]\nname = \"x\"\ncapabilities = [\"ffi\"]\n",
          "sputnik.toml");
  expect(!native_caps.ok(), "native extension capabilities are rejected");
}

int main() {
  test_native_extension_manifest();
  test_manifest_and_lock_are_deterministic();
  test_capability_manifest_and_policy_resolution();
  test_package_artifact_is_reproducible_and_signed();
  test_registry_publish_and_install_smoke();
  return 0;
}
