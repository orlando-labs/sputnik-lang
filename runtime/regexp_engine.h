#pragma once

// Shared PCRE2 semantics for compile-time validation, VM and direct native
// code.
#ifndef PCRE2_CODE_UNIT_WIDTH
#define PCRE2_CODE_UNIT_WIDTH 8
#endif
#include <algorithm>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <pcre2.h>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace sputnik::regexp {
struct Error : std::runtime_error {
  std::string kind;
  std::size_t byte_offset;
  Error(std::string type, std::string message, std::size_t offset = 0)
      : std::runtime_error(std::move(message)), kind(std::move(type)),
        byte_offset(offset) {}
};
inline std::string message(int code) {
  PCRE2_UCHAR buffer[256];
  const int n = pcre2_get_error_message(code, buffer, sizeof(buffer));
  return n < 0 ? "PCRE2 error " + std::to_string(code)
               : std::string(reinterpret_cast<char *>(buffer),
                             static_cast<std::size_t>(n));
}
inline std::uint32_t flag(char c) {
  switch (c) {
  case 'i':
    return PCRE2_CASELESS;
  case 'm':
    return PCRE2_MULTILINE;
  case 's':
    return PCRE2_DOTALL;
  case 'x':
    return PCRE2_EXTENDED;
  case 'U':
    return PCRE2_UNGREEDY;
  default:
    throw Error("ArgumentError", "unknown regexp flag: " + std::string(1, c));
  }
}
inline std::uint32_t flags(const std::string &text) {
  std::uint32_t out = 0;
  for (char c : text) {
    const auto bit = flag(c);
    if (out & bit)
      throw Error("ArgumentError",
                  "duplicate regexp flag: " + std::string(1, c));
    out |= bit;
  }
  return out;
}
inline std::string flag_text(std::uint32_t bits) {
  std::string out;
  for (char c : std::string("imsxU"))
    if (bits & flag(c))
      out += c;
  return out;
}
inline std::size_t next_cp(const std::string &s, std::size_t i) {
  if (i >= s.size())
    return s.size();
  do {
    ++i;
  } while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xc0) == 0x80);
  return i;
}
inline std::size_t cp_offset(const std::string &s, std::size_t byte) {
  std::size_t count = 0;
  for (std::size_t i = 0; i < std::min(byte, s.size()); i = next_cp(s, i))
    ++count;
  return count;
}
inline std::size_t byte_offset(const std::string &s, std::int64_t cp) {
  if (cp < 0)
    throw Error("ArgumentError", "regexp from must be non-negative");
  std::size_t i = 0;
  while (cp > 0 && i < s.size()) {
    i = next_cp(s, i);
    --cp;
  }
  if (cp)
    throw Error("IndexError", "regexp from exceeds string length");
  return i;
}
// Quote metacharacters and whitespace: safe in extended mode and
// independent of adjacent authored escapes. Non-ASCII UTF-8 stays readable.
inline std::string escape(const std::string &s) {
  static const char hex[] = "0123456789abcdef";
  std::string out;
  for (unsigned char c : s) {
    if (c >= 128 || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '_')
      out += static_cast<char>(c);
    else if (std::string("\\^$.|?*+()[]{}-#").find(static_cast<char>(c)) !=
             std::string::npos) {
      out += '\\';
      out += static_cast<char>(c);
    } else if (c > 32 && c != 127)
      out += static_cast<char>(c);
    else {
      out += "\\x{";
      out += hex[c >> 4];
      out += hex[c & 15];
      out += '}';
    }
  }
  return out;
}
struct Pattern {
  std::string source;
  std::uint32_t flags = 0;
  std::shared_ptr<pcre2_code> compiled;
  std::uint32_t group_count = 0;
  std::vector<std::pair<std::string, std::size_t>> names;
  std::uint32_t match_limit = 1000000;
  bool fragment = false;
  std::uint32_t newline = PCRE2_NEWLINE_LF;
};
inline Pattern compile(std::string source, std::uint32_t bits = 0,
                       std::uint32_t limit = 1000000) {
  int error = 0;
  PCRE2_SIZE offset = 0;
  std::unique_ptr<pcre2_compile_context, decltype(&pcre2_compile_context_free)>
      context(pcre2_compile_context_create(nullptr),
              pcre2_compile_context_free);
  if (!context)
    throw std::bad_alloc();
  pcre2_set_newline(context.get(), PCRE2_NEWLINE_LF);
  pcre2_set_bsr(context.get(), PCRE2_BSR_UNICODE);
  auto *raw =
      pcre2_compile(reinterpret_cast<PCRE2_SPTR>(source.data()), source.size(),
                    bits | PCRE2_UTF | PCRE2_UCP | PCRE2_NEVER_BACKSLASH_C,
                    &error, &offset, context.get());
  if (!raw)
    throw Error("RegexpCompileError",
                message(error) + " at byte " + std::to_string(offset), offset);
  Pattern out;
  out.source = std::move(source);
  out.flags = bits;
  out.match_limit = limit;
  out.compiled = std::shared_ptr<pcre2_code>(raw, pcre2_code_free);
  pcre2_pattern_info(raw, PCRE2_INFO_CAPTURECOUNT, &out.group_count);
  pcre2_pattern_info(raw, PCRE2_INFO_NEWLINE, &out.newline);
  std::uint32_t count = 0, size = 0;
  PCRE2_SPTR table = nullptr;
  pcre2_pattern_info(raw, PCRE2_INFO_NAMECOUNT, &count);
  pcre2_pattern_info(raw, PCRE2_INFO_NAMEENTRYSIZE, &size);
  pcre2_pattern_info(raw, PCRE2_INFO_NAMETABLE, &table);
  for (std::uint32_t i = 0; i < count; ++i) {
    const auto *p = table + i * size;
    const std::string name(reinterpret_cast<const char *>(p + 2));
    if (std::any_of(out.names.begin(), out.names.end(),
                    [&](const auto &n) { return n.first == name; }))
      throw Error("RegexpCompileError",
                  "duplicate capture names are not supported");
    out.names.emplace_back(name, (p[0] << 8) | p[1]);
  }
  std::sort(out.names.begin(), out.names.end(),
            [](const auto &a, const auto &b) { return a.second < b.second; });
  return out;
}
// Literal sites share immutable programs. Retain at most 64 entries; dynamic
// Regexp.compile calls remain explicit and do not populate this cache.
inline Pattern compile_literal(const std::string &source, std::uint32_t bits) {
  static std::mutex mutex;
  static std::deque<Pattern> programs;
  std::lock_guard<std::mutex> lock(mutex);
  for (const auto &p : programs)
    if (p.source == source && p.flags == bits)
      return p;
  auto result = compile(source, bits);
  if (programs.size() == 64)
    programs.pop_front();
  programs.push_back(result);
  return result;
}
inline void validate_fragment(const Pattern &p) {
  if (p.group_count || p.source.find("(?R") != std::string::npos ||
      p.source.find("(?0") != std::string::npos)
    throw Error("ArgumentError", "v1 regexp fragments cannot contain captures "
                                 "or whole-pattern recursion");
}
inline std::string fragment_source(const Pattern &p) {
  std::string disabled;
  for (char c : std::string("imsxU"))
    if (!(p.flags & flag(c)))
      disabled += c;
  return "(?" + flag_text(p.flags) + (disabled.empty() ? "" : "-" + disabled) +
         ":" + p.source + ")";
}
struct Capture {
  bool matched = false;
  std::size_t start = 0, end = 0;
  bool operator==(const Capture &r) const {
    return matched == r.matched &&
           (!matched || (start == r.start && end == r.end));
  }
};
inline std::size_t group(const Pattern &p, const std::string &name) {
  for (const auto &entry : p.names)
    if (entry.first == name)
      return entry.second;
  throw Error("RegexpGroupError", "unknown regexp group: " + name);
}
inline std::optional<std::vector<Capture>>
search(const Pattern &p, const std::string &s, std::size_t offset = 0,
       bool full = false, std::uint32_t options = 0) {
  std::unique_ptr<pcre2_match_data, decltype(&pcre2_match_data_free)> data(
      pcre2_match_data_create_from_pattern(p.compiled.get(), nullptr),
      pcre2_match_data_free);
  std::unique_ptr<pcre2_match_context, decltype(&pcre2_match_context_free)>
      context(pcre2_match_context_create(nullptr), pcre2_match_context_free);
  if (!data || !context)
    throw std::bad_alloc();
  pcre2_set_match_limit(context.get(), p.match_limit);
  pcre2_set_depth_limit(context.get(), 1000);
  pcre2_set_heap_limit(context.get(), 32768); // KiB; interpreter, no JIT
  const int rc = pcre2_match(
      p.compiled.get(), reinterpret_cast<PCRE2_SPTR>(s.data()), s.size(),
      offset, options | (full ? PCRE2_ANCHORED | PCRE2_ENDANCHORED : 0),
      data.get(), context.get());
  if (rc == PCRE2_ERROR_NOMATCH)
    return std::nullopt;
  if (rc < 0) {
    const bool limited = rc == PCRE2_ERROR_MATCHLIMIT ||
                         rc == PCRE2_ERROR_DEPTHLIMIT ||
                         rc == PCRE2_ERROR_HEAPLIMIT;
    throw Error(limited ? "RegexpMatchLimitError" : "RegexpError", message(rc));
  }
  const auto *ov = pcre2_get_ovector_pointer(data.get());
  std::vector<Capture> out(p.group_count + 1);
  for (std::size_t i = 0; i < out.size(); ++i)
    if (ov[2 * i] != PCRE2_UNSET) {
      if (ov[2 * i] > ov[2 * i + 1])
        throw Error("RegexpError", "reversed regexp capture range");
      out[i] = {true, ov[2 * i], ov[2 * i + 1]};
    }
  return out;
}
struct Cursor {
  std::size_t offset = 0;
  bool retry_nonempty = false, done = false;
  std::optional<std::vector<Capture>> next(const Pattern &p,
                                           const std::string &s) {
    if (done)
      return std::nullopt;
    auto result =
        search(p, s, offset, false,
               retry_nonempty ? PCRE2_NOTEMPTY_ATSTART | PCRE2_ANCHORED : 0);
    if (!result && retry_nonempty && offset < s.size()) {
      if ((p.newline == PCRE2_NEWLINE_CRLF || p.newline == PCRE2_NEWLINE_ANY ||
           p.newline == PCRE2_NEWLINE_ANYCRLF) &&
          s[offset] == '\r' && offset + 1 < s.size() && s[offset + 1] == '\n')
        offset += 2;
      else
        offset = next_cp(s, offset);
      retry_nonempty = false;
      result = search(p, s, offset);
    }
    if (!result) {
      done = true;
      return std::nullopt;
    }
    const auto &m = (*result)[0];
    offset = m.end;
    retry_nonempty = m.start == m.end;
    return result;
  }
};
inline std::vector<std::optional<std::string>> split(const Pattern &p,
                                                     const std::string &s,
                                                     std::size_t limit,
                                                     bool keep, bool captures) {
  struct Part {
    std::optional<std::string> text;
    bool capture;
  };
  std::vector<Part> parts;
  Cursor cursor;
  std::size_t last = 0, fields = 0;
  while (fields + 1 < limit) {
    auto ranges = cursor.next(p, s);
    if (!ranges)
      break;
    const auto &r = (*ranges)[0];
    parts.push_back({s.substr(last, r.start - last), false});
    ++fields;
    if (captures)
      for (std::size_t i = 1; i < ranges->size(); ++i) {
        const auto &c = (*ranges)[i];
        parts.push_back({c.matched ? std::optional<std::string>(
                                         s.substr(c.start, c.end - c.start))
                                   : std::nullopt,
                         true});
      }
    last = r.end;
  }
  parts.push_back({s.substr(last), false});
  std::size_t first = parts.size(), last_nonempty = 0;
  for (std::size_t i = 0; i < parts.size(); ++i)
    if (!parts[i].capture && parts[i].text && !parts[i].text->empty()) {
      first = std::min(first, i);
      last_nonempty = i;
    }
  std::vector<std::optional<std::string>> out;
  for (std::size_t i = 0; i < parts.size(); ++i)
    if (keep || parts[i].capture || (i >= first && i <= last_nonempty))
      out.push_back(std::move(parts[i].text));
  return out;
}
struct ReplacementPart {
  std::string text;
  std::optional<std::size_t> group;
};
inline std::vector<ReplacementPart> replacement(const Pattern &p,
                                                const std::string &s) {
  std::vector<ReplacementPart> out;
  std::string text;
  for (std::size_t i = 0; i < s.size();) {
    if (s[i] != '$') {
      text += s[i++];
      continue;
    }
    if (i + 1 < s.size() && s[i + 1] == '$') {
      text += '$';
      i += 2;
      continue;
    }
    if (!text.empty()) {
      out.push_back({std::move(text), {}});
      text.clear();
    }
    ++i;
    std::size_t index = 0;
    if (i < s.size() && s[i] >= '0' && s[i] <= '9') {
      const auto start = i;
      while (i < s.size() && s[i] >= '0' && s[i] <= '9')
        ++i;
      if (i - start > 2)
        throw Error("RegexpReplacementError",
                    "use ${number} for groups above 99");
      index = std::stoull(s.substr(start, i - start));
    } else if (i < s.size() && s[i] == '{') {
      const auto end = s.find('}', ++i);
      if (end == std::string::npos || end == i)
        throw Error("RegexpReplacementError", "invalid replacement group");
      const auto name = s.substr(i, end - i);
      i = end + 1;
      if (name.find_first_not_of("0123456789") == std::string::npos) {
        try {
          index = std::stoull(name);
        } catch (...) {
          throw Error("RegexpReplacementError", "group number too large");
        }
      } else {
        try {
          index = group(p, name);
        } catch (const Error &) {
          throw Error("RegexpReplacementError",
                      "unknown replacement group: " + name);
        }
      }
    } else
      throw Error("RegexpReplacementError",
                  "invalid dollar sequence in replacement");
    if (index > p.group_count)
      throw Error("RegexpReplacementError", "replacement group out of range");
    out.push_back({{}, index});
  }
  if (!text.empty())
    out.push_back({std::move(text), {}});
  return out;
}
inline std::string substitute(const std::vector<ReplacementPart> &parts,
                              const std::string &s,
                              const std::vector<Capture> &captures) {
  std::string out;
  for (const auto &part : parts) {
    if (!part.group)
      out += part.text;
    else {
      const auto &c = captures[*part.group];
      if (c.matched)
        out.append(s, c.start, c.end - c.start);
    }
  }
  return out;
}
inline void validate_holes(const std::vector<std::string> &chunks,
                           std::uint32_t bits) {
  if (chunks.empty())
    throw Error("ArgumentError", "empty regexp template");
  // PCRE2 can reinterpret an incomplete quantifier or hex escape as literal
  // syntax. A sentinel group alone cannot distinguish those from atom
  // boundaries.
  for (std::size_t i = 0; i + 1 < chunks.size(); ++i) {
    const auto &chunk = chunks[i];
    const auto brace = chunk.rfind('{');
    if (brace != std::string::npos &&
        chunk.find_first_not_of("0123456789, \t", brace + 1) ==
            std::string::npos) {
      std::size_t slashes = 0;
      for (std::size_t j = brace; j > 0 && chunk[j - 1] == '\\'; --j)
        ++slashes;
      if (slashes % 2 == 0)
        throw Error("RegexpCompileError",
                    "interpolation cannot occur inside a quantifier");
    }
    const auto slash = chunk.rfind('\\');
    if (slash != std::string::npos) {
      std::size_t slashes = 1;
      for (std::size_t j = slash; j > 0 && chunk[j - 1] == '\\'; --j)
        ++slashes;
      const auto tail = chunk.substr(slash + 1);
      if (slashes % 2 &&
          (tail == "x" ||
           (tail.size() == 2 && tail[0] == 'x' &&
            std::string("0123456789abcdefABCDEF").find(tail[1]) !=
                std::string::npos)))
        throw Error("RegexpCompileError",
                    "interpolation cannot complete a hex escape");
    }
  }
  std::string prefix;
  for (std::uint32_t id = 0;; ++id) {
    prefix = "SputnikHole" + std::to_string(id) + "_";
    if (std::none_of(chunks.begin(), chunks.end(), [&](const auto &chunk) {
          return chunk.find(prefix) != std::string::npos;
        }))
      break;
  }
  std::string source = chunks[0];
  for (std::size_t i = 1; i < chunks.size(); ++i)
    source += "(?<" + prefix + std::to_string(i) + ">)" + chunks[i];
  const auto p = compile(source, bits);
  for (std::size_t i = 1; i < chunks.size(); ++i) {
    const auto name = prefix + std::to_string(i);
    if (std::none_of(p.names.begin(), p.names.end(),
                     [&](const auto &entry) { return entry.first == name; }))
      throw Error("RegexpCompileError",
                  "interpolation must occur at a regexp expression boundary");
  }
}
} // namespace sputnik::regexp
