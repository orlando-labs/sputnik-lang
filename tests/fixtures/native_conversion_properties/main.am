package native_conversion_properties
export main

def check(value):
  if not value: raise ValueError("conversion property regression")

class Reduction:
  def mean(dim: 0, keepdim: false):
    if keepdim then dim + 1 else dim

def literal_case(value):
  case! value:
    when 0: 20
    when 1: 30
    else: 40

def main():
  values = [1, 2]
  alias = values.array
  alias.push!(3)
  check(values == [1, 2, 3])
  check(values.tuple == (1, 2, 3))
  check([0, *values, *(4, 5), *(6..7), *[], 8] == (0..8).array)
  check([*values[0...-1], 8] == [1, 2, 8])
  check(values[1..] == [2, 3])
  check(values.tuple[0...3] == values)
  check((1..3).array == values)
  check([1, 1, 2].set.count == 2)
  pairs = [("a", 7), ("b", 8)]
  check(pairs.map["b"] == 8)
  check("42".int == 42)
  check("2.5".float == 2.5)
  check(42.str == "42")
  check(Symbol === "token".symbol)
  check("false".bool == false and "true".bool == true)
  check(Reduction().mean(dim: 1, keepdim: true) == 2)
  check(literal_case(0) == 20 and literal_case(7) == 40)
  print "PASS native conversion properties"
  0
