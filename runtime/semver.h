#pragma once

#include "runtime/version.h"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace sputnik::language {
struct Version {
  std::uint64_t major = 0, minor = 0, patch = 0;
  std::vector<std::string> prerelease;
};

inline bool numeric(const std::string &s) {
  return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) {
    return c >= '0' && c <= '9';
  });
}
inline bool number(const std::string &s, std::uint64_t &out) {
  if (!numeric(s) || (s.size() > 1 && s[0] == '0')) return false;
  out = 0;
  for (char c : s) {
    if (out > (std::numeric_limits<std::uint64_t>::max() - (c - '0')) / 10) return false;
    out = out * 10 + (c - '0');
  }
  return true;
}
inline bool identifiers(const std::string &s, std::vector<std::string> &out,
                        bool prerelease) {
  if (s.empty() || s.back() == '.') return false;
  std::istringstream input(s);
  std::string part;
  while (std::getline(input, part, '.')) {
    if (part.empty() || !std::all_of(part.begin(), part.end(), [](unsigned char c) {
          return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                 (c >= 'A' && c <= 'Z') || c == '-';
        }) || (prerelease && numeric(part) && part.size() > 1 && part[0] == '0')) return false;
    out.push_back(part);
  }
  return true;
}
inline bool parse(const std::string &text, Version &out) {
  if (text.empty() || text.size() > 256) return false;
  out = {};
  auto core = text;
  const auto plus = core.find('+');
  if (plus != std::string::npos) {
    std::vector<std::string> build;
    if (!identifiers(core.substr(plus + 1), build, false)) return false;
    core.resize(plus);
  }
  const auto dash = core.find('-');
  if (dash != std::string::npos) {
    if (!identifiers(core.substr(dash + 1), out.prerelease, true)) return false;
    core.resize(dash);
  }
  const auto first = core.find('.'), second = core.find('.', first == std::string::npos ? core.size() : first + 1);
  return first != std::string::npos && second != std::string::npos &&
      number(core.substr(0, first), out.major) &&
      number(core.substr(first + 1, second - first - 1), out.minor) &&
      number(core.substr(second + 1), out.patch);
}
inline int compare(const Version &a, const Version &b) {
  if (a.major != b.major) return a.major < b.major ? -1 : 1;
  if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
  if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
  if (a.prerelease.empty() != b.prerelease.empty()) return a.prerelease.empty() ? 1 : -1;
  for (std::size_t i = 0; i < std::min(a.prerelease.size(), b.prerelease.size()); ++i) {
    const auto &x = a.prerelease[i], &y = b.prerelease[i];
    if (x == y) continue;
    const bool xn = numeric(x), yn = numeric(y);
    if (xn != yn) return xn ? -1 : 1;
    if (xn && x.size() != y.size()) return x.size() < y.size() ? -1 : 1;
    return x < y ? -1 : 1;
  }
  return a.prerelease.size() == b.prerelease.size() ? 0 : a.prerelease.size() < b.prerelease.size() ? -1 : 1;
}
struct Comparator { std::string op; Version version; };
using Clause = std::vector<Comparator>;
using Range = std::vector<Clause>;

// Exact SemVer, comparator intersections, ^/~ and || unions. Every branch
// must have an upper bound: a package cannot promise future major semantics.
inline bool parse_range(const std::string &text, Range &out) {
  if (text.empty() || text.size() > 1024) return false;
  out.clear();
  std::size_t start = 0;
  while (true) {
    const auto end = text.find("||", start);
    auto branch = text.substr(start, end == std::string::npos ? end : end - start);
    std::replace(branch.begin(), branch.end(), ',', ' ');
    std::istringstream input(branch);
    std::string token;
    Clause clause;
    bool upper = false;
    while (input >> token) {
      const auto split = token.find_first_not_of("<>=^~");
      if (split == std::string::npos) {
        std::string value;
        if (!(input >> value)) return false;
        token += value;
      }
      const auto at = token.find_first_not_of("<>=^~");
      const auto op = token.substr(0, at);
      Version value;
      if (at == std::string::npos || !parse(token.substr(at), value)) return false;
      if (op == "^" || op == "~") {
        clause.push_back({">=", value});
        Version limit = value;
        limit.prerelease.clear();
        auto increment = [](std::uint64_t &n) { if (n == UINT64_MAX) return false; ++n; return true; };
        if (op == "~") { if (!increment(limit.minor)) return false; limit.patch = 0; }
        else if (limit.major) { if (!increment(limit.major)) return false; limit.minor = limit.patch = 0; }
        else if (limit.minor) { if (!increment(limit.minor)) return false; limit.patch = 0; }
        else if (!increment(limit.patch)) return false;
        clause.push_back({"<", limit});
        upper = true;
      } else {
        if (!op.empty() && op != "=" && op != ">" && op != ">=" && op != "<" && op != "<=") return false;
        clause.push_back({op.empty() ? "=" : op, value});
        upper = upper || op.empty() || op == "=" || op == "<" || op == "<=";
      }
    }
    if (clause.empty() || !upper) return false;
    out.push_back(std::move(clause));
    if (end == std::string::npos) return true;
    start = end + 2;
  }
}
inline bool valid_requirement(const std::string &s) { Range r; return parse_range(s, r); }
inline bool satisfies(const std::string &version, const std::string &requirement) {
  Version v;
  Range range;
  if (!parse(version, v) || !parse_range(requirement, range)) return false;
  for (const auto &clause : range) {
    bool matches = true, prerelease = v.prerelease.empty();
    for (const auto &c : clause) {
      const int order = compare(v, c.version);
      if (!(c.op == "=" ? order == 0 : c.op == ">" ? order > 0 : c.op == ">=" ? order >= 0 : c.op == "<" ? order < 0 : order <= 0)) matches = false;
      if (!c.version.prerelease.empty() && v.major == c.version.major && v.minor == c.version.minor && v.patch == c.version.patch) prerelease = true;
    }
    if (matches && prerelease) return true;
  }
  return false;
}
inline bool compatible_producer(const std::string &producer, const std::string &host = SPUTNIK_VERSION) {
  Version p, h;
  return parse(producer, p) && parse(host, h) && p.major == h.major && compare(p, h) <= 0 &&
      (p.prerelease.empty() || compare(p, h) == 0);
}
// Unversioned local projects belong to the 1.x baseline, even on future tools.
inline constexpr const char *baseline_requirement = ">=1.0.0 <2.0.0";
inline std::string current_requirement() {
  return ">=" SPUTNIK_VERSION " <" + std::to_string(SPUTNIK_VERSION_MAJOR + 1) + ".0.0";
}
} // namespace sputnik::language
