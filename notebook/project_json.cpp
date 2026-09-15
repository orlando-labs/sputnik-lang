#include "notebook/project.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace amber::notebook {

namespace {

constexpr std::size_t kMaxJsonBytes = 16U * 1024U * 1024U;
constexpr std::size_t kMaxJsonDepth = 64U;

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
  } else {
    // Opaque future kinds may carry fields named source/mode; they are not
    // interpreted until that kind has a schema of its own.
    validate_extra_fields(cell.extra, {"id", "kind"}, path);
  }
}

} // namespace

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
      json.substr(version.begin, version.end - version.begin) != "1") {
    document_error_at("$.version", "version must be the number 1",
                      version.begin);
  }

  ProjectDocument document;
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
                                 "sheets", "modules", "auto_imports"},
                                json);
  validate_project_document(document);
  return document;
}

void validate_project_document(const ProjectDocument &document) {
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
                         "modules", "auto_imports"},
                        "document");
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
      "{\n  \"format\": \"amber-notebook\",\n  \"version\": 1,\n  \"title\": ";
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

} // namespace amber::notebook
