#include "tools/iamber/session.h"
#include "runtime/value_display.h"
#include "runtime/objects.h"
#include <cstdlib>
#include <iostream>

using namespace amber::runtime;
static void require(bool ok, const std::string &message) {
  if (!ok) { std::cerr << "result preview: " << message << '\n'; std::exit(1); }
}
int main() {
  const auto inspect = RuntimeStringifyMode::Inspect;
  const auto pretty = RuntimeStringifyMode::Pretty;
  auto list = make_intrusive<ListValue>();
  list->header.kind = HeapObjectKind::List;
  list->items = {Value::integer(1), Value::integer(2)};
  const auto array = Value::list(list);
  require(runtime_preview_value(array, inspect).text == "[1, 2]", "compact array");
  require(runtime_preview_value(array, pretty).text == "[\n  1,\n  2\n]", "structured pretty array");
  auto map = make_intrusive<MapValue>();
  map->header.kind = HeapObjectKind::Map;
  map->entries.push_back({Value::symbol(0), array});
  std::vector<std::string> symbols{"loss"};
  const auto formatted = runtime_preview_value(Value::map(map), pretty, nullptr, nullptr, &symbols);
  require(!formatted.truncated && formatted.text == "{\n  :loss: [\n    1,\n    2\n  ]\n}", "nested map formatting");

  // Both repeated DAGs and cycles must be safe; cycle != truncated content.
  list->items.push_back(array);
  require(runtime_preview_value(array, pretty).text.find("#<cycle>") != std::string::npos, "cycle detection");
  list->items.pop_back();
  Value dag = Value::integer(42);
  for (int i = 0; i < 30; ++i) {
    auto next = make_intrusive<ListValue>(); next->header.kind = HeapObjectKind::List;
    next->items = {dag, dag}; dag = Value::list(next);
  }
  auto bounded = runtime_preview_value(dag, pretty, nullptr, nullptr, nullptr, {65536, 25, 64});
  require(bounded.truncated && bounded.text.size() < 8192, "global traversal budget bounds exponential DAG");
  require(runtime_preview_value(dag, inspect, nullptr, nullptr, nullptr, {65536, 4096, 3}).truncated,
          "depth truncation is explicit");
  list->items.assign(200000, Value::integer(1));
  bounded = runtime_preview_value(array, inspect);
  require(bounded.truncated && bounded.text.size() <= 65536 && list->items.size() == 200000,
          "huge array bounded without modifying the value");

  std::vector<std::string> strings{std::string(2 * 1024 * 1024, 'x'), "😀Ж\n\"\\"};
  bounded = runtime_preview_value(Value::string(0), inspect, nullptr, &strings);
  require(bounded.truncated && bounded.text.size() <= 65536, "huge string bounded while escaping");
  for (std::size_t size = 0; size < 24; ++size) {
    bounded = runtime_preview_value(Value::string(1), inspect, nullptr, &strings, nullptr, {size, 4096, 16});
    require(bounded.text.size() <= size, "tiny byte budget");
    // Partial UTF-8 sequences would appear only as isolated prefixes/suffixes.
    require(bounded.text.find('\xf0') == std::string::npos || bounded.text.find("😀") != std::string::npos,
            "UTF-8 emoji is never split");
    require(bounded.text.find('\xd0') == std::string::npos || bounded.text.find("Ж") != std::string::npos,
            "UTF-8 Cyrillic is never split");
  }
  require(runtime_preview_value(Value::string(1), inspect, nullptr, &strings).text == "\"😀Ж\\n\\\"\\\\\"",
          "inspect escapes strings, not their content structure");
  require(runtime_preview_value(Value::string(1), RuntimeStringifyMode::Display, nullptr, &strings).text == strings[1],
          "interpolation preserves plain string contents");
  auto result = std::make_shared<ResultValue>(); result->payload = Value::result(result);
  require(runtime_preview_value(Value::result(result), inspect).text.find("#<cycle>") != std::string::npos,
          "Result wrapper cannot bypass cycle guard");
  result->payload = Value::null();
  auto big = std::make_shared<BigIntValue>(); big->magnitude.assign(100000, 123);
  require(runtime_preview_value(Value::big_int(big), inspect).truncated, "huge BigInt avoids decimal allocation");

  Session session;
  Cell cell; cell.id = amber::notebook::allocate_cell_id();
  cell.source = "metrics = {:accuracy: 98.69, :loss: [0.2, 0.1], :device: \"mps\"}\nmetrics\n";
  session.cells.push_back(cell);
  evaluate_from(&session, 0, true, false, false);
  require(session.cells[0].ok, "notebook evaluation: " + session.cells[0].error);
  require(session.cells[0].result_format.is_container &&
          session.cells[0].result_format.pretty.find("\n  :accuracy: 98.69") != std::string::npos,
          "actual notebook result carries pretty metadata");
  const auto legacy = evaluate_prefix(session.cells, 0);
  require(legacy.ok && legacy.result_format.is_container && legacy.result_format.pretty == session.cells[0].result_format.pretty,
          "both evaluation paths produce the same preview");
  session.cells[0].source = "1 / 0\n"; session.cells[0].dirty = true;
  evaluate_from(&session, 0, true, false, false);
  require(!session.cells[0].ok && session.cells[0].result_format.pretty.empty() && !session.cells[0].result_format.is_container,
          "failed run cannot display old pretty result");
  std::cout << "notebook_result_tests: ok\n";
}
