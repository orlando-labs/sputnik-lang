#include "runtime/config_document.h"
#include <yaml.h>
#define TOML_MAX_NESTED_VALUES 64
#include "third_party/tomlplusplus/toml.hpp"
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace sputnik::runtime {
namespace {
void check_source(const std::string &source) {
  if (source.size() > 65536 || source.find('\0') != std::string::npos)
    throw std::runtime_error("Config must fit 64 KiB and contain no NUL");
}
// LibYAML treats NEL/Unicode separators as YAML line breaks even inside a
// quoted JSON string. Escape them before using JSON as a serialization input,
// and when emitting strings, so conversion never normalizes their contents.
std::string escape_yaml_unicode(const std::string &source) {
  std::string out;
  constexpr char hex[] = "0123456789abcdef";
  for (std::size_t i = 0; i < source.size(); ++i) {
    const auto c = static_cast<unsigned char>(source[i]);
    if (c == 0x7f) out += "\\u007f";
    else if (c == 0xc2 && i + 1 < source.size() && static_cast<unsigned char>(source[i + 1]) >= 0x80 && static_cast<unsigned char>(source[i + 1]) <= 0x9f) {
      const auto next = static_cast<unsigned char>(source[++i]);
      out += "\\u00"; out += hex[next >> 4]; out += hex[next & 15];
    } else if (c == 0xe2 && i + 2 < source.size() && static_cast<unsigned char>(source[i + 1]) == 0x80 &&
               (static_cast<unsigned char>(source[i + 2]) == 0xa8 || static_cast<unsigned char>(source[i + 2]) == 0xa9)) {
      out += static_cast<unsigned char>(source[i + 2]) == 0xa8 ? "\\u2028" : "\\u2029"; i += 2;
    } else out += source[i];
  }
  return out;
}
std::string quote(const std::string &text) {
  std::string out = "\"";
  constexpr char hex[] = "0123456789abcdef";
  for (const unsigned char c : text) {
    if (c == '"' || c == '\\') { out += '\\'; out += c; }
    else if (c < 32) { out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
    else out += c;
  }
  return escape_yaml_unicode(out + '"');
}
std::string floating(double value) {
  if (!std::isfinite(value)) throw std::runtime_error("Config numbers must be finite");
  std::ostringstream out; out.imbue(std::locale::classic());
  out << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
  auto text = out.str();
  if (text.find_first_of(".eE") == std::string::npos) text += ".0";
  return text;
}
struct Node {
  enum Kind { Null, Bool, Int, Float, String, Sequence, Mapping } kind = Null;
  std::string scalar;
  using Ptr = std::shared_ptr<Node>;
  std::vector<Ptr> sequence;
  std::vector<std::pair<std::string, Ptr>> mapping;
};
struct YamlParser {
  yaml_parser_t parser {};
  yaml_event_t event {};
  bool has_event = false;
  unsigned nodes = 0;
  std::map<std::string, Node::Ptr> anchors;
  explicit YamlParser(const std::string &text) {
    if (!yaml_parser_initialize(&parser)) throw std::bad_alloc();
    yaml_parser_set_input_string(&parser, reinterpret_cast<const unsigned char *>(text.data()), text.size());
  }
  ~YamlParser() { if (has_event) yaml_event_delete(&event); yaml_parser_delete(&parser); }
  [[noreturn]] void fail(const std::string &message) const {
    const auto mark = has_event ? event.start_mark : parser.problem_mark;
    throw std::runtime_error("YAML " + std::to_string(mark.line + 1) + ":" + std::to_string(mark.column + 1) + ": " + message);
  }
  void next() {
    if (has_event) yaml_event_delete(&event);
    has_event = false;
    if (!yaml_parser_parse(&parser, &event)) fail(parser.problem ? parser.problem : "parse failed");
    has_event = true;
  }
  static std::string text(const yaml_char_t *value) { return value ? reinterpret_cast<const char *>(value) : ""; }
  void tag(const yaml_char_t *value, const std::string &expected) {
    const auto name = text(value);
    if (!name.empty() && name != "!" && name != "tag:yaml.org,2002:" + expected)
      fail("unsupported YAML tag: " + name);
  }
  Node::Ptr scalar() {
    auto node = std::make_shared<Node>();
    const auto &data = event.data.scalar;
    const auto name = text(data.tag);
    const auto value = std::string(reinterpret_cast<const char *>(data.value), data.length);
    const bool implicit = name.empty() && data.style == YAML_PLAIN_SCALAR_STYLE;
    const auto is = [&](const char *type) { return name == std::string("tag:yaml.org,2002:") + type; };
    static const std::regex integer(R"([+-]?(?:[0-9]+|0o[0-7]+|0x[0-9a-fA-F]+))");
    static const std::regex decimal(R"([+-]?(?:[0-9]+\.[0-9]*|\.[0-9]+|[0-9]+)(?:[eE][+-]?[0-9]+)?)");
    const bool nil = value.empty() || value == "~" || value == "null" || value == "Null" || value == "NULL";
    const bool truth = value == "true" || value == "True" || value == "TRUE";
    const bool falsehood = value == "false" || value == "False" || value == "FALSE";
    if (is("null") || (implicit && nil)) {
      if (!nil) fail("invalid null scalar");
      node->kind = Node::Null; node->scalar = "null";
    } else if (is("bool") || (implicit && (truth || falsehood))) {
      if (!truth && !falsehood) fail("invalid boolean scalar");
      node->kind = Node::Bool; node->scalar = truth ? "true" : "false";
    } else if (is("int") || (implicit && std::regex_match(value, integer))) {
      try {
        std::string number = value;
        auto sign = number.find_first_not_of("+-");
        int base = 10;
        if (sign != std::string::npos && number.compare(sign, 2, "0x") == 0) base = 16;
        if (sign != std::string::npos && number.compare(sign, 2, "0o") == 0) { base = 8; number.erase(sign, 2); }
        std::size_t end = 0;
        const auto parsed = std::stoll(number, &end, base);
        if (end != number.size() || !std::regex_match(value, integer)) fail("invalid integer scalar");
        node->kind = Node::Int; node->scalar = std::to_string(parsed);
      } catch (const std::invalid_argument &) { fail("invalid integer scalar"); }
        catch (const std::out_of_range &) { fail("integer must fit Int64"); }
    } else if (is("float") || (implicit && std::regex_match(value, decimal))) {
      std::istringstream stream(value); stream.imbue(std::locale::classic()); double number = 0;
      stream >> number;
      if (stream.fail() || !stream.eof() || !std::isfinite(number)) fail("number must fit a finite Float");
      node->kind = Node::Float; node->scalar = floating(number);
    } else {
      if (!name.empty() && name != "!" && !is("str")) fail("unsupported YAML tag: " + name);
      if (implicit && (value == ".inf" || value == ".Inf" || value == ".INF" || value == "+.inf" || value == "-.inf" ||
                       value == "+.Inf" || value == "-.Inf" || value == "+.INF" || value == "-.INF" || value == ".nan" || value == ".NaN" || value == ".NAN"))
        fail("config numbers must be finite");
      node->kind = Node::String; node->scalar = value;
    }
    return node;
  }
  Node::Ptr parse_node(unsigned depth = 0) {
    if (depth > 64 || ++nodes > 16384) fail("config exceeds depth 64 or 16384 nodes");
    Node::Ptr node;
    std::string anchor;
    if (event.type == YAML_ALIAS_EVENT) {
      const auto found = anchors.find(text(event.data.alias.anchor));
      if (found == anchors.end()) fail("unknown or recursive YAML alias");
      node = found->second; next(); return node;
    }
    if (event.type == YAML_SCALAR_EVENT) {
      anchor = text(event.data.scalar.anchor); node = scalar(); next();
    } else if (event.type == YAML_SEQUENCE_START_EVENT) {
      tag(event.data.sequence_start.tag, "seq"); anchor = text(event.data.sequence_start.anchor);
      node = std::make_shared<Node>(); node->kind = Node::Sequence; next();
      while (event.type != YAML_SEQUENCE_END_EVENT) node->sequence.push_back(parse_node(depth + 1));
      next();
    } else if (event.type == YAML_MAPPING_START_EVENT) {
      tag(event.data.mapping_start.tag, "map"); anchor = text(event.data.mapping_start.anchor);
      node = std::make_shared<Node>(); node->kind = Node::Mapping; next(); std::set<std::string> keys;
      while (event.type != YAML_MAPPING_END_EVENT) {
        // Config keys remain strings, including plain keys such as `on` or `1`.
        if (event.type != YAML_SCALAR_EVENT) fail("config mapping keys must be strings");
        tag(event.data.scalar.tag, "str");
        const std::string key(reinterpret_cast<const char *>(event.data.scalar.value), event.data.scalar.length);
        if (key == "<<") fail("YAML merge keys are not supported; use explicit fields");
        if (!keys.insert(key).second) fail("duplicate mapping key: " + key);
        next(); node->mapping.emplace_back(key, parse_node(depth + 1));
      }
      next();
    } else fail("expected a config value");
    if (!anchor.empty()) anchors[anchor] = node;
    return node;
  }
  Node::Ptr parse() {
    next(); if (event.type != YAML_STREAM_START_EVENT) fail("expected a YAML stream");
    next();
    if (event.type == YAML_STREAM_END_EVENT) return std::make_shared<Node>();
    if (event.type != YAML_DOCUMENT_START_EVENT) fail("expected a YAML document");
    if (const auto version = event.data.document_start.version_directive)
      if (version->major != 1 || version->minor != 2) fail("config uses the YAML 1.2 core schema");
    next(); auto root = parse_node();
    if (event.type != YAML_DOCUMENT_END_EVENT) fail("expected end of YAML document");
    next(); if (event.type != YAML_STREAM_END_EVENT) fail("config must contain one YAML document");
    return root;
  }
};
struct Writer {
  std::string out;
  unsigned nodes = 0;
  void enter(unsigned depth) {
    if (depth > 64 || ++nodes > 16384 || out.size() > 512 * 1024)
      throw std::runtime_error("Config exceeds depth 64, 16384 nodes or 512 KiB expanded data");
  }
  void yaml_json(const Node &node, unsigned depth = 0) {
    enter(depth);
    if (node.kind == Node::Sequence) {
      out += '['; bool first = true;
      for (const auto &item : node.sequence) { if (!first) out += ','; first = false; yaml_json(*item, depth + 1); }
      out += ']';
    } else if (node.kind == Node::Mapping) {
      out += '{'; bool first = true;
      for (const auto &[key, value] : node.mapping) {
        if (!first) out += ',';
        first = false; out += quote(key); out += ':'; yaml_json(*value, depth + 1);
      }
      out += '}';
    } else out += node.kind == Node::String ? quote(node.scalar) : node.kind == Node::Null ? "null" : node.scalar;
    if (out.size() > 512 * 1024) throw std::runtime_error("Config exceeds 512 KiB expanded data");
  }
  void toml(const toml::node &node, unsigned depth = 0) {
    enter(depth);
    if (const auto table = node.as_table()) {
      out += '{'; bool first = true;
      for (const auto &[key, value] : *table) {
        if (!first) out += ',';
        first = false; out += quote(std::string(key.str())); out += ':'; toml(value, depth + 1);
      }
      out += '}';
    } else if (const auto array = node.as_array()) {
      out += '['; bool first = true;
      for (const auto &value : *array) { if (!first) out += ','; first = false; toml(value, depth + 1); }
      out += ']';
    } else if (const auto value = node.as_string()) out += quote(value->get());
    else if (const auto value = node.as_integer()) out += std::to_string(value->get());
    else if (const auto value = node.as_floating_point()) out += floating(value->get());
    else if (const auto value = node.as_boolean()) out += value->get() ? "true" : "false";
    else {
      std::ostringstream text;
      if (const auto value = node.as_date()) text << value->get();
      else if (const auto value = node.as_time()) text << value->get();
      else if (const auto value = node.as_date_time()) text << value->get();
      out += quote(text.str());
    }
    if (out.size() > 512 * 1024) throw std::runtime_error("Config exceeds 512 KiB expanded data");
  }
};
void insert_toml(toml::table &table, const std::string &key, const Node &node);
void append_toml(toml::array &array, const Node &node);
template<class Put> void toml_value(const Node &node, Put put) {
  if (node.kind == Node::Null) throw std::runtime_error("TOML cannot represent null");
  if (node.kind == Node::Mapping) {
    toml::table value; for (const auto &[key, item] : node.mapping) insert_toml(value, key, *item); put(std::move(value));
  } else if (node.kind == Node::Sequence) {
    toml::array value; for (const auto &item : node.sequence) append_toml(value, *item); put(std::move(value));
  } else if (node.kind == Node::String) put(node.scalar);
  else if (node.kind == Node::Bool) put(node.scalar == "true");
  else if (node.kind == Node::Int) put(std::stoll(node.scalar));
  else if (node.kind == Node::Float) {
    std::istringstream stream(node.scalar); stream.imbue(std::locale::classic()); double value = 0; stream >> value; put(value);
  }
}
void insert_toml(toml::table &table, const std::string &key, const Node &node) {
  toml_value(node, [&](auto value) { table.insert(key, std::move(value)); });
}
void append_toml(toml::array &array, const Node &node) {
  toml_value(node, [&](auto value) { array.push_back(std::move(value)); });
}
void emit_yaml(const Node &node, std::string &out, unsigned indent = 0) {
  const auto nested = [](const Node &value) { return !value.mapping.empty() || !value.sequence.empty(); };
  if (node.kind == Node::Mapping && !node.mapping.empty()) {
    for (const auto &[key, value] : node.mapping) {
      out.append(indent, ' '); out += quote(key); out += ':';
      if (nested(*value)) { out += '\n'; emit_yaml(*value, out, indent + 2); }
      else { Writer writer; writer.yaml_json(*value); out += ' '; out += writer.out; out += '\n'; }
    }
  } else if (node.kind == Node::Sequence && !node.sequence.empty()) {
    for (const auto &value : node.sequence) {
      out.append(indent, ' '); out += '-';
      if (nested(*value)) { out += '\n'; emit_yaml(*value, out, indent + 2); }
      else { Writer writer; writer.yaml_json(*value); out += ' '; out += writer.out; out += '\n'; }
    }
  } else { Writer writer; writer.yaml_json(node); out.append(indent, ' '); out += writer.out; out += '\n'; }
  if (out.size() > 65536) throw std::runtime_error("Serialized config exceeds 64 KiB");
}
} // namespace
std::string config_to_json(const std::string &source, const std::string &format) {
  check_source(source);
  if (format == "json") return source; // Json.parse performs strict validation.
  Writer writer;
  if (format == "yaml") writer.yaml_json(*YamlParser(source).parse());
  else if (format == "toml") {
    try { writer.toml(toml::parse(source)); }
    catch (const toml::parse_error &error) {
      std::ostringstream message; message << "TOML " << error.source().begin << ": " << error.description();
      throw std::runtime_error(message.str());
    }
  } else throw std::runtime_error("Config format must be json, yaml or toml");
  return writer.out;
}
std::string config_from_json(const std::string &json, const std::string &format) {
  check_source(json);
  if (format == "json") return json;
  const auto escaped = escape_yaml_unicode(json);
  const auto node = YamlParser(escaped).parse();
  Writer bounded; bounded.yaml_json(*node);
  std::string result;
  if (format == "yaml") emit_yaml(*node, result);
  else if (format == "toml") {
    if (node->kind != Node::Mapping) throw std::runtime_error("TOML requires a mapping at the root");
    toml::table table;
    for (const auto &[key, value] : node->mapping) insert_toml(table, key, *value);
    std::ostringstream out; out << table; result = out.str();
  } else throw std::runtime_error("Config format must be json, yaml or toml");
  check_source(result);
  return result;
}
} // namespace sputnik::runtime
