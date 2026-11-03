package argparser_environment

export main

def check(value, message):
  raise ValueError("ArgParser environment: " + message) unless value

def options(parser):
  parser.arg("--count", type: Int, required: true, env: "SPUTNIK_ARGPARSER_TEST_COUNT")
  parser.arg("--text", env: "SPUTNIK_ARGPARSER_TEST_TEXT")
  parser.arg("--empty", required: true, env: "SPUTNIK_ARGPARSER_TEST_EMPTY")
  parser.flag("--enabled", env: "SPUTNIK_ARGPARSER_TEST_ENABLED")
  parser

def main():
  parsed = options(ArgParser(cmdline: [])).parse_or_raise()
  check(parsed[:count] == 42, "required typed option uses process environment")
  check(parsed[:text] == "ключ=значение", "Unicode and embedded equals remain exact")
  check(parsed[:empty] == "", "an explicitly empty environment value is present")
  check(parsed[:enabled] == true, "process environment uses the Bool converter")
  check(not parsed.has_key?("SPUTNIK_ARGPARSER_TEST_UNDECLARED"), "undeclared environment values are not returned")
  cli = options(ArgParser(cmdline: ["--count", "7", "--empty", "cli"])).parse_or_raise()
  check(cli[:count] == 7 and cli[:empty] == "cli", "explicit command line precedes environment")
  isolated = ArgParser(cmdline: [], env: {})
  isolated.arg("--count", type: Int, default: 99, env: "SPUTNIK_ARGPARSER_TEST_COUNT")
  check(isolated.parse_or_raise()[:count] == 99, "explicit empty map excludes the process environment")
  supplied = ArgParser(cmdline: [], env: {"SPUTNIK_ARGPARSER_TEST_COUNT": "9"})
  supplied.arg("--count", type: Int, env: "SPUTNIK_ARGPARSER_TEST_COUNT")
  supplied.arg("--text", env: "SPUTNIK_ARGPARSER_TEST_TEXT", default: "isolated")
  check(supplied.parse_or_raise()[:count] == 9 and supplied.parse_or_raise()[:text] == "isolated", "explicit map replaces rather than merges host values")
  defaults = ArgParser(cmdline: [], env: {})
  defaults.arg("--count", type: Int, default: 5, env: "SPUTNIK_ARGPARSER_TEST_COUNT")
  check(defaults.parse_or_raise()[:count] == 5, "missing environment falls back to default")
  print "PASS ArgParser process environment, CLI precedence and explicit isolation"
