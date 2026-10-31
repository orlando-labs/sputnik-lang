#pragma once
#include "package/package.h"
#include <set>
#include <string>
#include <vector>

namespace sputnik::pkg {
struct PackageHost {
  std::string sputnik_version = SPUTNIK_VERSION;
  std::string os, arch, os_version, libc, libc_version, cuda_driver, cuda_api_version;
  std::set<std::string> cpu_features, cuda_architectures;
  bool mps = false;
  bool rocm = false;
};
struct VariantPolicy {
  std::string variant;
  std::string backend = "auto";
  std::string bundle = "auto";
  bool allow_native = false;
  bool build_from_source = false;
  std::vector<PackageNativeRequirement> native_requirements;
};
PackageHost probe_package_host(bool include_accelerators = false);
bool package_gpu_backend(const std::string &backend);
void validate_package_target(const PackageTarget &target);
bool package_has_native(const PackageManifest &manifest);
// Empty means compatible. Does not load package libraries or compile code.
std::string package_incompatibility(const PackageManifest &manifest,
                                    const PackageHost &host,
                                    const VariantPolicy &policy,
                                    bool execution = false);
// Candidates must belong to the same release. Returns -1 when none is eligible;
// incomparable/equivalent best selectors require an explicit variant.
int select_package_variant(const std::vector<PackageManifest> &candidates,
                           const PackageHost &host, const VariantPolicy &policy,
                           bool execution = false);
} // namespace sputnik::pkg
