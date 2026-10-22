#pragma once
#include <filesystem>
#include <string>
#include "package/selection.h"

namespace sputnik::pkg {
inline constexpr const char *default_registry = "https://registry.sputnik-lang.online";
bool registry_reference(const std::string &name);
bool remote_registry(const std::string &registry);
std::filesystem::path registry_credentials(const std::string &origin);
int registry_command(int argc, char **argv);
struct RegistryDownload {
  std::filesystem::path path;
  std::string canonical_ref;
  std::string version;
};
// Destination is a caller-owned temporary directory. Credentials are bound to
// exactly one origin; redirects and metadata-selected origins are not followed.
RegistryDownload download_registry_package(const std::string &origin,
    const std::string &name, const std::string &version,
    const std::filesystem::path &destination,
    const std::filesystem::path &credentials = {},
    const VariantPolicy &policy = {}, const std::string &digest = {},
    bool allow_native_large = false);
} // namespace sputnik::pkg
