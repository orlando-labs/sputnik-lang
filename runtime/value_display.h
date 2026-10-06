#pragma once

#include "runtime/value.h"

#include <cstddef>
#include <string>
#include <vector>

namespace sputnik::bytecode {
struct BcModule;
}

namespace sputnik::runtime {

enum class RuntimeStringifyMode { Display, Inspect, Pretty };

struct RuntimePrettyPrintOptions {
  std::size_t max_width = 80;
  std::size_t max_depth = 20;
  std::size_t max_items = 100;
};

// Host-facing snapshots: budgets apply while walking/escaping the value, not
// after constructing an unbounded inspection string. Never invokes Sputnik code.
struct RuntimeValuePreviewOptions {
  std::size_t max_bytes = 64U * 1024U;
  std::size_t max_nodes = 4096;
  std::size_t max_depth = 16;
};
struct RuntimeValuePreview {
  std::string text;
  bool truncated = false;
};
RuntimeValuePreview runtime_preview_value(
    const Value &value, RuntimeStringifyMode mode,
    const bytecode::BcModule *module = nullptr,
    const std::vector<std::string> *runtime_strings = nullptr,
    const std::vector<std::string> *runtime_symbols = nullptr,
    RuntimeValuePreviewOptions options = {});

std::string runtime_uuid_to_string(const RuntimeUuidValue &value);
std::string runtime_time_to_iso8601(const RuntimeTimeValue &value);
std::string runtime_time_zone_to_string(const RuntimeTimeZoneValue &value);
std::string runtime_time_period_to_string(const RuntimeTimePeriodValue &value);

std::string runtime_stringify_value(
    const Value &value, RuntimeStringifyMode mode,
    const bytecode::BcModule *module = nullptr,
    const std::vector<std::string> *runtime_strings = nullptr,
    const std::vector<std::string> *runtime_symbols = nullptr,
    RuntimePrettyPrintOptions options = {});

std::string value_to_debug_string(
    const Value &value, const bytecode::BcModule *module = nullptr,
    const std::vector<std::string> *runtime_strings = nullptr,
    const std::vector<std::string> *runtime_symbols = nullptr);

} // namespace sputnik::runtime
