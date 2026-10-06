# Bool parsing in Sputnik

`Bool.parse(value) -> Bool` strictly parses a `Str`. It is available in the
prelude without an import and works in the VM and native executables.

| Result | Accepted strings |
| --- | --- |
| `true` | `"true"`, `"t"`, `"1"`, `"yes"`, `"on"` |
| `false` | `"false"`, `"f"`, `"0"`, `"no"`, `"off"`, `"null"` |

Parsing ignores ASCII letter case and removes ASCII whitespace from both
ends (space, tab, newline, carriage return, form feed, vertical tab).
Internal whitespace is significant.

```sputnik
Bool.parse(" YES ") # true
Bool.parse("f")     # false
Bool.parse("NULL")  # false

try:
  Bool.parse("flase")
rescue ValueError |error|:
  print error.message
```

Every other string, including an empty or whitespace-only string, raises
`ValueError`. Other types raise `TypeError`: `"null"` is accepted, while the
value `null` is not. Integers, booleans and symbols are not coerced to strings.
The method accepts exactly one positional argument, with no keywords or block.
There is no implicit truthiness conversion or fallback value.

## ArgParser

ArgParser uses the same text parser for `type: Bool`, explicit flag values,
positionals, rest arguments and environment fallbacks.

```sputnik
parser = ArgParser(cmdline: ["--enabled=t", "--color=null"])
parser.arg("--enabled", type: Bool)
parser.flag("--color")
args = parser.parse_or_raise()
args["enabled"] # true
args["color"]   # false
```

Invalid boolean text raises `ArgParser.InvalidValue` in strict parsing mode,
including explicit empty values such as `--color=`. A flag without an explicit
value still means `true`; a negated flag means `false`. An absent argument
continues to follow ArgParser's defaults and required-argument rules.

Existing `.bool`, `.cast(Bool)` and `.cast?(Bool)` conversions keep their
existing contract. Use `Bool.parse` for the accepted configuration spellings.
