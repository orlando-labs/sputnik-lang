#pragma once

#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace sputnik::pkg::registry_json {
// Bounded protocol documents. Preserve number spelling so sizes never pass
// through floating point, and reject ambiguous duplicate object keys.
struct Value {
  enum Kind { Null, Boolean, Number, String, Array, Object } kind = Null;
  std::string text;
  std::vector<Value> array;
  std::map<std::string, Value> object;
  const Value &at(const std::string &key) const {
    if (kind != Object || !object.count(key))
      throw std::runtime_error("missing registry field: " + key);
    return object.at(key);
  }
  std::string string() const {
    if (kind != String) throw std::runtime_error("expected registry string");
    return text;
  }
  bool truth() const { return kind == Boolean && text == "true"; }
};

class Parser {
  const std::string &s;
  std::size_t p = 0;
  [[noreturn]] void fail() const { throw std::runtime_error("invalid registry JSON"); }
  char peek() const { return p < s.size() ? s[p] : '\0'; }
  void ws() { while (peek() == ' ' || peek() == '\n' || peek() == '\r' || peek() == '\t') ++p; }
  void take(char c) { if (peek() != c) fail(); ++p; }
  unsigned hex() {
    unsigned n = 0;
    for (int i = 0; i < 4; ++i) {
      char c = peek(); ++p; n *= 16;
      if (c >= '0' && c <= '9') n += c - '0';
      else if (c >= 'a' && c <= 'f') n += c - 'a' + 10;
      else if (c >= 'A' && c <= 'F') n += c - 'A' + 10;
      else fail();
    }
    return n;
  }
  std::string str() {
    take('"'); std::string out;
    while (peek() != '"') {
      unsigned char c = peek(); ++p;
      if (c < 32) fail();
      if (c != '\\') { out += c; continue; }
      c = peek(); ++p;
      switch (c) {
      case '"': case '\\': case '/': out += c; break;
      case 'b': out += '\b'; break; case 'f': out += '\f'; break;
      case 'n': out += '\n'; break; case 'r': out += '\r'; break;
      case 't': out += '\t'; break;
      case 'u': {
        unsigned cp = hex();
        if (cp >= 0xd800 && cp <= 0xdbff) {
          take('\\'); take('u'); unsigned low = hex();
          if (low < 0xdc00 || low > 0xdfff) fail();
          cp = 0x10000 + ((cp - 0xd800) << 10) + low - 0xdc00;
        } else if (cp >= 0xdc00 && cp <= 0xdfff) fail();
        if (cp <= 0x7f) out += char(cp);
        else if (cp <= 0x7ff) { out += char(0xc0 | (cp >> 6)); out += char(0x80 | (cp & 63)); }
        else if (cp <= 0xffff) { out += char(0xe0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 63)); out += char(0x80 | (cp & 63)); }
        else { out += char(0xf0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 63)); out += char(0x80 | ((cp >> 6) & 63)); out += char(0x80 | (cp & 63)); }
        break;
      }
      default: fail();
      }
    }
    take('"'); return out;
  }
  Value value(unsigned depth) {
    if (depth > 64) fail(); ws(); Value v;
    if (peek() == '"') { v.kind = Value::String; v.text = str(); }
    else if (peek() == '{' || peek() == '[') {
      bool object = peek() == '{'; ++p; ws();
      v.kind = object ? Value::Object : Value::Array;
      const char end = object ? '}' : ']';
      if (peek() != end) for (;;) {
        ws(); std::string key;
        if (object) { key = str(); ws(); take(':'); }
        auto child = value(depth + 1);
        if (object) { if (!v.object.emplace(key, std::move(child)).second) fail(); }
        else v.array.push_back(std::move(child));
        ws(); if (peek() != ',') break; ++p;
      }
      take(end);
    } else if (peek() == '-' || (peek() >= '0' && peek() <= '9')) {
      auto start = p;
      if (peek() == '-') ++p;
      if (peek() == '0') ++p;
      else { if (peek() < '1' || peek() > '9') fail(); while (peek() >= '0' && peek() <= '9') ++p; }
      if (peek() == '.') { ++p; auto digit = p; while (peek() >= '0' && peek() <= '9') ++p; if (p == digit) fail(); }
      if (peek() == 'e' || peek() == 'E') { ++p; if (peek() == '+' || peek() == '-') ++p; auto digit = p; while (peek() >= '0' && peek() <= '9') ++p; if (p == digit) fail(); }
      v.kind = Value::Number; v.text = s.substr(start, p - start);
    } else {
      auto literal = [&](const char *word) { std::string w(word); if (s.compare(p, w.size(), w)) fail(); p += w.size(); v.text = w; };
      if (peek() == 'n') literal("null");
      else { v.kind = Value::Boolean; if (peek() == 't') literal("true"); else literal("false"); }
    }
    return v;
  }
public:
  explicit Parser(const std::string &input) : s(input) {}
  Value parse() { auto v = value(0); ws(); if (p != s.size()) fail(); return v; }
};
} // namespace sputnik::pkg::registry_json
