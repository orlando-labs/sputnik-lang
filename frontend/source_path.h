#pragma once
#include <string_view>

namespace sputnik {
inline bool is_source_path(std::string_view path) {
  const auto dot = path.find_last_of('.');
  if (dot == std::string_view::npos) return false;
  const auto suffix = path.substr(dot);
  return suffix == ".s" || suffix == ".spu" || suffix == ".sputnik";
}
} // namespace sputnik
