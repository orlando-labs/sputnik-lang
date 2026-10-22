#include "frontend/source_path.h"
#include "notebook/project.h"
#include "runtime/config_document.h"

#include <algorithm>
#include <cstdint>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <functional>
#include <sstream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace sputnik::notebook {

namespace {

constexpr std::size_t kMaxJsonBytes = 16U * 1024U * 1024U;
constexpr std::size_t kMaxJsonDepth = 64U;
constexpr std::size_t kMaxTextUtf16Length = 16U * 1024U * 1024U;

enum class JsonKind { Null, Boolean, Number, String, Array, Object };

struct JsonValue {
  JsonKind kind = JsonKind::Null;
  std::size_t begin = 0;
  std::size_t end = 0;
  std::string text;
  std::vector<JsonValue> array;
  std::vector<std::pair<std::string, JsonValue>> object;
};

bool is_json_control(std::uint32_t codepoint) {
  return codepoint < 0x20U || (codepoint >= 0x7FU && codepoint <= 0x9FU);
}

bool decode_utf8_sequence(const std::string &text, std::size_t offset,
                          std::size_t *length, std::uint32_t *codepoint) {
  if (offset >= text.size()) {
    return false;
  }
  const unsigned char first = static_cast<unsigned char>(text[offset]);
  if (first <= 0x7FU) {
    *length = 1U;
    *codepoint = first;
    return true;
  }

  std::size_t count = 0U;
  std::uint32_t value = 0U;
  std::uint32_t minimum = 0U;
  if ((first & 0xE0U) == 0xC0U) {
    count = 2U;
    value = first & 0x1FU;
    minimum = 0x80U;
  } else if ((first & 0xF0U) == 0xE0U) {
    count = 3U;
    value = first & 0x0FU;
    minimum = 0x800U;
  } else if ((first & 0xF8U) == 0xF0U) {
    count = 4U;
    value = first & 0x07U;
    minimum = 0x10000U;
  } else {
    return false;
  }
  if (offset + count > text.size()) {
    return false;
  }
  for (std::size_t index = 1U; index < count; ++index) {
    const unsigned char continuation =
        static_cast<unsigned char>(text[offset + index]);
    if ((continuation & 0xC0U) != 0x80U) {
      return false;
    }
    value = (value << 6U) | (continuation & 0x3FU);
  }
  if (value < minimum || value > 0x10FFFFU ||
      (value >= 0xD800U && value <= 0xDFFFU)) {
    return false;
  }
  *length = count;
  *codepoint = value;
  return true;
}

void append_utf8(std::uint32_t codepoint, std::string *output) {
  if (codepoint <= 0x7FU) {
    output->push_back(static_cast<char>(codepoint));
  } else if (codepoint <= 0x7FFU) {
    output->push_back(static_cast<char>(0xC0U | (codepoint >> 6U)));
    output->push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
  } else if (codepoint <= 0xFFFFU) {
    output->push_back(static_cast<char>(0xE0U | (codepoint >> 12U)));
    output->push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
    output->push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
  } else {
    output->push_back(static_cast<char>(0xF0U | (codepoint >> 18U)));
    output->push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3FU)));
    output->push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
    output->push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
  }
}

class JsonParser {
public:
  explicit JsonParser(const std::string &input, std::size_t limit = kMaxJsonBytes) : input_(input) {
    if (input_.size() > limit) {
      fail("input exceeds JSON byte limit", limit);
    }
  }

  JsonValue parse() {
    skip_whitespace();
    if (position_ == input_.size()) {
      fail("document is empty", position_);
    }
    JsonValue value = parse_value(0U);
    skip_whitespace();
    if (position_ != input_.size()) {
      fail("trailing data", position_);
    }
    return value;
  }

private:
  [[noreturn]] void fail(const std::string &message, std::size_t offset) const {
    throw std::runtime_error("project JSON error at byte " +
                             std::to_string(offset) + ": " + message);
  }

  static bool is_whitespace(unsigned char value) {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
  }

  void skip_whitespace() {
    while (position_ < input_.size() &&
           is_whitespace(static_cast<unsigned char>(input_[position_]))) {
      ++position_;
    }
  }

  char peek() const {
    return position_ < input_.size() ? input_[position_] : '\0';
  }

  void expect(char expected) {
    if (peek() != expected) {
      fail(std::string("expected '") + expected + "'", position_);
    }
    ++position_;
  }

  JsonValue parse_value(std::size_t depth) {
    if (depth > kMaxJsonDepth) {
      fail("nesting exceeds depth 64", position_);
    }
    skip_whitespace();
    if (position_ == input_.size()) {
      fail("expected a value", position_);
    }
    switch (peek()) {
    case 'n':
      return parse_literal("null", JsonKind::Null);
    case 'f':
      return parse_literal("false", JsonKind::Boolean);
    case 't':
      return parse_literal("true", JsonKind::Boolean);
    case '"':
      return parse_string();
    case '[':
      return parse_array(depth);
    case '{':
      return parse_object(depth);
    default:
      if (peek() == '-' || (peek() >= '0' && peek() <= '9')) {
        return parse_number();
      }
      fail("unexpected value", position_);
    }
  }

  JsonValue parse_literal(std::string_view literal, JsonKind kind) {
    const std::size_t begin = position_;
    if (input_.compare(position_, literal.size(), literal) != 0) {
      fail("invalid literal", begin);
    }
    position_ += literal.size();
    JsonValue value;
    value.kind = kind;
    value.begin = begin;
    value.end = position_;
    return value;
  }

  static int hex_value(char value) {
    if (value >= '0' && value <= '9') {
      return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
      return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
      return value - 'A' + 10;
    }
    return -1;
  }

  std::uint16_t parse_hex_unit() {
    if (position_ + 4U > input_.size()) {
      fail("truncated unicode escape", position_);
    }
    std::uint16_t result = 0;
    for (std::size_t index = 0U; index < 4U; ++index) {
      const int digit = hex_value(input_[position_ + index]);
      if (digit < 0) {
        fail("invalid unicode escape", position_ + index);
      }
      result = static_cast<std::uint16_t>((result << 4U) | digit);
    }
    position_ += 4U;
    return result;
  }

  JsonValue parse_string() {
    const std::size_t begin = position_;
    expect('"');
    JsonValue value;
    value.kind = JsonKind::String;
    value.begin = begin;
    while (position_ < input_.size()) {
      const std::size_t byte_offset = position_;
      const unsigned char byte =
          static_cast<unsigned char>(input_[position_++]);
      if (byte == '"') {
        value.end = position_;
        return value;
      }
      if (byte == '\\') {
        if (position_ == input_.size()) {
          fail("truncated string escape", position_);
        }
        const char escaped = input_[position_++];
        switch (escaped) {
        case '"':
          value.text.push_back('"');
          break;
        case '\\':
          value.text.push_back('\\');
          break;
        case '/':
          value.text.push_back('/');
          break;
        case 'b':
          value.text.push_back('\b');
          break;
        case 'f':
          value.text.push_back('\f');
          break;
        case 'n':
          value.text.push_back('\n');
          break;
        case 'r':
          value.text.push_back('\r');
          break;
        case 't':
          value.text.push_back('\t');
          break;
        case 'u': {
          const std::uint16_t high = parse_hex_unit();
          std::uint32_t codepoint = high;
          if (high >= 0xD800U && high <= 0xDBFFU) {
            if (position_ + 6U > input_.size() || input_[position_] != '\\' ||
                input_[position_ + 1U] != 'u') {
              fail("high surrogate must be followed by a low surrogate",
                   position_);
            }
            position_ += 2U;
            const std::uint16_t low = parse_hex_unit();
            if (low < 0xDC00U || low > 0xDFFFU) {
              fail("invalid low surrogate", position_ - 4U);
            }
            codepoint = 0x10000U +
                        ((static_cast<std::uint32_t>(high) - 0xD800U) << 10U) +
                        (static_cast<std::uint32_t>(low) - 0xDC00U);
          } else if (high >= 0xDC00U && high <= 0xDFFFU) {
            fail("unpaired low surrogate", byte_offset);
          }
          append_utf8(codepoint, &value.text);
          break;
        }
        default:
          fail("invalid string escape", position_ - 1U);
        }
        continue;
      }
      if (byte < 0x20U) {
        fail("unescaped control in string", byte_offset);
      }
      if (byte < 0x80U) {
        value.text.push_back(static_cast<char>(byte));
        continue;
      }

      const std::size_t sequence_start = byte_offset;
      std::size_t sequence_length = 0U;
      std::uint32_t codepoint = 0U;
      if (!decode_utf8_sequence(input_, sequence_start, &sequence_length,
                                &codepoint)) {
        fail("invalid UTF-8 in string", sequence_start);
      }
      value.text.append(input_, sequence_start, sequence_length);
      position_ = sequence_start + sequence_length;
    }
    fail("unterminated string", position_);
  }

  JsonValue parse_number() {
    const std::size_t begin = position_;
    if (peek() == '-') {
      ++position_;
      if (position_ == input_.size()) {
        fail("invalid number", begin);
      }
    }
    if (peek() == '0') {
      ++position_;
      if (position_ < input_.size() && input_[position_] >= '0' &&
          input_[position_] <= '9') {
        fail("leading zero in number", position_);
      }
    } else if (peek() >= '1' && peek() <= '9') {
      while (position_ < input_.size() && input_[position_] >= '0' &&
             input_[position_] <= '9') {
        ++position_;
      }
    } else {
      fail("invalid number", position_);
    }
    if (peek() == '.') {
      ++position_;
      if (position_ == input_.size() || peek() < '0' || peek() > '9') {
        fail("fraction requires digits", position_);
      }
      while (position_ < input_.size() && peek() >= '0' && peek() <= '9') {
        ++position_;
      }
    }
    if (peek() == 'e' || peek() == 'E') {
      ++position_;
      if (peek() == '+' || peek() == '-') {
        ++position_;
      }
      if (position_ == input_.size() || peek() < '0' || peek() > '9') {
        fail("exponent requires digits", position_);
      }
      while (position_ < input_.size() && peek() >= '0' && peek() <= '9') {
        ++position_;
      }
    }
    JsonValue value;
    value.kind = JsonKind::Number;
    value.begin = begin;
    value.end = position_;
    return value;
  }

  JsonValue parse_array(std::size_t depth) {
    const std::size_t begin = position_;
    expect('[');
    JsonValue value;
    value.kind = JsonKind::Array;
    value.begin = begin;
    skip_whitespace();
    if (peek() == ']') {
      ++position_;
      value.end = position_;
      return value;
    }
    while (true) {
      value.array.push_back(parse_value(depth + 1U));
      skip_whitespace();
      if (peek() == ']') {
        ++position_;
        value.end = position_;
        return value;
      }
      expect(',');
      skip_whitespace();
      if (peek() == ']') {
        fail("trailing comma in array", position_);
      }
    }
  }

  JsonValue parse_object(std::size_t depth) {
    const std::size_t begin = position_;
    expect('{');
    JsonValue value;
    value.kind = JsonKind::Object;
    value.begin = begin;
    std::set<std::string> keys;
    skip_whitespace();
    if (peek() == '}') {
      ++position_;
      value.end = position_;
      return value;
    }
    while (true) {
      if (peek() != '"') {
        fail("object key must be a string", position_);
      }
      JsonValue key = parse_string();
      if (!keys.insert(key.text).second) {
        fail("duplicate object key", key.begin);
      }
      skip_whitespace();
      expect(':');
      JsonValue member = parse_value(depth + 1U);
      value.object.emplace_back(std::move(key.text), std::move(member));
      skip_whitespace();
      if (peek() == '}') {
        ++position_;
        value.end = position_;
        return value;
      }
      expect(',');
      skip_whitespace();
      if (peek() == '}') {
        fail("trailing comma in object", position_);
      }
    }
  }

  const std::string &input_;
  std::size_t position_ = 0U;
};

[[noreturn]] void document_error(const std::string &path,
                                 const std::string &message) {
  throw std::runtime_error("invalid project document: " + path + ": " +
                           message);
}

[[noreturn]] void document_error_at(const std::string &path,
                                    const std::string &message,
                                    std::size_t offset) {
  throw std::runtime_error("project JSON error at byte " +
                           std::to_string(offset) + ": " + path + ": " +
                           message);
}

void require_object(const JsonValue &value, const std::string &path) {
  if (value.kind != JsonKind::Object) {
    document_error_at(path, "expected an object", value.begin);
  }
}

const JsonValue &require_member(const JsonValue &object, const std::string &key,
                                const std::string &path) {
  for (const auto &member : object.object) {
    if (member.first == key) {
      return member.second;
    }
  }
  document_error_at(path, "missing required member '" + key + "'", object.end);
}

std::string require_string(const JsonValue &value, const std::string &path) {
  if (value.kind != JsonKind::String) {
    document_error_at(path, "expected a string", value.begin);
  }
  return value.text;
}

const std::vector<JsonValue> &require_array(const JsonValue &value,
                                            const std::string &path) {
  if (value.kind != JsonKind::Array) {
    document_error_at(path, "expected an array", value.begin);
  }
  return value.array;
}

const JsonValue *find_member(const JsonValue &object, const std::string &key);

std::uint64_t parse_cell_id(const JsonValue &value, const std::string &path) {
  const std::string text = require_string(value, path);
  if (text.empty() || (text.size() > 1U && text.front() == '0')) {
    document_error_at(path, "cell id must be canonical decimal", value.begin);
  }
  std::uint64_t result = 0;
  for (const char digit : text) {
    if (digit < '0' || digit > '9') {
      document_error_at(path, "cell id must be canonical decimal", value.begin);
    }
    const auto digit_value = static_cast<std::uint64_t>(digit - '0');
    if (result >
        (std::numeric_limits<std::uint64_t>::max() - digit_value) / 10U) {
      document_error_at(path, "cell id is out of range", value.begin);
    }
    result = result * 10U + digit_value;
  }
  if (result == 0U || result == std::numeric_limits<std::uint64_t>::max()) {
    document_error_at(path, "cell id must be in [1, UINT64_MAX - 1]",
                      value.begin);
  }
  return result;
}

std::string raw_json(const std::string &json, const JsonValue &value) {
  return json.substr(value.begin, value.end - value.begin);
}

ProjectExtraFields parse_extras(const JsonValue &object,
                                const std::set<std::string> &reserved,
                                const std::string &json) {
  ProjectExtraFields extras;
  for (const auto &member : object.object) {
    if (reserved.count(member.first) != 0U) {
      continue;
    }
    extras.emplace(member.first, raw_json(json, member.second));
  }
  return extras;
}

ProjectCell parse_cell(const JsonValue &value, const std::string &json,
                       const std::string &path) {
  require_object(value, path);
  ProjectCell cell;
  cell.id = parse_cell_id(require_member(value, "id", path), path + ".id");
  cell.kind =
      require_string(require_member(value, "kind", path), path + ".kind");
  if (cell.kind.empty()) {
    document_error_at(path + ".kind", "cell kind must not be empty",
                      value.begin);
  }
  if (cell.kind == "code") {
    cell.source =
        require_string(require_member(value, "source", path), path + ".source");
    const std::string mode =
        require_string(require_member(value, "mode", path), path + ".mode");
    if (mode == "watch") {
      cell.mode = CellMode::Watch;
    } else if (mode == "manual") {
      cell.mode = CellMode::Manual;
    } else {
      document_error_at(path + ".mode", "mode must be 'watch' or 'manual'",
                        value.begin);
    }
    cell.extra = parse_extras(value, {"id", "kind", "source", "mode"}, json);
  } else if (cell.kind == "text") {
    cell.source =
        require_string(require_member(value, "source", path), path + ".source");
    const JsonValue *formatting = find_member(value, "formatting");
    if (formatting != nullptr) {
      if (formatting->kind != JsonKind::Object) {
        document_error_at(path + ".formatting", "expected an object",
                          formatting->begin);
      }
      cell.formatting = raw_json(json, *formatting);
    }
    validate_project_text_content({cell.source, cell.formatting});
    cell.extra =
        parse_extras(value, {"id", "kind", "source", "formatting"}, json);
  } else {
    // Future cell kinds are opaque.  In particular, source/mode are not
    // interpreted here and remain in extra when present.
    cell.extra = parse_extras(value, {"id", "kind"}, json);
  }
  return cell;
}

ProjectSheet parse_sheet(const JsonValue &value, const std::string &json,
                         const std::string &path) {
  require_object(value, path);
  ProjectSheet sheet;
  sheet.id = require_string(require_member(value, "id", path), path + ".id");
  sheet.title =
      require_string(require_member(value, "title", path), path + ".title");
  const auto &cells =
      require_array(require_member(value, "cells", path), path + ".cells");
  for (std::size_t index = 0U; index < cells.size(); ++index) {
    sheet.cells.push_back(parse_cell(
        cells[index], json, path + ".cells[" + std::to_string(index) + "]"));
  }
  sheet.extra = parse_extras(value, {"id", "title", "cells"}, json);
  return sheet;
}

ProjectModule parse_module(const JsonValue &value, const std::string &json,
                           const std::string &path) {
  require_object(value, path);
  ProjectModule module;
  module.id = require_string(require_member(value, "id", path), path + ".id");
  module.path =
      require_string(require_member(value, "path", path), path + ".path");
  module.extra = parse_extras(value, {"id", "path"}, json);
  return module;
}

void append_hex_escape(std::uint32_t codepoint, std::string *output) {
  static constexpr char digits[] = "0123456789abcdef";
  output->append("\\u");
  for (int shift = 12; shift >= 0; shift -= 4) {
    output->push_back(digits[(codepoint >> shift) & 0xFU]);
  }
}

void write_json_string(const std::string &text, std::string *output) {
  output->push_back('"');
  for (std::size_t position = 0U; position < text.size();) {
    const unsigned char byte = static_cast<unsigned char>(text[position]);
    if (byte < 0x80U) {
      switch (byte) {
      case '"':
        output->append("\\\"");
        break;
      case '\\':
        output->append("\\\\");
        break;
      case '\b':
        output->append("\\b");
        break;
      case '\f':
        output->append("\\f");
        break;
      case '\n':
        output->append("\\n");
        break;
      case '\r':
        output->append("\\r");
        break;
      case '\t':
        output->append("\\t");
        break;
      default:
        if (byte < 0x20U || byte == 0x7FU) {
          append_hex_escape(byte, output);
        } else {
          output->push_back(static_cast<char>(byte));
        }
        break;
      }
      ++position;
      continue;
    }
    std::size_t length = 0U;
    std::uint32_t codepoint = 0U;
    if (!decode_utf8_sequence(text, position, &length, &codepoint)) {
      document_error("string", "contains invalid UTF-8");
    }
    if (is_json_control(codepoint)) {
      if (codepoint <= 0xFFFFU) {
        append_hex_escape(codepoint, output);
      } else {
        const std::uint32_t adjusted = codepoint - 0x10000U;
        append_hex_escape(0xD800U + (adjusted >> 10U), output);
        append_hex_escape(0xDC00U + (adjusted & 0x3FFU), output);
      }
    } else {
      output->append(text, position, length);
    }
    position += length;
  }
  output->push_back('"');
}

