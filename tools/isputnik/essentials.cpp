#include "tools/isputnik/session.h"
#include "tools/isputnik/dependencies.h"

#include <filesystem>
#include <optional>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

namespace {
std::optional<std::filesystem::path> executable_directory() {
#if defined(__APPLE__)
  std::uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::string path(size, '\0');
  if (size && _NSGetExecutablePath(path.data(), &size) == 0)
    return std::filesystem::weakly_canonical(path.c_str()).parent_path();
#elif defined(__linux__)
  char path[4096];
  const auto size = readlink("/proc/self/exe", path, sizeof(path));
  if (size > 0 && static_cast<std::size_t>(size) < sizeof(path))
    return std::filesystem::path(std::string(path, size)).parent_path();
#endif
  return std::nullopt;
}
}

std::vector<BundledModuleSource> notebook_essential_sources() {
  namespace fs = std::filesystem;
  std::vector<fs::path> roots;
  if (const auto executable = executable_directory()) {
    roots.push_back(*executable / "../Resources/notebook-essentials");
    roots.push_back(*executable / "../share/sputnik-notebook/essentials");
  }
#ifdef SPUTNIK_NOTEBOOK_ESSENTIALS_DIR
  roots.emplace_back(SPUTNIK_NOTEBOOK_ESSENTIALS_DIR);
#endif
  for (const auto &root : roots) {
    if (!fs::is_directory(root)) continue;
    return load_directory_dependency({}, {root.string(), false, {}});
  }
  return {};
}
