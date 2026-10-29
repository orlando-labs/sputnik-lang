#pragma once

#include "profile/capabilities.h"

#include <cstdint>
#include <string>
#include <map>
#include <vector>

namespace sputnik::pkg {

struct PackageDiagnostic {
  std::string error_name;
  std::string message;
  std::string path;
};

struct PackageDependency {
  std::string name;
  std::string version;
  std::string source = "registry";
  std::string checksum;
  std::string path;
};

struct PackageModule {
  std::string name;
  std::string path;
  std::string representation = "source";
};

struct PackageNativeLibrary {
  std::string path;
  std::string role = "dependency";
  std::string abi;
};

struct PackageNativeRequirement {
  std::string id, kind, version, abi, build_id;
  std::string provider = "external";
  std::string runtime, package;
  std::vector<std::string> libraries;
};

struct PackageNativeProvider {
  std::string id, kind, version, abi, build_id, runtime_build_id;
  std::vector<std::string> include_dirs, library_dirs, link_libraries, files;
  std::vector<PackageNativeLibrary> libraries;
};

struct PackageTarget {
  std::string os = "any";
  std::string arch = "any";
  std::string min_os;
  std::string libc;
  std::string min_libc;
  std::string compute_backend = "none";
  std::vector<std::string> cpu_features;
  std::vector<std::string> compute_backends;
  std::string cuda_version;
  std::string min_cuda_driver;
  std::vector<std::string> cuda_architectures;
};

// A `[native.symbols]` entry: the logical name a `native def`/`native class`
// declares in source (`from "logical"`), mapped to the physical C symbol.
struct PackageNativeSymbol {
  std::string logical;
  std::string symbol;
};

// A `[[native.types]]` entry: the foreign-handle dispatch identity for a
// `native class` (the per-(package,type) tag) plus its ownership and reclaim.
struct PackageNativeType {
  std::string sputnik;
  std::string tag;
  std::string ownership;
  std::string destructor;
};

// A `[[native.errors]]` entry: a package-owned runtime error class registered
// into the active runtime error registry so typed rescue can match it.
struct PackageNativeError {
  std::string name;
  std::string parent;
  std::string default_message;
  std::string default_exit_code;
};

// A `[[native]]` extension unit: the build facts behind a package's native
// bindings (sources/flags/libs), the logical-name->symbol map, and the
// foreign-handle type table. The Sputnik surface declares which defs are native;
// this carries only how to build the native side (native-packages design §5).
struct PackageNativeExtension {
  std::string name;
  std::string language;
  std::string kind = "source";
  std::string library;
  std::uint32_t sputnik_ext_abi_version = 1;
  std::vector<std::string> required_runtime_features;
  std::vector<std::string> requirements;
  // Derived local preparation facts; never serialized in a distribution.
  std::vector<std::string> provider_fingerprints;
  bool provider_prepared = false;
  std::vector<PackageNativeLibrary> libraries;
  std::vector<std::string> sources;
  std::vector<std::string> headers;
  std::vector<std::string> include_dirs;
  std::vector<std::string> defines;
  std::vector<std::string> cxxflags;
  std::vector<std::string> link_libraries;
  // Manifest-relative library search paths and runtime loader paths.
  std::vector<std::string> library_dirs;
  std::vector<std::string> runtime_library_dirs;
  // Logical thunk names whose implementation may block the calling OS thread.
  // The VM parks its strand and dispatches these calls through the blocking
  // FFI executor; generated native code invokes them outside the world bridge
  // lock.
  std::vector<std::string> blocking_symbols;
  std::vector<PackageNativeSymbol> symbols;
  std::vector<PackageNativeType> types;
  std::vector<PackageNativeError> errors;
};

struct PackageProfileSet {
  std::vector<std::string> required_features;
  std::vector<std::string> optional_features;
  std::vector<std::string> forbidden_features;
  // sputnik.numeric-profile.v1 manifest mirror (profiles.numeric). Empty means
  // unspecified: source preambles (or the Int64/checked defaults) apply.
  std::string numeric_int;
  std::string numeric_overflow;
};

struct PackageManifest {
  std::string name;
  std::string version;
  std::string root_module;
  PackageProfileSet profiles;
  std::string source_root = ".";
  bool explicit_files = false;
  std::map<std::string, std::string> module_names;
  std::vector<PackageModule> modules;
  std::vector<PackageDependency> dependencies;
  std::vector<capability::CapabilityRequest> capabilities;
  std::vector<PackageNativeExtension> native_extensions;
  std::string schema;
  std::string variant = "source";
  std::string limit_profile = "standard";
  PackageTarget target;
  std::vector<std::string> files;
  std::vector<PackageDependency> artifact_dependencies;
  std::string kind = "library";
  std::string bundle = "none";
  std::vector<PackageDependency> artifact_build_dependencies;
  std::vector<PackageNativeRequirement> native_requirements;
  std::vector<PackageNativeProvider> native_providers;
  // Derived registry metadata when module payloads have not been downloaded.
  bool has_bytecode_delivery = false;
};

struct PackageManifestResult {
  PackageManifest manifest;
  std::vector<PackageDiagnostic> diagnostics;