void validate_utf8(const std::string &text, const std::string &path) {
  for (std::size_t position = 0U; position < text.size();) {
    std::size_t length = 0U;
    std::uint32_t codepoint = 0U;
    if (!decode_utf8_sequence(text, position, &length, &codepoint)) {
      document_error(path, "contains invalid UTF-8");
    }
    position += length;
  }
}

const JsonValue *find_member(const JsonValue &object, const std::string &key) {
  for (const auto &member : object.object) {
    if (member.first == key) {
      return &member.second;
    }
  }
  return nullptr;
}

std::uint64_t require_format_uint(const std::string &json,
                                  const JsonValue &value,
                                  const std::string &path) {
  if (value.kind != JsonKind::Number || value.begin >= value.end) {
    document_error_at(path, "expected a non-negative integer", value.begin);
  }
  const std::string text = json.substr(value.begin, value.end - value.begin);
  if (text.empty()) {
    document_error_at(path, "expected a non-negative integer", value.begin);
  }
  std::uint64_t result = 0U;
  for (char digit : text) {
    if (digit < '0' || digit > '9') {
      document_error_at(path, "expected a non-negative integer", value.begin);
    }
    const std::uint64_t digit_value =
        static_cast<std::uint64_t>(digit - '0');
    if (result > (std::numeric_limits<std::uint64_t>::max() - digit_value) /
                     10U) {
      document_error_at(path, "integer is out of range", value.begin);
    }
    result = result * 10U + digit_value;
  }
  return result;
}

// Rich-text offsets are UTF-16 code-unit offsets, while the source is UTF-8.
// Keep the conversion table small (one entry per UTF-16 boundary) and retain
// the LF boundary information needed by paragraph/table metadata.
struct TextLayout {
  std::size_t utf16_length = 0U;
  std::vector<std::size_t> byte_offsets;
  std::vector<bool> after_lf;
};

TextLayout make_text_layout(const std::string &source,
                            const std::string &path) {
  if (source.size() > kMaxTextUtf16Length * 4U) {
    document_error(path, "text exceeds 16 MiB limit");
  }

  TextLayout layout;
  layout.byte_offsets.reserve(source.size() + 1U);
  layout.byte_offsets.push_back(0U);
  layout.after_lf.push_back(false);
  for (std::size_t position = 0U; position < source.size();) {
    const std::size_t byte_start = position;
    std::size_t sequence_length = 0U;
    std::uint32_t codepoint = 0U;
    if (!decode_utf8_sequence(source, position, &sequence_length,
                              &codepoint)) {
      document_error(path, "contains invalid UTF-8");
    }
    const std::size_t units = codepoint > 0xFFFFU ? 2U : 1U;
    if (layout.utf16_length >
        std::numeric_limits<std::size_t>::max() - units) {
      document_error(path, "UTF-16 length is out of range");
    }
    layout.utf16_length += units;
    if (layout.utf16_length > kMaxTextUtf16Length) {
      document_error(path, "text exceeds 16 MiB limit");
    }
    // A supplementary code point has no legal offset between its two UTF-16
    // code units. Marking it as npos makes malformed ranges easy to reject.
    if (units == 2U) {
      layout.byte_offsets.push_back(std::numeric_limits<std::size_t>::max());
      layout.after_lf.push_back(false);
    }
    layout.byte_offsets.push_back(byte_start + sequence_length);
    layout.after_lf.push_back(codepoint == '\n');
    position += sequence_length;
  }
  return layout;
}

bool is_utf16_boundary(const TextLayout &layout, std::size_t offset) {
  return offset < layout.byte_offsets.size() &&
         layout.byte_offsets[offset] != std::numeric_limits<std::size_t>::max();
}

bool is_paragraph_start(const TextLayout &layout, std::size_t offset) {
  return is_utf16_boundary(layout, offset) &&
         (offset == 0U || layout.after_lf[offset]);
}

bool ends_after_lf(const TextLayout &layout, std::size_t offset) {
  return is_utf16_boundary(layout, offset) && layout.after_lf[offset];
}

std::size_t checked_format_size(const std::string &json,
                                const JsonValue &value,
                                const std::string &path) {
  const std::uint64_t result = require_format_uint(json, value, path);
  if (result > std::numeric_limits<std::size_t>::max()) {
    document_error_at(path, "integer is out of range", value.begin);
  }
  return static_cast<std::size_t>(result);
}

struct TextRange {
  std::size_t start = 0U;
  std::size_t end = 0U;
};

void validate_range_bounds(const TextLayout &layout, const TextRange &range,
                           const std::string &path, std::size_t offset) {
  if (range.start > layout.utf16_length || range.end < range.start ||
      range.end > layout.utf16_length || !is_utf16_boundary(layout, range.start) ||
      !is_utf16_boundary(layout, range.end)) {
    document_error_at(path, "range is out of bounds or not on a UTF-16 scalar boundary",
                      offset);
  }
}

std::string source_slice(const std::string &source, const TextLayout &layout,
                         const TextRange &range) {
  const std::size_t byte_start = layout.byte_offsets[range.start];
  const std::size_t byte_end = layout.byte_offsets[range.end];
  return source.substr(byte_start, byte_end - byte_start);
}

void validate_metadata_range_order(const TextRange &range,
                                   TextRange *previous, bool *have_previous,
                                   const std::string &path, std::size_t offset) {
  if (*have_previous &&
      (range.start < previous->start || range.start < previous->end)) {
    document_error_at(path, "ranges must be sorted and non-overlapping", offset);
  }
  *previous = range;
  *have_previous = true;
}

void validate_text_formatting_json(const std::string &source,
                                   const std::string &formatting,
                                   const std::string &path) {
  if (formatting.empty()) {
    return;
  }
  JsonValue root;
  try {
    root = JsonParser(formatting).parse();
  } catch (const std::runtime_error &error) {
    document_error(path, std::string("formatting is invalid JSON: ") +
                             error.what());
  }
  require_object(root, path);

  const JsonValue *version = find_member(root, "version");
  const JsonValue *runs = find_member(root, "runs");
  if (version == nullptr) {
    document_error_at(path + ".version", "missing required member 'version'",
                      root.end);
  }
  if (runs == nullptr) {
    document_error_at(path + ".runs", "missing required member 'runs'",
                      root.end);
  }
  if (version->kind != JsonKind::Number) {
    document_error_at(path + ".version", "version must be the number 1 or 2",
                      version->begin);
  }
  const std::string version_text =
      formatting.substr(version->begin, version->end - version->begin);
  const bool is_v1 = version_text == "1";
  const bool is_v2 = version_text == "2";
  if (!is_v1 && !is_v2) {
    document_error_at(path + ".version", "version must be the number 1 or 2",
                      version->begin);
  }
  for (const auto &member : root.object) {
    if (member.first != "version" && member.first != "runs" &&
        !(is_v2 && (member.first == "paragraphs" || member.first == "tables"))) {
      document_error_at(path + "." + member.first,
                        "unknown formatting member", member.second.begin);
    }
  }
  if (runs->kind != JsonKind::Array) {
    document_error_at(path + ".runs", "expected an array", runs->begin);
  }
  constexpr std::size_t kMaxFormattingRuns = 20000U;
  if (runs->array.size() > kMaxFormattingRuns) {
    document_error(path + ".runs", "formatting run count exceeds 20000");
  }

  const TextLayout layout = make_text_layout(source, path + ".source");

  std::size_t previous_start = 0U;
  std::size_t previous_end = 0U;
  bool have_previous = false;
  for (std::size_t index = 0U; index < runs->array.size(); ++index) {
    const JsonValue &run = runs->array[index];
    const std::string run_path = path + ".runs[" + std::to_string(index) + "]";
    require_object(run, run_path);
    const JsonValue *start_value = find_member(run, "start");
    const JsonValue *length_value = find_member(run, "length");
    if (start_value == nullptr) {
      document_error_at(run_path + ".start",
                        "missing required member 'start'", run.end);
    }
    if (length_value == nullptr) {
      document_error_at(run_path + ".length",
                        "missing required member 'length'", run.end);
    }
    for (const auto &member : run.object) {
      if (member.first != "start" && member.first != "length" &&
          member.first != "style" && member.first != "bold" &&
          member.first != "italic" && member.first != "underline") {
        document_error_at(run_path + "." + member.first,
                          "unknown formatting run member", member.second.begin);
      }
    }
    const std::uint64_t start =
        require_format_uint(formatting, *start_value, run_path + ".start");
    const std::uint64_t length =
        require_format_uint(formatting, *length_value, run_path + ".length");
    if (length == 0U) {
      document_error_at(run_path + ".length", "length must be positive",
                        length_value->begin);
    }
    if (start > std::numeric_limits<std::size_t>::max() ||
        length > std::numeric_limits<std::size_t>::max() -
                     static_cast<std::size_t>(start)) {
      document_error_at(run_path, "range is out of bounds", run.begin);
    }
    const std::size_t start_size = static_cast<std::size_t>(start);
    const std::size_t end_size = start_size + static_cast<std::size_t>(length);
    if (end_size > layout.utf16_length ||
        !is_utf16_boundary(layout, start_size) ||
        !is_utf16_boundary(layout, end_size)) {
      document_error_at(run_path, "range is not on a UTF-16 scalar boundary",
                        run.begin);
    }
    if (have_previous &&
        (start_size < previous_start || start_size < previous_end)) {
      document_error_at(run_path, "runs must be sorted and non-overlapping",
                        run.begin);
    }
    previous_start = start_size;
    previous_end = end_size;
    have_previous = true;

    const JsonValue *style = find_member(run, "style");
    if (style != nullptr) {
      const std::string value = require_string(*style, run_path + ".style");
      if (value != "body" && value != "heading1" && value != "heading2" &&
          value != "heading3" && value != "code") {
        document_error_at(run_path + ".style", "unknown text style",
                          style->begin);
      }
    }
    for (const char *flag : {"bold", "italic", "underline"}) {
      const JsonValue *member = find_member(run, flag);
      if (member != nullptr && member->kind != JsonKind::Boolean) {
        document_error_at(run_path + "." + flag, "expected a boolean",
                          member->begin);
      }
    }
  }

  if (!is_v2) {
    return;
  }

  const JsonValue *paragraphs = find_member(root, "paragraphs");
  const JsonValue *tables = find_member(root, "tables");
  const std::size_t paragraph_count =
      paragraphs == nullptr ? 0U : paragraphs->array.size();
  if (paragraph_count + runs->array.size() > kMaxFormattingRuns) {
    document_error(path, "formatting run and paragraph count exceeds 20000");
  }

  std::vector<TextRange> paragraph_ranges;
  TextRange previous_paragraph;
  bool have_previous_paragraph = false;
  if (paragraphs != nullptr) {
    if (paragraphs->kind != JsonKind::Array) {
      document_error_at(path + ".paragraphs", "expected an array",
                        paragraphs->begin);
    }
    paragraph_ranges.reserve(paragraphs->array.size());
    for (std::size_t index = 0U; index < paragraphs->array.size(); ++index) {
      const JsonValue &paragraph = paragraphs->array[index];
      const std::string paragraph_path =
          path + ".paragraphs[" + std::to_string(index) + "]";
      require_object(paragraph, paragraph_path);
      const JsonValue *start_value = find_member(paragraph, "start");
      const JsonValue *length_value = find_member(paragraph, "length");
      if (start_value == nullptr) {
        document_error_at(paragraph_path + ".start",
                          "missing required member 'start'", paragraph.end);
      }
      if (length_value == nullptr) {
        document_error_at(paragraph_path + ".length",
                          "missing required member 'length'", paragraph.end);
      }
      for (const auto &member : paragraph.object) {
        if (member.first != "start" && member.first != "length" &&
            member.first != "style" && member.first != "list") {
          document_error_at(paragraph_path + "." + member.first,
                            "unknown paragraph member", member.second.begin);
        }
      }
      const std::size_t start =
          checked_format_size(formatting, *start_value, paragraph_path + ".start");
      const std::size_t length = checked_format_size(
          formatting, *length_value, paragraph_path + ".length");
      if (length == 0U) {
        document_error_at(paragraph_path + ".length", "length must be positive",
                          length_value->begin);
      }
      if (length > std::numeric_limits<std::size_t>::max() - start) {
        document_error_at(paragraph_path, "range is out of bounds",
                          paragraph.begin);
      }
      const TextRange range{start, start + length};
      validate_range_bounds(layout, range, paragraph_path, paragraph.begin);
      if (!is_paragraph_start(layout, range.start) ||
          (!ends_after_lf(layout, range.end) &&
           range.end != layout.utf16_length)) {
        document_error_at(paragraph_path,
                          "range must cover a complete LF-delimited paragraph",
                          paragraph.begin);
      }
      validate_metadata_range_order(range, &previous_paragraph,
                                    &have_previous_paragraph, paragraph_path,
                                    paragraph.begin);
      paragraph_ranges.push_back(range);

      const JsonValue *style = find_member(paragraph, "style");
      if (style != nullptr) {
        const std::string value =
            require_string(*style, paragraph_path + ".style");
        if (value != "body" && value != "heading1" && value != "heading2" &&
            value != "heading3") {
          document_error_at(paragraph_path + ".style", "unknown paragraph style",
                            style->begin);
        }
      }
      const JsonValue *list = find_member(paragraph, "list");
      if (list != nullptr) {
        const std::string value = require_string(*list, paragraph_path + ".list");
        if (value != "bullet" && value != "numbered") {
          document_error_at(paragraph_path + ".list", "unknown paragraph list",
                            list->begin);
        }
        const std::string text = source_slice(source, layout, range);
        if (value == "bullet") {
          if (text.size() < std::string("•\t").size() ||
              text.compare(0U, std::string("•\t").size(), "•\t") != 0) {
            document_error_at(paragraph_path + ".list",
                              "bullet list paragraph must start with '•\\t'",
                              list->begin);
          }
        } else {
          std::size_t position = 0U;
          if (!text.empty() && text[position] >= '1' &&
              text[position] <= '9') {
            ++position;
            while (position < text.size() && text[position] >= '0' &&
                   text[position] <= '9') {
              ++position;
            }
          }
          if (position == 0U || position + 1U >= text.size() ||
              text[position] != '.' || text[position + 1U] != '\t') {
            document_error_at(
                paragraph_path + ".list",
                "numbered list paragraph must start with a positive decimal and '.\\t'",
                list->begin);
          }
        }
      }
    }
  }

  TextRange previous_table;
  bool have_previous_table = false;
  std::vector<TextRange> table_ranges;
  if (tables != nullptr) {
    if (tables->kind != JsonKind::Array) {
      document_error_at(path + ".tables", "expected an array", tables->begin);
    }
    if (tables->array.size() > 100U) {
      document_error(path + ".tables", "table count exceeds 100");
    }
    table_ranges.reserve(tables->array.size());
    for (std::size_t index = 0U; index < tables->array.size(); ++index) {
      const JsonValue &table = tables->array[index];
      const std::string table_path =
          path + ".tables[" + std::to_string(index) + "]";
      require_object(table, table_path);
      const JsonValue *start_value = find_member(table, "start");
      const JsonValue *length_value = find_member(table, "length");
      const JsonValue *columns_value = find_member(table, "columns");
      const JsonValue *cells_value = find_member(table, "cells");
      for (const auto &member : table.object) {
        if (member.first != "start" && member.first != "length" &&
            member.first != "columns" && member.first != "cells") {
          document_error_at(table_path + "." + member.first,
                            "unknown table member", member.second.begin);
        }
      }
      for (const auto &required :
           {std::pair<const JsonValue *, const char *>(start_value, "start"),
            std::pair<const JsonValue *, const char *>(length_value, "length"),
            std::pair<const JsonValue *, const char *>(columns_value, "columns"),
            std::pair<const JsonValue *, const char *>(cells_value, "cells")}) {
        if (required.first == nullptr) {
          document_error_at(table_path + "." + required.second,
                            "missing required member '" +
                                std::string(required.second) + "'",
                            table.end);
        }
      }
      const std::size_t start =
          checked_format_size(formatting, *start_value, table_path + ".start");
      const std::size_t length =
          checked_format_size(formatting, *length_value, table_path + ".length");
      if (length == 0U) {
        document_error_at(table_path + ".length", "length must be positive",
                          length_value->begin);
      }
      if (length > std::numeric_limits<std::size_t>::max() - start) {
        document_error_at(table_path, "range is out of bounds", table.begin);
      }
      const TextRange range{start, start + length};
      validate_range_bounds(layout, range, table_path, table.begin);
      if (!is_paragraph_start(layout, range.start) ||
          !ends_after_lf(layout, range.end)) {
        document_error_at(table_path,
                          "range must cover complete LF-delimited paragraphs",
                          table.begin);
      }
      validate_metadata_range_order(range, &previous_table, &have_previous_table,
                                    table_path, table.begin);
      table_ranges.push_back(range);

      const std::size_t columns = checked_format_size(
          formatting, *columns_value, table_path + ".columns");
      if (columns < 1U || columns > 12U) {
        document_error_at(table_path + ".columns", "columns must be in [1, 12]",
                          columns_value->begin);
      }
      if (cells_value->kind != JsonKind::Array) {
        document_error_at(table_path + ".cells", "expected an array",
                          cells_value->begin);
      }
      if (cells_value->array.empty() || cells_value->array.size() > 240U ||
          cells_value->array.size() % columns != 0U) {
        document_error_at(table_path + ".cells",
                          "cell count must be a positive multiple of columns and at most 240",
                          cells_value->begin);
      }
      std::size_t next_cell_start = range.start;
      for (std::size_t cell_index = 0U; cell_index < cells_value->array.size();
           ++cell_index) {
        const JsonValue &cell = cells_value->array[cell_index];
        const std::string cell_path = table_path + ".cells[" +
                                      std::to_string(cell_index) + "]";
        require_object(cell, cell_path);
        const JsonValue *cell_start_value = find_member(cell, "start");
        const JsonValue *cell_length_value = find_member(cell, "length");
        for (const auto &member : cell.object) {
          if (member.first != "start" && member.first != "length") {
            document_error_at(cell_path + "." + member.first,
                              "unknown table cell member", member.second.begin);
          }
        }
        if (cell_start_value == nullptr) {
          document_error_at(cell_path + ".start",
                            "missing required member 'start'", cell.end);
        }
        if (cell_length_value == nullptr) {
          document_error_at(cell_path + ".length",
                            "missing required member 'length'", cell.end);
        }
        const std::size_t cell_start = checked_format_size(
            formatting, *cell_start_value, cell_path + ".start");
        const std::size_t cell_length = checked_format_size(
            formatting, *cell_length_value, cell_path + ".length");
        if (cell_length == 0U ||
            cell_length > std::numeric_limits<std::size_t>::max() - cell_start) {
          document_error_at(cell_path + ".length", "length must be positive and in range",
                            cell_length_value->begin);
        }
        const TextRange cell_range{cell_start, cell_start + cell_length};
        validate_range_bounds(layout, cell_range, cell_path, cell.begin);
        if (cell_start != next_cell_start || !ends_after_lf(layout, cell_range.end)) {
          document_error_at(cell_path,
                            "cells must be consecutive and each must end after LF",
                            cell.begin);
        }
        next_cell_start = cell_range.end;
      }
      if (next_cell_start != range.end) {
        document_error_at(table_path + ".cells",
                          "cells must partition the complete table range",
                          cells_value->begin);
      }
    }
  }

  for (const TextRange &paragraph : paragraph_ranges) {
    for (const TextRange &table : table_ranges) {
      if (paragraph.start < table.end && table.start < paragraph.end) {
        document_error(path, "paragraph metadata and tables must not overlap");
      }
    }
  }
}

