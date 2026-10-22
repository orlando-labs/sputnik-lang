# Explicit superclass calls

Inside an instance method or a `class_method`, `super(args...)` calls the method
with the same name starting at the superclass of the method's lexical owner.
The receiver remains the original object (or class object). A further subclass
therefore does not change which parent an inherited method calls.

```sputnik
class Parent:
  def init(@value)
  def compute(n, scale: 2, &block): block((@value + n) * scale)

class Child < Parent:
  def init(value): super(value + 1)
  def compute(n, scale: 2, &block): super(n, scale: scale, &block) + 1
```

Arguments use ordinary parameter binding, including defaults, keywords, spreads,
and the explicit block channel. `super()` passes no arguments. Arguments and
blocks are not forwarded implicitly. A nested closure retains the enclosing
method's lexical owner after that method returns.

For an exception class directly inheriting the built-in `Exception`,
`super(message)` initializes `@message`; `super()` uses an empty string. The
initializer takes at most one positional argument and no keyword arguments or
block. Classes may instead initialize their own fields with
`def init(@status, @message)` and expose them with `attr`.

A missing superclass implementation raises `NoMethodError`. Calling `super`
outside an object method is rejected during lowering. The current contract is
superclass dispatch from class methods; mixin continuation is not specified.

HIR represents the lexical owner as an ordinary class reference so module graph
linking relocates it correctly. VM and direct native execution share the same
lookup and argument-binding rules, without a bytecode format change.

`tests/super_backend.s` covers constructors, deeper inheritance, keyword/block
forwarding, class methods, escaped closures, Exception initialization, and a
missing parent. `tests/fixtures/super_graph` covers linked module inheritance.
