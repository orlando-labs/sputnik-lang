package test.call_buffers_compat

export main

native def new_counter(value) from "buffers.counter_new"
native def counter_reclaims() from "buffers.counter_reclaims"

native class Counter from "buffers.Counter.long.native.tag" collected:
  def read() from "buffers.counter_read"
  def typed_value(value as Int) from "buffers.counter_value"
  def destroy!() from "buffers.counter_free"

def main():
  receiver = new_counter(7)
  result = receiver.typed_value(42)
  raise ValueError("ordinary fallback result") unless result.read == 42
  raise ValueError("ordinary fallback receiver") unless receiver.read == 7
  result.destroy!
  receiver.destroy!
  raise ValueError("ordinary fallback lifetime") unless counter_reclaims() == 2
  print "PASS resource leaf ordinary fallback"
  0