void validate_extra_fields(const ProjectExtraFields &extras,
                           const std::set<std::string> &reserved,
                           const std::string &path) {
  for (const auto &entry : extras) {
    const std::string &key = entry.first;
    const std::string &raw = entry.second;
    validate_utf8(key, path + ".<extra-key>");
    if (reserved.count(key) != 0U) {
      document_error(path + "." + key,
                     "extra field collides with a reserved member");
    }
    if (raw.size() > kMaxJsonBytes) {
      document_error(path + "." + key, "extra JSON exceeds 16 MiB limit");
    }
    try {
      JsonParser(raw).parse();
    } catch (const std::runtime_error &error) {
      document_error(path + "." + key,
                     std::string("extra field is invalid JSON: ") +
                         error.what());
    }
  }
}

bool is_identifier_start(char value) {
  return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
         value == '_';
}

bool is_identifier_continue(char value) {
  return is_identifier_start(value) || (value >= '0' && value <= '9');
}

void validate_module_id(const std::string &id, const std::string &path) {
  validate_utf8(id, path);
  if (id.empty() || !is_identifier_start(id.front()) ||
      !std::all_of(id.begin() + 1, id.end(), is_identifier_continue)) {
    document_error(path, "module id must match [A-Za-z_][A-Za-z0-9_]*");
  }
}

void validate_module_path(const std::string &path_value,
                          const std::string &path) {
  validate_utf8(path_value, path);
  if (path_value.empty() || path_value.front() == '/' ||
      path_value.back() == '/' || path_value.find('\\') != std::string::npos ||
      path_value.find(':') != std::string::npos || !sputnik::is_source_path(path_value)) {
    document_error(path,
                   "module path must be a relative slash-separated .s, .spu or .sputnik path");
  }
  std::size_t component_start = 0U;
  while (component_start < path_value.size()) {
    const std::size_t slash = path_value.find('/', component_start);
    const std::size_t component_end =
        slash == std::string::npos ? path_value.size() : slash;
    const std::string component =
        path_value.substr(component_start, component_end - component_start);
    if (component.empty() || component == "." || component == "..") {
      document_error(path, "module path contains an invalid component");
    }
    for (std::size_t position = component_start; position < component_end;) {
      std::size_t length = 0;
      std::uint32_t codepoint = 0;
      if (!decode_utf8_sequence(path_value, position, &length, &codepoint) ||
          is_json_control(codepoint)) {
        document_error(path, "module path contains a control character");
      }
      position += length;
    }
    if (slash == std::string::npos) {
      break;
    }
    component_start = slash + 1U;
  }
}

void validate_cell(const ProjectCell &cell, std::set<CellId> *cell_ids,
                   const std::string &path) {
  if (cell.id == 0U || cell.id == std::numeric_limits<CellId>::max()) {
    document_error(path + ".id", "cell id must be in [1, UINT64_MAX - 1]");
  }
  if (!cell_ids->insert(cell.id).second) {
    document_error(path + ".id", "cell id is not unique project-wide");
  }
  validate_utf8(cell.kind, path + ".kind");
  if (cell.kind.empty()) {
    document_error(path + ".kind", "cell kind must not be empty");
  }
  if (cell.kind == "code") {
    validate_utf8(cell.source, path + ".source");
    if (cell.mode != CellMode::Watch && cell.mode != CellMode::Manual) {
      document_error(path + ".mode", "cell mode is invalid");
    }
    validate_extra_fields(cell.extra, {"id", "kind", "source", "mode"}, path);
  } else if (cell.kind == "text") {
    validate_project_text_content({cell.source, cell.formatting});
    validate_extra_fields(cell.extra, {"id", "kind", "source", "formatting"},
                          path);
  } else {
    // Opaque future kinds may carry fields named source/mode; they are not
    // interpreted until that kind has a schema of its own.
    validate_extra_fields(cell.extra, {"id", "kind"}, path);
  }
}

} // namespace

bool project_image_option(const ProjectComponent &component, const std::string &key) {
  const auto found = component.extra.find(key);
  if (found == component.extra.end()) return false;
  const auto value = parse_project_input_value(found->second);
  if (const auto flag = std::get_if<bool>(&value)) return *flag;
  throw std::runtime_error("component." + key + " must be a boolean");
}

std::string project_image_channel(const ProjectComponent &component) {
  const auto found = component.extra.find("image_id");
  return found == component.extra.end() ? component.id :
      std::get<std::string>(parse_project_input_value(found->second));
}

std::vector<ProjectImageAsset> project_image_assets(const ProjectComponent &component) {
  const auto found = component.extra.find("image_items");
  if (found == component.extra.end()) return {};
  const auto &json = found->second;
  const auto value = JsonParser(json).parse();
  std::vector<ProjectImageAsset> result;
  for (const auto &item : require_array(value, "component.image_items")) {
    require_object(item, "image asset");
    ProjectImageAsset asset{require_string(require_member(item, "path", "image asset"), "image path"), ""};
    if (const auto caption = find_member(item, "caption")) asset.caption = require_string(*caption, "image caption");
    // Assets are ordinary immutable members of the project, never external links.
    if (asset.path.rfind("images/", 0) != 0 || asset.path.size() > 256 ||
        asset.path.find("..") != std::string::npos || asset.path.find('\\') != std::string::npos ||
        asset.path.find('\0') != std::string::npos || asset.path.find('/', 7) != std::string::npos ||
        (asset.path.size() < 11 || (asset.path.substr(asset.path.size() - 4) != ".png" && asset.path.substr(asset.path.size() - 4) != ".jpg")))
      throw std::runtime_error("invalid project image asset path");
    if (asset.caption.size() > 65536 || result.size() >= 64) throw std::runtime_error("image asset limits exceeded");
    result.push_back(std::move(asset));
  }
  return result;
}

std::string serialize_project_image_assets(const std::vector<ProjectImageAsset> &assets) {
  std::string result = "[";
  for (std::size_t i = 0; i < assets.size(); ++i) {
    if (i) result += ',';
    result += "{\"path\":"; write_json_string(assets[i].path, &result);
    result += ",\"caption\":"; write_json_string(assets[i].caption, &result); result += '}';
  }
  return result + ']';
}

std::string project_component_data_channel(const ProjectComponent &c, const std::string &kind) {
  const auto field = kind == "selection" ? "selection_id" : "annotation_id";
  const auto found = c.extra.find(field);
  const auto channel = found == c.extra.end() ? (kind != "selection" ? c.id + "_labels" : std::string{}) : std::get<std::string>(parse_project_input_value(found->second));
  if (channel.size() > 256 || channel.find('\0') != std::string::npos || (kind != "selection" && channel.empty()))
    throw std::runtime_error("invalid component data channel");
  return channel;
}
struct AnnotationAttributeDefinition {
  std::string title, type;
  std::set<std::string> options;
  bool operator==(const AnnotationAttributeDefinition &other) const { return title == other.title && type == other.type && options == other.options; }
};
struct AnnotationClassDefinition {
  std::string title;
  std::map<std::string, AnnotationAttributeDefinition> attributes;
  bool operator==(const AnnotationClassDefinition &other) const { return title == other.title && attributes == other.attributes; }
};
using AnnotationClassDefinitions = std::map<std::string, AnnotationClassDefinition>;
static std::string annotation_text(const JsonValue &object, const char *key, std::size_t limit) {
  const auto value = require_string(require_member(object, key, "annotation metadata"), key);
  if (value.size() > limit || value.find('\0') != std::string::npos) throw std::runtime_error(std::string("invalid annotation ") + key);
  return value;
}
static bool annotation_nonblank(const std::string &value) { return value.find_first_not_of(" \t\n\r\f\v") != std::string::npos; }
static AnnotationClassDefinitions parse_annotation_classes(const JsonValue &definitions) {
  AnnotationClassDefinitions classes;
  const auto &items = require_array(definitions, "annotation classes");
  if (items.size() > 32) throw std::runtime_error("annotations exceed 32 classes");
  for (const auto &definition : items) {
    require_object(definition, "annotation class");
    const auto id = annotation_text(definition, "id", 64), title = annotation_text(definition, "title", 128);
    if (id.empty() || !annotation_nonblank(title) || !classes.emplace(id, AnnotationClassDefinition{title, {}}).second) throw std::runtime_error("invalid/duplicate annotation class");
    const auto &attributes = require_array(require_member(definition, "attributes", "annotation class"), "attributes");
    if (attributes.size() > 16) throw std::runtime_error("annotation class exceeds 16 attributes");
    for (const auto &attribute : attributes) {
      require_object(attribute, "class attribute");
      const auto key = annotation_text(attribute, "id", 64), name = annotation_text(attribute, "title", 128), type = annotation_text(attribute, "type", 16);
      if (key.empty() || !annotation_nonblank(name) || (type != "text" && type != "number" && type != "boolean" && type != "choice")) throw std::runtime_error("invalid annotation class attribute");
      AnnotationAttributeDefinition spec{name, type, {}};
      const auto &options = require_array(require_member(attribute, "options", "attribute"), "options");
      if (options.size() > 64) throw std::runtime_error("too many attribute options");
      for (const auto &option : options) {
        const auto value = require_string(option, "attribute option");
        if (value.empty() || value.size() > 128 || value.find('\0') != std::string::npos || !spec.options.insert(value).second) throw std::runtime_error("invalid attribute option");
      }
      if ((type == "choice") != !options.empty() || !classes.at(id).attributes.emplace(key, spec).second) throw std::runtime_error("invalid/duplicate attribute definition");
    }
  }
  return classes;
}
static void validate_annotation_region(const std::string &json, const JsonValue &region, const AnnotationClassDefinitions &classes, bool required) {
  std::set<std::string> assigned;
  if (const auto values = find_member(region, "classes")) {
    for (const auto &value : require_array(*values, "region classes")) {
      const auto key = require_string(value, "class id");
      if (!classes.count(key) || !assigned.insert(key).second) throw std::runtime_error("unknown/duplicate annotation class");
    }
  } else if (required) throw std::runtime_error("annotation region requires classes");
  const auto attributes = find_member(region, "attributes");
  if (!attributes) { if (required) throw std::runtime_error("annotation region requires attributes"); return; }
  require_object(*attributes, "annotation attributes");
  for (const auto &[class_id, values] : attributes->object) {
    if (!assigned.count(class_id)) throw std::runtime_error("attributes require an assigned class");
    require_object(values, "class attribute values");
    for (const auto &[key, value] : values.object) {
      const auto &specs = classes.at(class_id).attributes;
      const auto found = specs.find(key);
      if (found == specs.end()) throw std::runtime_error("unknown secondary class attribute");
      const auto scalar = parse_project_input_value(raw_json(json, value)); const auto &type = found->second.type;
      const auto string = std::get_if<std::string>(&scalar);
      if (type == "text" && (!string || string->size() > 2048 || string->find('\0') != std::string::npos)) throw std::runtime_error("attribute requires bounded text");
      if (type == "choice" && (!string || !found->second.options.count(*string))) throw std::runtime_error("unknown attribute choice");
      if (type == "boolean" && !std::holds_alternative<bool>(scalar)) throw std::runtime_error("attribute requires Bool");
      if (type == "number" && !std::holds_alternative<double>(scalar) && !std::holds_alternative<std::int64_t>(scalar)) throw std::runtime_error("attribute requires a finite number");
    }
  }
}
static bool annotation_flag(const ProjectComponent &component, const char *key, bool fallback) {
  const auto found = component.extra.find(key); if (found == component.extra.end()) return fallback;
  const auto value = parse_project_input_value(found->second); const auto flag = std::get_if<bool>(&value);
  if (!flag) throw std::runtime_error(std::string(key) + " must be Bool");
  return *flag;
}
static AnnotationClassDefinitions annotation_configured_classes(const ProjectComponent &component) {
  if (const auto found = component.extra.find("annotation_classes"); found != component.extra.end()) {
    return parse_annotation_classes(JsonParser(found->second, 256U * 1024U).parse());
  }
  // Read a legacy schema only from already accepted, persisted metadata.
  const auto found = component.extra.find(component.kind == "audio_annotator" ? "audio_annotations" : "annotations");
  if (found != component.extra.end() && found->second != "null") {
    const auto root = JsonParser(found->second, 512U * 1024U).parse();
    if (const auto classes = find_member(root, "classes")) return parse_annotation_classes(*classes);
  }
  return {};
}
static void validate_annotation_configuration(const ProjectComponent &component) {
  (void)annotation_configured_classes(component);
  if (component.kind != "audio_annotator") return;
  for (const auto *prefix : {"audio_transcript", "audio_segment_transcript"}) {
    const std::string allowed = std::string(prefix) + "_allowed", required = std::string(prefix) + "_required";
    if (annotation_flag(component, required.c_str(), false) && !annotation_flag(component, allowed.c_str(), true)) throw std::runtime_error("a required transcript must also be allowed");
    (void)annotation_flag(component, allowed.c_str(), true);
  }
}
void validate_project_annotation_data(const ProjectComponent &component, const std::string &kind, const std::string &json) {
  if (json == "null" || kind == "selection") return;
  const auto root = JsonParser(json, 512U * 1024U).parse();
  const auto definitions = find_member(root, "classes");
  const auto actual = definitions ? parse_annotation_classes(*definitions) : AnnotationClassDefinitions{};
  if (annotation_configured_classes(component) != actual) throw std::runtime_error("annotation classes/attributes changed; use the schema configured in Board Designer");
  if (kind != "audio_annotations") return;
  auto transcript = [&](const JsonValue &object, bool segment) {
    const auto text = annotation_text(object, "transcript", segment ? 8192 : 65536);
    const char *allowed = segment ? "audio_segment_transcript_allowed" : "audio_transcript_allowed";
    const char *required = segment ? "audio_segment_transcript_required" : "audio_transcript_required";
    if (!annotation_flag(component, allowed, true) && !text.empty()) throw std::runtime_error("transcript is disabled in Board Designer");
    if (annotation_flag(component, required, false) && !annotation_nonblank(text)) throw std::runtime_error(segment ? "each segment requires a transcript" : "a whole-track transcript is required");
  };
  transcript(root, false);
  for (const auto &segment : require_array(require_member(root, "segments", "audio annotations"), "segments")) transcript(segment, true);
}
static void validate_audio_annotations(const std::string &json, const JsonValue &root) {
  auto text = [&](const JsonValue &object, const char *key, std::size_t limit) {
    const auto value = require_string(require_member(object, key, "audio annotations"), key);
    if (value.size() > limit || value.find('\0') != std::string::npos) throw std::runtime_error(std::string("invalid audio annotation ") + key);
    return value;
  };
  auto integer = [&](const JsonValue &object, const char *key, std::int64_t low, std::int64_t high) {
    const auto value = parse_project_input_value(raw_json(json, require_member(object, key, "audio annotations")));
    const auto number = std::get_if<std::int64_t>(&value);
    if (!number || *number < low || *number > high) throw std::runtime_error(std::string("invalid audio annotation ") + key);
    return *number;
  };
  const auto rate = integer(root, "sample_rate", 8000, 192000), channels = integer(root, "channels", 1, 2);
  const auto frames = integer(root, "frames", 1, (16 * 1024 * 1024 - 64) / (channels * 4));
  const auto source = text(root, "source", 16), channel = text(root, "channel", 256);
  const auto cell = integer(root, "cell", 0, INT64_MAX);
  if (channel.empty() || (source != "input" && source != "cell") || (source == "input" ? cell != 0 : cell == 0)) throw std::runtime_error("invalid audio annotation source");
  (void)text(root, "transcript", 65536);
  const auto classes = parse_annotation_classes(require_member(root, "classes", "audio annotations"));
  const auto &segments = require_array(require_member(root, "segments", "audio annotations"), "segments");
  if (segments.size() > 1000) throw std::runtime_error("audio annotations exceed 1000 segments");
  std::set<std::string> ids;
  for (const auto &segment : segments) {
    require_object(segment, "audio segment");
    const auto id = text(segment, "id", 64);
    if (id.empty() || !ids.insert(id).second) throw std::runtime_error("invalid/duplicate audio segment");
    (void)text(segment, "title", 256); (void)text(segment, "transcript", 8192);
    const auto first = integer(segment, "start_frame", 0, frames - 1), last = integer(segment, "end_frame", 1, frames);
    if (first >= last) throw std::runtime_error("empty/reversed audio segment");
    const auto low = find_member(segment, "low_hz"), high = find_member(segment, "high_hz");
    const bool has_low = low && low->kind != JsonKind::Null, has_high = high && high->kind != JsonKind::Null;
    if (has_low != has_high) throw std::runtime_error("frequency selection requires both bounds");
    if (has_low) {
      auto frequency = [&](const JsonValue &value) {
        const auto scalar = parse_project_input_value(raw_json(json, value));
        if (const auto v = std::get_if<double>(&scalar)) return *v;
        if (const auto v = std::get_if<std::int64_t>(&scalar)) return double(*v);
        throw std::runtime_error("frequency must be numeric");
      };
      const auto a = frequency(*low), b = frequency(*high);
      if (!std::isfinite(a) || !std::isfinite(b) || a < 0 || a >= b || b > double(rate) / 2) throw std::runtime_error("frequency rectangle exceeds Nyquist bounds");
    }
    validate_annotation_region(json, segment, classes, true);
  }
}
static void validate_project_component_data_impl(const std::string &kind, const std::string &json, bool whole_image_pixels) {
  if (kind != "selection" && kind != "annotations" && kind != "audio_annotations") throw std::runtime_error("unknown component data kind");
  if (json == "null") return;
  if (json.size() > (kind == "audio_annotations" ? 512U : 256U) * 1024U) throw std::runtime_error("accepted component data exceeds its size limit");
  const auto root = JsonParser(json, 512U * 1024U).parse(); require_object(root, "component data");
  const auto version = parse_project_input_value(raw_json(json, require_member(root, "version", "component data")));
  if (!std::holds_alternative<std::int64_t>(version) || std::get<std::int64_t>(version) != 1) throw std::runtime_error("unsupported component data version");
  auto hash = [&](const char *key) {
    const auto value = require_string(require_member(root, key, "component data"), key);
    if (value.size() != 64 || value.find_first_not_of("0123456789abcdef") != std::string::npos) throw std::runtime_error("invalid component data asset/source identity");
  };
  if (kind == "selection") {
    hash("source"); (void)parse_project_input_value(raw_json(json, require_member(root, "key", "selection")));
    const auto &row = require_member(root, "row", "selection"); require_object(row, "selected row");
    if (row.object.size() > 32) throw std::runtime_error("selected row exceeds 32 fields");
    for (const auto &[name, value] : row.object) {
      if (name.size() > 1024) throw std::runtime_error("selected column name exceeds limits");
      if (value.kind == JsonKind::Null) continue;
      const auto field = parse_project_input_value(raw_json(json, value));
      if (const auto text = std::get_if<std::string>(&field); text && text->size() > 24576) throw std::runtime_error("selected field exceeds display limits");
    }
    return;
  }
  if (kind == "audio_annotations") { hash("asset"); validate_audio_annotations(json, root); return; }
  if (kind != "annotations") throw std::runtime_error("unknown component data kind");
  hash("asset");
  const auto channel = require_string(require_member(root, "channel", "annotations"), "channel");
  if (channel.empty() || channel.size() > 256 || channel.find('\0') != std::string::npos) throw std::runtime_error("invalid annotation image channel");
  auto number = [&](const JsonValue &object, const char *key) {
    const auto value = parse_project_input_value(raw_json(json, require_member(object, key, "annotations")));
    if (const auto n = std::get_if<std::int64_t>(&value)) return double(*n);
    if (const auto n = std::get_if<double>(&value); n && std::isfinite(*n)) {
      if (whole_image_pixels && std::floor(*n) != *n) throw std::runtime_error("image annotation coordinates and sizes must be whole pixels");
      return *n;
    }
    throw std::runtime_error("annotation coordinates must be finite numbers");
  };
  const auto width = number(root, "width"), height = number(root, "height");
  if (width < 1 || height < 1 || width > 8192 || height > 8192 || width * height > 16000000 || std::floor(width) != width || std::floor(height) != height)
    throw std::runtime_error("invalid annotation image size");
  const auto &shapes = require_array(require_member(root, "shapes", "annotations"), "shapes");
  const auto definitions = find_member(root, "classes");
  const auto classes = definitions ? parse_annotation_classes(*definitions) : AnnotationClassDefinitions{};
  if (shapes.size() > 500) throw std::runtime_error("annotations exceed 500 shapes");
  std::set<std::string> ids;
  for (const auto &shape : shapes) {
    require_object(shape, "shape");
    validate_annotation_region(json, shape, classes, false);
    const auto id = require_string(require_member(shape, "id", "shape"), "shape.id"), label = require_string(require_member(shape, "label", "shape"), "shape.label");
    const auto type = require_string(require_member(shape, "kind", "shape"), "shape.kind");
    if (id.empty() || id.size() > 64 || id.find('\0') != std::string::npos || label.size() > 128 || label.find('\0') != std::string::npos || !ids.insert(id).second)
      throw std::runtime_error("invalid annotation identity/label");
    const auto x = number(shape, "x"), y = number(shape, "y");
    if (x < 0 || y < 0 || x >= width || y >= height) throw std::runtime_error("annotation lies outside the source image");
    if (type == "rect") {
      const auto w = number(shape, "width"), h = number(shape, "height");
      if (w <= 0 || h <= 0 || x + w > width || y + h > height) throw std::runtime_error("annotation rectangle exceeds the source image");
    } else if (type != "point") throw std::runtime_error("annotation kind must be point or rect");
  }
}
void validate_project_component_data(const std::string &kind, const std::string &json) {
  validate_project_component_data_impl(kind, json, true);
}
ProjectComponentDataSource project_component_data_source(const std::string &kind, const std::string &json) {
  // Source identity is also needed to replace images carrying a legacy draft.
  validate_project_component_data_impl(kind, json, false); ProjectComponentDataSource out; if (json == "null") return out;
  const auto root = JsonParser(json, 512U * 1024U).parse();
  out.identity = require_string(require_member(root, kind == "selection" ? "source" : "asset", "data"), "identity");
  if (kind == "audio_annotations") {
    out.channel = require_string(require_member(root, "channel", "data"), "channel");
    out.source = require_string(require_member(root, "source", "data"), "source");
    auto number = [&](const char *key) { return std::uint64_t(std::get<std::int64_t>(parse_project_input_value(raw_json(json, require_member(root, key, "data"))))); };
    out.rate = number("sample_rate"); out.channels = number("channels"); out.frames = number("frames"); out.cell = number("cell");
  }
  if (kind == "annotations") {
    out.channel = require_string(require_member(root, "channel", "data"), "channel");
    const auto number = [&](const char *key) {
      const auto value = parse_project_input_value(raw_json(json, require_member(root, key, "data")));
      return std::holds_alternative<double>(value) ? unsigned(std::get<double>(value)) : unsigned(std::get<std::int64_t>(value));
    };
    out.width = number("width"); out.height = number("height");
  }
  return out;
}
ProjectComponentDataEdit parse_project_component_data_edit(const std::string &json) {
  const auto root = JsonParser(json, 2U * 1024U * 1024U).parse(); require_object(root, "component data edit");
  ProjectComponentDataEdit edit;
  edit.board = require_string(require_member(root, "board", "edit"), "board");
  edit.component = require_string(require_member(root, "component", "edit"), "component");
  edit.kind = require_string(require_member(root, "kind", "edit"), "kind");
  edit.json = require_string(require_member(root, "json", "edit"), "json");
  validate_project_component_data(edit.kind, edit.json); return edit;
}

