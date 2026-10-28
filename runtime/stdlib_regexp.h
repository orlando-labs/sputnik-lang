#pragma once

#include "runtime/stdlib_registry.h"

#include "runtime/regexp_engine.h"
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace sputnik::runtime {

using RuntimeRegexpCaptureRange = regexp::Capture;
struct RuntimeRegexpPatternValue : regexp::Pattern {};

struct RuntimeRegexpMatchValue {
  std::string source;
  std::shared_ptr<RuntimeRegexpPatternValue> pattern;
  std::vector<RuntimeRegexpCaptureRange> captures;
};

std::string
runtime_regexp_pattern_to_string(const RuntimeRegexpPatternValue &pattern);
std::string
runtime_regexp_match_to_string(const RuntimeRegexpMatchValue &match);

// String-owned overloads (`Str#replace`, `Str#replaced`, `~=`) enter the
// regexp module from the VM's scalar string dispatch.
SendStatus regexp_string_replace(NativeStdlibCall &call,
                                 const std::string &self);
SendStatus regexp_string_match_operator(NativeStdlibCall &call,
                                        const std::string &self);

} // namespace sputnik::runtime
