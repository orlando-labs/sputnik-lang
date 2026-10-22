#pragma once
#include "bytecode/format.h"
#include <charconv>
#include <limits>
#include "runtime/objects.h"
#include "runtime/value_display.h"
#include <algorithm>
#include <cmath>

// An inert, bounded table captured while the result Value is owned by its
// runtime. It never reparses Pretty/Inspect and never invokes user callbacks.
namespace sputnik::runtime {

inline std::string notebook_table_snapshot(const sputnik::runtime::Value &value,
    const sputnik::bytecode::BcModule *module,
    const std::vector<std::string> *strings, const std::vector<std::string> *symbols,
    const std::vector<std::string> *columns = nullptr) {
  using namespace sputnik::runtime;
  const std::vector<Value> *items = value.is_list() && value.as_list() ? &value.as_list()->items :
      value.is_tuple() && value.as_tuple() ? &value.as_tuple()->items : nullptr;
  if (!items) return {};
  constexpr std::size_t max_rows = 4096, max_columns = 32, max_bytes = 1024 * 1024;
  const auto rows = std::min(items->size(), max_rows);
  bool truncated = rows < items->size();
  const bool records = rows && (*items)[0].is_map();
  const bool vectors = rows && ((*items)[0].is_list() || (*items)[0].is_tuple());
  auto sequence = [](const Value &v) -> const std::vector<Value> * {
    return v.is_list() && v.as_list() ? &v.as_list()->items :
        v.is_tuple() && v.as_tuple() ? &v.as_tuple()->items : nullptr;
  };
  auto text = [&](const Value &v, std::size_t budget = 4096) {
    auto result = runtime_preview_value(v, RuntimeStringifyMode::Display, module, strings, symbols, {budget, 32, 4});
    truncated = truncated || result.truncated;
    return result.text;
  };
  auto quote = [](const std::string &s) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string result = "\"";
    for (unsigned char c : s) {
      if (c == '\"' || c == '\\') { result += '\\'; result += c; }
      else if (c < 32) { result += "\\u00"; result += hex[c >> 4]; result += hex[c & 15]; }
      else result += c;
    }
    return result + '\"';
  };
  auto key_text = [&](const Value &key) -> const std::string * {
    if (key.is_heap_string()) return key.as_heap_string() ? &key.as_heap_string()->text : nullptr;
    const auto table = key.is_symbol()
        ? (symbols && !symbols->empty() ? symbols : module ? &module->symbols : nullptr)
        : (strings && !strings->empty() ? strings : module ? &module->strings : nullptr);
    const auto id = key.is_symbol() ? key.as_symbol().symbol_id : key.as_string().string_id;
    return table && id < table->size() ? &(*table)[id] : nullptr;
  };
  std::vector<std::string> names, types;
  // Column identity uses the full key, independently of its bounded title.
  // Distinct long keys must never overwrite each other's captured values.
  std::vector<const std::string *> keys;
  auto column_for = [&](const std::string *key) {
    return std::find_if(keys.begin(), keys.end(), [&](const auto candidate) { return *candidate == *key; });
  };
  if (columns) {
    if (records || columns->empty() || columns->size() > max_columns) return {};
    for (const auto &column : *columns) {
      if (column.size() > 1024 || std::find(names.begin(), names.end(), column) != names.end()) return {};
      names.push_back(column);
    }
  } else if (records) {
    for (std::size_t i = 0; i < rows; ++i) {
      if (!(*items)[i].is_map() || !(*items)[i].as_map()) return {};
      const auto &entries = (*items)[i].as_map()->entries;
      if (entries.size() > max_columns) truncated = true;
      for (std::size_t j = 0; j < std::min(entries.size(), max_columns); ++j) {
        if (!entries[j].key.is_string() && !entries[j].key.is_symbol()) return {};
        const auto key = key_text(entries[j].key);
        if (!key) return {};
        if (column_for(key) == keys.end()) {
          if (names.size() == max_columns) { truncated = true; continue; }
          keys.push_back(key);
          names.push_back(text(entries[j].key, 256));
        }
      }
    }
  } else if (vectors) {
    std::size_t width = 0;
    for (std::size_t i = 0; i < rows; ++i) {
      const auto row = sequence((*items)[i]); if (!row) return {};
      width = std::max(width, row->size());
    }
    if (width > max_columns) truncated = true;
    for (std::size_t i = 0; i < std::min(width, max_columns); ++i) names.push_back("Column " + std::to_string(i + 1));
  } else names.push_back("Value");
  if (names.empty()) names.push_back("Value");
  types.resize(names.size());
  std::string encoded_rows = "[";
  std::size_t retained = 0;
  for (std::size_t i = 0; i < rows; ++i) {
    std::vector<const Value *> values(names.size(), nullptr);
    if (records) {
      const auto &entries = (*items)[i].as_map()->entries;
      for (std::size_t j = 0; j < std::min(entries.size(), max_columns); ++j) {
        const auto key = key_text(entries[j].key);
        const auto column = column_for(key);
        if (column != keys.end()) values[column - keys.begin()] = &entries[j].value;
      }
    } else if (vectors) {
      const auto row = sequence((*items)[i]);
      for (std::size_t j = 0; j < std::min(row->size(), names.size()); ++j) values[j] = &(*row)[j];
    } else values[0] = &(*items)[i];
    std::string row = "{\"id\":" + quote(std::to_string(i)) + ",\"values\":[";
    for (std::size_t j = 0; j < values.size(); ++j) {
      if (j) row += ',';
      const auto v = values[j];
      std::string type;
      if (!v || v->is_null()) row += "null";
      else if (v->is_integer()) { row += std::to_string(v->as_integer()); type = "integer"; }
      else if (v->is_float() && std::isfinite(v->as_float())) { char buffer[64];
        const auto encoded = std::to_chars(buffer, buffer + sizeof(buffer), v->as_float(), std::chars_format::general, std::numeric_limits<double>::max_digits10);
        row.append(buffer, encoded.ptr); type = "number"; }
      else if (v->is_bool()) { row += v->as_bool() ? "true" : "false"; type = "boolean"; }
      else { row += quote(text(*v)); type = "text"; }
      if (!type.empty()) {
        if (types[j].empty()) types[j] = type;
        else if (types[j] != type) types[j] = (types[j] == "integer" && type == "number") || (types[j] == "number" && type == "integer") ? "number" : "mixed";
      }
    }
    row += "]}";
    if (encoded_rows.size() + row.size() > max_bytes - 65536) { truncated = true; break; }
    if (retained++) encoded_rows += ',';
    encoded_rows += row;
  }
  std::string result = "{\"columns\":[";
  for (std::size_t j = 0; j < names.size(); ++j) {
    if (j) result += ',';
    result += "{\"id\":" + quote(std::to_string(j)) + ",\"title\":" + quote(names[j]) + ",\"type\":" + quote(types[j].empty() ? "null" : types[j]) + '}';
  }
  return result + "],\"rows\":" + encoded_rows + "],\"total_rows\":" + std::to_string(items->size()) + ",\"truncated\":" + (truncated ? "true}" : "false}");
}

} // namespace sputnik::runtime
