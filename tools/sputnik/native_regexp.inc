R"SPUTNIKPP(
static NativeValue sputnik_native_call_closure(const NativeValue &,
                                               const NativeArgsView &);
static NativeValue sputnik_native_call_scoped_closure(const NativeValue &,
                                                      const NativeArgsView &);
using RegexpError = sputnik::regexp::Error;
static std::string native_regexp_text_arg(const NativeValue &v) {
  if (native_value_is_string(v))
    return native_string_text(v);
  throw RegexpError("TypeError", "regexp text must be Str");
}
static std::string native_regexp_key(const NativeValue &v) {
  if (v.tag == NativeValue::Tag::Symbol)
    return native_symbol_text(v.scalar_value);
  return native_regexp_text_arg(v);
}
static std::size_t native_regexp_count(const NativeValue &v,
                                       const std::string &name) {
  if (v.tag != NativeValue::Tag::Integer)
    throw RegexpError("TypeError", name + " must be Int");
  if (v.scalar_value < 0)
    throw RegexpError(name == "group" ? "RegexpGroupError" : "ArgumentError",
                      name + " must be non-negative");
  return static_cast<std::size_t>(v.scalar_value);
}
static bool native_regexp_bool(const NativeValue &v) {
  if (v.tag != NativeValue::Tag::Bool)
    throw RegexpError("TypeError", "regexp option must be Bool");
  return v.scalar_value != 0;
}
static const NativeValue *
native_regexp_kw(std::initializer_list<std::pair<std::string, NativeValue>> kw,
                 const std::string &name) {
  for (const auto &e : kw)
    if (e.first == name)
      return &e.second;
  return nullptr;
}
static void native_regexp_reject_unknown_kw(
    std::initializer_list<std::pair<std::string, NativeValue>> kw,
    std::initializer_list<const char *> allowed) {
  for (const auto &e : kw)
    if (std::none_of(allowed.begin(), allowed.end(),
                     [&](const char *s) { return e.first == s; }))
      throw RegexpError("TypeError", "unknown regexp keyword: " + e.first);
}
template <class F> static NativeValue native_regexp_guard(F f) {
  try {
    return f();
  } catch (const RegexpError &e) {
    throw NativeRaised{native_named_error(e.kind, e.what())};
  }
}
static std::string native_regexp_pattern_to_string(const NativeRegexp &p) {
  return "/" + p.source + "/" + sputnik::regexp::flag_text(p.flags);
}
static std::optional<std::string>
native_regexp_group_text(const NativeRegexpMatch &m, std::size_t i) {
  if (i >= m.captures.size())
    throw RegexpError("RegexpGroupError", "regexp group index out of range");
  const auto &r = m.captures[i];
  return r.matched ? std::optional<std::string>(
                         m.source.substr(r.start, r.end - r.start))
                   : std::nullopt;
}
static std::string native_regexp_match_to_string(const NativeRegexpMatch &m) {
  return native_regexp_group_text(m, 0).value_or("");
}
static NativeValue
native_regexp_match_value(const NativeValue &p, const std::string &s,
                          std::vector<sputnik::regexp::Capture> ranges) {
  NativeRegexpMatch m;
  m.source = s;
  m.pattern = p;
  m.captures = std::move(ranges);
  return NativeValue::regexp_match(std::move(m));
}
static NativeValue native_regexp_group_value(const NativeRegexpMatch &m,
                                             const NativeValue &v) {
  return native_regexp_guard([&] {
    const auto i = v.tag == NativeValue::Tag::Integer
                       ? native_regexp_count(v, "group")
                       : sputnik::regexp::group(as_regexp(m.pattern),
                                                native_regexp_key(v));
    auto s = native_regexp_group_text(m, i);
    return s ? NativeValue::heap_string(*s) : NativeValue::nullv();
  });
}
static NativeValue native_regexp_search_value(const NativeValue &p,
                                              const NativeValue &s, bool full,
                                              bool boolean,
                                              std::size_t offset = 0) {
  auto result = sputnik::regexp::search(as_regexp(p), native_regexp_text_arg(s),
                                        offset, full);
  if (boolean)
    return NativeValue::boolean(bool(result));
  return result ? native_regexp_match_value(p, native_regexp_text_arg(s),
                                            std::move(*result))
                : NativeValue::nullv();
}
static NativeValue native_regexp_match_operator(const NativeValue &lhs,
                                                const NativeValue &rhs) {
  return native_regexp_guard([&] {
    if (lhs.tag == NativeValue::Tag::Regexp && native_value_is_string(rhs))
      return native_regexp_search_value(lhs, rhs, false, true);
    if (rhs.tag == NativeValue::Tag::Regexp && native_value_is_string(lhs))
      return native_regexp_search_value(rhs, lhs, false, true);
    throw RegexpError("TypeError",
                      "match operator expects one Str and one Regexp.Pattern");
  });
}
static NativeValue native_regexp_replace_string(
    const NativeValue &value, const NativeValue &pv,
    const NativeValue &replacement, bool has_replacement,
    const NativeValue &block, bool has_block,
    std::initializer_list<std::pair<std::string, NativeValue>> kw,
    bool first = false) {
  return native_regexp_guard([&] {
    if (first) native_regexp_reject_unknown_kw(kw, {});
    else native_regexp_reject_unknown_kw(kw, {"count"});
    const auto &p = as_regexp(pv);
    const auto s = native_regexp_text_arg(value);
    std::size_t limit = first ? 1 : SIZE_MAX;
    if (auto v = native_regexp_kw(kw, "count")) {
      if (v->tag == NativeValue::Tag::Integer)
        limit = native_regexp_count(*v, "count");
      else if (native_regexp_key(*v) != "all")
        throw RegexpError("ArgumentError", "invalid count");
    }
    std::optional<std::vector<sputnik::regexp::ReplacementPart>> parts;
    if (has_block) {
      if (has_replacement)
        throw RegexpError("ArgumentError",
                          "replacement and block are mutually exclusive");
    } else {
      if (!has_replacement)
        throw RegexpError("ArgumentError", "replacement or block required");
      parts =
          sputnik::regexp::replacement(p, native_regexp_text_arg(replacement));
    }
    std::string out;
    std::size_t last = 0, count = 0;
    sputnik::regexp::Cursor cursor;
    while (count < limit) {
      auto ranges = cursor.next(p, s);
      if (!ranges)
        break;
      const auto &r = (*ranges)[0];
      out.append(s, last, r.start - last);
      if (parts)
        out += sputnik::regexp::substitute(*parts, s, *ranges);
      else
        out += native_regexp_text_arg(sputnik_native_call_scoped_closure(
            block, {native_regexp_match_value(pv, s, *ranges)}));
      last = r.end;
      ++count;
    }
    out.append(s, last, std::string::npos);
    return NativeValue::heap_string(std::move(out));
  });
}
static NativeValue native_regexp_each(const NativeValue &pv,
                                      const NativeValue &subject,
                                      const NativeValue &block) {
  return native_regexp_guard([&] {
    const auto &p = as_regexp(pv);
    const auto s = native_regexp_text_arg(subject);
    sputnik::regexp::Cursor cursor;
    std::int64_t count = 0;
    while (auto ranges = cursor.next(p, s)) {
      sputnik_native_call_scoped_closure(
          block, {native_regexp_match_value(pv, s, *ranges)});
      ++count;
    }
    return NativeValue::integer(count);
  });
}
static NativeValue native_regexp_send(
    const NativeValue &self, std::string_view sel,
    std::initializer_list<NativeValue> args,
    std::initializer_list<std::pair<std::string, NativeValue>> kw) {
  return native_regexp_guard([&]() -> NativeValue {
    const auto arity = [&](std::size_t n) {
      if (args.size() != n)
        throw RegexpError("TypeError", "wrong regexp arity");
    };
    const auto nullkw = [&] { native_regexp_reject_unknown_kw(kw, {}); };
    if (self.tag == NativeValue::Tag::RegexpModule) {
      if (sel == "__compile_template") {
        arity(3);
        nullkw();
        auto it = args.begin();
        const auto &chunks = as_list(*it++).items;
        const auto &values = as_list(*it++).items;
        const auto flags = sputnik::regexp::flags(native_regexp_text_arg(*it));
        std::vector<std::string> texts;
        for (const auto &v : chunks)
          texts.push_back(native_regexp_text_arg(v));
        if (texts.size() != values.size() + 1)
          throw RegexpError("ArgumentError", "invalid regexp template shape");
        if(!values.empty()) sputnik::regexp::validate_holes(texts, flags);
        std::string source = texts[0];
        for (std::size_t i = 0; i < values.size(); ++i) {
          std::string part;
          if (values[i].tag == NativeValue::Tag::Regexp) {
            const auto &p = as_regexp(values[i]);
            if (!p.fragment)
              throw RegexpError("TypeError", "use Regexp.fragment");
            part = sputnik::regexp::fragment_source(p);
          } else
            part = sputnik::regexp::escape(native_regexp_text_arg(values[i]));
          source += "(?:" + part + ")" + texts[i + 1];
        }
        NativeRegexp p;
        static_cast<sputnik::regexp::Pattern &>(p) =
            values.empty() ? sputnik::regexp::compile_literal(source, flags)
                           : sputnik::regexp::compile(source, flags);
        return NativeValue::regexp(std::move(p));
      }
      if (sel == "default_match_limit") {
        arity(0);
        nullkw();
        return NativeValue::integer(1000000);
      }
      if (sel == "version") {
        arity(0);
        nullkw();
        return NativeValue::map(
            {{"engine", NativeValue::heap_string("PCRE2")},
             {"version",
              NativeValue::heap_string(std::to_string(PCRE2_MAJOR) + "." +
                                       std::to_string(PCRE2_MINOR))},
             {"jit", NativeValue::boolean(false)}});
      }
      arity(1);
      if (sel == "escape") {
        nullkw();
        return NativeValue::heap_string(
            sputnik::regexp::escape(native_regexp_text_arg(*args.begin())));
      }
      if (sel != "compile" && sel != "new" && sel != "r" && sel != "__call__" &&
          sel != "fragment" && sel != "valid?" && sel != "try_compile")
        throw RegexpError("NoMethodError", "unknown Regexp method");
      native_regexp_reject_unknown_kw(kw, {"flags", "ignore_case", "multiline",
                                           "dot_all", "extended", "ungreedy",
                                           "literal", "match_limit"});
      auto source = native_regexp_text_arg(*args.begin());
      std::uint32_t bits = 0;
      std::size_t limit = 1000000;
      if (auto v = native_regexp_kw(kw, "flags"))
        bits = sputnik::regexp::flags(native_regexp_key(*v));
      for (const auto &e :
           std::vector<std::pair<std::string, char>>{{"ignore_case", 'i'},
                                                     {"multiline", 'm'},
                                                     {"dot_all", 's'},
                                                     {"extended", 'x'},
                                                     {"ungreedy", 'U'}})
        if (auto v = native_regexp_kw(kw, e.first)) {
          if (native_regexp_bool(*v))
            bits |= sputnik::regexp::flag(e.second);
          else
            bits &= ~sputnik::regexp::flag(e.second);
        }
      if (auto v = native_regexp_kw(kw, "literal"); v && native_regexp_bool(*v))
        source = sputnik::regexp::escape(source);
      if (auto v = native_regexp_kw(kw, "match_limit");
          v && v->tag != NativeValue::Tag::Null)
        limit = native_regexp_count(*v, "match_limit");
      if (limit > UINT32_MAX)
        throw RegexpError("ArgumentError", "match_limit exceeds uint32");
      NativeRegexp p;
      try {
        static_cast<sputnik::regexp::Pattern &>(p) = sputnik::regexp::compile(
            source, bits, static_cast<std::uint32_t>(limit));
      } catch (const RegexpError &e) {
        if (e.kind == "RegexpCompileError") {
          if (sel == "valid?")
            return NativeValue::boolean(false);
          if (sel == "try_compile")
            return NativeValue::result(false,
                                       native_named_error(e.kind, e.what()));
        }
        throw;
      }
      if (sel == "valid?")
        return NativeValue::boolean(true);
      if (sel == "fragment") {
        sputnik::regexp::validate_fragment(p);
        p.fragment = true;
      }
      auto result = NativeValue::regexp(std::move(p));
      return sel == "try_compile" ? NativeValue::result(true, result) : result;
    }
    if (self.tag == NativeValue::Tag::Regexp) {
      const auto &p = as_regexp(self);
      if (args.size() == 0) {
        nullkw();
        if (sel == "source")
          return NativeValue::heap_string(p.source);
        if (sel == "to_str" || sel == "inspect")
          return NativeValue::heap_string(native_regexp_pattern_to_string(p));
        if (sel == "group_count")
          return NativeValue::integer(p.group_count);
        if (sel == "flags")
          return NativeValue::heap_string(sputnik::regexp::flag_text(p.flags));
        if (sel == "names") {
          std::vector<NativeValue> out;
          for (const auto &n : p.names)
            out.push_back(NativeValue::heap_string(n.first));
          return NativeValue::list(std::move(out));
        }
      }
      if (sel != "match" && sel != "find" && sel != "match?" && sel != "matches?" &&
          sel != "full_match" && sel != "full_match?" && sel != "~=" &&
          sel != "scan" && sel != "split")
        throw RegexpError("NoMethodError", "unknown Regexp.Pattern method");
      arity(1);
      const auto &subject = *args.begin();
      const auto s = native_regexp_text_arg(subject);
      if (sel == "scan" || sel == "split") {
        native_regexp_reject_unknown_kw(
            kw, sel == "split"
                    ? std::initializer_list<const char *>{"limit", "keep_empty",
                                                          "include_captures"}
                    : std::initializer_list<const char *>{});
        std::size_t limit = SIZE_MAX;
        bool keep = false, captures = false;
        if (auto v = native_regexp_kw(kw, "limit");
            v && v->tag != NativeValue::Tag::Null) {
          limit = native_regexp_count(*v, "limit");
          if (!limit)
            throw RegexpError("ArgumentError",
                              "split limit must be positive or null");
        }
        if (auto v = native_regexp_kw(kw, "keep_empty"))
          keep = native_regexp_bool(*v);
        if (auto v = native_regexp_kw(kw, "include_captures"))
          captures = native_regexp_bool(*v);
        std::vector<NativeValue> out;
        if (sel == "split") {
          for (const auto &v :
               sputnik::regexp::split(p, s, limit, keep, captures))
            out.push_back(v ? NativeValue::heap_string(*v)
                            : NativeValue::nullv());
        } else {
          sputnik::regexp::Cursor cursor;
          while (auto ranges = cursor.next(p, s))
            out.push_back(native_regexp_match_value(self, s, *ranges));
        }
        return NativeValue::list(std::move(out));
      }
      native_regexp_reject_unknown_kw(kw, {"from"});
      std::size_t offset = 0;
      if (auto v = native_regexp_kw(kw, "from"))
        offset =
            sputnik::regexp::byte_offset(s, native_regexp_count(*v, "from"));
      const bool full = sel == "full_match" || sel == "full_match?";
      if (full && offset)
        throw RegexpError("ArgumentError",
                          "full_match does not accept nonzero from");
      return native_regexp_search_value(self, subject, full,
                                        sel == "match?" || sel == "matches?" ||
                                            sel == "full_match?" ||
                                            sel == "~=", offset);
    }
    if (self.tag == NativeValue::Tag::RegexpMatch) {
      nullkw();
      const auto &m = as_regexp_match(self);
      if (sel == "[]" || sel == "group") {
        arity(1);
        return native_regexp_group_value(m, *args.begin());
      }
      if (sel == "range" || sel == "byte_range") {
        if (args.size() > 1)
          throw RegexpError("ArgumentError", "range expects at most one group");
        const auto i = args.size() == 0
                           ? 0
                           : (args.begin()->tag == NativeValue::Tag::Integer
                                  ? native_regexp_count(*args.begin(), "group")
                                  : sputnik::regexp::group(
                                        as_regexp(m.pattern),
                                        native_regexp_key(*args.begin())));
        if (i >= m.captures.size())
          throw RegexpError("RegexpGroupError", "group out of range");
        const auto &r = m.captures[i];
        if (!r.matched)
          return NativeValue::nullv();
        NativeRange range;
        range.start = sel == "range"
                          ? sputnik::regexp::cp_offset(m.source, r.start)
                          : r.start;
        range.finish = sel == "range"
                           ? sputnik::regexp::cp_offset(m.source, r.end)
                           : r.end;
        range.inclusive_end = false;
        return NativeValue::range(range);
      }
      if (sel == "begin" || sel == "end" || sel == "start" || sel == "finish" ||
          sel == "byte_begin" || sel == "byte_end") {
        if (args.size() > 1)
          throw RegexpError("ArgumentError",
                            "offset expects at most one group");
        const auto i = args.size() == 0
                           ? 0
                           : (args.begin()->tag == NativeValue::Tag::Integer
                                  ? native_regexp_count(*args.begin(), "group")
                                  : sputnik::regexp::group(
                                        as_regexp(m.pattern),
                                        native_regexp_key(*args.begin())));
        if (i >= m.captures.size())
          throw RegexpError("RegexpGroupError", "group out of range");
        const auto &r = m.captures[i];
        if (!r.matched)
          return NativeValue::nullv();
        const auto b = sel == "begin" || sel == "start" || sel == "byte_begin"
                           ? r.start
                           : r.end;
        return NativeValue::integer(
            sel.substr(0, 5) == "byte_"
                ? b
                : sputnik::regexp::cp_offset(m.source, b));
      }
      arity(0);
      if (sel == "text" || sel == "to_str" || sel == "inspect")
        return NativeValue::heap_string(native_regexp_match_to_string(m));
      if (sel == "source")
        return NativeValue::heap_string(m.source);
      if (sel == "pattern")
        return m.pattern;
      if (sel == "count" || sel == "size" || sel == "length")
        return NativeValue::integer(m.captures.size() - 1);
      if (sel == "pre_match")
        return NativeValue::heap_string(
            m.source.substr(0, m.captures[0].start));
      if (sel == "post_match")
        return NativeValue::heap_string(m.source.substr(m.captures[0].end));
      if (sel == "captures") {
        std::vector<NativeValue> out;
        for (std::size_t i = 1; i < m.captures.size(); ++i) {
          auto t = native_regexp_group_text(m, i);
          out.push_back(t ? NativeValue::heap_string(*t)
                          : NativeValue::nullv());
        }
        return NativeValue::list(std::move(out));
      }
      if (sel == "named_captures") {
        std::vector<std::pair<std::string, NativeValue>> out;
        for (const auto &n : as_regexp(m.pattern).names) {
          auto t = native_regexp_group_text(m, n.second);
          out.emplace_back(n.first, t ? NativeValue::heap_string(*t)
                                      : NativeValue::nullv());
        }
        return NativeValue::map(std::move(out));
      }
    }
    throw RegexpError("TypeError", "unsupported regexp operation");
  });
}
)SPUTNIKPP"