std::string project_audio_source(const ProjectComponent &c) {
  const auto found = c.extra.find("audio_source");
  const auto source = found == c.extra.end() ? std::string("file") : std::get<std::string>(parse_project_input_value(found->second));
  if (source != "file" && source != "input" && source != "cell") throw std::runtime_error("invalid audio annotation source mode");
  return source;
}
bool project_owns_audio_input(const ProjectComponent &c) {
  return c.kind == "audio_input" || (c.kind == "audio_annotator" && project_audio_source(c) == "file");
}
std::string project_audio_channel(const ProjectComponent &c) {
  const auto found = c.extra.find("audio_id");
  const auto id = found == c.extra.end() ? c.id : std::get<std::string>(parse_project_input_value(found->second));
  if (id.empty() || id.size() > 256 || id.find('\0') != std::string::npos) throw std::runtime_error("invalid audio input channel");
  return id;
}
std::string project_audio_asset(const ProjectComponent &c) {
  const auto found = c.extra.find("audio_asset");
  if (found == c.extra.end()) return {};
  const auto path = std::get<std::string>(parse_project_input_value(found->second));
  if (path.empty()) return {};
  if (path.rfind("audio/", 0) || path.size() > 256 || path.find("..") != std::string::npos ||
      path.find('\\') != std::string::npos || path.find('\0') != std::string::npos || path.find('/', 6) != std::string::npos)
    throw std::runtime_error("invalid project audio asset path");
  const auto dot = path.rfind('.');
  if (dot == std::string::npos || runtime::audio_format(path.substr(dot + 1)) != path.substr(dot + 1))
    throw std::runtime_error("audio asset requires a canonical file extension");
  return path;
}
ProjectAudioEdit parse_project_audio_edit(const std::string &json) {
  const auto value = JsonParser(json, 24U * 1024U * 1024U).parse();
  require_object(value, "audio edit"); ProjectAudioEdit out;
  out.board = require_string(require_member(value, "board", "audio edit"), "board");
  out.component = require_string(require_member(value, "component", "audio edit"), "component");
  out.name = require_string(require_member(value, "name", "audio edit"), "name");
  if (out.name.size() > 1024 || out.name.find('\0') != std::string::npos) throw std::runtime_error("invalid audio input name");
  const auto bytes = runtime::audio_unbase64(require_string(require_member(value, "data", "audio edit"), "data"));
  if (!bytes.empty()) {
    runtime::AudioDisplay audio; audio.bytes = bytes;
    audio.format = runtime::audio_format(require_string(require_member(value, "format", "audio edit"), "format"));
    (void)runtime::audio_encode(audio); out.audio = std::move(audio);
  }
  return out;
}

ProjectImageEdit parse_project_image_edit(const std::string &json) {
  const auto value = JsonParser(json, 96U * 1024U * 1024U).parse();
  require_object(value, "image edit");
  ProjectImageEdit result;
  result.board = require_string(require_member(value, "board", "image edit"), "board");
  result.component = require_string(require_member(value, "component", "image edit"), "component");
  result.caption = require_string(require_member(value, "caption", "image edit"), "caption");
  const auto selected = parse_project_input_value(raw_json(json, require_member(value, "selected", "image edit")));
  if (!std::holds_alternative<std::int64_t>(selected) || std::get<std::int64_t>(selected) < 0)
    throw std::runtime_error("image selection must be a nonnegative integer");
  result.selected = std::get<std::int64_t>(selected);
  std::size_t bytes = 0, text = result.caption.size();
  for (const auto &item : require_array(require_member(value, "images", "image edit"), "images")) {
    runtime::NotebookDisplay image;
    image.mime = require_string(require_member(item, "mime", "image"), "mime");
    image.caption = require_string(require_member(item, "caption", "image"), "caption");
    const auto encoded = require_string(require_member(item, "data", "image"), "data");
    if (encoded.size() > 24U * 1024U * 1024U || encoded.size() % 4) throw std::runtime_error("invalid base64 image size");
    auto digit = [](char c) -> int {
      if (c >= 'A' && c <= 'Z') return c - 'A'; if (c >= 'a' && c <= 'z') return c - 'a' + 26;
      if (c >= '0' && c <= '9') return c - '0' + 52; if (c == '+') return 62; if (c == '/') return 63; return -1;
    };
    for (std::size_t i = 0; i < encoded.size(); i += 4) {
      const int a = digit(encoded[i]), b = digit(encoded[i+1]);
      const int c = encoded[i+2] == '=' ? 0 : digit(encoded[i+2]), d = encoded[i+3] == '=' ? 0 : digit(encoded[i+3]);
      const bool pad2 = encoded[i+2] == '=', pad1 = encoded[i+3] == '=';
      if (a < 0 || b < 0 || c < 0 || d < 0 || (pad2 && !pad1) ||
          ((pad1 || pad2) && i + 4 != encoded.size()) || (pad2 && (b & 15)) || (pad1 && !pad2 && (c & 3)))
        throw std::runtime_error("invalid base64 image data");
      image.bytes += static_cast<char>((a << 2) | (b >> 4));
      if (!pad2) image.bytes += static_cast<char>((b << 4) | (c >> 2));
      if (!pad1) image.bytes += static_cast<char>((c << 6) | d);
    }
    const auto size = runtime::notebook_image_dimensions(image.bytes, image.mime);
    image.width = size.first; image.height = size.second;
    runtime::notebook_validate_image(image);
    bytes += image.bytes.size(); text += image.caption.size();
    if (result.images.size() >= 64 || bytes > 64U * 1024U * 1024U || text > 256U * 1024U || result.caption.size() > 65536)
      throw std::runtime_error("image collection limits exceeded");
    result.images.push_back(std::move(image));
  }
  if (result.selected >= std::max<std::size_t>(1, result.images.size())) throw std::runtime_error("image selection is out of range");
  return result;
}

runtime::NotebookInputValue parse_project_input_value(const std::string &json) {
  const auto value = JsonParser(json).parse();
  if (value.kind == JsonKind::Object) {
    runtime::NotebookInputRange range;
    auto number = [&](const char *key) -> runtime::NotebookInputNumber {
      const auto &field = require_member(value, key, "range");
      if (field.kind != JsonKind::Number) throw std::runtime_error(std::string("range.") + key + " must be numeric");
      const auto parsed = parse_project_input_value(raw_json(json, field));
      if (const auto i = std::get_if<std::int64_t>(&parsed)) return *i;
      return std::get<double>(parsed);
    };
    if (value.object.size() != 4) throw std::runtime_error("range requires start, finish, inclusive_end and step");
    range.start = number("start"); range.finish = number("finish"); range.step = number("step");
    const auto &inclusive = require_member(value, "inclusive_end", "range");
    if (inclusive.kind != JsonKind::Boolean) throw std::runtime_error("range.inclusive_end must be boolean");
    range.inclusive_end = raw_json(json, inclusive) == "true";
    const bool integers = std::holds_alternative<std::int64_t>(range.start) && std::holds_alternative<std::int64_t>(range.finish);
    if (integers && !std::holds_alternative<std::int64_t>(range.step)) throw std::runtime_error("integer range step must be Int64");
    if (std::visit([](auto n) { return n == 0; }, range.step)) throw std::runtime_error("range step must not be zero");
    return range;
  }
  if (value.kind == JsonKind::String) return value.text;
  if (value.kind == JsonKind::Boolean) return raw_json(json, value) == "true";
  if (value.kind == JsonKind::Number) {
    const auto number = raw_json(json, value);
    try {
      if (number.find_first_of(".eE") == std::string::npos)
        return static_cast<std::int64_t>(std::stoll(number));
      std::istringstream stream(number);
      stream.imbue(std::locale::classic());
      double result = 0;
      stream >> result;
      if (!stream.fail() && stream.eof() && std::isfinite(result)) return result;
    } catch (const std::exception &) {}
  }
  throw std::runtime_error("project input must be a finite number, Int64, boolean, string or range");
}

std::string project_config_json(const std::string &source, bool pretty) {
  if (source.size() > 65536 || source.find('\0') != std::string::npos)
    throw std::runtime_error("JSON config must fit 64 KiB and contain no NUL");
  auto fail = [&](std::size_t offset, const std::string &message) -> void {
    std::size_t line = 1, column = 1;
    for (std::size_t i = 0; i < std::min(offset, source.size()); ++i) {
      if (source[i] == '\n') { ++line; column = 1; }
      else if ((static_cast<unsigned char>(source[i]) & 0xc0U) != 0x80U) ++column;
    }
    throw std::runtime_error("JSON " + std::to_string(line) + ":" + std::to_string(column) + ": " + message);
  };
  JsonValue root;
  try { root = JsonParser(source).parse(); }
  catch (const std::exception &error) {
    const std::string message = error.what(), prefix = "project JSON error at byte ";
    const auto end = message.find(": ", prefix.size());
    if (message.rfind(prefix, 0) == 0 && end != std::string::npos)
      fail(std::stoull(message.substr(prefix.size(), end - prefix.size())), message.substr(end + 2));
    throw;
  }
  std::string output;
  auto newline = [&](unsigned depth) { output += '\n'; output.append(depth * 2, ' '); };
  std::function<void(const JsonValue &, unsigned)> visit = [&](const auto &value, unsigned depth) {
    if (value.kind == JsonKind::Number) {
      try { (void)parse_project_input_value(raw_json(source, value)); }
      catch (...) { fail(value.begin, "number must fit Int64 or a finite Float"); }
    }
    if (value.kind == JsonKind::Array) {
      if (pretty) output += '[';
      for (std::size_t i = 0; i < value.array.size(); ++i) {
        if (pretty) { if (i) output += ','; newline(depth + 1); }
        visit(value.array[i], depth + 1);
      }
      if (pretty) { if (!value.array.empty()) newline(depth); output += ']'; }
    } else if (value.kind == JsonKind::Object) {
      if (pretty) output += '{';
      for (std::size_t i = 0; i < value.object.size(); ++i) {
        if (pretty) {
          if (i) output += ',';
          newline(depth + 1); write_json_string(value.object[i].first, &output); output += ": ";
        }
        visit(value.object[i].second, depth + 1);
      }
      if (pretty) { if (!value.object.empty()) newline(depth); output += '}'; }
    } else if (pretty) output += raw_json(source, value);
    if (output.size() > 65536) throw std::runtime_error("Formatted JSON exceeds 64 KiB");
  };
  visit(root, 0);
  return pretty ? output : source;
}

std::string project_config_document(const std::string &source, const std::string &format, bool pretty) {
  if (format == "json") return project_config_json(source, pretty);
  (void)runtime::config_to_json(source, format);
  if (pretty) throw std::runtime_error("Format is available for JSON; YAML/TOML source retains comments and layout");
  return source;
}

static std::string config_control_format(const std::string &json) {
  const auto control = JsonParser(json).parse();
  const auto format = find_member(control, "config_format");
  return format ? require_string(*format, "control.config_format") : "json";
}

std::map<std::string, std::string> parse_project_input_edits(const std::string &json) {
  const auto root = JsonParser(json).parse(); require_object(root, "input edits");
  if (root.object.size() > 1024) throw std::runtime_error("input batch exceeds 1024 values");
  std::map<std::string, std::string> result;
  for (const auto &entry : root.object) {
    const auto text = require_string(entry.second, "input value JSON");
    (void)parse_project_input_value(text);
    if (!result.emplace(entry.first, text).second) throw std::runtime_error("duplicate input in batch");
  }
  return result;
}

