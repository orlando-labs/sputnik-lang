# Sputnik cheat sheet

Compact syntax reference + everyday recipes. Based on the working-tree [language spec](../sputnik_unified_final_spec.md), especially Parts I–III; [runtime grammar](../sputnik_runtime_project_design.md) fills in block parameters and lexical rules. Examples are independent; names such as `users`, `cfg`, and `load` stand for application values/APIs. Optional profiles and reference additions are marked separately.

## 1. Essentials

```sputnik
# Indentation defines suites; a colon opens a nonempty body.
def double(x): x * 2                 # implicit return: last expression
def no_op()                         # intentionally empty: omit the colon
n = 1_000; n += 1
hex = 0xFF; bits = 0b1010; mode = 0o755
ratio = 1.25e-3
name = "Ada"
text = "Hello, #{name}! #{2 ** 10}"  # interpolation; escapes: \n \t \u{1F600}
literal = "\#{name}"                # literal interpolation marker
state = :ready                      # Symbol
```

Only `false` and `null` are falsy: `0`, `""`, `[]`, `{}` are truthy. Names may end in `?` (predicate) or `!` (conventional mutation/danger); these suffixes are part of the name. Inline comments need whitespace before `#`.

| Job | Syntax / rule |
| --- | --- |
| Arithmetic | `+ - * / // % **`; `//` floors; `**` associates right: `2 ** 3 ** 2 == 512`; `-2 ** 2 == -4` |
| Compare / match | `== != < <= > >=`; `T === value` tests a type/matcher |
| Bit operations | `& \| ^ << >>`; `^` is XOR, not power |
| Logic | `not`, `and`, `or`; that precedence order; `and`/`or` short-circuit and return operands |
| Membership | `x in xs`; `key in map`; `"sub" in text` → receiver's `contains?` |
| Membership method | `x.in?(container)` → `container.contains?(x)`; exactly one container, `Bool` result |
| Value alternatives | `x.one_of?(a, b)` / `x.one_of?(*targets)` → receiver-side `==`, short-circuit; no targets → `false` |
| Default only for null | `value ?? fallback` (preserves `false` and `0`; specified, but see local binary note below) |
| Default for falsy | `value or fallback` |
| Assign a null default | `value ?= fallback`; preserves `false`, `0`, empty strings and collections |
| Assign a falsy default | `value or= fallback`; replaces `false` and `null` |
| Explicit absence | `text.trimmed.presence() or "anonymous"`; `xs.nonempty() or defaults`; `n.nonzero() or 100` |
| Last result | `$_` is read-only, scoped to the current callable/block; expression statements **including assignments and logging** update it |

Multiline string-tag macros can repeat a template region at runtime:

```sputnik
from system import cmd
command = cmd"""
  echo
  %for value with_index i in values:
    --item=#{value}
  """
```

`%for` uses ordinary patterns / Map arguments / `with_index`; the iterable is
evaluated once. Dedent closes the body, extra body indent is stripped, and the
directive emits no newline. `%#` is a template comment; `%%` emits `%`. Plain
untagged strings keep percent lines literal. The provider preserves interpolation
semantics: HTML escaping, SQL parameters, or opaque command arguments. Only
`%for` is implemented; `%if` is not. See [the provider contract](engineering/string-template-loops-v1.md).

External bodies use a declaration-only function: `template("./page.html.sv")`
immediately above `def page(title as Str, subtitle: "")`. Its parameters/imports
are visible in the body. Register `source_formats` (`suffix`, `module`, `macro`)
in the build manifest; call the function normally. `.sv` files need no delimiters
and preserve file layout. [External templates](engineering/external-templates-v1.md).

## 2. Collections, ranges, and absence

