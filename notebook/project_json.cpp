#include "notebook/project.h"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace amber::notebook {

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
  explicit JsonParser(const std::string &input) : input_(input) {
    if (input_.size() > kMaxJsonBytes) {
      fail("input exceeds 16 MiB limit", kMaxJsonBytes);
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
      path_value.find(':') != std::string::npos || path_value.size() < 3U ||
      path_value.compare(path_value.size() - 3U, 3U, ".am") != 0) {
    document_error(path,
                   "module path must be a relative slash-separated .am path");
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

runtime::NotebookInputValue parse_project_input_value(const std::string &json) {
  const auto value = JsonParser(json).parse();
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
  throw std::runtime_error("project input must be a finite number, Int64, boolean or string");
}

std::string serialize_project_input_value(const runtime::NotebookInputValue &value) {
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
      (input.type != "number" && input.type != "integer" && input.type != "boolean" && input.type != "string"))
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
  for (const auto &item : require_array(require_member(root, "components", "board"), "board.components")) {
    require_object(item, "component");
    ProjectComponent component;
    component.id = require_string(require_member(item, "id", "component"), "component.id");
    component.kind = require_string(require_member(item, "kind", "component"), "component.kind");
    component.title = require_string(require_member(item, "title", "component"), "component.title");
    if (const auto v = find_member(item, "input")) component.input = require_string(*v, "component.input");
    if (const auto v = find_member(item, "cell")) component.cell = parse_cell_id(*v, "component.cell");
    if (const auto v = find_member(item, "binding")) component.binding = require_string(*v, "component.binding");
    component.extra = parse_extras(item, {"id", "kind", "title", "input", "cell", "binding"}, json);
    board.components.push_back(std::move(component));
  }
  board.extra = parse_extras(root, {"id", "title", "sheet", "columns", "components"}, json);
  return board;
}

std::string serialize_project_board(const ProjectBoard &board) {
  std::string out = "{";
  const auto field = [&](const std::string &key, const std::string &value) {
    if (out.back() != '{') out += ',';
    write_json_string(key, &out); out += ':'; write_json_string(value, &out);
  };
  field("id", board.id); field("title", board.title); field("sheet", board.sheet);
  out += ",\"columns\":" + std::to_string(board.columns) + ",\"components\":[";
  for (std::size_t i = 0; i < board.components.size(); ++i) {
    const auto &c = board.components[i]; if (i) out += ','; out += '{';
    field("id", c.id); field("kind", c.kind); field("title", c.title);
    if (!c.input.empty()) field("input", c.input);
    if (c.cell) field("cell", std::to_string(c.cell));
    if (!c.binding.empty()) field("binding", c.binding);
    for (const auto &extra : c.extra) { out += ','; write_json_string(extra.first, &out); out += ':' + extra.second; }
    out += '}';
  }
  out += ']';
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

ProjectDocument parse_project_document(const std::string &json) {
  JsonValue root = JsonParser(json).parse();
  require_object(root, "$");

  const JsonValue &format = require_member(root, "format", "$");
  if (require_string(format, "$.format") != "amber-notebook") {
    document_error_at("$.format", "format must be 'amber-notebook'",
                      format.begin);
  }
  const JsonValue &version = require_member(root, "version", "$");
  if (version.kind != JsonKind::Number ||
      (raw_json(json, version) != "1" && raw_json(json, version) != "2" && raw_json(json, version) != "3")) {
    document_error_at("$.version", "version must be 1, 2 or 3",
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
      require_object(item, "dependency");
      ProjectDependency dependency;
      dependency.path = require_string(require_member(item, "path", "dependency"), "dependency.path");
      if (const auto value = find_member(item, "auto_import")) {
        if (value->kind != JsonKind::Boolean) throw std::runtime_error("dependency.auto_import must be boolean");
        dependency.auto_import = raw_json(json, *value) == "true";
      }
      dependency.extra = parse_extras(item, {"path", "auto_import"}, json);
      document.dependencies.push_back(std::move(dependency));
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
  validate_project_document(document);
  return document;
}

void validate_project_document(const ProjectDocument &document) {
  if ((document.version < 1 || document.version > 3) ||
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
    for (const auto &c : board.components) {
      validate_module_id(c.id, "component.id"); validate_utf8(c.title, "component.title");
      validate_utf8(c.binding, "component.binding");
      if (!components.insert(c.id).second) throw std::runtime_error("duplicate component id");
      if (c.kind != "input" && c.kind != "text" && c.kind != "plot" && c.kind != "run")
        throw std::runtime_error("unsupported board component kind: " + c.kind);
      if (c.kind == "input" && !inputs.count(c.input)) throw std::runtime_error("component references unknown input");
      // A missing output target remains an explicit broken binding in the UI;
      // editing a sheet must not become impossible because a board references it.
      if (c.kind != "input" && c.cell == 0) throw std::runtime_error("output component requires a cell id");
      validate_extra_fields(c.extra, {"id", "kind", "title", "input", "cell", "binding"}, "component");
    }
    validate_extra_fields(board.extra, {"id", "title", "sheet", "columns", "components"}, "board");
  }
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
      "{\n  \"format\": \"amber-notebook\",\n  \"version\": " + std::to_string(document.version) + ",\n  \"title\": ";
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

} // namespace amber::notebook
