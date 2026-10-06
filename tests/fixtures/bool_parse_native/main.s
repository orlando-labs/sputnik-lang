package bool_parse_native
export main

def check(value):
  if not value: raise ValueError("Bool.parse regression")

def invalid_text(value):
  try:
    Bool.parse(value)
    false
  rescue ValueError:
    true

def invalid_type(value):
  try:
    Bool.parse(value)
    false
  rescue TypeError:
    true

def invalid_flag(value):
  parser = ArgParser(cmdline: ["--flag=" + value])
  parser.flag("--flag")
  try:
    parser.parse_or_raise()
    false
  rescue Exception |error|:
    error.message.contains?("expects Bool")

def invalid_option(value):
  parser = ArgParser(cmdline: ["--value=" + value])
  parser.arg("--value", type: Bool)
  try:
    parser.parse_or_raise()
    false
  rescue Exception |error|:
    error.message.contains?("expects Bool")

def invalid_env(value):
  parser = ArgParser(cmdline: [], env: {"ENABLED": value})
  parser.flag("--enabled", env: "ENABLED")
  try:
    parser.parse_or_raise()
    false
  rescue Exception |error|:
    error.message.contains?("expects Bool")

def main():
  ["true", "t", "1", "yes", "on", " TrUe ", " T "].each |value|:
    check(Bool.parse(value) == true)
  ["false", "f", "0", "no", "off", "null", " NuLl "].each |value|:
    check(Bool.parse(value) == false)
  check(Bool.parse("\tYES\r\n"))
  check(Bool === Bool.parse("null"))
  check(invalid_text("maybe"))
  check(invalid_text(""))
  check(invalid_text(" "))
  check(invalid_text("flase"))
  check(invalid_text("2"))
  check(invalid_type(null))
  check(invalid_type(true))
  check(invalid_type(0))
  check(invalid_type(:yes))
  check("true".bool and not "false".bool)
  ["", " ", "maybe", "2", "flase"].each |value|:
    check(invalid_flag(value))
    check(invalid_option(value))
    check(invalid_env(value))

  parser = ArgParser(cmdline: ["--value= T ", "--flag=null", " F "], env: {"ENABLED": " NuLl "})
  parser.arg("--value", type: Bool)
  parser.flag("--flag")
  parser.pos("positional", type: Bool)
  parser.flag("--enabled", env: "ENABLED")
  args = parser.parse_or_raise()
  check(args["value"] == true and args["flag"] == false)
  check(args["positional"] == false and args["enabled"] == false)
  print "PASS Bool.parse"
  0
