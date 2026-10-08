package native.dispatch_cache

export main

class MethodValue:
  def value(): 42

class PropertyValue:
  def init(@value)
  attr value from @value

class RequiredValue:
  def value(value): value

class OptionalValue:
  def value(value, offset: 1): value + offset

# One explicit CALL/SEND site must preserve both the method and its flags.
# Interleaving another receiver's property must never turn this into a getter.
def explicit_value(receiver): receiver.value()

def argument_value(receiver): receiver.value(41)

def argument_worker(optional, iterations):
  receiver = if optional then OptionalValue() else RequiredValue()
  expected = if optional then 42 else 41
  failures = 0
  iterations.times:
    correct = try:
      argument_value(receiver) == expected
    rescue TypeError:
      false
    failures += 1 unless correct
  failures

def worker(method_receiver, iterations):
  receiver = if method_receiver then MethodValue() else PropertyValue(42)
  failures = 0
  iterations.times:
    correct = try:
      value = explicit_value(receiver)
      method_receiver and value == 42
    rescue TypeError:
      not method_receiver
    failures += 1 unless correct
  failures

def main():
  workers = Array.of(8) |index|:
    method_receiver = index % 2 == 0
    task.spawn: worker(method_receiver, 50000)
  failures = workers.map: $it.wait()
  total = failures.sum
  argument_workers = Array.of(8) |index|:
    optional = index % 2 == 0
    task.spawn: argument_worker(optional, 50000)
  argument_failures = argument_workers.map: $it.wait()
  total += argument_failures.sum
  print "dispatch failures #{total}"
  raise ValueError("native cache mixed method flags") unless total == 0
  print "PASS native dispatch cache"
  0
