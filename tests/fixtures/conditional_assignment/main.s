package conditional_assignment
export main

def check(condition):
  if not condition:
    raise ValueError("conditional assignment failed")

class Target:
  def init():
    @items = [null]
    @receivers = 0
    @indices = 0
    @defaults = 0
  attr items
  attr receivers
  attr indices
  attr defaults
  def receiver():
    @receivers += 1
    @items
  def index():
    @indices += 1
    0
  def fallback():
    @defaults += 1
    12

class Box:
  def init():
    @value = null
    @gets = 0
    @sets = 0
  attr gets
  attr sets
  prop value:
    get:
      @gets += 1
      @value
    set(v):
      @sets += 1
      @value = v
      999

class Bag:
  def init(): @values = {}
  def []?(key): @values[?key]
  def []=(key, value):
    @values[key] = value
    999

def main():
  a = null
  b = false
  check((a?=7) == 7)
  check((b ?= 8) == false)
  check((b or= 9) == 9)
  check((a or= 99) == 7)
  m = {flag: false, nil: null, zero: 0}
  check((m[:missing] ?= 10) == 10)
  check((m[:nil] ?= 11) == 11)
  check((m[:flag] ?= 99) == false)
  check((m[:flag] or= 13) == 13)
  check((m[:zero] or= 99) == 0)
  xs = [null, false, 0]
  check((xs[0] ?= 14) == 14)
  check((xs[1] ?= 99) == false)
  check((xs[-2] or= 15) == 15)
  check((xs[-1] or= 99) == 0)
  target = Target()
  target.receiver()[target.index()] ?= target.fallback()
  target.receiver()[target.index()] or= target.fallback()
  check(target.items == [12])
  check(target.receivers == 2 and target.indices == 2 and target.defaults == 1)
  box = Box()
  check((box.value ?= false) == false)
  check((box.value ?= 99) == false)
  check((box.value or= 16) == 16)
  check((box.value or= 99) == 16)
  check(box.gets == 4 and box.sets == 2)
  bag = Bag()
  check((bag[:answer] ?= 17) == 17)
  check((bag[:answer] or= 99) == 17)
  print "PASS conditional assignment"
  0
