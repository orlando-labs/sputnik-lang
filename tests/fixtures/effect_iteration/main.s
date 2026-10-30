export main

def check(value): raise ValueError("effect iteration regression") unless value

class TripleContainer:
  def each(&blk):
    blk(10, 20, 30)
    blk(40, 50, 60)
    99

class DeferredContainer:
  def init(): @blocks = []
  def each(&blk): @blocks.push!((&blk))
  def fire():
    @blocks.each |blk|: blk(10)

class MissingContainer:
  def method_missing(selector, *args): selector

class StaticContainer:
  class_method def each(&blk): blk(42)

def early_return():
  for x in (0..):
    return x + 42
  -1

def return_from_ensure(finalized):
  for x in [1]:
    try:
      try:
        break
      ensure:
        return 42
    ensure:
      finalized.push!(43)
  -1

def main():
  seen = []
  result = for x in (0..15:2):
    seen.push!(x)
  check(result == null and seen == [0, 2, 4, 6, 8, 10, 12, 14])
  seen = []
  result = for x with_index i in (1...100500:42):
    seen.push!((x, i))
    if i == 2: break
  check(result == null and seen == [(1, 0), (43, 1), (85, 2)])
  seen = []
  for k, v with_index i in {a: 10, b: 20}:
    seen.push!((k, v, i))
  check(seen == [(:a, 10, 0), (:b, 20, 1)])
  seen = []
  for (a, b) in [(1, 2), (3, 4)]:
    seen.push!(a + b)
  check(seen == [3, 7])
  seen = []
  result = for a, b, c with_index i in TripleContainer():
    seen.push!((a, b, c, i))
    break
  check(result == null and seen == [(10, 20, 30, 0)])
  finalized = []
  result = 9223372036854775807.times |i|:
    try:
      break
    ensure:
      finalized.push!(i)
  check(result == null and finalized == [0])
  seen = []
  result = (0..).for_each |x|:
    if x == 1: next
    seen.push!(x)
    if x == 3: break
  check(result == null and seen == [0, 2, 3])
  check(early_return() == 42)
  seen = []
  for x in [1, 2]:
    inner = loop:
      break x
    seen.push!(inner)
  check(seen == [1, 2])
  check(([10, 20, 30].find |x|: x > 10) == 20)
  check(([10, 20, 30].find_index |x|: x > 10) == 1)
  check(([10, 20, 30].find_with_index |x|: x > 10) == (20, 1))
  check(([10, 20, 30].find.with_index |x, i|: i == 2) == 30)
  check(([10, 20, 30].find_index.with_index |x, i|: i == 2) == 2)
  check(([10, 20, 30].find_with_index.with_index |x, i|: i == 2) == (30, 2))
  check(([null].find_with_index |x|: true) == (null, 0))
  check(([1].find_with_index |x|: false) == null)
  check(((0..).find_with_index |x|: x == 5) == (5, 5))
  check((([0, 1, 2, 3, 4, 5, 6].lazy.select |x|: x % 2 == 0).find_with_index.with_index |x, i|: i == 3) == (6, 3))
  check(([].for_each |x|: raise ValueError("empty iterator called block")) == null)
  evaluations = 0
  get_items = ||:
    evaluations = evaluations + 1
    [1, 2]
  x = 99
  for x in get_items(): null
  check(x == 99 and evaluations == 1)
  seen = []
  for round in [1, 2]:
    for x with_index i in [10, 20]:
      seen.push!((round, x, i))
      break
  check(seen == [(1, 10, 0), (2, 10, 0)])
  seen = []
  for outer in [1, 2]:
    for inner in [1, 2]:
      seen.push!((outer, inner))
      break
  check(seen == [(1, 1), (2, 1)])
  finalized = []
  failed = try:
    for x in [1]:
      try:
        break
      ensure:
        finalized.push!(x)
        raise ValueError("ensure overrides break")
    false
  rescue ValueError:
    true
  check(failed and finalized == [1])
  seen = []
  result = TripleContainer().for_each |a, b, c|:
    seen.push!(a + b + c)
  check(result == null and seen == [60, 150])
  seen = []
  [10, 20].each.with_index |x, i|: seen.push!((x, i))
  check(seen == [(10, 0), (20, 1)])
  check(([10, 20].map.with_index |x, i|: x + i) == [10, 21])
  predicate = |x, i|: i == 1
  check([10, 20].find.with_index(&predicate) == 20)
  calls = 0
  result = [10, 20, 30].find_with_index |x|:
    calls = calls + 1
    x == 20
  check(result == (20, 1) and calls == 2)
  result = [1, 2].for_each |x|:
    try:
      try:
        break
      ensure:
        finalized.push!(10)
    ensure:
      finalized.push!(20)
  check(result == null and finalized == [1, 10, 20])
  saved = DeferredContainer()
  seen = []
  for round in [1, 2]:
    for x with_index i in saved: seen.push!(i)
  saved.fire()
  saved.fire()
  check(seen == [0, 0, 1, 1])
  seen = []
  for with_index in [10, 20]: seen.push!(with_index)
  check(seen == [10, 20])
  check((MissingContainer().for_each: null) == :for_each)
  seen = []
  result = StaticContainer.for_each |x|: seen.push!(x)
  check(result == null and seen == [42])
  finalized = []
  failed = try:
    for x in [1]:
      try:
        try:
          break
        ensure:
          raise ValueError("replace break")
      ensure:
        finalized.push!(42)
    false
  rescue ValueError:
    true
  check(failed and finalized == [42])
  finalized = []
  check(return_from_ensure(finalized) == 42 and finalized == [43])
  print "PASS effect iteration and indexed search"
  0

main()
