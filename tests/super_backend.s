package test.super_backend
export main

class Base:
  def init(@value)
  def value(): @value
  def compute(n, factor: 2, &block): block((@value + n) * factor)
  def delayed(): "base"

class Child < Base:
  def init(value): super(value + 1)
  def value(): super() + 10
  def compute(n, factor: 2, &block): super(n, factor: factor, &block) + 1
  def delayed():
    | |: super()

class Grandchild < Child:
  def init(value): super(value * 2)

class DetailError < Exception:
  attr status
  attr message
  def init(@status, message): super(message)

class Root:
  def absent(): super()

class StaticBase:
  class_method def value(): 7
class StaticChild < StaticBase:
  class_method def value(): super() + 1

def main():
  obj = Grandchild(3)
  raise ValueError("lexical constructor dispatch") unless obj.value == 17
  calculated = obj.compute(1, factor: 3) |v|: v + 1
  raise ValueError("keywords and block") unless calculated == 26
  raise ValueError("lexical delayed super") unless Child(1).delayed.() == "base"
  raise ValueError("class side super") unless StaticChild.value == 8
  caught = false
  try: raise DetailError(409, "detail")
  rescue DetailError |e|: caught = e.status == 409 and e.message == "detail"
  raise ValueError("Exception initializer") unless caught
  caught = false
  try: Root().absent()
  rescue NoMethodError: caught = true
  raise ValueError("missing parent") unless caught
  print "PASS super backend"
  0
