#pragma once
#include "runtime/notebook_table.h"

inline std::string notebook_result_table(const sputnik::runtime::Value &value,
    const sputnik::bytecode::BcModule *module,
    const std::vector<std::string> *strings, const std::vector<std::string> *symbols) {
  return sputnik::runtime::notebook_table_snapshot(value, module, strings, symbols);
}
