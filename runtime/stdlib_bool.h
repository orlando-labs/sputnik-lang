#pragma once

#include <string>
#include <string_view>

namespace sputnik::runtime {

// Shared by Bool.parse and both ArgParser execution paths. Failure leaves
// the output untouched; callers must report an error rather than use it.
inline bool parse_bool_text(std::string_view text, bool *out) {
  const auto is_space = [](char ch) {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' ||
           ch == '\v';
  };
  while (!text.empty() && is_space(text.front())) {
    text.remove_prefix(1);
  }
  while (!text.empty() && is_space(text.back())) {
    text.remove_suffix(1);
  }
  if (text.empty() || text.size() > 5) {
    return false;
  }
  std::string lowered(text);
  for (char &ch : lowered) {
    if (ch >= 'A' && ch <= 'Z') {
      ch = static_cast<char>(ch - 'A' + 'a');
    }
  }
  if (lowered == "true" || lowered == "t" || lowered == "1" ||
      lowered == "yes" || lowered == "on") {
    *out = true;
    return true;
  }
  if (lowered == "false" || lowered == "f" || lowered == "0" ||
      lowered == "no" || lowered == "off" || lowered == "null") {
    *out = false;
    return true;
  }
  return false;
}

} // namespace sputnik::runtime