```sputnik
xs = [1, 2, 3]                      # Array
pair = (1, "a"); single = (1,)      # Tuple: comma matters
tags = {:read, :write}; empty = Set{}
user = {name:, active: true}         # {name:} means {name: name}
opts = {mode::fast}                 # mode: :fast
lookup = {(user_id): user}          # computed map key: parentheses
strict = StrictMap{name: 1, "name": 2}

ys = [0, *xs, 4 if enabled?]        # conditional entry is omitted when false
opts = {**defaults, debug: true if debug?}
tags = {:read, *extra if enabled?}

xs[-1]                             # last element; invalid index → IndexError
xs[?99]                            # optional index → null on absence
user[:name]                        # strict lookup; missing key → KeyError
user[?:email]                      # optional key → null on absence
user[:email] ?= "unknown"          # insert a missing key or replace null
xs[0] ?= 10                        # replace only a null element
xs[1] or= 20                       # replace false or null
maybe_user.?.[?:email]              # tolerate BOTH null receiver and missing key

(1..5:2).array                     # [1, 3, 5]; inclusive end
(1...5:2).array                    # [1, 3]; exclusive end
(5..1:-1).array                    # descending: specify negative step
xs[1...3]; xs[0..:2]; xs[-1..:-1]   # slice / every second / reversed copy
Array.of(3): []                    # three distinct mutable arrays
Array.of(5) |i|: i * i             # [0, 1, 4, 9, 16]; alias: Array.build
Array.filled(3, 0)                 # repeated value; objects share one reference
5.times.map: $it * 2               # [0, 2, 4, 6, 8]
```

`{}` is an empty Map; `{x}` is a one-element Set. Ordinary Map treats `:name` and `"name"` as the same key and exports name keys as strings. `StrictMap` distinguishes them. Duplicate map keys overwrite the value; sets deduplicate. `Map{…}` / `Set{…}` are explicit constructors; `HashMap{…}`, `StrictHashMap{…}`, `HashSet{…}` also have spellings, but hash-backed performance is not guaranteed by the current reference profile.

`[?key]` handles absence, not arbitrary errors, and is read-only syntax. Use `contains?(key)` / `has_index?(i)` to distinguish missing entries from stored `null`. `.?.` guards only a null receiver; protect each nullable step. Open ranges (`1..`, `1..:2`) need a finite bound before materialization/spread; slicing supplies one. Step zero is invalid.

`?=` and `or=` evaluate the target once and evaluate the RHS only when a write is needed; they return the retained or assigned value. Indexed targets use optional lookup and ordinary assignment: a missing map key can be inserted, while an out-of-bounds array write raises `IndexError`. `a?=b` means `a ?= b`; use `ready? = false` to assign to a predicate-suffixed name.

`in?` and `one_of?` work on every value, including `null`, `false`, collections,
and type objects. `2.in?([1, 2])` is true; `2.one_of?([1, 2])` is false because
an unspread collection is one target. `[1, 2].one_of?(*[[1, 2]])` is true.
`one_of?` uses equality, never type/pattern matching: `5.one_of?(Int, 1..10)` is
false. Both methods reject keywords/blocks and require `Bool` protocol results;
unsupported `in?` containers raise `TypeError`. Argument expressions evaluate
before comparisons begin; only comparisons short-circuit. User methods override
the standard fallback normally.

## 3. Blocks and chains — spaces matter

```sputnik
users.map |u|: u.email.trimmed
users.map: $it.email.trimmed         # $it == $it1 == _1; $it2 == _2, etc.
numbers.map: $it * 2 .select: $it > 0 .reduce(0): $it1 + $it2
pairs.map |(key, value)|: "#{key}=#{value}"  # destructuring block parameter

result = numbers
  .select: $it > 0
  .map if scale? |n|: n * 10
  .uniq()
```

**No space before a dot** keeps it inside a one-line block; **space + dot** resumes the outer chain. A block attaches to the nearest call on its left. Numbered arguments exist only inside blocks without `|…|`, must use consecutive positions, and always belong to the nearest block. `map($it * 2)` is not block syntax.

Bare calls also accept `name: expression` or an indented `name:` block, including
expression position (`result = name: value`). The inline colon touches the name;
`name :symbol` does not start an inline block. Map bodies such as
`json: {ok: true}` use the same general syntax.

Bare call arguments cannot start on another line. For multiline arguments,
open `method(` on the call's line. An expression `(a, b)` after a completed
block is a separate tuple, including when used as an implicit return.

