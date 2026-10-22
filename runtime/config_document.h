#pragma once
#include <string>

namespace sputnik::runtime {
// Pure config codecs shared by notebook validation and runtime readers.
// Sources are UTF-8, at most 64 KiB; expanded JSON is bounded to 512 KiB,
// 16K nodes and depth 64. No filesystem, VM, evaluation or custom constructors.
std::string config_to_json(const std::string &source, const std::string &format);
std::string config_from_json(const std::string &json, const std::string &format);
} // namespace sputnik::runtime
