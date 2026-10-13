package alias.main

import alias.provider as kernels
import alias.other
import alias.facade as facade
from alias.provider import add as imported_add

export main

def assert(value):
  raise ValueError("module alias regression") unless value

def invoke(mod, n):
  mod.add(n, extra: 5)

def main():
  assert(kernels.add(1) == 11)
  assert(kernels.add(1, extra: 4) == 12)
  assert(kernels.plus(1, extra: 4) == 12)
  assert(other.add(6) == 12)
  assert(invoke(kernels, 2) == 14)
  assert(kernels.add(*[1], **{extra: 4}) == 12)
  assert(kernels.sum(35) == 42)
  assert(kernels.choose(0) == 42 and kernels.choose(41) == 41)
  stored = [kernels][0]
  assert(stored.choose(0) == 42 and stored.choose(41) == 41)
  assert(stored.add(3) == 13)
  callable = stored.add
  assert(callable(4, extra: 1) == 12)
  assert(callable == imported_add)
  assert(facade.add == callable)
  assert(facade.Box == kernels.Box)
  assert(kernels.Box.new(42).value == 42)
  result = kernels.apply(2) |n|: n * 3
  assert(result == 27)
  assert(stored == kernels)
  captured = |n|: stored.add(n)
  assert(captured(32) == 42)
  try:
    stored._helper(1)
    assert(false)
  rescue NoMethodError:
    null
  try:
    stored.add = null
    assert(false)
  rescue ReadOnlyPropertyError:
    null
  try:
    stored.init(1)
    assert(false)
  rescue NoMethodError:
    null
  42
