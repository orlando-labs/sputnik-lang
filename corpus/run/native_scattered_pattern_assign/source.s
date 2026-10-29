def check(condition):
  raise ValueError("scattered pattern assignment regression") unless condition

def hold(&block): &block

def unpack(a, untouched, b):
  getter = hold(): (a, b)
  (a, b) = (b, a)
  [a, untouched, b, getter()]

def failure(a, untouched, b):
  try:
    (a, 8, b) = (30, 9, 40)
  rescue MatchError:
    null
  [a, untouched, b]

def original_rhs(a, untouched, b):
  (a, b) = a

def after_if(a, untouched, b):
  prepared = if a > 0:
    (b, a)
  else:
    (a, b)
  (a, b) = prepared
  [a, untouched, b]

def main():
  result = unpack(1, 2, 3)
  check(result == [3, 2, 1, (3, 1)])
  check(failure(1, 2, 3) == [1, 2, 3])
  check(original_rhs((1, 3), 2, 3) == (1, 3))
  check(after_if(1, 2, 3) == [3, 2, 1])
  42
