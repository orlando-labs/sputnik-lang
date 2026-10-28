#include "runtime/stdlib_regexp.h"
#include "runtime/errors.h"
#include "runtime/objects.h"
#include <limits>

namespace sputnik::runtime {
namespace {
using regexp::Error;
// The legacy host keyword guard sets a terminal VM fault. Domain handlers
// raise through their normal facade so malformed calls can be rescued too.
bool keywords(NativeStdlibCall &c, std::initializer_list<const char *> allowed) {
  std::size_t recognized = 0;
  for (const auto *name : allowed)
    if (c.keyword(name)) ++recognized;
  if (recognized != c.kw_args.size())
    throw Error("TypeError", "unknown or duplicate regexp keyword argument");
  return true;
}
std::string text(NativeStdlibCall &c, const Value &v) {
  if (!v.is_string())
    throw Error("TypeError", "regexp text must be Str");
  const auto s = c.text_of(v);
  if (!s)
    throw Error("TypeError", "invalid regexp text");
  return *s;
}
std::string key_text(NativeStdlibCall &c, const Value &v) {
  const auto s = c.text_of(v);
  if (!s)
    throw Error("TypeError", "expected Str or Symbol");
  return *s;
}
bool boolean(const Value &v) {
  if (!v.is_bool())
    throw Error("TypeError", "regexp option must be Bool");
  return v.as_bool();
}
std::size_t nonnegative(const Value &v, const std::string &name) {
  if (!v.is_integer())
    throw Error("TypeError", name + " must be Int");
  if (v.as_integer() < 0)
    throw Error("ArgumentError", name + " must be non-negative");
  return static_cast<std::size_t>(v.as_integer());
}
std::shared_ptr<RuntimeRegexpPatternValue> pattern(const Value &v) {
  if (!v.is_regexp_pattern() || !v.as_regexp_pattern())
    throw Error("TypeError", "expected Regexp.Pattern");
  return v.as_regexp_pattern();
}
std::shared_ptr<RuntimeRegexpMatchValue>
make_match(std::shared_ptr<RuntimeRegexpPatternValue> p, const std::string &s,
           std::vector<regexp::Capture> captures) {
  auto m = std::make_shared<RuntimeRegexpMatchValue>();
  m->source = s;
  m->pattern = std::move(p);
  m->captures = std::move(captures);
  return m;
}
Value match_value(std::shared_ptr<RuntimeRegexpPatternValue> p,
                  const std::string &s,
                  std::optional<std::vector<regexp::Capture>> captures) {
  return captures ? Value::regexp_match(
                        make_match(std::move(p), s, std::move(*captures)))
                  : Value::null();
}
std::size_t group_index(NativeStdlibCall &c, const RuntimeRegexpMatchValue &m,
                        const Value &v) {
  if (v.is_integer() && v.as_integer() < 0)
    throw Error("RegexpGroupError", "regexp group index out of range");
  const auto i = v.is_integer() ? nonnegative(v, "group")
                                : regexp::group(*m.pattern, key_text(c, v));
  if (i >= m.captures.size())
    throw Error("RegexpGroupError", "regexp group index out of range");
  return i;
}
Value group_value(NativeStdlibCall &c, const RuntimeRegexpMatchValue &m,
                  std::size_t i) {
  const auto &r = m.captures.at(i);
  return r.matched ? c.string_value(m.source.substr(r.start, r.end - r.start))
                   : Value::null();
}
std::uint32_t compile_flags(NativeStdlibCall &c) {
  std::uint32_t bits = 0;
  if (auto v = c.keyword("flags"))
    bits = regexp::flags(key_text(c, *v));
  for (const auto &entry :
       std::vector<std::pair<std::string, char>>{{"ignore_case", 'i'},
                                                 {"multiline", 'm'},
                                                 {"dot_all", 's'},
                                                 {"extended", 'x'},
                                                 {"ungreedy", 'U'}})
    if (auto v = c.keyword(entry.first)) {
      if (boolean(*v))
        bits |= regexp::flag(entry.second);
      else
        bits &= ~regexp::flag(entry.second);
    }
  return bits;
}
SendStatus compile(NativeStdlibCall &c) {
  if (!c.require_arity(1) || !c.require_no_block() ||
      !keywords(c, {"flags", "ignore_case", "multiline",
                                  "dot_all", "extended", "ungreedy", "literal",
                                  "match_limit"}))
    return SendStatus::Faulted;
  auto source = text(c, c.args[0]);
  const auto bits = compile_flags(c);
  if (auto v = c.keyword("literal"); v && boolean(*v))
    source = regexp::escape(source);
  std::size_t limit = 1000000;
  if (auto v = c.keyword("match_limit"); v && !v->is_null())
    limit = nonnegative(*v, "match_limit");
  if (limit > UINT32_MAX)
    throw Error("ArgumentError", "match_limit exceeds uint32");
  auto p = std::make_shared<RuntimeRegexpPatternValue>();
  static_cast<regexp::Pattern &>(*p) =
      regexp::compile(source, bits, static_cast<std::uint32_t>(limit));
  if (c.selector == "fragment") {
    regexp::validate_fragment(*p);
    p->fragment = true;
  }
  *c.out = Value::regexp_pattern(std::move(p));
  return SendStatus::Matched;
}
SendStatus compile_template(NativeStdlibCall &c) {
  if (!c.require_arity(3) || !c.require_no_block() ||
      !keywords(c, {}))
    return SendStatus::Faulted;
  if (!c.args[0].is_list() || !c.args[1].is_list())
    throw Error("TypeError", "regexp template expects chunk and value lists");
  const auto &values = c.args[1].as_list()->items;
  std::vector<std::string> chunks;
  for (const auto &v : c.args[0].as_list()->items)
    chunks.push_back(text(c, v));
  if (chunks.size() != values.size() + 1)
    throw Error("ArgumentError", "invalid regexp template shape");
  const auto bits = regexp::flags(text(c, c.args[2]));
  if (!values.empty())
    regexp::validate_holes(chunks, bits);
  std::string source = chunks[0];
  for (std::size_t i = 0; i < values.size(); ++i) {
    std::string part;
    if (values[i].is_regexp_pattern()) {
      auto p = pattern(values[i]);
      if (!p->fragment)
        throw Error("TypeError",
                    "use Regexp.fragment for regexp syntax interpolation");
      part = regexp::fragment_source(*p);
    } else
      part = regexp::escape(text(c, values[i]));
    source += "(?:" + part + ")" + chunks[i + 1];
  }
  auto p = std::make_shared<RuntimeRegexpPatternValue>();
  static_cast<regexp::Pattern &>(*p) =
      values.empty() ? regexp::compile_literal(source, bits)
                     : regexp::compile(source, bits);
  *c.out = Value::regexp_pattern(std::move(p));
  return SendStatus::Matched;
}
SendStatus search(NativeStdlibCall &c,
                  const std::shared_ptr<RuntimeRegexpPatternValue> &p,
                  const std::string &s) {
  if (!c.require_no_block() || !keywords(c, {"from"}))
    return SendStatus::Faulted;
  std::size_t offset = 0;
  if (auto v = c.keyword("from"))
    offset = regexp::byte_offset(
        s, static_cast<std::int64_t>(nonnegative(*v, "from")));
  const bool full = c.selector == "full_match" || c.selector == "full_match?";
  if (full && offset)
    throw Error("ArgumentError", "full_match does not accept nonzero from");
  auto captures = regexp::search(*p, s, offset, full);
  if (c.selector == "match?" || c.selector == "matches?" ||
      c.selector == "full_match?" || c.selector == "~=")
    *c.out = Value::boolean(bool(captures));
  else
    *c.out = match_value(p, s, std::move(captures));
  return SendStatus::Matched;
}
SendStatus replace(NativeStdlibCall &c,
                   std::shared_ptr<RuntimeRegexpPatternValue> p,
                   const std::string &s, std::size_t replacement_index) {
  if (!(c.selector == "replace_first" ? keywords(c, {})
                                      : keywords(c, {"count"})))
    return SendStatus::Faulted;
  std::size_t limit = c.selector == "replace_first" ? 1 : SIZE_MAX;
  if (auto v = c.keyword("count")) {
    if (v->is_integer())
      limit = nonnegative(*v, "count");
    else if (key_text(c, *v) != "all")
      throw Error("ArgumentError", "count must be Int or :all");
  }
  std::optional<std::vector<regexp::ReplacementPart>> parts;
  if (c.block.is_null()) {
    if (c.args.size() != replacement_index + 1)
      throw Error("ArgumentError", "replacement or block required");
    parts = regexp::replacement(*p, text(c, c.args[replacement_index]));
  } else {
    if (c.args.size() != replacement_index)
      throw Error("ArgumentError",
                  "replacement and block are mutually exclusive");
    if (c.block_suspension_in_property_arm("regexp replacement"))
      return SendStatus::Faulted;
  }
  std::string out;
  std::size_t last = 0, count = 0;
  regexp::Cursor cursor;
  while (count < limit) {
    auto ranges = cursor.next(*p, s);
    if (!ranges)
      break;
    const auto &r = (*ranges)[0];
    out.append(s, last, r.start - last);
    if (parts)
      out += regexp::substitute(*parts, s, *ranges);
    else {
      auto b = c.call_block(c.block,
                            {Value::regexp_match(make_match(p, s, *ranges))});
      if (b.status == StdlibBlockStatus::Raised)
        return c.raise(b.exception);
      if (b.status != StdlibBlockStatus::Returned)
        return SendStatus::Faulted;
      out += text(c, b.value);
    }
    last = r.end;
    ++count;
  }
  out.append(s, last, std::string::npos);
  *c.out = c.string_value(std::move(out));
  return SendStatus::Matched;
}
SendStatus enumerate(NativeStdlibCall &c,
                     std::shared_ptr<RuntimeRegexpPatternValue> p,
                     const std::string &s) {
  if (!keywords(c,
          c.selector == "split"
              ? std::initializer_list<const char *>{"limit", "keep_empty",
                                                    "include_captures"}
              : std::initializer_list<const char *>{}))
    return SendStatus::Faulted;
  const bool split = c.selector == "split", each = c.selector == "each_match";
  if (each && c.block.is_null())
    throw Error("ArgumentError", "each_match requires a block");
  if (!each && !c.require_no_block())
    return SendStatus::Faulted;
  bool keep = false, captures = false;
  std::size_t limit = SIZE_MAX;
  if (auto v = c.keyword("keep_empty"))
    keep = boolean(*v);
  if (auto v = c.keyword("include_captures"))
    captures = boolean(*v);
  if (auto v = c.keyword("limit"); v && !v->is_null()) {
    limit = nonnegative(*v, "limit");
    if (!limit)
      throw Error("ArgumentError", "split limit must be positive or null");
  }
  if (split) {
    std::vector<Value> out;
    for (const auto &v : regexp::split(*p, s, limit, keep, captures))
      out.push_back(v ? c.string_value(*v) : Value::null());
    *c.out = c.make_list(std::move(out));
    return SendStatus::Matched;
  }
  std::vector<Value> out;
  std::size_t count = 0;
  regexp::Cursor cursor;
  while (auto ranges = cursor.next(*p, s)) {
    auto m = make_match(p, s, *ranges);
    if (each) {
      auto b = c.call_block(c.block, {Value::regexp_match(m)});
      if (b.status == StdlibBlockStatus::Raised)
        return c.raise(b.exception);
      if (b.status != StdlibBlockStatus::Returned)
        return SendStatus::Faulted;
      ++count;
    } else
      out.push_back(Value::regexp_match(m));
  }
  *c.out = each ? Value::integer(static_cast<std::int64_t>(count))
                : c.make_list(std::move(out));
  return SendStatus::Matched;
}
SendStatus pattern_dispatch(NativeStdlibCall &c) {
  auto p = pattern(c.receiver);
  const auto &sel = c.selector;
  if (c.args.empty() && c.block.is_null() && c.kw_args.empty()) {
    if (sel == "source" || sel == "to_str" || sel == "inspect")
      *c.out = c.string_value(
          sel == "source" ? p->source : runtime_regexp_pattern_to_string(*p));
    else if (sel == "group_count")
      *c.out = Value::integer(p->group_count);
    else if (sel == "flags")
      *c.out = c.string_value(regexp::flag_text(p->flags));
    else if (sel == "names") {
      std::vector<Value> names;
      for (const auto &n : p->names)
        names.push_back(c.string_value(n.first));
      *c.out = c.make_list(std::move(names));
    } else
      return SendStatus::NotHandled;
    return SendStatus::Matched;
  }
  if (sel != "replace" && sel != "replaced" && sel != "replace_first" &&
      sel != "scan" && sel != "each_match" && sel != "split" &&
      sel != "match" && sel != "find" && sel != "match?" && sel != "matches?" &&
      sel != "full_match" && sel != "full_match?" &&
      sel != "~=")
    return SendStatus::NotHandled;
  if (c.args.empty())
    throw Error("ArgumentError", "regexp operation requires a source string");
  const auto s = text(c, c.args[0]);
  if (sel == "replace" || sel == "replaced" || sel == "replace_first")
    return replace(c, p, s, 1);
  if (!c.require_arity(1))
    return SendStatus::Faulted;
  if (sel == "scan" || sel == "each_match" || sel == "split")
    return enumerate(c, p, s);
  if (sel == "match" || sel == "find" || sel == "match?" || sel == "matches?" ||
      sel == "full_match" || sel == "full_match?" ||
      sel == "~=")
    return search(c, p, s);
  return SendStatus::NotHandled;
}
SendStatus match_dispatch(NativeStdlibCall &c) {
  auto m = c.receiver.as_regexp_match();
  const auto &sel = c.selector;
  if (sel == "==" || sel == "!=" || sel == "===")
    return SendStatus::NotHandled;
  if (!c.require_no_block() || !keywords(c, {}))
    return SendStatus::Faulted;
  if (sel == "[]" || sel == "group") {
    if (!c.require_arity(1))
      return SendStatus::Faulted;
    *c.out = group_value(c, *m, group_index(c, *m, c.args[0]));
    return SendStatus::Matched;
  }
  if (sel == "range" || sel == "byte_range") {
    if (c.args.size() > 1)
      throw Error("ArgumentError", "range expects at most one group");
    const auto i = c.args.empty() ? 0 : group_index(c, *m, c.args[0]);
    const auto &r = m->captures[i];
    if (!r.matched) {
      *c.out = Value::null();
      return SendStatus::Matched;
    }
    auto range = make_intrusive<InstanceValue>();
    range->class_index = kNativeSyntheticClassIndex;
    range->ivars[kNativeRangeMarker] = Value::boolean(true);
    range->ivars["start"] = Value::integer(
        sel == "range" ? regexp::cp_offset(m->source, r.start) : r.start);
    range->ivars["finish"] = Value::integer(
        sel == "range" ? regexp::cp_offset(m->source, r.end) : r.end);
    range->ivars["inclusive_end"] = Value::boolean(false);
    range->ivars["step"] = Value::null();
    *c.out = Value::instance(std::move(range));
    return SendStatus::Matched;
  }
  if (sel == "begin" || sel == "end" || sel == "start" || sel == "finish" ||
      sel == "byte_begin" || sel == "byte_end") {
    if (c.args.size() > 1)
      throw Error("ArgumentError", "offset expects at most one group");
    const auto i = c.args.empty() ? 0 : group_index(c, *m, c.args[0]);
    const auto &r = m->captures[i];
    const bool begin = sel == "begin" || sel == "start" || sel == "byte_begin";
    const auto b = begin ? r.start : r.end;
    *c.out =
        r.matched
            ? Value::integer(static_cast<std::int64_t>(
                  sel.rfind("byte_", 0) == 0 ? b
                                             : regexp::cp_offset(m->source, b)))
            : Value::null();
    return SendStatus::Matched;
  }
  if (!c.require_arity(0))
    return SendStatus::Faulted;
  if (sel == "text" || sel == "to_str" || sel == "inspect")
    *c.out = group_value(c, *m, 0);
  else if (sel == "source")
    *c.out = c.string_value(m->source);
  else if (sel == "pattern")
    *c.out = Value::regexp_pattern(m->pattern);
  else if (sel == "count" || sel == "size" || sel == "length")
    *c.out = Value::integer(m->captures.size() - 1);
  else if (sel == "pre_match")
    *c.out = c.string_value(m->source.substr(0, m->captures[0].start));
  else if (sel == "post_match")
    *c.out = c.string_value(m->source.substr(m->captures[0].end));
  else if (sel == "captures") {
    std::vector<Value> values;
    for (std::size_t i = 1; i < m->captures.size(); ++i)
      values.push_back(group_value(c, *m, i));
    *c.out = c.make_list(std::move(values));
  } else if (sel == "named_captures") {
    std::vector<std::pair<std::string, Value>> values;
    for (const auto &n : m->pattern->names)
      values.emplace_back(n.first, group_value(c, *m, n.second));
    *c.out = c.make_object(std::move(values));
  } else
    return SendStatus::NotHandled;
  return SendStatus::Matched;
}
SendStatus dispatch(NativeStdlibCall &c) {
  try {
    if (c.receiver.is_regexp_pattern())
      return pattern_dispatch(c);
    if (c.receiver.is_regexp_match())
      return match_dispatch(c);
    if (c.selector == "default_match_limit") {
      if (!c.require_arity(0))
        return SendStatus::Faulted;
      *c.out = Value::integer(1000000);
      return SendStatus::Matched;
    }
    if (c.selector == "version") {
      if (!c.require_arity(0))
        return SendStatus::Faulted;
      *c.out = c.make_object(
          {{"engine", c.string_value("PCRE2")},
           {"version", c.string_value(std::to_string(PCRE2_MAJOR) + "." +
                                      std::to_string(PCRE2_MINOR))},
           {"jit", Value::boolean(false)}});
      return SendStatus::Matched;
    }
    if (c.selector == "try_compile") {
      try {
        auto status = compile(c);
        if (status != SendStatus::Matched)
          return status;
        *c.out = make_result_value(true, *c.out);
      } catch (const Error &e) {
        if (e.kind != "RegexpCompileError")
          throw;
        auto error = std::make_shared<ErrorInstanceValue>();
        error->error_id = *runtime_error_id(e.kind);
        error->message = e.what();
        *c.out =
            make_result_value(false, Value::error_instance(std::move(error)));
      }
      return SendStatus::Matched;
    }
    if (c.selector == "compile" || c.selector == "new" || c.selector == "r" ||
        c.selector == "__call__" || c.selector == "fragment")
      return compile(c);
    if (c.selector == "__compile_template")
      return compile_template(c);
    if (c.selector == "valid?") {
      try {
        auto status = compile(c);
        if (status != SendStatus::Matched)
          return status;
        *c.out = Value::boolean(true);
      } catch (const Error &e) {
        if (e.kind != "RegexpCompileError")
          throw;
        *c.out = Value::boolean(false);
      }
      return SendStatus::Matched;
    }
    if (c.selector == "escape") {
      if (!c.require_arity(1) || !c.require_no_block() ||
          !keywords(c, {}))
        return SendStatus::Faulted;
      *c.out = c.string_value(regexp::escape(text(c, c.args[0])));
      return SendStatus::Matched;
    }
    return SendStatus::NotHandled;
  } catch (const Error &e) {
    return c.fault(e.kind, e.what());
  }
}
} // namespace
std::string
runtime_regexp_pattern_to_string(const RuntimeRegexpPatternValue &p) {
  return "/" + p.source + "/" + regexp::flag_text(p.flags);
}
std::string runtime_regexp_match_to_string(const RuntimeRegexpMatchValue &m) {
  const auto &r = m.captures[0];
  return m.source.substr(r.start, r.end - r.start);
}
SendStatus regexp_string_match_operator(NativeStdlibCall &c,
                                        const std::string &s) {
  try {
    if (c.selector != "~=")
      return SendStatus::NotHandled;
    if (!c.require_arity(1))
      return SendStatus::Faulted;
    return search(c, pattern(c.args[0]), s);
  } catch (const Error &e) {
    return c.fault(e.kind, e.what());
  }
}
SendStatus regexp_string_replace(NativeStdlibCall &c, const std::string &s) {
  try {
    if (c.args.empty())
      return SendStatus::NotHandled;
    return replace(c, pattern(c.args[0]), s, 1);
  } catch (const Error &e) {
    return c.fault(e.kind, e.what());
  }
}
RuntimeNativeModuleDescriptor regexp_module_descriptor() {
  return {{{"Regexp", RuntimeNativeTypeKind::Regexp}},
          {{RuntimeNativeTypeKind::Regexp, &dispatch}},
          {},
          {{RuntimeNativeTypeKind::Regexp, "__call__"}},
          {{"RegexpError", "Exception"},
           {"RegexpCompileError", "RegexpError"},
           {"RegexpGroupError", "RegexpError"},
           {"RegexpMatchLimitError", "RegexpError"},
           {"RegexpReplacementError", "RegexpError"}}};
}
void register_regexp(NativeRegistry &r) {
  register_native_module_descriptor(r, regexp_module_descriptor());
}
void register_regexp_runtime_module(RuntimeModuleRegistry &m,
                                    RuntimeDispatchRegistry &d,
                                    RuntimeTypeRegistry &t,
                                    RuntimeErrorRegistry *e) {
  auto desc = regexp_module_descriptor();
  register_runtime_module_descriptor(m, desc);
  register_runtime_dispatch_descriptor(d, desc);
  register_runtime_type_descriptor(t, desc);
  if (e)
    register_runtime_error_descriptor(*e, desc);
}
} // namespace sputnik::runtime
