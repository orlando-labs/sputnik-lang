package conformance.run

export probe, main

def check(condition, message): raise ValueError(message) unless condition

def intern(text, symbol = text.symbol): symbol
def keyword(text, symbol: text.symbol): symbol

def dependent(text, symbol = intern(text), following = (symbol.str + "-next").symbol):
  [symbol, following]

def nested(text, values = dependent(text)): values
def echo(value:): value

class SymbolBox:
  attr value
  def init(text, @value = text.symbol)

class SymbolProperty:
  def init(@text)
  def resolve(value = @text.symbol): value
  prop value: resolve

class SymbolClause:
  def resolve(text, symbol = text.symbol): symbol

  def value(text):
    when Str:
      resolve(text)

class SymbolKeywords:
  def init(@text)
  def resolve(value = @text.symbol): value
  prop kwargs: {value: resolve}

def probe():
  saved = []
  expected = []
  12.times |i|:
    text = "default-#{i}"
    value = intern(text)
    check(value.str == text, "positional default exports symbol")
    check(keyword("keyword-#{i}").str == "keyword-#{i}", "keyword default exports symbol")
    check(SymbolBox("constructor-#{i}").value.str == "constructor-#{i}", "constructor default exports symbol")
    values = nested("nested-#{i}")
    check(values[0].str == "nested-#{i}" and values[1].str == "nested-#{i}-next", "nested and dependent defaults share names")
    check(SymbolProperty("property-#{i}").value.str == "property-#{i}", "property preserves default names")
    check(SymbolClause().value("clause-#{i}").str == "clause-#{i}", "clause preserves default names")
    check(echo(**SymbolKeywords("spread-#{i}")).str == "spread-#{i}", "spread protocol preserves default names")
    check(intern("unused", value) == value, "supplied argument skips default")
    saved.push!(value)
    expected.push!(text)
    # Parent interning after a default must not reuse an escaped child's id.
    other = "parent-#{i}".symbol
    check(other != value and other.str == "parent-#{i}", "parent and child names keep distinct ids")

  texts = saved.map |value|: value.str
  check(texts == expected, "escaped symbols survive pooled reuse")
  true

def main():
  probe()
  print "PASS default runtime names"
  0
