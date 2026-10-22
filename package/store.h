#pragma once

#include "package/package.h"
#include "package/selection.h"

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace sputnik::pkg {

struct DistributionInfo;

// One shared package home; an application's packages/ directory overrides it.
std::filesystem::path package_home();
std::string package_host_target();
bool version_less(const std::string &left, const std::string &right);

struct ResolvedPackageModule {
  std::string name;
  std::string path;
  std::string source;
  std::vector<std::uint8_t> bytecode;
  std::string package_artifact;
  std::string package_digest;
};

struct LocatedPackage {
  PackageManifest manifest;
  std::filesystem::path directory;
  std::filesystem::path artifact_path;
  std::string digest;
  std::shared_ptr<const PackageArtifact> artifact;
  std::shared_ptr<const DistributionInfo> distribution;
  std::string source_kind = "artifact", source = "local";
};

struct PackagePin {
  std::string version, variant, digest, source_kind, source;
};
std::map<std::string, PackagePin> read_package_lock(const std::filesystem::path &path);
void record_prepared_native(const LocatedPackage &package);
void verify_prepared_native(const LocatedPackage &package);

// Discovery reads metadata only. Resolving an import activates a whole package
// version, validates its bytes and exact dependencies, and never executes code.
class PackageResolver {
public:
  explicit PackageResolver(std::filesystem::path entry_directory,
                           std::filesystem::path home = {},
                           bool use_lock = true);
  std::optional<ResolvedPackageModule> resolve(const std::string &module);
  void require(const std::vector<PackageDependency> &dependencies);
  const std::vector<LocatedPackage> &selected() const { return selected_; }
  std::string lockfile() const;
  std::string search_description() const;
  void prefer_backend(const std::string &backend) { selection_.backend = backend; }
  void prefer_variant(const std::string &name, const std::string &variant) { variants_[name] = variant; }

private:
  struct Candidate {
    LocatedPackage package;
    int priority = 0;
  };
  std::filesystem::path entry_directory_, home_;
  bool discovered_ = false;
  bool locked_ = false;
  std::vector<Candidate> candidates_;
  std::vector<LocatedPackage> selected_;
  std::map<std::string, std::size_t> active_;
  std::map<std::string, PackagePin> pins_;
  PackageHost host_;
  bool cuda_probed_ = false;
  VariantPolicy selection_;
  std::map<std::string, std::string> variants_;
  std::map<std::string, PackageNativeRequirement> native_requirements_;
  std::map<std::string, PackageNativeProvider> native_providers_;
  std::map<std::string, std::string> requirements_;
  void discover();
  void read_lockfile();
  bool shadowed(const Candidate &candidate) const;
  void activate(std::size_t candidate);
  void require_one(const PackageDependency &dependency);
};

// Local registries use the existing
// <name>/<version>/<name>-<version>.sputnikpkg layout. These functions also
// accept a direct artifact path.
LocatedPackage read_package(const std::filesystem::path &path,
                            const std::string &signing_key = {});
LocatedPackage read_source_package(const std::filesystem::path &directory);
std::filesystem::path
find_registry_package(const std::filesystem::path &registry,
                      const std::string &name, const std::string &version = {},
                      const VariantPolicy &policy = {});
std::filesystem::path install_in_home(const std::filesystem::path &artifact,
                                      const std::filesystem::path &home,
                                      const std::string &signing_key = {},
                                      const std::string &origin = {},
                                      const std::function<void(const std::filesystem::path &)> &prepare = {});
// Remove installed version directories only. An omitted version selects every
// installed version of this package; source checkouts and dependencies remain.
std::vector<std::string> uninstall_from_home(const std::string &name,
                                             const std::filesystem::path &home,
                                             const std::string &version = {},
                                             const std::string &variant = {});

} // namespace sputnik::pkg