Conditional chain segments use `.call(args) if condition` or `.call if condition |x|: body` (also `unless`). A skipped segment passes its receiver through unchanged and skips its arguments/block. Its condition does not guard later segments. Adjacent `.a().b()` calls belong to one segment; whitespace before the next dot starts another.

## 4. Functions, types, and callable values

```sputnik
def greet(name as Str, prefix: "Hi") -> Str:
  "#{prefix}, #{name}"
greet("Ada", prefix: "Hello")
greet(*["Ada"], **{"prefix": "Hello"})

def apply(value, &block): block(value)
def relay(xs, &block): xs.map(&block) # forward through the block channel
def notify(value, &block): block.?.(value)  # missing block is null

f = &Math.answer                    # reference: never invoke the target
f()                                # call a callable value
method = &User#full_name            # unbound instance method
method(user)                       # receiver is its first argument
factory = User; factory("Ada")      # class call == User.new("Ada")

obj.member                         # property read OR implicit nullary method call
obj.member()                       # explicit method call; properties reject this
obj.member.()                      # read member, then call the resulting value
callback.?.(42)                    # call only when callback is non-null

import build_info as info
info.version                       # namespace member: implicit nullary call
from build_info import version
version                            # imported function: implicit nullary call
version()                          # explicit call, exactly once
callback = &version                # copy the function without calling it
callback                           # invoke the stored function
copy = &callback                   # read that function value without calling
apply((&callback))                 # positional function value
apply(&callback)                   # forward through the block channel

"42".int                           # conversion property; also Int("42") / .to_int()
value.cast(Int)                    # explicit conversion protocol
value as Int                       # check existing type; NEVER convert
```

Parameters: `x`, `x = default`, `key:`, `key: default`, `x as T`, `key as T: default`; `@x` / `@@x` additionally store the bound parameter in instance/class storage. Defaults run **on every call**. In `def init(@x, @y = x)`, use local `x` for the new argument; `@x` in a default reads old state.

Types: `T?` = `T | Null`; unions `Int | Str`; tuples `(Int, Str)`; generics `Array[Int]`, `Map[Str, Int]`; records `{id: Int, **Str}` (extra values must be strings). Annotations are optional; full static inference is not implied. Conversion properties: `.str .int .float .bool .symbol .array .tuple .set .map`; do not add `()` to these aliases.

`*args` expands finite Array/Tuple/Range values; `**opts` accepts maps or a **readable `kwargs` property** producing a valid map/view. Keyword keys must be valid parameter names; duplicate keyword arguments are errors. Positional arguments precede keywords. A trailing `&block` pass must be last and use a local name; bind `&Namespace.fn` to a local first. `&obj.method` is not supported; use a closure adapter.

## 5. Classes, properties, and composition

```sputnik
mixin Named:
  def label(): self.name

class User:
  include Named                     # instance-side methods
  def init(@name, @email: null)
  attr var name                     # read/write @name
  attr email                        # read-only @email
  attr set password from @secret    # write-only, custom backing field
  prop greeting: "Hello, #{@name}"  # computed getter
  prop normalized_name:
    get: @name.trimmed
    set(value): @name = value.trimmed
  class_prop kind: :user
  class_method def build(name): User(name)

class Admin < User                  # inheritance; empty class needs no colon
u = User("Ada", email: "ada@example.org")
u.name = "Grace"                    # property setter
```

`@field` is storage, not a public accessor; expose it with `attr`/`prop`. `class_prop` and `@@field` are class-side. Property arms must not suspend. Inside object methods, lexical names win; an unresolved name falls back to `self.name`.

Reopen with another `class User:` / `mixin Named:`. `include A, B` mixes into instances; `extend FactoryMixin` in a class mixes into its class object. Local methods win, then later mixins, then superclass. Runtime method tools: `define_method(User, :greet) |name|: …`, `send(user, :greet, "Ada")`, and `def method_missing(selector, …): …` (signature depends on handled calls). Changes to classes/method tables require an open dispatch world. Custom indexing uses `def [](key): …` and `def []=(key, value): …`.

