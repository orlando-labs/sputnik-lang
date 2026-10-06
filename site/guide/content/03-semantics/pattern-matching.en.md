---
id: pattern-matching
title: Pattern matching
summary: case/when on types, literals, ranges and map destructuring.
category: semantics
order: 40
prerequisites: [functions]
related: [blocks]
status: draft
spec_refs:
  - anchor: 9-pattern-matching-v1
    label: Spec §9
  - anchor: 10-multi-clause-def
    label: Spec §10
---

# Pattern matching

`case` is an expression: it tests a value against `when` clauses top to bottom
and returns the body of the first that matches. Destructuring, guards and type
matchers are language semantics, not a protocol built on top of `if`.

## Type matchers

A type name in `when` matches when the value is of that type:

```amber
def label(v):
  case v:
    when Int:
      "int"
    when Str:
      "str"
    when Float:
      "float"
    else:
      "other"

def probe():
  "#{label(5)}:#{label("hi")}:#{label(1.5)}:#{label(true)}"

probe()
# => "int:str:float:other"
```

## Clause order and literals

Clauses are tested in order, so a narrower matcher goes higher. Literals and
ranges are also valid in `when`:

```amber
def classify(n):
  case n:
    when 1..5:
      "low"
    when Int:
      "int-arm"
    else:
      "other"

def probe():
  "#{classify(3)}:#{classify(7)}"

probe()
# => "low:int-arm"
```

`classify(3)` falls into the `1..5` range, while `7` reaches the general `Int`.

## Map destructuring

`when` can take a map apart by named keys and bind them to fresh names. This
works together with name-indifferent access (`:key` and `"key"`):

```amber
def probe():
  p = Json.parse("{\"user_id\": 42, \"name\": \"Ada\"}")
  case p:
    when {user_id: id, name: n}:
      "#{n}##{id}"
    else:
      "no-match"

probe()
# => "Ada#42"
```

> [!tip]
> A type matcher also matches the type itself: `case Int: when Int:` is true.
> That is handy when you dispatch on type values.

A related topic is how blocks drive filtering and projection:
[blocks and block suffix](guide:blocks).
