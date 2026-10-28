package regexp_unicode
export main
from Regexp import r as rx

def check(value): raise ValueError("regexp Unicode regression") unless value

def isolated():
  Regexp = 7
  r"\d+".match?("42")

def main():
  check(r"^[a-z]+$"mi.match?("12\nABC\n34"))
  check(r'^[a-z]+$'im.match?("ABC"))
  check(rx"\d+".match?("٤٢"))
  check(r"(?mi)^[a-z]+$".match?("12\nABC\n34"))
  check(Regexp.compile("^[a-z]+$", multiline: true, ignore_case: true).match?("0\nABC"))
  check(r"a.b"s.full_match?("a\nb"))
  check(r"a.+b"U.match("a1b2b")[0] == "a1b")
  check(r"a b"x.full_match?("ab"))
  check(r"\p{L}+".full_match?("Привет"))
  hit = r"(?<letter>.)(?<optional>x)?".match("я")
  check(hit[0] == "я" and hit[:letter] == "я" and hit["optional"] == null)
  check(hit.begin(:letter) == 0 and hit.end == 1 and hit.byte_end == 2)
  check(hit.pattern.group_count == 2 and hit.pattern.names == ["letter", "optional"])
  check(hit.range(:letter).to_array == [0] and hit.byte_range(:letter).to_array == [0, 1])
  check(hit == r"(?<letter>.)(?<optional>x)?".match("я"))
  check(hit.named_captures["letter"] == "я")
  check(r"(?<=я)б".match("яб", from: 1).start == 1)
  check(r"\Aб".match("яб", from: 1) == null)
  check(r"\d+".full_match?("123\n") == false)
  check(r"\d+" ~= "abc123")
  check("abc123" ~= r"\d+")
  check(not ("abc" ~= r"\d+"))
  if r"(?<key>\w+)=(\d+)".match("port=8080"):
    check($_[:key] == "port" and $_[2] == "8080")
  else:
    raise ValueError("regexp condition")

  unicode_block = r"""
    \AПривет🙂\z\
    """
  check(unicode_block.full_match?("Привет🙂"))
  nested = r"\A#{r"x".source}\z"
  check(nested.full_match?("x"))

  data = "user+ # - ] \\ я"
  check(r"\A#{data}\z"x.full_match?(data))
  check(r"\{#{data}\}".full_match?("{" + data + "}"))
  check(r"\x41#{data}".full_match?("A" + data))
  check(rx'\A#{data}\z'x.full_match?(data))
  check(r"#{"a" + "b"}".full_match?("ab"))
  frag = Regexp.fragment("a|b")
  composed = r"\A#{frag}\z"
  check(composed.full_match?("a") and composed.full_match?("b"))
  scoped = Regexp.fragment("a b")
  check(r"\A#{scoped}\z"x.full_match?("a b"))
  scoped = Regexp.fragment("a", ignore_case: true)
  check(r"\A#{scoped}\z".full_match?("A"))
  check(not composed.match?("ax") and not composed.match?("xb"))
  total = 0
  count = r"\d".each_match("1я2") |m|:
    total += Int.parse(m[0])
  check(count == 2 and total == 3)
  check(r"x".replace_first("xx", "y") == "yx")
  check(Regexp.valid?("[") == false)
  check(Regexp.try_compile("[").err?)
  check(Regexp.try_compile("x").ok?)
  check(Regexp.version["engine"] == "PCRE2")
  zero = r"(?:|a)".scan("a")
  check(zero.length == 3 and zero[0].text == "" and zero[1].text == "a" and zero[2].text == "")
  check(r"(?:)".scan("я").length == 2)
  check(r"(?:|a)".replace("a", "x") == "xxx")
  check("я".replace(r"(?:)", "-") == "-я-")
  check(r"(?<key>\w+)=(\d+)".replace("a=12", "${key}:$2") == "a:12")
  check(r",".split(",,a,,") == ["a"])
  check(r"(?:)".split("я") == ["я"])
  check(r"(x)?y".replace("y", "[$1]") == "[]")
  check(r",".split(",a,,b,", keep_empty: true) == ["", "a", "", "b", ""])
  check(r",".split(",a,,b,") == ["a", "", "b"])
  check(r",".split("a,b,c", limit: 2) == ["a", "b,c"])
  check(r"(,)|(;)".split("a,b", include_captures: true) == ["a", ",", null, "b"])
  check(Regexp.compile("a b#c", literal: true, extended: true).full_match?("a b#c"))
  check("a,b".split(",") == ["a", "b"])

  rescued = false
  try:
    Regexp.compile("[")
  rescue RegexpCompileError:
    rescued = true
  check(rescued)
  rescued = false
  try:
    Regexp.compile("\\C")
  rescue RegexpCompileError:
    rescued = true
  check(rescued)
  rescued = false
  try:
    hit[3]
  rescue RegexpGroupError:
    rescued = true
  check(rescued)
  rescued = false
  try:
    r"x".replace("no match", "$no")
  rescue RegexpReplacementError:
    rescued = true
  check(rescued)
  rescued = false
  try:
    Regexp.compile("x", flags: "ii")
  rescue ArgumentError:
    rescued = true
  check(rescued)
  rescued = false
  try:
    Regexp.compile("(a+)+$", match_limit: 1).match("aaaaaaaaab")
  rescue RegexpMatchLimitError:
    rescued = true
  check(rescued)
  rescued = false
  try:
    "abc" ~= "def"
  rescue TypeError:
    rescued = true
  check(rescued)
  rescued = false
  try:
    r"a".replace_first("aa", "b", count: 2)
  rescue TypeError:
    rescued = true
  check(rescued)
  rescued = false
  try:
    r"a".group("aa")
  rescue NoMethodError:
    rescued = true
  check(rescued)
  check(isolated())
  rescued = false
  try:
    1 ~= 2
  rescue TypeError:
    rescued = true
  check(rescued)
  print("PASS regexp Unicode, flags, interpolation and matching")
  42