std::string serialize_project_input_value(const runtime::NotebookInputValue &value) {
  if (const auto range = std::get_if<runtime::NotebookInputRange>(&value)) {
    auto number = [](const runtime::NotebookInputNumber &n) {
      return std::visit([](auto v) { return serialize_project_input_value(runtime::NotebookInputValue(v)); }, n);
    };
    return "{\"start\":" + number(range->start) + ",\"finish\":" + number(range->finish) +
        ",\"inclusive_end\":" + (range->inclusive_end ? "true" : "false") + ",\"step\":" + number(range->step) + "}";
  }
  if (const auto text = std::get_if<std::string>(&value)) {
    std::string result; write_json_string(*text, &result); return result;
  }
  if (const auto boolean = std::get_if<bool>(&value)) return *boolean ? "true" : "false";
  if (const auto integer = std::get_if<std::int64_t>(&value)) return std::to_string(*integer);
  const auto number = std::get<double>(value);
  if (!std::isfinite(number)) throw std::runtime_error("non-finite project input");
  std::ostringstream out; out.imbue(std::locale::classic());
  out << std::setprecision(17) << number;
  auto result = out.str();
  if (result.find_first_of(".eE") == std::string::npos) result += ".0";
  return result;
}

void validate_project_input_value(const ProjectInput &input, const runtime::NotebookInputValue &value) {
  const bool numeric = std::holds_alternative<double>(value) || std::holds_alternative<std::int64_t>(value);
  if ((input.type == "number" && !numeric) ||
      (input.type == "integer" && !std::holds_alternative<std::int64_t>(value)) ||
      (input.type == "boolean" && !std::holds_alternative<bool>(value)) ||
      (input.type == "string" && !std::holds_alternative<std::string>(value)) ||
      (input.type == "range" && !std::holds_alternative<runtime::NotebookInputRange>(value)) ||
      (input.type != "number" && input.type != "integer" && input.type != "boolean" && input.type != "string" && input.type != "range"))
    throw std::runtime_error("input " + input.id + ": value does not match " + input.type);
  if (numeric) {
    const long double n = std::holds_alternative<double>(value) ?
        std::get<double>(value) : static_cast<long double>(std::get<std::int64_t>(value));
    if (!std::isfinite(n) || (input.minimum && n < *input.minimum) || (input.maximum && n > *input.maximum))
      throw std::runtime_error("input " + input.id + ": value is outside its bounds");
  } else if (input.minimum || input.maximum) {
    throw std::runtime_error("non-numeric input cannot have numeric bounds");
  }
  if (const auto text = std::get_if<std::string>(&value)) {
    validate_utf8(*text, "input " + input.id);
    if (text->size() > 65536 || text->find('\0') != std::string::npos)
      throw std::runtime_error("input string must fit 64 KiB and contain no NUL");
  }
  if (std::holds_alternative<runtime::NotebookInputRange>(value))
    (void)parse_project_input_value(serialize_project_input_value(value));
}

static void validate_range_slider(const std::string &json, const runtime::NotebookInputRange &range, bool check_value) {
  const auto root = JsonParser(json).parse(); require_object(root, "range control");
  const auto field = find_member(root, "style");
  const auto style = field ? require_string(*field, "control.style") : "automatic";
  if (style == "automatic") return;
  if (style != "slider" && style != "range_sliders") throw std::runtime_error("Range control requires automatic, slider or range_sliders");
  const bool integer = std::holds_alternative<std::int64_t>(range.start) && std::holds_alternative<std::int64_t>(range.finish);
  auto number = [](const runtime::NotebookInputNumber &v) { return std::visit([](auto n) { return static_cast<long double>(n); }, v); };
  auto setting = [&](const char *key) {
    const auto parsed = parse_project_input_value(raw_json(json, require_member(root, key, "range control")));
    long double result;
    if (const auto n = std::get_if<std::int64_t>(&parsed)) result = *n;
    else if (const auto n = std::get_if<double>(&parsed)) result = *n;
    else throw std::runtime_error("range slider bounds/increment must be numeric");
    if (!std::isfinite(result) || std::abs(result) >= 1e100L ||
        (integer && (std::floor(result) != result || std::abs(result) > 9007199254740991.0L)))
      throw std::runtime_error("invalid range slider bounds/increment; integer sliders require whole numbers within 2^53-1");
    return result;
  };
  const bool separate = style == "range_sliders";
  const auto start_min = setting(separate ? "start_minimum" : "minimum");
  const auto start_max = setting(separate ? "start_maximum" : "maximum");
  const auto finish_min = separate ? setting("finish_minimum") : start_min;
  const auto finish_max = separate ? setting("finish_maximum") : start_max;
  if (start_min >= start_max || finish_min >= finish_max || start_min > finish_max)
    throw std::runtime_error("range slider domains must increase and allow finish >= start");
  if (find_member(root, "step") && setting("step") <= 0) throw std::runtime_error("range slider increment must be positive");
  if (check_value) {
    const auto start = number(range.start), finish = number(range.finish);
    if (start > finish || start < start_min || start > start_max || finish < finish_min || finish > finish_max)
      throw std::runtime_error("range slider value must satisfy its endpoint bounds and finish >= start");
  }
}

