#include <json-c/json.h>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
using Object = std::unique_ptr<json_object, decltype(&json_object_put)>;
Object own(json_object *p) { if (!p) throw std::runtime_error("JSON allocation/parse"); return {p, json_object_put}; }
Object parse(const std::string &s) { return own(json_tokener_parse(s.c_str())); }
json_object *field(json_object *o, const char *key) {
  json_object *v = nullptr;
  if (!json_object_object_get_ex(o, key, &v)) throw std::runtime_error("missing field");
  return v;
}
std::int64_t integer(json_object *o, const char *key) { return json_object_get_int64(field(o, key)); }
void add(json_object *o, const char *key, json_object *v) {
  if (json_object_object_add(o, key, v)) throw std::runtime_error("JSON add");
}
void verify(json_object *p, json_object *expected) {
  if (!json_object_equal(p, expected)) throw std::runtime_error("JSON DOM mismatch");
}
struct Result { std::int64_t checksum, documents, pretty, rows; };
Result main_workload() {
  Result r{};
  for (std::int64_t i = 0; i < 1000; ++i) {
    auto document = own(json_object_new_object());
    add(document.get(), "id", json_object_new_int64(i));
    add(document.get(), "value", json_object_new_int64(i * 2));
    add(document.get(), "group", json_object_new_int64(i % 7));
    add(document.get(), "name", json_object_new_string("event"));
    auto items = own(json_object_new_array());
    for (int n = 0; n < 3; ++n) json_object_array_add(items.get(), json_object_new_int64(i + n));
    add(document.get(), "items", items.release());
    const std::string compact = json_object_to_json_string_ext(document.get(), JSON_C_TO_STRING_PLAIN);
    auto parsed = parse(compact);
    verify(parsed.get(), document.get()); ++r.documents;
    r.checksum += integer(parsed.get(), "id") + integer(parsed.get(), "value") + integer(parsed.get(), "group") +
      json_object_get_int64(json_object_array_get_idx(field(parsed.get(), "items"), 1));
    if (i < 20) {
      const std::string pretty = json_object_to_json_string_ext(parsed.get(), JSON_C_TO_STRING_PRETTY);
      if (pretty.find('\n') == std::string::npos) throw std::runtime_error("not pretty");
      auto reparsed = parse(pretty); verify(reparsed.get(), document.get()); ++r.pretty;
      r.checksum += json_object_get_int64(json_object_array_get_idx(field(reparsed.get(), "items"), 2));
    }
  }
  std::ifstream input("bench/polyglot/build/json/events.jsonl");
  if (!input) throw std::runtime_error("events.jsonl");
  std::string line;
  while (std::getline(input, line)) {
    auto row = parse(line);
    if (json_object_object_length(row.get()) != 4 || integer(row.get(), "id") != r.rows ||
        integer(row.get(), "value") != r.rows * 17 % 100000 || integer(row.get(), "group") != r.rows % 8 ||
        std::string(json_object_get_string(field(row.get(), "name"))) != "event-" + std::to_string(r.rows % 100))
      throw std::runtime_error("row mismatch");
    r.checksum += integer(row.get(), "id") * 3 + integer(row.get(), "value") - integer(row.get(), "group");
    ++r.rows;
  }
  if (!input.eof()) throw std::runtime_error("read error");
  r.checksum += r.rows;
  return r;
}
void print_work(const Result &r) {
  std::cout << "BENCH_WORK {\"checksum\":" << r.checksum << ",\"documents\":" << r.documents
            << ",\"pretty\":" << r.pretty << ",\"rows\":" << r.rows << "}\n";
}
} // namespace
int main() { const auto r = main_workload(); print_work(r); std::cout << r.checksum << '\n'; }
