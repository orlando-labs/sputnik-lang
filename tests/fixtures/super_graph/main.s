package test.super_graph
from test.super_base import Parent
export main

class Child < Parent:
  def init(value): super(value + 2)
  def adjusted(offset: 1): super(offset: offset) * 3

class Grandchild < Child:
  def init(value): super(value * 2)

def main():
  child = Grandchild(5)
  raise ValueError("linked lexical constructor") unless child.value == 12
  raise ValueError("linked inherited method") unless child.adjusted(offset: 2) == 42
  print "PASS linked super"
  0
