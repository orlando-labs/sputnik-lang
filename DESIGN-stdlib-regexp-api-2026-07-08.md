# DESIGN: Regexp, Unicode matching and the `r` string tag

Date: 2026-07-08; revised 2026-10-28.
Status: implemented core contract, with explicit deferred decisions below.

The same PCRE2 8-bit engine is used for compiler validation, VM execution and
full native execution. UTF and Unicode character properties (UCP) are always
on. PCRE2 10.40+ is a build/runtime dependency. The implementation does not use
`std::regex` for this module.

## 1. Literals, modifiers and source

```sputnik
from Regexp import r as rx

digits = r"\d+"
letters = r'^[a-z]+$'mi
same = rx"^[a-z]+$"im
unicode = r"\p{L}+"
```

`r` is available from the prelude. A from-import alias has the same behavior.
The head, opening delimiter and optional suffix must be adjacent. The suffix is
stored in the string-template AST as `tag_suffix`, so it is available to other
string-tag providers too; its meaning belongs to the provider.

| Suffix / compile keyword | Meaning |
| --- | --- |
| `i` / `ignore_case` | Unicode case-insensitive matching |
| `m` / `multiline` | `^` / `$` operate at line boundaries |
| `s` / `dot_all` | `.` includes newline |
| `x` / `extended` | Pattern whitespace and comments are insignificant |
| `U` / `ungreedy` | Invert the default quantifier greediness |

Order is immaterial. Unknown or duplicate suffix letters are compile-time
errors. `u` is unnecessary; `g` is not supported because the operation selects
one or all matches. `m` has no effect on `[a-z]` by itself.

Inline modifiers remain supported, including scoped options:

```sputnik
r"(?mi)^[a-z]+$"
r"first(?i:second)"
Regexp.compile(source, flags: "mi")
Regexp.compile(source, multiline: true, ignore_case: true)
```

Suffix and `flags:` establish initial compilation options. Explicit Bool
keywords override their corresponding initial bits. Inline modifiers then obey
PCRE2 scope rules. `Pattern.flags` is the canonical initial flag string
(`imsxU` order), not a summary of scoped settings inside the source.

Short literals use `r"…"` or `r'…'`. Triple-quoted blocks require a newline after
the opener and use the ordinary text-block normalization rules, including
dedent, trailing whitespace removal, line joining and final newline handling.
Use extended mode for a visually formatted pattern:

```sputnik
pattern = r"""
  \A
  (?<key>\w+) = (?<value>\d+)
  \z
  """x
```

The provider consumes authored escape parts (`AstStringEscape.source`), not
cooked Sputnik escapes (`value`). Untagged strings retain ordinary string
semantics. The lexer preserves source escapes for adjacent string-tag
candidates, including aliases; string interpolation remains parsed in both
single- and double-quoted tagged forms.

Static literals are checked during expansion, including literals in unused
functions. A bounded 64-entry cache reuses immutable literal programs.
Dynamic templates validate their static structure at expansion and
compile the assembled pattern at execution. Errors identify the literal span
and the PCRE2 byte offset in normalized pattern source, with the diagnostic
location mapped back through text-block normalization to the authored source.

## 2. Module and pattern API

```sputnik
Regexp.compile(source,
  flags: "", ignore_case: false, multiline: false, dot_all: false,
  extended: false, ungreedy: false, literal: false, match_limit: null)
Regexp.new(source, ...)               # alias of compile
Regexp.try_compile(source, ...)       # Result[Pattern, RegexpCompileError]
Regexp.valid?(source, ...)            # false only for invalid regexp syntax
Regexp.escape(text)                   # Str; safe with extended mode
Regexp.fragment(source, ...)          # explicitly trusted syntax, restricted v1
Regexp.default_match_limit            # 1000000
Regexp.version                        # engine, version, jit metadata

pattern.source
pattern.flags                        # initial flag string
pattern.names                        # names in capture order
pattern.group_count
pattern.match(text, from: 0)          # Match?; search
pattern.match?(text, from: 0)         # Bool; search
pattern.full_match(text)              # Match?; whole subject
pattern.full_match?(text)             # Bool; whole subject
pattern.scan(text)                    # List[Match]
pattern.each_match(text) |match|: ...  # streamed matches; returns count
pattern.split(text, limit: null, keep_empty: false, include_captures: false)
pattern.replace(text, replacement, count: :all)
pattern.replace_first(text, replacement) # exactly one; no count: keyword
pattern.replace(text) |match|: ...
```