void validate_project_control_values(const ProjectDocument &document, const runtime::NotebookInputSnapshot &values) {
  auto text = [](const ProjectComponent &c, const char *key, const std::string &fallback = std::string{}) {
    const auto found = c.extra.find(key); if (found == c.extra.end()) return fallback;
    const auto value = parse_project_input_value(found->second);
    if (!std::holds_alternative<std::string>(value)) throw std::runtime_error(std::string(key) + " must be text");
    return std::get<std::string>(value);
  };
  auto date = [](const std::string &value, bool time) {
    if (value.empty()) return true;
    if (value.size() != (time ? 20U : 10U)) return false;
    auto digits = [&](unsigned at, unsigned count) {
      int out = 0; for (unsigned i = 0; i < count; ++i) { const char c = value[at+i]; if (c < '0' || c > '9') return -1; out = out * 10 + c - '0'; } return out;
    };
    const int year = digits(0,4), month = digits(5,2), day = digits(8,2);
    if (value[4] != '-' || value[7] != '-' || year < 1 || month < 1 || month > 12 || day < 1) return false;
    const int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    if (day > days[month-1] + (month == 2 && leap ? 1 : 0)) return false;
    if (!time) return true;
    return value[10] == 'T' && value[13] == ':' && value[16] == ':' && value[19] == 'Z' &&
        digits(11,2) >= 0 && digits(11,2) < 24 && digits(14,2) >= 0 && digits(14,2) < 60 && digits(17,2) >= 0 && digits(17,2) < 60;
  };
  std::function<void(const std::vector<ProjectComponent> &, CellId)> visit = [&](const auto &components, CellId owner) {
    for (const auto &c : components) {
      if (c.panels) for (const auto &panel : *c.panels) visit(panel.components, owner);
      if (c.kind == "range_input") {
        const auto value = values.find(c.input);
        if (value == values.end() || !std::holds_alternative<runtime::NotebookInputRange>(value->second))
          throw std::runtime_error("Range Input requires a Range project input");
        if (c.cell || !c.binding.empty() || c.extra.count("range_lower") || c.extra.count("range_upper"))
          throw std::runtime_error("Range Input binds one Range parameter, without cell or numeric parameter bindings");
      }
      auto validate_control = [&](const std::string &id, const std::string &control) {
        const auto value = values.find(owner ? runtime::notebook_cell_input_key(owner, id) : id);
        const auto parsed = JsonParser(control).parse();
        const auto style = find_member(parsed, "style");
        if (style && require_string(*style, "control.style") == "json") {
          if (value == values.end() || !std::holds_alternative<std::string>(value->second))
            throw std::runtime_error("Config Editor requires a String input");
          (void)project_config_document(std::get<std::string>(value->second), config_control_format(control));
        }
        if (value != values.end()) if (const auto range = std::get_if<runtime::NotebookInputRange>(&value->second))
          validate_range_slider(control, *range, true);
      };
      if (c.kind == "input" || c.kind == "range_input") {
        if (const auto control = c.extra.find("control"); control != c.extra.end()) validate_control(c.input, control->second);
      }
      if (c.kind == "parameter_form") if (const auto fields = c.extra.find("fields"); fields != c.extra.end()) {
        const auto parsed = JsonParser(fields->second).parse();
        for (const auto &item : require_array(parsed, "form.fields")) if (const auto control = find_member(item, "control"))
          validate_control(require_string(require_member(item, "input", "field"), "field.input"), raw_json(fields->second, *control));
      }
      if (c.kind == "date_input") {
        const auto mode = text(c, "date_mode", "date_time"), zone = text(c, "date_time_zone", "UTC");
        if (mode != "date" && mode != "date_time") throw std::runtime_error("date_mode must be date or date_time");
        if (zone.empty() || zone.size() > 128 || zone.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_/-+:") != std::string::npos)
          throw std::runtime_error("invalid date input time zone");
        const auto value = values.find(c.input);
        if (value == values.end() || !std::holds_alternative<std::string>(value->second)) throw std::runtime_error("Date Input requires a String project input");
        if (!date(std::get<std::string>(value->second), mode == "date_time")) throw std::runtime_error("Date Input requires a valid YYYY-MM-DD date or YYYY-MM-DDTHH:MM:SSZ UTC timestamp");
      }
    }
  };
  for (const auto &board : document.boards) visit(board.components, 0);
  for (const auto &sheet : document.sheets) for (const auto &cell : sheet.cells) {
    if (cell.kind != "code") continue;
    if (const auto found = cell.extra.find("cell_ui"); found != cell.extra.end()) {
      const auto ui = parse_project_cell_ui(found->second);
      visit(ui.pre, cell.id); visit(ui.post, cell.id);
    }
  }
}

std::shared_ptr<const runtime::NotebookInputSnapshot> project_input_defaults(const ProjectDocument &document) {
  auto result = std::make_shared<runtime::NotebookInputSnapshot>();
  for (const auto &input : document.inputs) {
    validate_project_input_value(input, input.initial);
    auto value = input.initial;
    if (input.type == "number" && std::holds_alternative<std::int64_t>(value))
      value = static_cast<double>(std::get<std::int64_t>(value));
    result->emplace(input.id, std::move(value));
  }
  for (const auto &sheet : document.sheets) for (const auto &cell : sheet.cells) {
    const auto saved = cell.extra.find("cell_ui");
    if (cell.kind != "code" || saved == cell.extra.end()) continue;
    for (const auto &input : parse_project_cell_ui(saved->second).inputs) {
      auto value = input.initial;
      if (input.type == "number" && std::holds_alternative<std::int64_t>(value))
        value = static_cast<double>(std::get<std::int64_t>(value));
      result->emplace(runtime::notebook_cell_input_key(cell.id, input.id), std::move(value));
    }
  }
  std::function<void(const std::vector<ProjectComponent> &)> audio_inputs = [&](const auto &components) {
    for (const auto &c : components) {
      if (c.panels) for (const auto &panel : *c.panels) audio_inputs(panel.components);
      if (project_owns_audio_input(c)) {
        const auto key = runtime::audio_input_key(project_audio_channel(c));
        const auto path = project_audio_asset(c);
        const auto previous = result->find(key);
        if (previous != result->end() && previous->second != runtime::NotebookInputValue(path))
          throw std::runtime_error("Audio Input channels must be unique");
        (*result)[key] = path;
      }
    }
  };
  for (const auto &board : document.boards) audio_inputs(board.components);
  std::function<void(const std::vector<ProjectComponent> &)> accepted_data = [&](const auto &components) {
    for (const auto &c : components) {
      if (c.panels) for (const auto &panel : *c.panels) accepted_data(panel.components);
      for (const auto &kind : {std::string("selection"), std::string("annotations"), std::string("audio_annotations")}) {
        const bool enabled = kind == "selection" ? c.kind == "table" : kind == "audio_annotations" ? c.kind == "audio_annotator" : (c.kind == "image" || c.kind == "image_carousel") && project_image_option(c, "image_annotator");
        if (!enabled) continue;
        const auto channel = project_component_data_channel(c, kind); if (channel.empty()) continue;
        const auto found = c.extra.find(kind); auto json = found == c.extra.end() ? std::string("null") : found->second;
        // Preserve valid legacy geometry for repair in the editor, but never
        // expose fractional image coordinates to notebook consumers.
        validate_project_component_data_impl(kind, json, false);
        if (json != "null" && kind != "selection") {
          try { validate_project_component_data(kind, json); validate_project_annotation_data(c, kind, json); }
          catch (const std::exception &) { json = "null"; } // Retain the old draft in the manifest for review.
        }
        if (kind == "annotations" && json != "null" && project_component_data_source(kind, json).channel != project_image_channel(c)) json = "null";
        if (kind == "audio_annotations" && json != "null") {
          const auto source = project_component_data_source(kind, json);
          if (source.channel != project_audio_channel(c) || source.source != (project_audio_source(c) == "cell" ? "cell" : "input") || source.cell != (project_audio_source(c) == "cell" ? c.cell : 0)) json = "null";
        }
        if (!result->emplace(runtime::notebook_component_data_key(kind, channel), json).second) throw std::runtime_error("component data channels must have one owner");
      }
    }
  };
  for (const auto &board : document.boards) accepted_data(board.components);
  validate_project_control_values(document, *result);
  return result;
}

static std::vector<ProjectBoardRow> parse_board_rows(const JsonValue &rows, const std::string &json) {
  std::vector<ProjectBoardRow> result;
  for (const auto &item : require_array(rows, "layout.rows")) {
    require_object(item, "board row");
    ProjectBoardRow row;
    row.id = require_string(require_member(item, "id", "board row"), "row.id");
    if (const auto height = find_member(item, "height")) {
      const auto value = parse_project_input_value(raw_json(json, *height));
      if (const auto n = std::get_if<double>(&value)) row.height = *n;
      else if (const auto n = std::get_if<std::int64_t>(&value)) row.height = *n;
      else throw std::runtime_error("row.height must be numeric");
    }
    row.extra = parse_extras(item, {"id", "height"}, json);
    result.push_back(std::move(row));
  }
  return result;
}

static std::vector<ProjectComponent> parse_board_components(const JsonValue &items, const std::string &json, unsigned depth = 0) {
  if (depth > 8) throw std::runtime_error("tabs nesting exceeds 8 levels");
  std::vector<ProjectComponent> result;
  for (const auto &item : require_array(items, "layout.components")) {
    require_object(item, "component");
    ProjectComponent component;
    component.id = require_string(require_member(item, "id", "component"), "component.id");
    component.kind = require_string(require_member(item, "kind", "component"), "component.kind");
    component.title = require_string(require_member(item, "title", "component"), "component.title");
    if (const auto v = find_member(item, "input")) component.input = require_string(*v, "component.input");
    if (const auto v = find_member(item, "cell")) component.cell = parse_cell_id(*v, "component.cell");
    if (const auto v = find_member(item, "binding")) component.binding = require_string(*v, "component.binding");
    if (const auto v = find_member(item, "row")) component.row = require_string(*v, "component.row");
    if (const auto v = find_member(item, "span")) {
      const auto value = parse_project_input_value(raw_json(json, *v));
      const auto n = std::get_if<std::int64_t>(&value);
      if (!n || *n < 1 || *n > 12) throw std::runtime_error("component.span must be 1..12");
      component.span = static_cast<unsigned>(*n);
    }
    if (const auto v = find_member(item, "h_align")) component.h_align = require_string(*v, "component.h_align");
    if (const auto v = find_member(item, "v_align")) component.v_align = require_string(*v, "component.v_align");
    if (const auto panels = find_member(item, "panels")) {
      component.panels.emplace();
      for (const auto &panel_item : require_array(*panels, "component.panels")) {
        require_object(panel_item, "tab panel");
        ProjectBoardPanel panel;
        panel.id = require_string(require_member(panel_item, "id", "tab panel"), "panel.id");
        panel.title = require_string(require_member(panel_item, "title", "tab panel"), "panel.title");
        panel.rows = parse_board_rows(require_member(panel_item, "rows", "tab panel"), json);
        panel.components = parse_board_components(require_member(panel_item, "components", "tab panel"), json, depth + 1);
        panel.extra = parse_extras(panel_item, {"id", "title", "rows", "components"}, json);
        component.panels->push_back(std::move(panel));
      }
    }
    component.extra = parse_extras(item, {"id", "kind", "title", "input", "cell", "binding", "row", "span", "h_align", "v_align", "panels"}, json);
    result.push_back(std::move(component));
  }
  return result;
}

ProjectBoard parse_project_board(const std::string &json) {
  const auto root = JsonParser(json).parse(); require_object(root, "board");
  ProjectBoard board;
  board.id = require_string(require_member(root, "id", "board"), "board.id");
  board.title = require_string(require_member(root, "title", "board"), "board.title");
  board.sheet = require_string(require_member(root, "sheet", "board"), "board.sheet");
  if (const auto columns = find_member(root, "columns")) {
    const auto value = parse_project_input_value(raw_json(json, *columns));
    const auto n = std::get_if<std::int64_t>(&value);
    if (!n || *n < 1 || *n > 6) throw std::runtime_error("board columns must be 1..6");
    board.columns = static_cast<unsigned>(*n);
  }
  if (const auto rows = find_member(root, "rows")) board.rows = parse_board_rows(*rows, json);
  board.components = parse_board_components(require_member(root, "components", "board"), json);
  board.extra = parse_extras(root, {"id", "title", "sheet", "columns", "components", "rows"}, json);
  return board;
}

static std::string serialize_board_rows(const std::vector<ProjectBoardRow> &rows) {
  std::string out = "[";
  for (const auto &row : rows) {
    if (out.back() != '[') out += ',';
    out += "{\"id\":"; write_json_string(row.id, &out);
    if (row.height) out += ",\"height\":" + serialize_project_input_value(*row.height);
    for (const auto &extra : row.extra) { out += ','; write_json_string(extra.first, &out); out += ':' + extra.second; }
    out += '}';
  }
  return out + ']';
}

static std::string serialize_board_components(const std::vector<ProjectComponent> &components, unsigned depth = 0) {
  if (depth > 8) throw std::runtime_error("tabs nesting exceeds 8 levels");
  std::string out = "[";
  const auto field = [&](const std::string &key, const std::string &value) {
    if (out.back() != '{') out += ',';
    write_json_string(key, &out); out += ':'; write_json_string(value, &out);
  };
  for (const auto &c : components) {
    if (out.back() != '[') out += ',';
    out += '{';
    field("id", c.id); field("kind", c.kind); field("title", c.title);
    if (!c.input.empty()) field("input", c.input);
    if (c.cell) field("cell", std::to_string(c.cell));
    if (!c.binding.empty()) field("binding", c.binding);
    if (!c.row.empty()) {
      field("row", c.row);
      out += ",\"span\":" + std::to_string(c.span);
      field("h_align", c.h_align); field("v_align", c.v_align);
    }
    if (c.panels) {
      out += ",\"panels\":[";
      for (const auto &panel : *c.panels) {
        if (out.back() != '[') out += ',';
        out += '{'; field("id", panel.id); field("title", panel.title);
        out += ",\"rows\":" + serialize_board_rows(panel.rows);
        out += ",\"components\":" + serialize_board_components(panel.components, depth + 1);
        for (const auto &extra : panel.extra) { out += ','; write_json_string(extra.first, &out); out += ':' + extra.second; }
        out += '}';
      }
      out += ']';
    }
    for (const auto &extra : c.extra) { out += ','; write_json_string(extra.first, &out); out += ':' + extra.second; }
    out += '}';
  }
  return out + ']';
}

std::string serialize_project_board(const ProjectBoard &board) {
  std::string out = "{";
  const auto field = [&](const std::string &key, const std::string &value) {
    if (out.back() != '{') out += ',';
    write_json_string(key, &out); out += ':'; write_json_string(value, &out);
  };
  field("id", board.id); field("title", board.title); field("sheet", board.sheet);
  out += ",\"columns\":" + std::to_string(board.columns) + ",\"components\":" + serialize_board_components(board.components);
  if (board.rows) out += ",\"rows\":" + serialize_board_rows(*board.rows);
  for (const auto &extra : board.extra) { out += ','; write_json_string(extra.first, &out); out += ':' + extra.second; }
  return out + '}';
}

ProjectInput parse_project_input(const std::string &json) {
  const auto item = JsonParser(json).parse();
  require_object(item, "input");
  ProjectInput input;
  input.id = require_string(require_member(item, "id", "input"), "input.id");
  input.title = require_string(require_member(item, "title", "input"), "input.title");
  input.type = require_string(require_member(item, "type", "input"), "input.type");
  input.initial = parse_project_input_value(raw_json(json, require_member(item, "default", "input")));
  for (const auto &name : {"minimum", "maximum"}) {
    if (const auto v = find_member(item, name)) {
      const auto number = parse_project_input_value(raw_json(json, *v));
      double n;
      if (const auto d = std::get_if<double>(&number)) n = *d;
      else if (const auto i = std::get_if<std::int64_t>(&number)) n = static_cast<double>(*i);
      else throw std::runtime_error("input bound must be numeric");
      (std::string(name) == "minimum" ? input.minimum : input.maximum) = n;
    }
  }
  input.extra = parse_extras(item, {"id", "title", "type", "default", "minimum", "maximum"}, json);
  return input;
}

ProjectCellUI parse_project_cell_ui(const std::string &json) {
  const auto root = JsonParser(json).parse(); require_object(root, "cell_ui");
  ProjectCellUI ui;
  for (const auto &item : require_array(require_member(root, "inputs", "cell_ui"), "cell_ui.inputs")) {
    // Exact scalar JSON strings match the existing form/preset transport.
    const auto defaults = require_string(require_member(item, "default_json", "cell input"), "default_json");
    std::string definition = "{";
    for (const auto &field : item.object) if (field.first != "default_json") {
      if (definition.back() != '{') definition += ',';
      write_json_string(field.first, &definition); definition += ':' + raw_json(json, field.second);
    }
    definition += ",\"default\":" + defaults + '}';
    ui.inputs.push_back(parse_project_input(definition));
  }
  ui.pre = parse_board_components(require_member(root, "pre", "cell_ui"), json);
  ui.post = parse_board_components(require_member(root, "post", "cell_ui"), json);
  ui.extra = parse_extras(root, {"inputs", "pre", "post"}, json);
  return ui;
}

std::string serialize_project_cell_ui(const ProjectCellUI &ui) {
  std::string out = "{\"inputs\":[";
  for (const auto &input : ui.inputs) {
    if (out.back() != '[') out += ',';
    out += "{\"id\":"; write_json_string(input.id, &out);
    out += ",\"title\":"; write_json_string(input.title, &out);
    out += ",\"type\":"; write_json_string(input.type, &out);
    out += ",\"default_json\":"; write_json_string(serialize_project_input_value(input.initial), &out);
    if (input.minimum) out += ",\"minimum\":" + serialize_project_input_value(*input.minimum);
    if (input.maximum) out += ",\"maximum\":" + serialize_project_input_value(*input.maximum);
    for (const auto &extra : input.extra) { out += ','; write_json_string(extra.first, &out); out += ':' + extra.second; }
    out += '}';
  }
  out += "],\"pre\":" + serialize_board_components(ui.pre) + ",\"post\":" + serialize_board_components(ui.post);
  for (const auto &extra : ui.extra) { out += ','; write_json_string(extra.first, &out); out += ':' + extra.second; }
  return out + '}';
}

void validate_project_cell_ui(const ProjectCellUI &ui, CellId owner) {
  if (ui.inputs.size() > 128 || ui.pre.size() + ui.post.size() > 64)
    throw std::runtime_error("cell permits at most 128 inputs and 64 components");
  ProjectDocument document;
  document.title = "Cell UI validation"; document.active_sheet = "main";
  ProjectSheet sheet; sheet.id = "main"; sheet.title = "Cell";
  document.sheets.push_back(std::move(sheet));
  document.version = 3; document.inputs = ui.inputs;
  ProjectBoard board; board.id = "cell_ui"; board.title = "Cell"; board.sheet = document.active_sheet;
  std::set<std::string> ids;
  for (const auto *zone : {&ui.pre, &ui.post}) for (auto component : *zone) {
    if (!ids.insert(component.id).second) throw std::runtime_error("duplicate cell component id");
    if (component.kind != "input" && component.kind != "parameter_form" && component.kind != "live_chart" &&
        component.kind != "metric" && component.kind != "image" && component.kind != "table" && component.kind != "audio")
      throw std::runtime_error("component is not supported in a cell: " + component.kind);
    if (!component.row.empty() || component.panels || (component.cell && component.cell != owner))
      throw std::runtime_error("cell components belong to their owner, without board grids");
    if (component.kind == "image") for (const auto &flag : {"image_allow_read", "image_allow_load"}) {
      const auto found = component.extra.find(flag);
      if (found != component.extra.end() && found->second != "false")
        throw std::runtime_error("cell Image is a display; use a board for image editing");
    }
    if (component.kind == "metric" || component.kind == "table" || component.kind == "audio") component.cell = owner;
    board.components.push_back(std::move(component));
  }
  document.boards.push_back(std::move(board));
  validate_extra_fields(ui.extra, {"inputs", "pre", "post"}, "cell_ui");
  validate_project_document(document);
}

ProjectDependency parse_project_dependency(const std::string &json) {
  const auto item = JsonParser(json).parse();
  require_object(item, "dependency");
  ProjectDependency dependency;
  dependency.path = require_string(require_member(item, "path", "dependency"), "dependency.path");
  validate_utf8(dependency.path, "dependency.path");
  if (dependency.path.empty() || dependency.path.size() > 4096 || dependency.path.find('\0') != std::string::npos)
    throw std::runtime_error("invalid dependency path");
  if (const auto value = find_member(item, "auto_import")) {
    if (value->kind != JsonKind::Boolean) throw std::runtime_error("dependency.auto_import must be boolean");
    dependency.auto_import = raw_json(json, *value) == "true";
  }
  dependency.extra = parse_extras(item, {"path", "auto_import"}, json);
  validate_extra_fields(dependency.extra, {"path", "auto_import"}, "dependency");
  return dependency;
}

ProjectDocument parse_project_document(const std::string &json) {
  JsonValue root = JsonParser(json).parse();
  require_object(root, "$");

  const JsonValue &format = require_member(root, "format", "$");
  if (require_string(format, "$.format") != "sputnik-notebook") {
    document_error_at("$.format", "format must be 'sputnik-notebook'",
                      format.begin);
  }
  const JsonValue &version = require_member(root, "version", "$");
  if (version.kind != JsonKind::Number ||
      (raw_json(json, version) != "1" && raw_json(json, version) != "2" && raw_json(json, version) != "3" && raw_json(json, version) != "4")) {
    document_error_at("$.version", "version must be 1, 2, 3 or 4",
                      version.begin);
  }

  ProjectDocument document;
  document.version = static_cast<unsigned>(std::stoul(raw_json(json, version)));
  if (document.version >= 2) {
    for (const auto &item : require_array(require_member(root, "inputs", "$"), "inputs")) {
      document.inputs.push_back(parse_project_input(raw_json(json, item)));
    }
    for (const auto &item : require_array(require_member(root, "boards", "$"), "boards"))
      document.boards.push_back(parse_project_board(raw_json(json, item)));
  } else if (find_member(root, "inputs") || find_member(root, "boards")) {
    throw std::runtime_error("project inputs and boards require schema version 2");
  }
  if (document.version >= 3) {
    for (const auto &item : require_array(require_member(root, "dependencies", "$"), "dependencies")) {
      document.dependencies.push_back(parse_project_dependency(raw_json(json, item)));
    }
  } else if (find_member(root, "dependencies")) {
    throw std::runtime_error("external dependencies require schema version 3");
  }
  document.title =
      require_string(require_member(root, "title", "$.title"), "$.title");
  document.active_sheet = require_string(
      require_member(root, "active_sheet", "$.active_sheet"), "$.active_sheet");

  const auto &sheets =
      require_array(require_member(root, "sheets", "$.sheets"), "$.sheets");
  for (std::size_t index = 0U; index < sheets.size(); ++index) {
    document.sheets.push_back(parse_sheet(
        sheets[index], json, "$.sheets[" + std::to_string(index) + "]"));
  }

  const auto &modules =
      require_array(require_member(root, "modules", "$.modules"), "$.modules");
  for (std::size_t index = 0U; index < modules.size(); ++index) {
    document.modules.push_back(parse_module(
        modules[index], json, "$.modules[" + std::to_string(index) + "]"));
  }

  const auto &auto_imports = require_array(
      require_member(root, "auto_imports", "$.auto_imports"), "$.auto_imports");
  for (std::size_t index = 0U; index < auto_imports.size(); ++index) {
    document.auto_imports.push_back(require_string(
        auto_imports[index], "$.auto_imports[" + std::to_string(index) + "]"));
  }

  document.extra = parse_extras(root,
                                {"format", "version", "title", "active_sheet",
                                 "sheets", "modules", "auto_imports", "inputs", "boards", "dependencies"},
                                json);
  // Upgrade the old pair-of-scalars widget to a real Range parameter. Keep
  // the scalar definitions so existing authored code can still read them.
  std::function<void(std::vector<ProjectComponent> &)> upgrade_ranges = [&](auto &components) {
    for (auto &c : components) {
      if (c.panels) for (auto &panel : *c.panels) upgrade_ranges(panel.components);
      if (c.kind != "range_input" || !c.input.empty()) continue;
      const auto low = c.extra.find("range_lower"), high = c.extra.find("range_upper");
      if (low == c.extra.end() || high == c.extra.end()) continue;
      auto bound = [&](const std::string &json) -> runtime::NotebookInputNumber {
        const auto id = std::get<std::string>(parse_project_input_value(json));
        for (const auto &input : document.inputs) if (input.id == id) {
          if (const auto n = std::get_if<std::int64_t>(&input.initial)) return *n;
          if (const auto n = std::get_if<double>(&input.initial)) return *n;
        }
        throw std::runtime_error("legacy Range Input requires two numeric parameters");
      };
      runtime::NotebookInputRange range; range.start = bound(low->second); range.finish = bound(high->second);
      auto id = c.id;
      while (std::any_of(document.inputs.begin(), document.inputs.end(), [&](const auto &input) { return input.id == id; })) id += "_range";
      document.inputs.push_back(ProjectInput{id, c.title, "range", range, {}, {}, {}});
      c.input = id; c.extra.erase("range_lower"); c.extra.erase("range_upper");
    }
  };
  for (auto &board : document.boards) upgrade_ranges(board.components);
  validate_project_document(document);
  return document;
}

PeriodicWorkSettings project_periodic_work_settings(const ProjectComponent &component) {
  PeriodicWorkSettings settings;
  const auto string = [&](const std::string &key, std::string &target) {
    const auto found = component.extra.find(key);
    if (found == component.extra.end()) return;
    const auto value = parse_project_input_value(found->second);
    const auto text = std::get_if<std::string>(&value);
    if (!text) throw std::runtime_error("periodic work " + key + " must be a string");
    target = *text;
  };
  const auto boolean = [&](const std::string &key, bool &target) {
    const auto found = component.extra.find(key);
    if (found == component.extra.end()) return;
    const auto value = parse_project_input_value(found->second);
    const auto flag = std::get_if<bool>(&value);
    if (!flag) throw std::runtime_error("periodic work " + key + " must be boolean");
    target = *flag;
  };
  string("module_id", settings.module_id); string("function", settings.function);
  validate_module_id(settings.module_id, "periodic work module_id");
  validate_module_id(settings.function, "periodic work function");
  string("timing", settings.timing); string("retention", settings.retention);
  if (settings.timing != "fixed_rate" && settings.timing != "fixed_delay")
    throw std::runtime_error("periodic work timing must be fixed_rate or fixed_delay");
  if (settings.retention != "none" && settings.retention != "all" && settings.retention != "tail")
    throw std::runtime_error("periodic work retention must be none, all or tail");
  if (const auto found = component.extra.find("period_seconds"); found != component.extra.end()) {
    const auto value = parse_project_input_value(found->second);
    if (const auto real = std::get_if<double>(&value)) settings.period_seconds = *real;
    else if (const auto integer = std::get_if<std::int64_t>(&value)) settings.period_seconds = *integer;
    else throw std::runtime_error("periodic work period_seconds must be numeric");
  }
  if (!std::isfinite(settings.period_seconds) || settings.period_seconds < 0.01 || settings.period_seconds > 31536000)
    throw std::runtime_error("periodic work period must be 0.01..31536000 seconds");
  if (const auto found = component.extra.find("tail_limit"); found != component.extra.end()) {
    const auto value = parse_project_input_value(found->second);
    const auto integer = std::get_if<std::int64_t>(&value);
    if (!integer || *integer < 1 || *integer > 1000000)
      throw std::runtime_error("periodic work tail_limit must be 1..1000000");
    settings.tail_limit = static_cast<unsigned>(*integer);
  }
  boolean("store_values", settings.store_values); boolean("store_errors", settings.store_errors);
  boolean("run_immediately", settings.run_immediately); boolean("stop_on_error", settings.stop_on_error);
  boolean("pause_when_hidden", settings.pause_when_hidden);
  return settings;
}

static void validate_board_input_control(const std::string &json, const ProjectInput &input) {
  if (input.type == "range") {
    validate_range_slider(json, std::get<runtime::NotebookInputRange>(input.initial), true);
    return;
  }
  const auto root = JsonParser(json).parse(); require_object(root, "input control");
  const auto style_value = find_member(root, "style");
  const auto style = style_value ? require_string(*style_value, "control.style") : "automatic";
  if (style != "automatic" && style != "number" && style != "slider" && style != "stepper" &&
      style != "select" && style != "segmented" && style != "radio" && style != "json")
    throw std::runtime_error("unsupported input control style");
  if (style == "json") {
    if (input.type != "string") throw std::runtime_error("Config Editor requires a String input");
    (void)project_config_document(std::get<std::string>(input.initial), config_control_format(json));
  }
  const bool numeric = input.type == "number" || input.type == "integer";
  if ((style == "number" || style == "slider" || style == "stepper") && !numeric)
    throw std::runtime_error("numeric control requires a numeric input");
  std::optional<double> minimum = input.minimum, maximum = input.maximum;
  for (const auto name : {"minimum", "maximum", "step"}) if (const auto item = find_member(root, name)) {
    const auto parsed = parse_project_input_value(raw_json(json, *item));
    double number;
    if (const auto v = std::get_if<double>(&parsed)) number = *v;
    else if (const auto v = std::get_if<std::int64_t>(&parsed)) number = *v;
    else throw std::runtime_error("input control bound/step must be numeric");
    if (!numeric || !std::isfinite(number) || std::abs(number) >= 1e100)
      throw std::runtime_error("invalid input control bound/step");
    if (std::string(name) == "step") {
      if (number <= 0 || (input.type == "integer" && (std::floor(number) != number || number > 9007199254740991.0)))
        throw std::runtime_error("invalid input step");
    } else {
      if (input.type == "integer" && std::floor(number) != number) throw std::runtime_error("integer bounds must be integral");
      if (std::string(name) == "minimum") minimum = minimum ? std::max(*minimum, number) : number;
      else maximum = maximum ? std::min(*maximum, number) : number;
    }
  }
  if (minimum && maximum && *minimum > *maximum) throw std::runtime_error("input control bounds do not overlap");
  if (style == "slider" && (!minimum || !maximum || *minimum >= *maximum ||
      (input.type == "integer" && (std::abs(*minimum) > 9007199254740991.0 || std::abs(*maximum) > 9007199254740991.0))))
    throw std::runtime_error("slider requires increasing finite bounds; use Number or Stepper for large integers");
  if (const auto precision = find_member(root, "precision")) {
    const auto p = parse_project_input_value(raw_json(json, *precision));
    const auto n = std::get_if<std::int64_t>(&p);
    if (!n || *n < 0 || *n > 12) throw std::runtime_error("precision must be 0..12");
  }
  if (const auto unit = find_member(root, "unit")) {
    const auto text = require_string(*unit, "control.unit");
    if (text.size() > 256) throw std::runtime_error("unit label is too long");
  }
  const bool choice = style == "select" || style == "segmented" || style == "radio";
  if (choice && input.type != "string" && input.type != "integer") throw std::runtime_error("choice requires a String or Integer input");
  const auto options = find_member(root, "options");
  if (choice && !options) throw std::runtime_error("choice requires options");
  if (options) {
    const auto &items = require_array(*options, "control.options");
    if (items.size() > 256 || (choice && items.empty())) throw std::runtime_error("choice requires 1..256 options");
    std::set<std::string> values;
    for (const auto &item : items) {
      require_object(item, "option");
      if (require_string(require_member(item, "label", "option"), "option.label").size() > 256)
        throw std::runtime_error("option label is too long");
      const auto value = parse_project_input_value(require_string(require_member(item, "value_json", "option"), "option.value_json"));
      validate_project_input_value(input, value);
      if (!values.insert(serialize_project_input_value(value)).second) throw std::runtime_error("duplicate option value");
    }
  }
}

static const JsonValue *p1_member(const JsonValue &root, const char *key) { return find_member(root, key); }

ProjectActionInvocation project_action_invocation(const ProjectComponent &component,
    const runtime::NotebookInputSnapshot &inputs) {
  const auto module = component.extra.find("module_id"), function = component.extra.find("function");
  if (module == component.extra.end() || function == component.extra.end()) throw std::runtime_error("action requires a module and function");
  const auto module_id = std::get<std::string>(parse_project_input_value(module->second));
  const auto function_id = std::get<std::string>(parse_project_input_value(function->second));
  const auto identifier = [](const std::string &name) {
    if (name.empty() || name.size() > 256 || !(std::isalpha(static_cast<unsigned char>(name[0])) || name[0] == '_')) return false;
    return std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::isalnum(c) || c == '_'; });
  };
  if (!identifier(module_id) || !identifier(function_id)) throw std::runtime_error("invalid action module/function name");
  auto snapshot = std::make_shared<runtime::NotebookInputSnapshot>(inputs);
  std::string source = "import " + module_id + "\n" + module_id + "." + function_id + "(";
  if (const auto settings = component.extra.find("p1"); settings != component.extra.end()) {
    const auto root = JsonParser(settings->second).parse(); require_object(root, "p1");
    if (const auto arguments = find_member(root, "arguments")) {
      const auto &items = require_array(*arguments, "action.arguments");
      if (items.size() > 64) throw std::runtime_error("action exceeds 64 arguments");
      std::set<std::string> names; bool named = false;
      for (std::size_t i = 0; i < items.size(); ++i) {
        const auto &item = items[i]; require_object(item, "argument");
        if (i) source += ", ";
        if (const auto name = find_member(item, "name"); name && !require_string(*name, "argument.name").empty()) {
          const auto text = require_string(*name, "argument.name");
          if (!identifier(text) || !names.insert(text).second) throw std::runtime_error("invalid/duplicate named argument");
          named = true; source += text + ": ";
        } else if (named) throw std::runtime_error("positional arguments must precede named arguments");
        const auto binding = find_member(item, "input"), constant = find_member(item, "value_json");
        if (bool(binding) == bool(constant)) throw std::runtime_error("argument requires one input or scalar constant");
        runtime::NotebookInputValue value;
        if (binding) {
          const auto id = require_string(*binding, "argument.input");
          const auto found = inputs.find(id);
          if (found == inputs.end()) throw std::runtime_error("action input unavailable: " + id);
          value = found->second;
        } else value = parse_project_input_value(require_string(*constant, "argument.value_json"));
        auto key = "__sputnik_action_" + std::to_string(i);
        while (snapshot->count(key)) key += '_';
        snapshot->emplace(key, std::move(value));
        source += "notebook.input(" + serialize_project_input_value(key) + ")";
      }
    }
  }
  return {source + ")\n", snapshot};
}

