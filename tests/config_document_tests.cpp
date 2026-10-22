#include "runtime/config_document.h"
#include <iostream>
#include <stdexcept>
#include <string>
using sputnik::runtime::config_to_json;
using sputnik::runtime::config_from_json;
static void check(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }
static void rejects(const std::string &source, const char *format) {
  try { (void)config_to_json(source, format); }
  catch (const std::exception &) { return; }
  throw std::runtime_error(std::string("accepted invalid ") + format + ": " + source.substr(0, 100));
}
int main() {
  try {
    const auto yaml = config_to_json("# header\nseed: 9223372036854775807\nmin: -9223372036854775808\nflag: true\non: off\nhex: 0xff\noctal: 0o10\nfloat: 1e2\nstring: !!str 12\ntext: |\n  first\n  second\nbase: &base {rate: 0.25}\ncopy: *base\n", "yaml");
    check(yaml.find("9223372036854775807") != std::string::npos && yaml.find("-9223372036854775808") != std::string::npos, "YAML Int64 rounded");
    check(yaml.find("\"on\":\"off\"") != std::string::npos && yaml.find("\"hex\":255") != std::string::npos && yaml.find("\"octal\":8") != std::string::npos, "YAML core typing differs");
    check(yaml.find("\"copy\":{\"rate\":0.25}") != std::string::npos && yaml.find("first\\u000asecond") != std::string::npos, "YAML aliases/block text failed");
    for (const auto &source : {"a: [1, 2", "a: 1\na: 2", "a: 1\n\"\\u0061\": 2", "x: .inf", "x: .nan", "x: !custom hi", "? [a,b]\n: value", "x: &x [*x]", "x: *missing", "x: 9223372036854775808", "x: -9223372036854775809", "x: 1e999", "---\na: 1\n---\nb: 2", "%YAML 1.1\n---\na: on", "x: {<<: {a: 1}}"}) rejects(source, "yaml");
    rejects(std::string(66, '[') + "0" + std::string(66, ']'), "yaml");
    rejects("text: \"" + std::string(1, char(0xff)) + "\"", "yaml");
    const auto toml = config_to_json("seed = 9_223_372_036_854_775_807\nhex = 0xff\nstarted = 2026-10-22T12:00:00Z\nmessage = '''first\nsecond'''\n[[items]]\nname = 'one'\n[[items]]\nname = 'two'\n", "toml");
    check(toml.find("9223372036854775807") != std::string::npos && toml.find("2026-10-22T12:00:00Z") != std::string::npos && toml.find("\"items\":[{") != std::string::npos, "TOML tables/date/Int64 failed");
    for (const auto &source : {"a = [1,", "a = 1\na = 2", "x = 9223372036854775808", "x = inf", "x = nan", "[unclosed", "[a]\n[a]"}) rejects(source, "toml");
    const std::string json = "{\"seed\":9223372036854775807,\"rate\":1.0,\"items\":[{\"name\":\"alpha\"}],\"empty\":{}}";
    for (const auto *format : {"yaml", "toml"}) {
      const auto generated = config_from_json(json, format);
      const auto parsed = config_to_json(generated, format);
      check(parsed.find("9223372036854775807") != std::string::npos && parsed.find("1.0") != std::string::npos && parsed.find("alpha") != std::string::npos, "serialization changed values");
    }
    const auto unicode = std::string("{\"text\":\"") + "\xc2\x85\xe2\x80\xa8\xe2\x80\xa9\x7f" + "\"}";
    for (const auto *format : {"yaml", "toml"}) {
      const auto text = config_from_json(unicode, format);
      const auto roundtrip = config_to_json(text, format);
      check(roundtrip == "{\"text\":\"\\u0085\\u2028\\u2029\\u007f\"}", "serialization normalized Unicode string contents");
    }
    for (const auto &json : {"[1,2]", "{\"x\":null}"}) {
      bool rejected = false; try { (void)config_from_json(json, "toml"); } catch (...) { rejected = true; }
      check(rejected, "TOML accepted unrepresentable JSON");
    }
    check(config_to_json("", "toml") == "{}" && config_to_json("", "yaml") == "null", "empty document semantics");
    std::cout << "Config codecs passed: YAML/TOML parsing, errors, limits, exact Int64, dates, aliases and serialization\n";
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
