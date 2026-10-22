package test.foreign_bridges

import pycall.math as math
import pycall
from rbcall.json import JSON as json
import rbcall

export main

def check(value):
  raise ValueError("combined bridge assertion") unless value

def main():
  check(json.generate([math.sqrt(81)]) == "[9.0]")
  numbers = rbcall.wrap([1, 4, 9])
  roots = numbers.map |x|: math.sqrt(x)
  check(roots.to_a == [1.0, 2.0, 3.0])
  fn = pycall.callback(|x|: json.generate([x]))
  check(fn(42) == "[42]")
  print("PASS both foreign runtimes")
  0
