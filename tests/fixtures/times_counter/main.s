export main

def check(value): raise ValueError("times counter regression") unless value

def first_index():
  9223372036854775807.times |i|:
    return i
  -1

def main():
  indices = 5.times
  check(not (Array === indices))
  check(indices.array == [0, 1, 2, 3, 4])
  check(indices.to_array() == [0, 1, 2, 3, 4])
  check(indices.each == [0, 1, 2, 3, 4])
  seen = []
  materialized = indices.each |i|:
    seen.push!(i)
    i * 10
  check(materialized == [0, 1, 2, 3, 4] and seen == materialized)
  check(indices.array == [0, 1, 2, 3, 4])
  mapped = indices.map |i|: i * 2
  check(mapped == [0, 2, 4, 6, 8])
  threaded = indices.threaded(1).each |i|: i * 10
  parallel = indices.parallel(1).each |i|: i * 10
  check(threaded == [0, 1, 2, 3, 4] and parallel == threaded)
  check(0.times.array == [] and (-3).times.to_array() == [])
  check(0.times.each == [] and (-3).times.each == [])
  empty_calls = 0
  zero = 0.times: empty_calls = empty_calls + 1
  negative = (-3).times: empty_calls = empty_calls + 1
  check(zero == null and negative == null and empty_calls == 0)
  limit = 5
  seen = []
  result = limit.times |i|:
    limit = 0
    if i == 2:
      next
    seen.push!(i)
  check(result == null and seen == [0, 1, 3, 4])
  huge = 9223372036854775807.times
  check(not (Array === huge) and first_index() == 0)
  finalized = []
  failed = try:
    9223372036854775807.times |i|:
      try:
        raise ValueError("stop")
      ensure:
        finalized.push!(i)
    false
  rescue ValueError:
    true
  check(failed and finalized == [0])
  print "PASS times counter and explicit materialization"
  0

main()
