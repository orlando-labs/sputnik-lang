package app.external
export main, command, empty, quotes

macro def source_of(value):
  return Ast.lift(value.source)

template "./args.cmd.sv"
def command(values, prefix: "--item=")

template("./empty.cmd.sv")
def empty()

template("./quotes.cmd.sv")
def quotes()

template("./page.text.sv")
def page(title as Str, values)

template("./raw.source.sv")
def source()

def check(value, message):
  raise ValueError("external template: " + message) unless value

def main():
  args = command(["a b", "$(evil);", ""])
  check(args.argv == ["echo", "--item=a b", "--item=$(evil);", "--item="], "opaque arguments and defaults")
  check(command(["x"], prefix: "value=").argv == ["echo", "value=x"], "ordinary keyword arguments")
  check(empty().argv == ["echo"], "empty input loop")
  check(quotes().argv == ["echo", "quoted"], "literal triple quotes are file content")
  check(page("Мир", [1, 2]) == "  Привет, Мир!\n  [1]\n  [2]\n  title\n", "layout, Unicode and lexical parameters")
  check(source() == "\"\"\"literal\"\"\" \#{1 + 2}", "external source and no final newline")
  print "PASS external templates"
  true