static void validate_p1(const ProjectComponent &c, const ProjectDocument &document) {
  const auto settings = c.extra.find("p1");
  if (settings == c.extra.end()) {
    if (c.kind == "action") (void)project_action_invocation(c, *project_input_defaults(document));
    return;
  }
  const auto root = JsonParser(settings->second).parse(); require_object(root, "p1");
  const auto string = [&](const char *key, std::size_t limit) {
    if (const auto item = p1_member(root, key)) {
      const auto value = require_string(*item, key);
      if (value.size() > limit || value.find('\0') != std::string::npos) throw std::runtime_error(std::string("invalid p1.") + key);
      return value;
    }
    return std::string();
  };
  string("unit", 256); string("extensions", 512); string("target_component", 256);
  const auto target = string("target_kind", 16);
  if (!target.empty() && target != "cell" && target != "sheet" && target != "service" && target != "action" && target != "runtime") throw std::runtime_error("invalid execution target");
  const auto direction = string("improvement", 16);
  if (!direction.empty() && direction != "higher" && direction != "lower" && direction != "neutral") throw std::runtime_error("invalid metric improvement");
  for (const auto key : {"percentage", "collapsed"}) if (const auto item = p1_member(root, key))
    if (!std::holds_alternative<bool>(parse_project_input_value(raw_json(settings->second, *item)))) throw std::runtime_error("p1 flag requires boolean");
  for (const auto key : {"target", "baseline", "precision"}) if (const auto item = p1_member(root, key)) {
    const auto value = parse_project_input_value(raw_json(settings->second, *item));
    double n;
    if (const auto number = std::get_if<double>(&value)) n = *number;
    else if (const auto integer = std::get_if<std::int64_t>(&value)) n = *integer;
    else throw std::runtime_error("metric setting requires a number");
    if (!std::isfinite(n) || (std::string(key) == "precision" && (n < 0 || n > 12 || std::floor(n) != n))) throw std::runtime_error("invalid metric setting");
  }
  if (c.kind == "action") (void)project_action_invocation(c, *project_input_defaults(document));
  if (const auto series = p1_member(root, "series")) {
    const auto &items = require_array(*series, "series");
    if (items.size() > 16) throw std::runtime_error("chart exceeds 16 series");
    std::set<std::string> ids;
    for (const auto &item : items) {
      const auto id = require_string(require_member(item, "id", "series"), "series.id");
      const auto color = require_string(require_member(item, "stroke", "series"), "series.stroke");
      if (id.empty() || id.size() > 256 || !ids.insert(id).second || id.find('\0') != std::string::npos ||
          require_string(require_member(item, "title", "series"), "series.title").size() > 256 ||
          color.size() != 7 || color[0] != '#' || color.find_first_not_of("0123456789abcdefABCDEF", 1) != std::string::npos) throw std::runtime_error("invalid chart series");
    }
  }
  std::set<std::string> selection;
  if (const auto selected = p1_member(root, "preset_inputs")) for (const auto &item : require_array(*selected, "preset inputs")) {
    const auto id = require_string(item, "preset input");
    if (!selection.insert(id).second || std::none_of(document.inputs.begin(), document.inputs.end(), [&](const auto &v) { return v.id == id; })) throw std::runtime_error("unknown/duplicate preset input");
  }
  if (const auto presets = p1_member(root, "presets")) {
    const auto &items = require_array(*presets, "presets");
    if (items.size() > 128) throw std::runtime_error("component exceeds 128 presets");
    std::set<std::string> ids;
    for (const auto &item : items) {
      const auto id = require_string(require_member(item, "id", "preset"), "preset.id");
      const auto title = require_string(require_member(item, "title", "preset"), "preset.title");
      if (id.empty() || id.size() > 256 || !ids.insert(id).second || title.empty() || title.size() > 256) throw std::runtime_error("invalid preset name/identity");
      const auto &values = require_member(item, "values", "preset");
      const auto edits = parse_project_input_edits(raw_json(settings->second, values));
      for (const auto &edit : edits) {
        const auto &input_id = edit.first, &json = edit.second;
        const auto input = std::find_if(document.inputs.begin(), document.inputs.end(), [&](const auto &v) { return v.id == input_id; });
        if (input == document.inputs.end() || (!selection.empty() && !selection.count(input_id))) throw std::runtime_error("preset input unavailable");
        validate_project_input_value(*input, parse_project_input_value(json));
      }
    }
  }
}

