#pragma once

#include "package/package.h"

#include <filesystem>
#include <functional>
#include <map>
#include <set>

namespace sputnik::pkg {

// The public .sputnikpkg distribution format: ordinary deterministic tar.gz.
// The compiler callback is deliberately absent from verification/inspection.
struct DistributionBuildOptions {
  std::string module_mode = "source";
  std::set<std::string> bytecode_modules;
  std::string variant;
  // SHA-256 of the producing compiler executable, or "unspecified" for API
  // users.
  std::string toolchain_revision = "unspecified";
  std::function<std::vector<std::uint8_t>(const PackageModule &)> compile;
};

struct DistributionMember {
  std::string path;
  std::string sha256;
  std::uint64_t size = 0;
};

struct DistributionInfo {
  PackageManifest manifest;
  std::vector<DistributionMember> members;
  std::string sha256;
  std::uint64_t compressed_size = 0;
  std::uint64_t expanded_size = 0;
  std::size_t source_modules = 0;
  std::size_t bytecode_modules = 0;
};

DistributionInfo
build_distribution(const PackageManifest &manifest,
                   const std::filesystem::path &root,
                   const std::filesystem::path &output,
                   const DistributionBuildOptions &options = {});
DistributionInfo inspect_distribution(const std::filesystem::path &file);
// Inspect an already-open regular seekable file, preserving its current offset.
// The caller retains ownership; no path lookup is performed for archive input.
DistributionInfo inspect_distribution_fd(int fd);
// Require the admitted profile before reading any payload beyond the manifest.
// The archive cannot increase the caller's compressed/member/expanded budgets.
DistributionInfo inspect_distribution_fd(int fd, const std::string &required_profile);
// Extract only after verification; destination must not exist. No code loads.
DistributionInfo extract_distribution(const std::filesystem::path &file,
                                      const std::filesystem::path &destination);
std::string distribution_to_json(const DistributionInfo &info);
std::string render_distribution_manifest(const PackageManifest &manifest);
std::string file_sha256(const std::filesystem::path &file);

// Explicit local provider descriptors are colon-separated directories in
// SPUTNIK_NATIVE_PROVIDERS. All declared bytes are checked before preparation.
// No network, prefix search, dependency programs or library loading occurs.
std::vector<PackageNativeExtension>
prepare_native_providers(const PackageManifest &manifest,
                         const std::filesystem::path &root,
                         const std::filesystem::path &cache);

} // namespace sputnik::pkg
