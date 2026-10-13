package alias.provider

export add, apply, sum, choose, Box
export add as plus

offset = 7

def _helper(n):
  n + offset

def add(n, extra: offset - 4):
  _helper(n) + extra

def apply(n, &block):
  block(_helper(n))

def sum(n):
  _helper(n)

def choose(0):
  offset * 6

def choose(n) if n > 0:
  _helper(n) - offset

class Box:
  def init(@value):
    null

  def value():
    @value