Inside a method defined in a class, `super(args...)` calls the same method on the lexical superclass with the original receiver. Use `super()` for no arguments and `super(value, option: option, &block)` for explicit forwarding. This also works in constructors, `class_method` bodies, and nested closures. See [superclass calls](engineering/super-dispatch.md).

## 6. Pattern matching and multi-clause functions

```sputnik
[head, *tail] = xs                  # mismatch → MatchError
{id: user_id, **rest} = payload

label = case! payload:
  when {id: n, name:} if Int === n and n > 0: "#{n}: #{name}"
  when null: "missing"
  else: "other"

def format(x, mode::short):
  when {mode::short}: x.str         # map pattern matches arguments by name
  else: "Value: #{x}"

def fact(0): 1                     # consecutive simple defs form clauses
def fact(n) if n > 0: n * fact(n - 1)
```

| Pattern | Meaning |
| --- | --- |
| `_`, `name`, `42`, `:ok`, `null` | Ignore / bind / match literal |
| `^expected` | Compare with an existing binding |
| `(a, b)`, `[head, *tail]` | Destructure sequence; spec core places rest last |
| `{id:}`, `{id: n}`, `{id:, **rest}` | Bind named key / rename / capture remaining keys |
| `{id:, **_}`, `{id:, **null}` | Ignore extras / forbid extras; extras allowed by default |
| `Int`, `Point(x, y)`, `Point(x:, y:)` | Type/constant test; typed positional/key destructuring |
| `whole as [a, b]`, `n as Int` | Bind whole value, then match the right-hand pattern |
| `(0, x) \| (x, 0)` | Alternatives must bind the same names; wrap OR patterns inside block `\|…\|` |
| `in choices`, `n as in choices`, `{status: in [409, 410]}` | Membership via `in?`; RHS is an expression, no new bindings; works in every pattern context |
| `when 1..10:` | Matcher expression (`===`); only in `case` / `case!` |
| `pattern(matcher) with {id:, **null}` | Dynamic matcher with explicit bindings; only case/clause-def contexts |

First matching clause whose guard passes wins. Unmatched `case` returns `null`; `case!` and clause-def raise `MatchError`. User objects participate via `===`, `deconstruct()`, `deconstruct_keys(keys)`; dynamic matchers use `match(value)`. Rich function signatures use one `def` with `when` clauses; tuple clauses match positional arguments, map clauses match argument names.

`in choices` evaluates the current container once when reached, including inside
structures, block parameters, and assignment; a skipped arm does not evaluate it.
RHS names refer to the outer environment and cannot read bindings introduced by
the same pattern. Parenthesize expression-level `|`: `in (left | right)`.
`Int | Str` matches types; `in [Int, Str]` checks membership among type objects.

## 7. Conditions and control flow

```sputnik
label = if ready? then "ready" else "waiting"
print "ready" if ready?             # statement modifier; also unless
unless ready?: print "waiting"

value = if ready?:
  compute()
else if cached?:                    # aliases: elif, elsif
  cached_value
else:
  null

while work?: step()
until done?: step()
do:
  step()
while work?
found = loop:
  if ready?: break :ok

for x in (0..15:2):
  if x == 6: break
  print x

for x with_index i in (1...100500:42):
  print x, i
  if i == 2: break

first_big = catch(:found):
  xs.each |x|:
    if x > 100: throw :found, x
  null
```

`if`, `case`, and loops are expressions. A loop returns `break value`, otherwise `null`. `for` lowers to `for_each`; both, like block-form `times`, always return `null`. Bare `break` ends this effect iteration; `break value` is forbidden here. `break` still cannot exit `each`/`map`/`select`. Use `find`/`any?`/lazy bounds for routine early exit; `catch`/`throw` crosses nested calls/blocks. `next value` ends only the current block invocation. `return value` in a call-site block exits the **creating method/lambda**; a standalone lambda returns locally. Bare `return` and bare `next` use `$_`, not necessarily `null`.

`then` runs actions left to right in the same scope and updates `$_` after each:
`load() then normalize($_)`. It has lower precedence than assignment;
`x = first() then y = second()` performs two assignments. A trailing `if`/`unless`
guards the whole sequence: `rollback() then break if invalid?`.

