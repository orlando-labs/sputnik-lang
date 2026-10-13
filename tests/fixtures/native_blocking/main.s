package test.blocking

from test.blocking.bindings import worker, inline_worker, batch, fail, context, cancel_wait, cancel_counts, Probe

export main

def check(value, message: "blocking declaration regression"):
  raise ValueError(message) unless value

def with_batch(&block): batch(block)

def return_from_batch():
  with_batch(): return 7
  0

def exercise():
  check(not inline_worker())
  check(worker())
  callback = worker
  check(callback())
  check(callback(*[]))
  probe = Probe()
  check(probe.constructed_on_worker)
  check(probe.worker)
  check(not probe.inline_worker())
  rescued = false
  restored = false
  try:
    fail()
  rescue ValueError:
    rescued = true
  ensure:
    restored = true
  check(rescued and restored, message: "worker fault rescue/ensure")
  rescued = false
  try:
    batch(| |: raise ValueError("callback fault"))
  rescue ValueError:
    rescued = true
  check(rescued, message: "callback fault rescue")
  check(return_from_batch() == 7, message: "callback non-local return")
  check(worker() and not inline_worker())
  check(batch(| |: worker() and inline_worker() and probe.worker))
  before = context()
  local = task.local()
  local.with(77):
    with_batch():
      task.sleep(0.01)
      check(context() == before and local.get() == 77, message: "FFI callback logical context")
      with_batch(): check(context() == before and local.get() == 77)
  check(context() == before and local.get() == null, message: "FFI scope restoration")
  true

def main():
  # CALL, CALL_SPREAD, native constructor and foreign SEND execute inside a
  # resumable VM task. The imported module also exercises linked code ids.
  child = task.spawn: exercise()
  check(child.wait())
  cancelled = task.async: cancel_wait()
  started = Time.monotonic
  until cancel_counts()[0] == 1:
    check((Time.monotonic - started).total_nanoseconds < 3000000000)
    task.sleep(0.001)
  check(cancelled.cancel())
  caught = try:
    cancelled.wait()
    false
  rescue CancelledError:
    true
  check(caught and cancelled.cancelled? and cancel_counts() == [1, 1], message: "cancel waits for foreign cleanup")
  print "PASS source blocking native dispatch"
  0
