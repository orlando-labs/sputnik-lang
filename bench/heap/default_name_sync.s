# Repeated default evaluation leases a pooled child VM with unchanged names.
# Keep interning outside the measured region; verify each repeat's result.
2000.times |i|: "default-benchmark-#{i}".symbol

def seed(): 41
def defaulted(value = seed() + 1): value

value = 0
6.times |repeat|:
  started = Time.monotonic
  20000.times:
    value = defaulted()
  elapsed = (Time.monotonic - started).total_nanoseconds
  raise ValueError("default result changed") unless value == 42
  print "NAMES_BENCH #{elapsed}" if repeat > 0
