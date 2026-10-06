package conformance.run

export probe, main

def check(condition, message):
  raise ValueError(message) unless condition

class Parent:
  def set_parent!(): @parent = 7

  def after_init!():
    @events.push!("child hook")
    @calls = if @calls == null then 1 else @calls + 1
    @snapshot = instance_fields
    999

class Child < Parent:
  attr calls
  attr snapshot

  def init(@events, @seed = 3):
    set_parent!
    @z = null
    @a = @seed + 1
    @extra = [1]
    @events.push!("child init")
    return 42

  def reset!(events): init(events, 9)
  def change!(): @a = 50

  prop expensive:
    raise ValueError("instance_fields invoked a getter")

class Outer:
  attr child

  def init(@events, factory):
    @child = factory(events)
    @events.push!("outer init")

  def after_init!():
    raise ValueError("child hook precedes parent hook") unless @child.calls == 1
    @events.push!("outer hook")

class Empty:
  def after_init!(): @ready = true

class AutoOnly:
  def init(@unread)

class Ensured:
  def init(@events):
    try:
      return 1
    ensure:
      @events.push!("ensure")

  def after_init!(): @events.push!("ensured hook")

class CustomFields:
  def instance_fields(): {custom: 12}

class RequiredHook:
  def after_init!(required): required

class Broken:
  def init(@events):
    @events.push!("broken init")
    raise ValueError("init failure")

  def after_init!(): @events.push!("unexpected hook")

class BrokenHook:
  def after_init!(): raise ValueError("hook failure")

class Clause:
  def init(0): @value = 10
  def init(4) if 4 > 0: @value = 4
  def after_init!(): @value += 1
  attr value

class Inherited < Child

def probe():
  events = []
  outer = Outer(events, Child)
  check(events == ["child init", "child hook", "outer init", "outer hook"], "construction order")
  child = outer.child
  check(child.calls == 1, "hook runs once")
  fields = child.snapshot
  check(fields.keys == ["a", "calls", "events", "extra", "parent", "seed", "z"], "sorted actual fields, inherited writes and overflow slots")
  check(fields["a"] == 4 and fields["parent"] == 7 and fields["z"] == null, "auto-assign and null values")
  check(not fields.has_key?("snapshot"), "snapshot does not see later field additions")
  fields["a"] = 100
  check(child.instance_fields["a"] == 4, "snapshot membership is independent")
  child.change!
  check(fields["a"] == 100 and child.instance_fields["a"] == 50, "snapshot values are captured")
  fields["extra"].push!(2)
  check(child.instance_fields["extra"] == [1, 2], "snapshot is shallow")
  child.reset!(events)
  check(child.calls == 1, "ordinary init send does not trigger lifecycle hook")
  check(Empty().instance_fields["ready"] == true, "hook without init")
  check(AutoOnly(8).instance_fields["unread"] == 8, "unread auto-assigned field names")
  check(CustomFields().instance_fields == {custom: 12}, "user member overrides field builtin")
  check(Child.new([], 5).snapshot["a"] == 6, "explicit new construction")
  check(Inherited([], 6).snapshot["a"] == 7, "inherited init and hook")
  check(Clause(0).value == 11 and Clause(4).value == 5, "clause constructors")
  caught = 0
  try:
    Broken(events)
  rescue ValueError |error|:
    caught += 1 if error.message == "init failure"
  check(events.last == "broken init", "failed init skips hook")
  try:
    BrokenHook()
  rescue ValueError |error|:
    caught += 1 if error.message == "hook failure"
  check(caught == 2, "constructor and hook exceptions are catchable")
  try:
    RequiredHook()
  rescue TypeError |error|:
    caught += 1
  check(caught == 3, "hook is invoked with zero arguments")
  cleanups = []
  Ensured(cleanups)
  check(cleanups == ["ensure", "ensured hook"], "initializer cleanup precedes hook")
  true

def main():
  probe()
  print "PASS after_init! + instance_fields"
  0