`source` and subjects are Str; patterns are compiled explicitly. `literal: true`
escapes source before compilation and preserves literal whitespace even with
extended mode. `valid?` and `try_compile` do not suppress option/type errors.

`from:` is a nonnegative codepoint offset in `0..text.length`. Negative offsets
raise ArgumentError; offsets beyond the subject raise IndexError. The engine
receives the whole subject and a start offset, preserving lookbehind and
anchors. Full matching rejects a nonzero `from:`.

`.full_match` uses engine anchoring at both ends. `^…$` is not equivalent:
`$` may match before a final newline, and `m` changes anchor meaning. For
explicit source anchors use `\A…\z`.

The Str overloads `text.replace(pattern, replacement, count: ...)` and
`text.replaced(...)` retain their existing pure behavior; block replacements
are supported from the Str side as well. Substring replacements and ordinary
`Str.split(Str)` keep their existing semantics.

## 3. Symmetric match operator

```sputnik
text ~= pattern                       # Bool, search
pattern ~= text                       # same predicate
not (text ~= pattern)                 # boolean negation

if match = pattern.match(text):
  print(match[1])
```

`~=` means correspondence, with regexp search semantics. In particular,
`"abc123" ~= r"\d+"` is true; whole-string checking remains explicit.
Both arguments are evaluated exactly once, left to right. The supported pairs
are `(Str, Pattern)` and `(Pattern, Str)`; their order does not change the result.
Unsupported pairs raise TypeError. Match limits and engine errors propagate
and never become `false`.

Comparison precedence (4) and left associativity match `==`. Logical `not`
binds below comparisons, so `not text ~= pattern` means `not (text ~= pattern)`.
Do not chain match predicates as though they were numeric comparisons.

The previous `=~` and `!~` operators are removed from the lexer, parser and
both execution backends. Use `~=` for predicates, `not` for negation and
`.match` for captures. There is no symbolic negative match operator.

`~=` does not participate in collection equality/hash. Numeric tolerances,
glob matching and a custom symmetric matcher protocol are deferred; two Str
operands do not acquire implicit fuzzy semantics.

## 4. Match values and Unicode

A Match keeps its source and pattern alive. Capture strings are valid UTF-8.
Public positions are Unicode codepoints, not grapheme clusters. Byte positions
are explicit interop methods.

```sputnik
match[0]                       # full match
match[1]                       # numbered capture
match[:name]                   # same as match["name"]
match.group(:name)
match.begin(:name)             # codepoint offset
match.end(:name)
match.start                    # group 0 begin
match.finish                   # group 0 end
match.byte_begin(:name)
match.byte_end(:name)
match.range(:name)             # half-open Range of codepoint positions
match.byte_range(:name)        # half-open Range of UTF-8 byte positions
match.text
match.source
match.pattern
match.pre_match
match.post_match
match.captures
match.named_captures
```

Unknown names and out-of-range indices raise RegexpGroupError. An unmatched
optional group returns null, including its positions/ranges. An empty matched
group returns `""` and a zero-width range. Duplicate capture names are rejected.
Pattern equality uses authored normalized source, initial flags and fragment
branding; Match equality uses source, pattern and capture ranges.

`\C` is disabled with PCRE2_NEVER_BACKSLASH_C so a capture cannot cut a UTF-8
codepoint into individual bytes. Invalid subject UTF is an error. Default
newline convention is LF; BSR uses Unicode newline sequences. Explicit PCRE2
newline directives are honored.

## 5. Interpolation

```sputnik
prefix = "user+ #"
pattern = r"\A#{prefix}\d+\z"x
```

An ordinary hole accepts Str data. It is escaped for literal matching and
inserted as one noncapturing expression; metacharacters, spaces and `#` remain
data even in extended mode. Do not call `Regexp.escape` before interpolation:
that would insert the escape characters themselves.

Holes must occur at complete regexp-expression boundaries. The static template
is compiled with sentinel capture groups, whose presence verifies that PCRE2
actually parsed each hole as an expression. A hole inside a character class,
escape sequence, quantifier, group header, `\Q…\E`, or extended-mode comment
is rejected. Runtime assembly is compiled again after values are inserted.