In an ordinary call-site block, `break value` exits the accepting call and
becomes its result; bare `break` returns `null`. Forwarding `&blk` preserves
the original target. All `ensure` bodies run; `rescue` does not catch the exit.
An expired target or another task raises `LocalJumpError`.

Mixing inline `if … then … else …` with a sequence requires parentheses
(`ParenthesesError` otherwise). Even with parentheses, prefer multiline code:
`sputnik lint file.s` warns about mixed conditionals (`L1001`), nested sequences
and three or more actions on one line (`L1002`). `obj.then()` remains a method call.

`r"\d+"` and `r'\d+'` create Unicode regexps. Append flags directly: `r"^[a-z]+$"mi` (`i`, `m`, `s`, `x`, `U`); inline `(?mi)` also works. `text ~= pattern` and `pattern ~= text` are the same search predicate; negate with `not`. Use `pattern.match(text)` for captures. In `r"#{text}"`, Str holes are escaped literal data. A completed condition publishes its result to `$_`; `and`/`or` do not publish intermediate operands.

## 8. Everyday collection recipes

| Pattern | Idiom |
| --- | --- |
| Clean and deduplicate | `users.map: $it.email.trimmed.downcased .uniq()` |
| Filter, then transform | `xs.select: $it > 0 .map: $it * 2` |
| Transform and discard falsy results | `xs.filter_map: lookup($it)`; removes `false`/`null`, keeps `0`/`""` |
| Flatten mapped results | `groups.flat_map: $it.members` |
| Aggregate | `xs.reduce(0): $it1 + $it2`; `xs.sum`; `xs.product` |
| Iterate for effects | `for x in xs: …`; `xs.for_each \|x\|: …`; `for x with_index i in xs: …` |
| Value / index / both | `xs.find \|x\|: …`; `xs.find_index \|x\|: …`; `xs.find_with_index \|x\|: …` → `(x, i)` |
| Index in predicate | `xs.find.with_index \|x, i\|: …` → `x`; `xs.find_with_index.with_index \|x, i\|: …` → `(x, i)` |
| Find / quantify | `xs.find: $it > 10`; `xs.any?: $it > 10`; also `all?`, `none?` |
| Group / partition | `users.group: $it.role`; `xs.partition: $it > 0` |
| Count by value / key | `xs.counts`; `users.counts: $it.role` → Map of counts; `tally` is an alias with the same optional block |
| Sort / unique by key | `xs.sort: $it1 - $it2`; `users.uniq: $it.id` |
| Index / windows / pairs | `xs.each_with_index \|x, i\|: …`; `xs.each(3, step: 1) \|window\|: …`; `xs.each_pair \|(a, b)\|: …` |
| Lazy bounded work | `(1..).lazy.select: $it.even? .first(5)` |
| Zip / compact / flatten | `xs.zip(ys)` truncates to shortest; `xs.compact` drops null; `xs.flattened` flattens nested arrays |
| Set operations | `a.union(b)`, `.intersection(b)`, `.difference(b)`, `.symmetric_difference(b)`; aliases `\| & - ^` |
| Render text | `xs.join(", ")`; `text.lines`; `text.replaced("old", "new")` |
| Transform a map | `m.map \|k, v\|: …` → Array; `m.transformed \|k, v\|: (k, v * 2)` → Map |
| Transform keys / values | `m.transformed_keys \|k, v\|: k.str + "_copy"`; `m.transformed_values \|v, k\|: v * 2` |
| Transform a map in place | `m.transform! \|k, v\|: (k.str + "_new", v * 2)` → receiver; `transform_keys!` / `transform_values!` change one side |
| Keep map keys | `m.select \|k, v\|: v > 0`; `m.transformed_values \|v\|: v * 2` |
| Merge with conflict handling | `left.merge(right) \|key, old, new\|: old + new` |
| Nested optional lookup | `cfg.dig(:server, :port) ?? 8080` |
| Memoize, including false/null | `cache.get_or_set!(key): expensive(key)`; block runs only for absent keys |
| Copy | `xs.copy` is shallow; `xs.deep_copy` preserves cycles/shared-reference topology |

