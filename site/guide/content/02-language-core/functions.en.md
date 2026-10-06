---
id: functions
title: Functions, methods and classes
summary: def, one-liner form, @attr constructor fields, class methods and bare-nullary send.
category: language-core
order: 30
prerequisites: [overview]
related: [blocks, pattern-matching]
status: draft
spec_refs:
  - anchor: 8-функции-методы-классы-и-объектная-модель
    label: Spec §8
---

# Functions, methods and classes

## `def`

A function is declared with `def`. Both multi-line and one-liner forms are
supported; the body is an expression, and its value becomes the result:

```amber
def add(a, b):
  a + b

def double(x): x * 2

def probe():
  add(double(3), 4)

probe()
# => 10
```

## Classes and constructor fields

`class` declares a type. Writing `@attr` directly in the `init` signature
declares an instance field on the spot — no manual argument shuffling:

```amber
class Collection:
  def init(@count): pass

  def size():
    @count

def probe():
  items = Collection(40)
  items.size()

probe()
# => 40
```

A class object is called as a constructor: `Collection(40)` builds an instance
and assigns `@count`.

## Bare-nullary send

A method that takes no arguments can be called without parentheses — a
**bare-nullary send**. The forms `items.size` and `items.size()` are
equivalent. The same holds for class methods declared with `class_method`:

```amber
class Collection:
  def init(@count): pass
  def size(): @count

class Build:
  class_method def version():
    20

def probe():
  items = Collection(40)
  items.size + items.size() + Build.version + Build.version()

probe()
# => 120
```

> [!note]
> Bare-nullary send works for methods, fields and class methods. Parentheses are
> only needed when you pass arguments or a block.

Next: dispatching on the shape of arguments is
[pattern matching](guide:pattern-matching) and multi-clause `def`.