  bool ok() const { return diagnostics.empty(); }
};

struct PackageModuleBlob {
  std::string name;
  std::string path;
  std::string digest;
  std::vector<std::uint8_t> bytes;
};

struct PackageNativeBlob {
  std::string extension_name;
  std::string kind; // "source" or "header"
  std::string path;
  std::string digest;
  std::vector<std::uint8_t> bytes;
};

struct PackageNativeExtensionMetadata {
  std::string name;
  std::uint32_t sputnik_ext_abi_version = 1;
  std::string target_triple;
  std::string native_source_digest;
  std::string exported_symbol_digest;
  std::vector<PackageNativeType> types;
  std::vector<PackageNativeError> errors;
};

struct PackageSignature {
  std::string algorithm;
  std::string key_id;
  std::string digest;
};

struct PackageArtifact {
  PackageManifest manifest;
  std::string manifest_digest;
  std::string lockfile;
  std::string lock_digest;
  std::vector<PackageModuleBlob> modules;
  std::vector<PackageNativeBlob> native_blobs;
  std::vector<PackageNativeExtensionMetadata> native_extensions;
  PackageSignature signature;
};

struct PackageBuildOptions {
  std::string key_id;
  std::string signing_key;
  std::string target_triple;
  std::vector<PackageNativeBlob> native_blobs;
};

struct PackageBuildResult {
  bool ok = false;
  std::string serialized;
  PackageArtifact artifact;
  std::vector<PackageDiagnostic> diagnostics;
};

struct PackageParseResult {
  PackageArtifact artifact;
  std::vector<PackageDiagnostic> diagnostics;

  bool ok() const { return diagnostics.empty(); }
};

struct PackageVerifyResult {
  bool ok = false;
  bool structurally_valid = false;
  bool signature_present = false;
  bool signature_checked = false;
  bool signature_valid = false;
  std::string package_name;
  std::string version;
  std::string root_module;
  std::string digest;
  std::vector<PackageDiagnostic> diagnostics;
};

struct PackageRegistryResult {
  bool ok = false;
  std::string package_name;
  std::string version;
  std::string installed_path;
  std::vector<PackageDiagnostic> diagnostics;
};

PackageManifestResult parse_manifest(const std::string &source,
                                     const std::string &path = {});

std::string manifest_to_json(const PackageManifest &manifest);
std::string render_manifest(const PackageManifest &manifest);
std::string render_lockfile(const PackageManifest &manifest);
std::vector<PackageNativeExtensionMetadata>
native_extension_metadata(const std::vector<PackageNativeExtension> &extensions,
                          const std::vector<PackageNativeBlob> &blobs,
                          const std::string &target_triple = {});

PackageBuildResult
build_package_artifact(const PackageManifest &manifest,
                       const std::vector<PackageModuleBlob> &modules,
                       const PackageBuildOptions &options = {});

PackageParseResult parse_package_artifact(const std::string &serialized,
                                          const std::string &path = {});
PackageVerifyResult verify_package_artifact(const std::string &serialized,
                                            const std::string &signing_key = {},
                                            const std::string &path = {});

PackageRegistryResult
install_package_artifact(const std::string &serialized,
                         const std::string &registry_root,
                         const std::string &signing_key = {});
PackageRegistryResult
publish_package_artifact(const std::string &serialized,
                         const std::string &registry_root,
                         const std::string &signing_key = {});

std::string artifact_to_json(const PackageArtifact &artifact);
std::string verify_result_to_json(const PackageVerifyResult &result);
std::string registry_result_to_json(const PackageRegistryResult &result);

} // namespace sputnik::pkg