Collections are eager unless made lazy. Prefer an explicit seed for `reduce` on possibly empty input. `each` returns its receiver; `for_each`, `for`, and `n.times: …` return `null`. All search forms return `null` on no match. `.with_index` appends a zero-based index to block arguments without changing the method result. `for` preserves the argument/pattern contract of `each`, including `for k, v in map:`; ranges keep `..`/`...`/`:step`. Map blocks normally receive **key, value**; `transformed_values` receives **value, optional key**. Mutation methods include `push!`, `filter_map!` (Array/Set), `compact!`, `flatten!`; pure string forms include `trimmed`, `upcased`, `downcased`, `reversed`.

## 9. Errors, cleanup, and explicit results

```sputnik
class InputError < Exception:
  def init(@message)
  attr message

def parse_count(text):
  Int(text)
rescue TypeError, ValueError |e|:
  0
ensure:
  record_attempt()                  # runs on success, failure, return, or unwind

value = try:
  risky()
rescue |e|:                         # catch all ordinary raised values
  fallback(e)
ensure:
  release()

raise InputError("bad input")
Ok(42).map: $it * 2                 # Ok(84)
Err("missing").or(0)               # 0
Err("missing").or_else |e|: recover(e)
Err(InputError("bad")).or_raise    # bridge to raise/rescue
```

`rescue A, B |e|:` uses commas between types. `ensure:` preserves the pending result unless cleanup itself fails. Re-raise explicitly with `raise e`; bare `raise` is unsupported. `throw` is tagged control flow and bypasses `rescue`. Results expose `.ok?`, `.err?`, `.value`, `.error`; wrong-state payload access raises `ValueError`.

## 10. Concurrency and output

```sputnik
import task
from sync import Channel, Mutex, Atomic

local = task.async: 20 + 1           # cooperative, same strand
parallel = task.spawn: 21           # parallel, new strand
local.wait() + parallel.wait()      # 42; wait rethrows child failures

xs = [1, 2, 3, 4, 5, 6, 7, 8]
serial = xs.map: $it * $it           # sequential array pass
squares = xs.threaded(4, scatter: :chunks).map: $it * $it
# [1, 4, 9, 16, 25, 36, 49, 64]; 4 workers, 2 consecutive items per chunk

counter = Atomic.new(0)
counter.update: $it + 1              # CAS may retry: keep block side-effect-free
mutex = Mutex.new()
mutex.synchronize: critical_work()   # unlocks on unwind

print "Hello, #{name}"              # display, newline per argument; returns null
p value                             # inspect; returns value (Tuple for several)
pp value                            # pretty inspect; same return convention
```

`.threaded(4, scatter: :chunks)` splits the array into contiguous chunks with one task per worker. `map` waits for completion and preserves input order; input elements and results must be shareable. For faster passes over large arrays with similar CPU work per element, give each chunk enough work to amortize scheduling overhead. The default `scatter: :atomic` assigns the next item dynamically and suits uneven work.

Structured scopes join children. The spec's explicit root scope is `async |task|:` (see local binary note below). `task.async` can capture same-strand mutable state; `task.spawn` requires shareable captures (for example deeply frozen data). `task.sleep(seconds)` / `.yield()` cooperate; handles offer `.wait(timeout: seconds)`, `.cancel()`, `.resume()`. `Channel.new(capacity: 0)` is rendezvous; positive capacity buffers; `.send(v)` / `.recv()` communicate shareable values. Close explicitly with `.close()`; a drained closed channel raises `ChannelClosedError`. `Mutex` is non-reentrant. `task.flow` adds `scatter_map(items, workers: 4) |item|: …` and `scatter_reduce`.

## 11. Modules

```sputnik
package app.users                    # first non-comment form; optional for scripts
import net.http as http              # bare import net.http binds http
from app.models import User as Person
import "../backend/models" as models
from "../backend/models.s" import Account

def display(user): user.name
export display, Person as User       # private by default; explicit re-export
```

