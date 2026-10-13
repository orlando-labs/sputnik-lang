export main

def check(value): raise ValueError("trivial method regression") unless value

class Source:
  def init(@value)
  def value(): @value
  def constant(): 42
  def identity(value): value
  def receiver(): self
  def missing(): @missing
  def enabled?(): true
  def nothing(): null
  def ratio(): 0.25

class Derived < Source:
  def constant(): 43

mixin IncludedValue:
  def constant(): 44

class Included < Source:
  include IncludedValue

class PropertySource:
  prop value: 42

class Forwarder:
  def init(@target)
  def value(): @target.value
  def explicit(): @target.value()
  def converted(): @target.array

class CallbackSource:
  def init(@callback)
  def value(): @callback()

class ConstructorHook:
  def init(@first, @second)
  def after_init!(): @seen = @first + @second
  def seen(): @seen

class ConstructorHookChild < ConstructorHook

class DefaultConstructor:
  def init(@value: 41)
  def value(): @value

class CapturedDefaultConstructor:
  def init(base, @callback = (|offset|: base + offset))
  def call(offset): @callback(offset)

class FailingConstructorHook:
  def init(@value)
  def after_init!(): raise ValueError(@value)

def constant(receiver): receiver.constant

def main():
  source = Source([42])
  check(source.value[0] == 42 and source.identity(source.value) == [42])
  check(source.receiver == source and source.missing == null)
  check(source.enabled? and source.nothing == null and source.ratio == 0.25)
  receivers = [source, Derived(0), Included(0)]
  100.times |index|:
    check(constant(receivers[index % 3]) == 42 + index % 3)
  check(Forwarder(source).value == [42])
  check(Forwarder(PropertySource()).value == 42)
  check(Forwarder((1, 2, 3)).converted == [1, 2, 3])
  check(Forwarder(source).explicit == [42])
  # A forwarded user call can clear the only caller reference to the wrapper.
  # Its logical self and target must survive until the complete invocation ends.
  holders = [null]
  callback = | |:
    holders[0] = null
    42
  holders[0] = Forwarder(CallbackSource(callback))
  check(holders[0].value == 42 and holders[0] == null)
  check(ConstructorHook(20, 22).seen == 42)
  check(ConstructorHookChild(21, 22).seen == 43)
  check(DefaultConstructor().value == 41 and DefaultConstructor(value: 42).value == 42)
  check(CapturedDefaultConstructor(40).call(2) == 42)
  hook_failed = try:
    FailingConstructorHook("hook runs after empty init")
    false
  rescue ValueError:
    true
  check(hook_failed)
  print "PASS trivial methods and forwarding"
  0

main()
