#pragma once
#include <string>

namespace sputnik::frontend {
// Bridge imports may use an unscoped provider or one publisher namespace.
// The foreign library suffix never becomes a Sputnik module dependency.
inline std::string foreign_import_provider(const std::string &path) {
  for (const auto *bridge : {"pycall", "rbcall"}) {
    const std::string prefix = std::string(bridge) + ".";
    if (path.rfind(prefix, 0) == 0) return bridge;
    const auto dot = path.find('.');
    if (dot != std::string::npos && path.compare(dot + 1, prefix.size(), prefix) == 0)
      return path.substr(0, dot + 1) + bridge;
  }
  return {};
}
}