Imports are static, top-level, before ordinary declarations/statements. Imported names are live, read-only aliases. `.s`, `.spu`, and `.sputnik` source files are accepted. From the repository root: `build/sputnik program.s` runs a file; `build/sputnik parse program.s` inspects syntax.

`sputnik run backend/main.s` and `sputnik build backend/main.s -o build/app`
need no manifest. String paths starting with `./` or `../` are relative to the
importing file, without interpolation. Extensions are optional; the default
alias is the filename stem. Equivalent paths/symlinks initialize one module once.
Named local imports search the importer and entry directories (dots map
to subdirectories; the entry's package prefix may be stripped). Installed
packages need no `dependencies`. Optional nearest `sputnik.build.json` / YAML
settings can hold path overrides and template formats without listing sources
or repeating the entry. `sputnik pkg lock backend/main.s` pins the package graph;
local source edits stay live.

Package installation and imports select a compatible host variant automatically,
including GPU runtimes. `--backend cpu` forces CPU; `--backend` and `--bundle`
default to `auto`. An optional lock pins exact variants and hashes; an installed
GPU package does not require a lock to import.

`import my.orm as orm` replaces the module path throughout the graph: `import orm`, `from orm import Model`, and dependency imports all use `my.orm`. Qualified aliases such as `import my.orm as vendor.orm` and submodule suffixes work too. Repeated imports of the same binding are allowed; the module initializes once. Scripts and notebook cells need no additional manifest.

## 12. Optional profiles and reference additions

These extend the everyday core; availability depends on the selected toolchain/host.

**Local binary note:** the checked `build/sputnik` parses the main examples, but rejects the specified `??` operator and root `async |task|:` form. For a null-only default today, bind once and use `if value == null then fallback else value`; the imported `task.async` / `task.spawn` API provides the current task spelling. Spec coverage is not a promise of full runtime/profile support.

| Profile | Compact surface |
| --- | --- |
| Numeric | File preamble `numeric:` with indented `int: Int64` and `overflow: checked` (defaults); opt-in `wrapping`/`saturating`; explicit `BigInt(value)`. [Numeric contract](engineering/numeric-profile-v1.md) |
| Effects | `def pure(x as Int) -> Int !{}: x * 2`; effects such as `!{net, async}`; callable type `Fn[Int -> Int !{}]` |
| Schema | `schema User:` with fields `id as Int`, `name as Str`, `email as Str?`; versioned `schema Order v1:`; metadata such as `deprecated` / `@pii` |
| Contracts | `require condition`, `ensure condition`, `old(expr)`; class `invariant condition`; `property "name" \|xs as Array[Int]\|:` + `expect condition` |
| Workflow | `workflow ImportOrders:`; `step fetch !{net} retry: {max: 3}:`; `step commit !{db} idempotency_key: key:`; `compensate commit !{db}:` |
| Accelerator | `gpu.map(xs) \|x\| -> F32 !{}: x * 2.0`; restricted pure kernel subset |
| Notebook watch | `h = Kernel.watch(x)` (also `@x`, `@@x`); `Kernel.revision(x)`; `Kernel.unwatch(h)` |
| Other runtime profiles | Capabilities/sandbox, observability/replay, DataFrame, Wasm components, AI tooling/provenance, privacy/taint/lineage, Frozen: see spec Part IV / runtime design for APIs and manifests |

The current reference corpus also includes useful forms not fully consolidated into the main spec:

```sputnik
inc = |x|: x + 1                     # standalone lexical closure
make_adder = |n|: |x|: x + n
def collect(*args, **kwargs): (args, kwargs)  # rest: Tuple / keyword map
[first, *middle, last] = [1, 2, 3, 4]       # middle sequence rest
```

Sources: [lambda](../corpus/run/lambda_literal/source.s), [rest parameters](../corpus/run/rest_middle_combo/source.s), [middle destructuring](../corpus/run/destructure_mid_rest/source.s). The separate [named multiblock RFC](../sputnik_v20.9_named_multiblock_rfc.md) is **Draft**, not part of this core sheet: it proposes `def request(url, &success:, &error:)` with `request(url) with:` and named callback suites.