void validate_project_document(const ProjectDocument &document) {
  if ((document.version < 1 || document.version > 4) ||
      (document.version == 1 && (!document.inputs.empty() || !document.boards.empty())))
    throw std::runtime_error("project inputs and boards require schema version 2");
  if (document.version < 3 && !document.dependencies.empty())
    throw std::runtime_error("external dependencies require schema version 3");
  if (document.dependencies.size() > 128) throw std::runtime_error("too many package dependencies");
  std::set<std::string> dependency_paths;
  for (const auto &dependency : document.dependencies) {
    validate_utf8(dependency.path, "dependency.path");
    if (dependency.path.empty() || dependency.path.size() > 4096 || dependency.path.find('\0') != std::string::npos ||
        !dependency_paths.insert(dependency.path).second)
      throw std::runtime_error("invalid or duplicate dependency path");
    validate_extra_fields(dependency.extra, {"path", "auto_import"}, "dependency");
  }
  validate_utf8(document.title, "title");
  validate_utf8(document.active_sheet, "active_sheet");
  if (document.sheets.empty()) {
    document_error("sheets", "at least one sheet is required");
  }

  std::set<std::string> sheet_ids;
  std::set<CellId> cell_ids;
  for (std::size_t sheet_index = 0U; sheet_index < document.sheets.size();
       ++sheet_index) {
    const ProjectSheet &sheet = document.sheets[sheet_index];
    const std::string sheet_path =
        "sheets[" + std::to_string(sheet_index) + "]";
    validate_utf8(sheet.id, sheet_path + ".id");
    validate_utf8(sheet.title, sheet_path + ".title");
    if (sheet.id.empty()) {
      document_error(sheet_path + ".id", "sheet id must not be empty");
    }
    if (!sheet_ids.insert(sheet.id).second) {
      document_error(sheet_path + ".id", "sheet id is not unique");
    }
    for (std::size_t cell_index = 0U; cell_index < sheet.cells.size();
         ++cell_index) {
      const auto &cell = sheet.cells[cell_index];
      if (const auto ui = cell.extra.find("cell_ui"); ui != cell.extra.end()) {
        if (document.version < 4 || cell.kind != "code")
          throw std::runtime_error("cell components require a code cell and schema version 4");
        validate_project_cell_ui(parse_project_cell_ui(ui->second), cell.id);
      }
      validate_cell(sheet.cells[cell_index], &cell_ids,
                    sheet_path + ".cells[" + std::to_string(cell_index) + "]");
    }
    validate_extra_fields(sheet.extra, {"id", "title", "cells"}, sheet_path);
  }
  if (sheet_ids.count(document.active_sheet) == 0U) {
    document_error("active_sheet", "does not name an existing sheet");
  }

  std::set<std::string> module_ids;
  std::set<std::string> module_paths;
  for (std::size_t index = 0U; index < document.modules.size(); ++index) {
    const ProjectModule &module = document.modules[index];
    const std::string path = "modules[" + std::to_string(index) + "]";
    validate_module_id(module.id, path + ".id");
    validate_module_path(module.path, path + ".path");
    if (!module_ids.insert(module.id).second) {
      document_error(path + ".id", "module id is not unique");
    }
    if (!module_paths.insert(module.path).second) {
      document_error(path + ".path", "module path is not unique");
    }
    validate_extra_fields(module.extra, {"id", "path"}, path);
  }
  std::set<std::string> auto_import_ids;
  for (std::size_t index = 0U; index < document.auto_imports.size(); ++index) {
    const std::string path = "auto_imports[" + std::to_string(index) + "]";
    validate_utf8(document.auto_imports[index], path);
    if (!auto_import_ids.insert(document.auto_imports[index]).second) {
      document_error(path, "auto_import is not unique");
    }
    if (module_ids.count(document.auto_imports[index]) == 0U) {
      document_error(path, "does not reference a known module id");
    }
  }
  validate_extra_fields(document.extra,
                        {"format", "version", "title", "active_sheet", "sheets",
                         "modules", "auto_imports", "inputs", "boards", "dependencies"},
                        "document");
  std::set<std::string> inputs, boards;
  if (document.inputs.size() > 1024 || document.boards.size() > 128)
    throw std::runtime_error("project exceeds 1024 inputs / 128 boards");
  for (const auto &input : document.inputs) {
    validate_utf8(input.id, "input.id"); validate_utf8(input.title, "input.title");
    if (input.id.empty() || input.id.size() > 256 || input.id.find('\0') != std::string::npos || !inputs.insert(input.id).second)
      throw std::runtime_error("invalid or duplicate project input id");
    if ((input.minimum && !std::isfinite(*input.minimum)) || (input.maximum && !std::isfinite(*input.maximum)) ||
        (input.minimum && input.maximum && *input.minimum > *input.maximum))
      throw std::runtime_error("invalid input bounds");
    validate_project_input_value(input, input.initial);
    validate_extra_fields(input.extra, {"id", "title", "type", "default", "minimum", "maximum"}, "input");
  }
  for (const auto &board : document.boards) {
    validate_module_id(board.id, "board.id"); validate_utf8(board.title, "board.title");
    if (!boards.insert(board.id).second || !sheet_ids.count(board.sheet))
      throw std::runtime_error("duplicate board or unknown controller sheet");
    if (board.columns < 1 || board.columns > 6 || board.components.size() > 256)
      throw std::runtime_error("board requires 1..6 columns and at most 256 components");
    std::set<std::string> components;
    std::size_t row_count = 0, panel_count = 0;
    std::function<void(const std::vector<ProjectBoardRow> *, const std::vector<ProjectComponent> &, unsigned)> validate_grid;
    validate_grid = [&](const auto *rows, const auto &items, unsigned depth) {
      if (depth > 8) throw std::runtime_error("tabs nesting exceeds 8 levels");
      std::map<std::string, unsigned> row_spans;
      if (rows) {
        if ((row_count += rows->size()) > 256) throw std::runtime_error("board permits at most 256 rows");
        for (const auto &row : *rows) {
          validate_module_id(row.id, "row.id");
          if (!row_spans.emplace(row.id, 0).second) throw std::runtime_error("duplicate board row id");
          if (row.height && (!std::isfinite(*row.height) || *row.height < 48 || *row.height > 4096))
            throw std::runtime_error("row height must be 48..4096 points");
          validate_extra_fields(row.extra, {"id", "height"}, "board row");
        }
      }
      for (const auto &c : items) {
        validate_module_id(c.id, "component.id"); validate_utf8(c.title, "component.title");
        validate_utf8(c.binding, "component.binding");
        if (!components.insert(c.id).second) throw std::runtime_error("duplicate component id");
        if (components.size() > 256) throw std::runtime_error("board permits at most 256 components across all panels");
        const bool periodic = c.kind == "periodic_work";
        if (periodic) {
          if (depth != 0 || !c.row.empty() || c.cell || !c.input.empty() || !c.binding.empty())
            throw std::runtime_error("periodic work belongs to the board service tray, without row or cell bindings");
          (void)project_periodic_work_settings(c);
        } else if (rows) {
          const auto row = row_spans.find(c.row);
          if (row == row_spans.end()) throw std::runtime_error("component references unknown board row");
          if (c.span < 1 || c.span > 12 || (row->second += c.span) > 12)
            throw std::runtime_error("board row components must fit 12 columns");
        } else if (!c.row.empty()) throw std::runtime_error("component row requires board.rows");
        if (c.h_align != "leading" && c.h_align != "center" && c.h_align != "trailing" && c.h_align != "stretch")
          throw std::runtime_error("invalid component horizontal alignment");
        if (c.v_align != "top" && c.v_align != "center" && c.v_align != "bottom" && c.v_align != "stretch")
          throw std::runtime_error("invalid component vertical alignment");
        const bool image_component = c.kind == "image" || c.kind == "image_carousel";
        const bool text_cell = c.kind == "rich_text";
        const bool form = c.kind == "parameter_form";
        const bool group = c.kind == "group" || c.kind == "split";
        const bool independent = c.kind == "parameter_presets" || c.kind == "action" || c.kind == "execution_controls" || c.kind == "status" || c.kind == "stopwatch" || c.kind == "run_history" || c.kind == "experiment_compare" || c.kind == "task_monitor";
        const bool p1_output = c.kind == "metric" || c.kind == "log" || c.kind == "artifacts";
        if (!group && !independent && !p1_output && c.kind != "file_input" && c.kind != "audio_input" && c.kind != "audio_annotator" && c.kind != "range_input" && c.kind != "date_input" && c.kind != "input" && !form && c.kind != "audio" && c.kind != "table" && c.kind != "text" && c.kind != "result" && c.kind != "progress" && !text_cell && c.kind != "plot" && c.kind != "live_chart" && c.kind != "run" && c.kind != "tabs" && !image_component && !periodic)
          throw std::runtime_error("unsupported board component kind: " + c.kind);
        if ((c.kind == "input" || c.kind == "file_input") && !inputs.count(c.input)) throw std::runtime_error("component references unknown input");
        validate_p1(c, document);
        if (c.kind == "file_input") {
          const auto input = std::find_if(document.inputs.begin(), document.inputs.end(), [&](const auto &v) { return v.id == c.input; });
          if (input->type != "string") throw std::runtime_error("File Input requires a String project input");
        }
        if ((c.kind == "input" || c.kind == "range_input") && c.extra.count("control")) {
          const auto input = std::find_if(document.inputs.begin(), document.inputs.end(), [&](const auto &v) { return v.id == c.input; });
          if (input == document.inputs.end()) throw std::runtime_error("component references unknown input");
          validate_board_input_control(c.extra.at("control"), *input);
        }
        if (form) {
          if (c.cell || !c.input.empty() || !c.binding.empty()) throw std::runtime_error("parameter form owns fields, without cell bindings");
          const auto fields = c.extra.find("fields");
          if (fields == c.extra.end()) throw std::runtime_error("parameter form requires fields");
          const auto parsed = JsonParser(fields->second).parse();
          const auto &items = require_array(parsed, "form.fields");
          if (items.size() > 128) throw std::runtime_error("form exceeds 128 fields");
          std::set<std::string> used;
          for (const auto &item : items) {
            require_object(item, "form field");
            const auto id = require_string(require_member(item, "input", "field"), "field.input");
            const auto input = std::find_if(document.inputs.begin(), document.inputs.end(), [&](const auto &v) { return v.id == id; });
            if (input == document.inputs.end() || !used.insert(id).second) throw std::runtime_error("unknown or duplicate form input");
            if (const auto control = find_member(item, "control")) validate_board_input_control(raw_json(fields->second, *control), *input);
          }
        }
        // A missing output target remains an explicit broken binding in the UI;
        // editing a sheet must not become impossible because a board references it.
        if (!group && !independent && c.kind != "file_input" && c.kind != "audio_input" && c.kind != "audio_annotator" && c.kind != "range_input" && c.kind != "date_input" && c.kind != "input" && !form && c.kind != "live_chart" && c.kind != "tabs" && !text_cell && !image_component && !periodic && c.cell == 0) throw std::runtime_error("output component requires a cell id");
        if (text_cell) {
          if (c.cell || !c.input.empty() || !c.binding.empty())
            throw std::runtime_error("text cell owns its source, without cell or input bindings");
          const auto content = c.extra.find("text_content");
          if (content == c.extra.end()) throw std::runtime_error("text cell requires text_content");
          (void)parse_project_text_content(content->second);
          if (const auto mode = c.extra.find("text_mode"); mode != c.extra.end()) {
            const auto value = parse_project_input_value(mode->second);
            const auto text = std::get_if<std::string>(&value);
            if (!text || (*text != "richtext" && *text != "markdown"))
              throw std::runtime_error("text_mode must be richtext or markdown");
          }
        }
        for (const auto field : {"selection_id", "selection_key", "selection_asset", "annotation_id"}) {
          const auto found = c.extra.find(field); if (found == c.extra.end()) continue;
          const auto value = parse_project_input_value(found->second); const auto text = std::get_if<std::string>(&value);
          if (!text || text->size() > 256 || text->find('\0') != std::string::npos) throw std::runtime_error("invalid shared selection/annotation field");
        }
        if (c.kind == "audio_annotator" || c.kind == "image" || c.kind == "image_carousel") validate_annotation_configuration(c);
        if (project_owns_audio_input(c)) {
          if (c.cell || !c.input.empty() || !c.binding.empty()) throw std::runtime_error("Audio Input owns its accepted asset, without cell bindings");
          (void)project_audio_channel(c); (void)project_audio_asset(c);
          if (const auto recorder = c.extra.find("audio_recording"); recorder != c.extra.end())
            if (!std::holds_alternative<bool>(parse_project_input_value(recorder->second))) throw std::runtime_error("audio_recording must be Bool");
          if (const auto name = c.extra.find("audio_name"); name != c.extra.end()) {
            const auto value = parse_project_input_value(name->second); const auto text = std::get_if<std::string>(&value);
            if (!text || text->size() > 1024 || text->find('\0') != std::string::npos) throw std::runtime_error("invalid audio input name");
          }
        }
        if (c.kind == "audio_annotator") {
          (void)project_audio_channel(c);
          if (project_audio_source(c) == "cell" && !c.cell) throw std::runtime_error("audio annotator cell source requires a cell id");
          for (const auto *key : {"audio_view", "audio_fft_size"}) if (const auto field = c.extra.find(key); field != c.extra.end()) {
            const auto value = parse_project_input_value(field->second);
            if (std::string(key) == "audio_view") {
              const auto text = std::get_if<std::string>(&value); if (!text || (*text != "waveform" && *text != "spectrogram")) throw std::runtime_error("invalid audio annotation view");
            } else {
              const auto size = std::get_if<std::int64_t>(&value); if (!size || (*size != 256 && *size != 512 && *size != 1024 && *size != 2048 && *size != 4096)) throw std::runtime_error("invalid audio FFT size");
            }
          }
        }
        if (c.kind == "audio") {
          if (const auto flag = c.extra.find("show_waveform"); flag != c.extra.end()) {
            if (!std::holds_alternative<bool>(parse_project_input_value(flag->second)))
              throw std::runtime_error("show_waveform must be Bool");
          }
          if (const auto id = c.extra.find("audio_id"); id != c.extra.end()) {
            const auto value = parse_project_input_value(id->second);
            const auto text = std::get_if<std::string>(&value);
            if (!text || text->size() > 256 || text->find('\0') != std::string::npos)
              throw std::runtime_error("invalid audio channel id");
          }
        }
        if (c.kind == "table") {
          if (const auto id = c.extra.find("table_id"); id != c.extra.end()) {
            const auto value = parse_project_input_value(id->second);
            const auto text = std::get_if<std::string>(&value);
            if (!text || text->size() > 256 || text->find('\0') != std::string::npos)
              throw std::runtime_error("table_id must be text of at most 256 bytes without NUL");
          }
        }
        if (c.kind == "progress" || c.kind == "stopwatch") {
          if (const auto id = c.extra.find(c.kind == "progress" ? "progress_id" : "stopwatch_id"); id != c.extra.end()) {
            const auto value = parse_project_input_value(id->second);
            const auto text = std::get_if<std::string>(&value);
            if (!text || text->size() > 256 || text->find('\0') != std::string::npos)
              throw std::runtime_error(std::string(c.kind == "progress" ? "progress_id" : "stopwatch_id") +
                                       " must be a string of at most 256 bytes without NUL");
          }
        }
        if (c.kind == "tabs" || group) {
          if (c.kind == "split") {
            if (!c.panels || c.panels->size() != 2) throw std::runtime_error("Split View requires exactly two panels");
            auto number = [&](const char *key, double fallback) {
              const auto found = c.extra.find(key); if (found == c.extra.end()) return fallback;
              const auto value = parse_project_input_value(found->second);
              if (auto real = std::get_if<double>(&value)) return *real;
              if (auto integer = std::get_if<std::int64_t>(&value)) return double(*integer);
              throw std::runtime_error(std::string(key) + " must be numeric");
            };
            const auto fraction = number("split_fraction", 0.5), minimum = number("split_min", 120);
            if (fraction < 0.1 || fraction > 0.9 || minimum < 48 || minimum > 1024) throw std::runtime_error("invalid Split View ratio or minimum size");
            if (const auto axis = c.extra.find("split_axis"); axis != c.extra.end()) {
              const auto value = parse_project_input_value(axis->second); const auto text = std::get_if<std::string>(&value);
              if (!text || (*text != "horizontal" && *text != "vertical")) throw std::runtime_error("invalid split axis");
            }
          }
          if (c.kind == "group" && (!c.panels || c.panels->size() != 1)) throw std::runtime_error("group requires exactly one grid panel");
          if (!c.panels || c.panels->empty() || c.panels->size() > 32 || (panel_count += c.panels->size()) > 128)
            throw std::runtime_error("tabs require 1..32 panels, at most 128 per board");
          std::set<std::string> panel_ids;
          for (const auto &panel : *c.panels) {
            validate_module_id(panel.id, "panel.id"); validate_utf8(panel.title, "panel.title");
            if (!panel_ids.insert(panel.id).second) throw std::runtime_error("duplicate tab panel id");
            validate_extra_fields(panel.extra, {"id", "title", "rows", "components"}, "tab panel");
            validate_grid(&panel.rows, panel.components, depth + 1);
          }
        } else if (c.panels) throw std::runtime_error("only tabs and group components may own panels");
        if (image_component) {
          for (const auto key : {"image_allow_save", "image_allow_load", "image_allow_read", "image_annotator"})
            (void)project_image_option(c, key);
          (void)project_image_assets(c);
        for (const auto key : {"image_series_caption", "image_revision"}) {
          if (const auto value = c.extra.find(key); value != c.extra.end()) {
            const auto parsed = parse_project_input_value(value->second);
            if (!std::holds_alternative<std::string>(parsed) || std::get<std::string>(parsed).size() > 65536)
              throw std::runtime_error(std::string("invalid component.") + key);
          }
        }
        if (const auto value = c.extra.find("image_selection"); value != c.extra.end()) {
          const auto parsed = parse_project_input_value(value->second);
          if (!std::holds_alternative<std::int64_t>(parsed) || std::get<std::int64_t>(parsed) < 0 || std::get<std::int64_t>(parsed) > 63)
            throw std::runtime_error("image_selection must be an integer in 0..63");
        }
          for (const std::string key : {"image_id", "image_mode", "caption"}) {
            const auto value = c.extra.find(key);
            if (value == c.extra.end()) continue;
            const auto parsed = parse_project_input_value(value->second);
            const auto text = std::get_if<std::string>(&parsed);
            if (!text) throw std::runtime_error("component." + key + " must be a string");
            if (key == "image_id" && (text->empty() || text->size() > 256 || text->find('\0') != std::string::npos))
              throw std::runtime_error("image requires a valid image_id");
            if (key == "image_mode" && *text != "fit" && *text != "fill" && *text != "auto-height")
              throw std::runtime_error("image_mode must be fit, fill or auto-height");
            if (key == "caption" && text->size() > 65536) throw std::runtime_error("image caption exceeds 64 KiB");
          }
        }
        if (c.kind == "live_chart") {
          for (const auto &key : {"chart_id", "x_label", "y_label", "stroke", "caption"}) {
            const auto value = c.extra.find(key);
            if (value == c.extra.end()) continue;
            const auto parsed = parse_project_input_value(value->second);
            const auto text = std::get_if<std::string>(&parsed);
            if (!text) throw std::runtime_error(std::string("component.") + key + " must be a string");
            if (std::string(key) == "chart_id" && (text->empty() || text->size() > 256 || text->find('\0') != std::string::npos))
              throw std::runtime_error("live chart requires a valid chart_id");
            if ((std::string(key) == "x_label" || std::string(key) == "y_label") && text->size() > 65536)
              throw std::runtime_error("live chart axis label is too long");
            if (std::string(key) == "caption" && text->size() > 65536)
              throw std::runtime_error("live chart caption exceeds 64 KiB");
            if (std::string(key) == "stroke" && (text->size() != 7 || (*text)[0] != '#' ||
                text->find_first_not_of("0123456789abcdefABCDEF", 1) != std::string::npos))
              throw std::runtime_error("live chart stroke must be #rrggbb");
          }
          for (const std::string axis : {"x", "y"}) {
            std::optional<double> minimum, maximum;
            for (const auto &key : {axis + "_min", axis + "_max"}) {
              const auto value = c.extra.find(key);
              if (value == c.extra.end()) continue;
              const auto parsed = parse_project_input_value(value->second);
              double number;
              if (const auto integer = std::get_if<std::int64_t>(&parsed)) number = *integer;
              else if (const auto real = std::get_if<double>(&parsed)) number = *real;
              else throw std::runtime_error(std::string("component.") + key + " must be numeric");
              if (!std::isfinite(number) || std::abs(number) >= 1e100)
                throw std::runtime_error("live chart axis bound must be finite with magnitude below 1e100");
              (key == axis + "_min" ? minimum : maximum) = number;
            }
            if (minimum && maximum && *maximum - *minimum <= std::max(1e-100, std::max(std::abs(*minimum), std::abs(*maximum)) * 1e-13))
              throw std::runtime_error("live chart " + axis + "_max must exceed " + axis + "_min with a resolvable interval");
          }
        }
        // Chart settings remain extension fields in ProjectComponent. Only
        // members parsed into the typed component must be reserved here.
        validate_extra_fields(c.extra, {"id", "kind", "title", "input", "cell", "binding", "row", "span", "h_align", "v_align", "panels"}, "component");
      }
    };
    validate_grid(board.rows ? &*board.rows : nullptr, board.components, 0);
    validate_extra_fields(board.extra, {"id", "title", "sheet", "columns", "components", "rows"}, "board");
  }
  (void)project_input_defaults(document);
}

std::string serialize_project_document(const ProjectDocument &document) {
  validate_project_document(document);
  std::string output;
  output.reserve(1024U);
  auto write_extra = [&output](const ProjectExtraFields &extras,
                               unsigned indent) {
    for (const auto &entry : extras) {
      output += ",\n";
      output.append(indent, ' ');
      write_json_string(entry.first, &output);
      output += ": ";
      output += entry.second;
    }
  };

  output =
      "{\n  \"format\": \"sputnik-notebook\",\n  \"version\": " + std::to_string(document.version) + ",\n  \"title\": ";
  write_json_string(document.title, &output);
  output += ",\n  \"active_sheet\": ";
  write_json_string(document.active_sheet, &output);
  output += ",\n  \"sheets\": [";
  for (std::size_t sheet_index = 0U; sheet_index < document.sheets.size();
       ++sheet_index) {
    if (sheet_index != 0U) {
      output.push_back(',');
    }
    const ProjectSheet &sheet = document.sheets[sheet_index];
    output += "\n    {\n      \"id\": ";
    write_json_string(sheet.id, &output);
    output += ",\n      \"title\": ";
    write_json_string(sheet.title, &output);
    output += ",\n      \"cells\": [";
    for (std::size_t cell_index = 0U; cell_index < sheet.cells.size();
         ++cell_index) {
      if (cell_index != 0U) {
        output.push_back(',');
      }
      const ProjectCell &cell = sheet.cells[cell_index];
      output += "\n        {\n          \"id\": \"" + std::to_string(cell.id) +
                "\",\n          \"kind\": ";
      write_json_string(cell.kind, &output);
      if (cell.kind == "code") {
        output += ",\n          \"source\": ";
        write_json_string(cell.source, &output);
        output += ",\n          \"mode\": ";
        write_json_string(cell.mode == CellMode::Watch ? "watch" : "manual",
                          &output);
      } else if (cell.kind == "text") {
        output += ",\n          \"source\": ";
        write_json_string(cell.source, &output);
        if (!cell.formatting.empty()) {
          output += ",\n          \"formatting\": ";
          output += cell.formatting;
        }
      }
      write_extra(cell.extra, 10);
      output += "\n        }";
    }
    if (!sheet.cells.empty())
      output += "\n      ";
    output.push_back(']');
    write_extra(sheet.extra, 6);
    output += "\n    }";
  }
  output += "\n  ],\n  \"modules\": [";
  for (std::size_t index = 0U; index < document.modules.size(); ++index) {
    if (index != 0U) {
      output.push_back(',');
    }
    const ProjectModule &module = document.modules[index];
    output += "\n    {\n      \"id\": ";
    write_json_string(module.id, &output);
    output += ",\n      \"path\": ";
    write_json_string(module.path, &output);
    write_extra(module.extra, 6);
    output += "\n    }";
  }
  if (!document.modules.empty())
    output += "\n  ";
  output += "],\n  \"auto_imports\": [";
  for (std::size_t index = 0U; index < document.auto_imports.size(); ++index) {
    if (index != 0U) {
      output.push_back(',');
    }
    output += "\n    ";
    write_json_string(document.auto_imports[index], &output);
  }
  if (!document.auto_imports.empty())
    output += "\n  ";
  output.push_back(']');
  if (document.version >= 2) {
    output += ",\n  \"inputs\": [";
    for (std::size_t i = 0; i < document.inputs.size(); ++i) {
      const auto &input = document.inputs[i]; if (i) output += ',';
      output += "{\"id\":"; write_json_string(input.id, &output);
      output += ",\"title\":"; write_json_string(input.title, &output);
      output += ",\"type\":"; write_json_string(input.type, &output);
      output += ",\"default\":" + serialize_project_input_value(input.initial);
      if (input.minimum) output += ",\"minimum\":" + serialize_project_input_value(*input.minimum);
      if (input.maximum) output += ",\"maximum\":" + serialize_project_input_value(*input.maximum);
      write_extra(input.extra, 4); output += '}';
    }
    output += "],\n  \"boards\": [";
    for (std::size_t i = 0; i < document.boards.size(); ++i) {
      if (i) output += ',';
      output += serialize_project_board(document.boards[i]);
    }
    output += ']';
  }
  if (document.version >= 3) {
    output += ",\n  \"dependencies\": [";
    for (std::size_t i = 0; i < document.dependencies.size(); ++i) {
      const auto &dependency = document.dependencies[i]; if (i) output += ',';
      output += "{\"path\":"; write_json_string(dependency.path, &output);
      output += std::string(",\"auto_import\":") + (dependency.auto_import ? "true" : "false");
      write_extra(dependency.extra, 4); output += '}';
    }
    output += ']';
  }
  write_extra(document.extra, 2);
  output += "\n}\n";
  if (output.size() > kMaxJsonBytes) {
    document_error("document", "serialized JSON exceeds 16 MiB limit");
  }
  // Raw metadata can be valid at depth zero yet exceed the document limit
  // when embedded in a cell. Never emit a document the reader cannot reopen.
  (void)JsonParser(output).parse();
  return output;
}

ProjectDocument make_project_document(std::string title) {
  ProjectDocument document;
  document.title = std::move(title);
  document.active_sheet = "main";
  ProjectSheet sheet;
  sheet.id = "main";
  sheet.title = "Main";
  ProjectCell cell;
  cell.id = allocate_cell_id();
  cell.kind = "code";
  cell.mode = CellMode::Watch;
  sheet.cells.push_back(std::move(cell));
  document.sheets.push_back(std::move(sheet));
  return document;
}

ProjectTextContent parse_project_text_content(const std::string &json) {
  JsonValue root = JsonParser(json).parse();
  require_object(root, "$text");
  const JsonValue &source = require_member(root, "source", "$text");
  ProjectTextContent content;
  content.source = require_string(source, "$text.source");
  if (const JsonValue *formatting = find_member(root, "formatting")) {
    if (formatting->kind != JsonKind::Object) {
      document_error_at("$text.formatting", "expected an object",
                        formatting->begin);
    }
    content.formatting = raw_json(json, *formatting);
  }
  for (const auto &member : root.object) {
    if (member.first != "source" && member.first != "formatting") {
      document_error_at("$text." + member.first,
                        "unknown text content member", member.second.begin);
    }
  }
  validate_project_text_content(content);
  return content;
}

std::string serialize_project_text_content(const ProjectTextContent &content) {
  validate_project_text_content(content);
  std::string output = "{\"source\": ";
  write_json_string(content.source, &output);
  if (!content.formatting.empty()) {
    output += ", \"formatting\": ";
    output += content.formatting;
  }
  output.push_back('}');
  return output;
}

void validate_project_text_content(const ProjectTextContent &content) {
  validate_utf8(content.source, "text.source");
  std::size_t utf16_length = 0U;
  for (std::size_t position = 0U; position < content.source.size();) {
    std::size_t sequence_length = 0U;
    std::uint32_t codepoint = 0U;
    if (!decode_utf8_sequence(content.source, position, &sequence_length,
                              &codepoint)) {
      document_error("text.source", "contains invalid UTF-8");
    }
    utf16_length += codepoint > 0xFFFFU ? 2U : 1U;
    if (utf16_length > kMaxTextUtf16Length) {
      document_error("text.source", "text exceeds 16 MiB limit");
    }
    position += sequence_length;
  }
  if (!content.formatting.empty()) {
    if (content.formatting.size() > kMaxJsonBytes) {
      document_error("text.formatting", "formatting JSON exceeds 16 MiB limit");
    }
    validate_text_formatting_json(content.source, content.formatting,
                                  "text.formatting");
  }
}

} // namespace sputnik::notebook
