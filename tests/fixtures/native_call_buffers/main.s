package test.call_buffers

export main

native def sum12(a, b, c, d, e, f, g, h, i, j, k, l) from "buffers.sum"
native def _churn(items, text, callback) from "buffers.churn"
native def fail() from "buffers.fail"
native def new_counter(value) from "buffers.counter_new"
native def counter_reclaims() from "buffers.counter_reclaims"

native class Counter from "buffers.Counter.long.native.tag" collected:
  def read() from "buffers.counter_read"
  def destroy!() from "buffers.counter_free"

def check(value): raise ValueError("call buffer regression") unless value

def keep(&block): block

def churn(items, text, &block): _churn(items, text, block)

def defaults(first, second: first + sum12(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12)):
  second

def main():
  check(sum12(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12) == 78)
  check(defaults(4) == 82 and defaults(4, second: 42) == 42)
  items = Array.of(64): $it
  text = "borrowed text survives arena growth and nested native calls"
  count = 0
  result = churn(items, text) |value|:
    count += 1
    value + sum12(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12)
  check(count == 1 and result[0] == items and result[1] == text and result[2] == 2094)
  # An escaped closure with more captures than the inline capture storage.
  a = 1
  b = 2
  c = 3
  d = 4
  e = 5
  callback = keep() |value|:
    count += 1
    a + b + c + d + e + value
  20.times |i|: check(callback(i) == 15 + i)
  check(count == 21)
  # Removing the caller's reference during a call must keep its captures alive.
  clearing = null
  clearing = keep() |value|:
    clearing = null
    a + b + c + d + e + value
  check(clearing(27) == 42 and clearing == null)
  caught = try:
    churn(items, text) |value|: fail()
    false
  rescue ValueError:
    true
  check(caught)
  check(sum12(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12) == 78)
  check(result[0][63] == 63 and result[1] == text and result[2] == 2094)
  # A collected handle returned by a nested callback survives both call arenas.
  counter = new_counter(42)
  returned = churn(items, text) |value|: counter
  alias = returned[2]
  counter = null
  check(alias.read == 42 and counter_reclaims() == 0)
  check(alias.destroy! and not alias.destroy! and counter_reclaims() == 1)
  caught_lifetime = try:
    returned[2].read
    false
  rescue LifetimeError:
    true
  check(caught_lifetime and counter_reclaims() == 1)
  print "PASS native call buffers"
  0