Trusted syntax needs an explicit fragment:

```sputnik
alternative = Regexp.fragment("cat|dog")
pattern = r"\A#{alternative}\z"
```

Fragments are grouped with their own initial flag scope, so the result means `\A(?:cat|dog)\z`. V1 fragments
cannot contain captures or whole-pattern recursion; capture numbering/name composition requires a separate
contract. A normal Pattern is not silently treated as a trusted fragment.
Interpolation evaluates expressions once, left to right. General non-Str
stringification in holes is deferred.

## 6. Iteration, replacement and split

All global operations use the same progress algorithm. After an empty match,
first retry an anchored nonempty match at the same position. If that fails,
advance one codepoint (or one CRLF sequence when the pattern's newline mode
requires it). The final empty match is delivered once.

Thus `r"(?:|a)".scan("a")` yields full texts `["", "a", ""]` and replacing all
of them with `"x"` yields `"xxx"`. Search always retains the original subject;
restarting on a sliced suffix would break anchors and lookbehind.

Replacement grammar:

- `$0` and `$1`..`$99` reference groups; `${number}` supports larger indices.
- `${name}` references a named group; `$$` is a literal dollar.
- Unknown references and all other dollar sequences raise RegexpReplacementError.
- Unmatched existing groups substitute empty text.
- Replacement syntax is validated even when no match exists or `count: 0`.
- `count` accepts a nonnegative Int or `:all`.
- A replacement argument and a block are mutually exclusive. A block must
  return Str; its output has no replacement grammar, so it also handles literal
  user-supplied replacement strings. Exceptions/control transfer propagate.

`split` omits leading/trailing empty primary fields unless `keep_empty: true`,
while preserving internal empty fields. `limit` is null or a positive Int; it
counts primary fields before boundary-empty removal, and the last field holds
the unsplit remainder. `limit: 1` performs no split; an empty subject still follows `keep_empty`. Captured separators are additional entries, excluded from the limit;
unmatched captures are null. With captures enabled the type is List[Str?].

## 7. Errors and resource limits

Canonical rescuable names are:

```text
RegexpError < Exception
RegexpCompileError < RegexpError
RegexpGroupError < RegexpError
RegexpMatchLimitError < RegexpError
RegexpReplacementError < RegexpError
```

Dotted names such as `Regexp.CompileError` are not introduced aliases.
Static compile failures are compiler diagnostics; dynamic failures are rescuable.
PCRE2 match/depth/heap limit failures map to RegexpMatchLimitError.

Default match limit is 1000000; `match_limit:` is a uint32 override. Depth is
bounded at 1000, heap at 32768 KiB. JIT is disabled in this revision so both
execution backends use the same engine path. These are engine limits, not a
wall-clock deadline or a cumulative budget for scan/replace/output allocation.
Their exact firing points can change with PCRE2 versions. Deployments needing
a total service budget still need input/output limits and cancellation policy.

## 8. `$_`, conditions and pattern matching

A completed condition publishes its original value to `$_` before entering a
branch. `and`/`or` do not publish intermediate operand values. Therefore
Match-returning `.match` can expose captures through `$_`; Bool-returning
`~=` publishes Bool. Named assignment in a condition is a separate convenience
for a local binding.

Predicates in a `case` belong in guards:

```sputnik
case value:
  when _ if r"\d+".full_match?(value):
    print("matched")
```

Bare Regexp structural patterns are not introduced.

## 9. Coverage and remaining decisions

`make test-regexp-unicode` verifies Unicode, all modifiers, imports/aliases,
interpolation, fragments, captures, positions, global progress, replacements,
splitting and rescues in VM and full native without fallback. It also checks
compile-time failures for malformed literals, bad modifiers, unsafe hole
positions and `\C`. Existing regexp and `$_` tests remain required.

Deferred decisions: non-Str interpolation conversion; capture-bearing or
recursive fragment composition; approximate numeric/glob/custom
matcher protocols; cumulative operation/deadline/output budgets; version-pinned
frozen-image reproducibility. Compiler diagnostics map normalized literal offsets back through dedent,
trimming and line joining. These limitations do not change the implemented
Unicode semantics.
