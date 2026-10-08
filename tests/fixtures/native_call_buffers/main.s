package test.call_buffers

export main

native def sum12(a, b, c, d, e, f, g, h, i, j, k, l) from "buffers.sum"
native def _churn(items, text, callback) from "buffers.churn"
native def fail() from "buffers.fail"
native def list_at(items, index) from "buffers.list_at"
native def new_counter(value) from "buffers.counter_new"
native def counter_reclaims() from "buffers.counter_reclaims"

native class Counter from "buffers.Counter.long.native.tag" collected:
  def read() from "buffers.counter_read"
  def leaf_probe() from "buffers.leaf_probe"
  def leaf_fail() from "buffers.leaf_fail"
  def destroy!() from "buffers.counter_free"

def check(value): raise ValueError("call buffer regression") unless value

def keep(&block): block

def churn(items, text, &block): _churn(items, text, block)

def defaults(first, second: first + sum12(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12)):
  second

def keyword9(a: 1, b: 2, c: 3, d: 4, e: 5, f: 6, g: 7, h: 8, i: 9):
  [a, b, c, d, e, f, g, h, i]

def scalar_defaults(a: null, b: false, c: true, d: 0.25, e: "word", f: :token):
  [a, b, c, d, e, f]

def mutable_default(items: []):
  items.push!(42)
  items

def escaped_default(holder, callback: (|offset|: holder[0] + offset)): callback

def forwarded(first, *middle, last, enabled: true, **extras, &callback):
  check(enabled and extras[:offset] == 1)
  callback(first + middle.sum + last + extras[:offset])

class Reader:
  def init(@value)
  attr value from @value

class DerivedReader < Reader:
  def value(): @value + 1

class FrameOwner:
  def init(@value)
  attr value from @value

  def after_callback(&callback):
    callback()
    @value

  def escape(&callback):
    keep() |offset|: @value + callback(offset)

  def handled(&callback):
    try:
      callback()
      raise ValueError("frame owner probe")
    rescue ValueError:
      @value
    ensure:
      @value += 1

  def early(&callback):
    [1].each:
      callback()
      return @value
    0

class SingleArgument:
  def call(value): value

class DefaultArgument:
  def call(value, offset: 0): value + offset

def polymorphic_call(receiver): receiver.call(42)

def main():
  check(sum12(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12) == 78)
  check(defaults(4) == 82 and defaults(4, second: 42) == 42)
  check(keyword9() == [1, 2, 3, 4, 5, 6, 7, 8, 9])
  check(keyword9(a: 42, i: 100)[0] == 42 and keyword9(i: 100)[8] == 100)
  check(scalar_defaults() == [null, false, true, 0.25, "word", :token])
  first_default = mutable_default()
  first_default.push!(100)
  check(first_default == [42, 100] and mutable_default() == [42])
  default_callback = escaped_default([10])
  check(default_callback(32) == 42)
  forwarded_result = forwarded(1, 2, 3, 4, 5, offset: 1) |value|: value == 16
  check(forwarded_result)
  check(Reader([42]).value[0] == 42 and DerivedReader(41).value() == 42)
  # The invocation owns self/block even after a callback drops the caller's
  # reference; frames may borrow those slots without changing their lifetime.
  holder = [FrameOwner(42)]
  read_result = holder[0].after_callback:
    holder[0] = null
  check(read_result == 42 and holder[0] == null)
  holder[0] = FrameOwner(42)
  escaped_owner = holder[0].escape |offset|: offset
  holder[0] = null
  check(escaped_owner(0) == 42 and escaped_owner(1) == 43)
  owner = FrameOwner(42)
  holder[0] = owner
  handled_result = owner.handled:
    holder[0] = null
  check(handled_result == 42 and owner.value == 43)
  holder[0] = FrameOwner(42)
  early_result = holder[0].early:
    holder[0] = null
  check(early_result == 42 and holder[0] == null)
  # A cached site must change its parameter-shaping metadata with the receiver.
  receivers = [SingleArgument(), DefaultArgument()]
  100.times |index|: check(polymorphic_call(receivers[index % 2]) == 42)
  # ABI list access must still materialize lazy Unicode chars and return null
  # for missing items or a value that is not a list.
  chars = "Aя🙂".chars
  check(list_at(chars, 1) == "я" and list_at(chars, 2) == "🙂")
  check(chars == ["A", "я", "🙂"])
  check(list_at(chars, 3) == null and list_at(42, 0) == null)
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
  check(alias.leaf_probe == 42)
  caught_leaf = false
  try:
    alias.leaf_fail
  rescue ValueError:
    caught_leaf = true
  check(caught_leaf and alias.leaf_probe == 42)
  check(alias.destroy! and not alias.destroy! and counter_reclaims() == 1)
  caught_lifetime = try:
    returned[2].read
    false
  rescue LifetimeError:
    true
  check(caught_lifetime and counter_reclaims() == 1)
  print "PASS native call buffers"
  0
