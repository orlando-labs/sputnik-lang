---
id: blocks
title: Blocks, lambdas and block suffix
summary: A block travels through a dedicated call channel and becomes an explicit closure.
category: language-core
order: 20
prerequisites: [overview]
related: [functions, pattern-matching]
status: draft
spec_refs:
  - anchor: 4-postfix-выражения-чейнинг-и-block-suffix
    label: Spec §4
  - anchor: 5-блоки-лямбда-аргументы-и-placeholders
    label: Spec §5
---

# Blocks, lambdas and block suffix

A block is a piece of code passed to a call through a **dedicated channel**,
not as a positional argument. This is a signature Amber trait: data pipelines
stay on one readable line, without callback forests.

## Block suffix

A block is written right after a call, with parameters in `|...|`:

```amber
def probe():
  xs = [0, 1, 2, 3]
  xs.filter_map |x|:
    if x % 2 == 0:
      x * 10
    else:
      null

probe()
# => [0, 20]
```

`filter_map` calls the block for each element; a `null` from the block drops the
element, any other value is kept. The block here is an ordinary expression with
a body.

## Lambda literals

The same block can be captured as a standalone value — a lambda `|params|: body`.
A lambda captures its lexical scope and is a first-class callable:

```amber
def probe():
  inc = |x|: x + 1
  add = |a, b|: a + b
  n = 100
  cap = |x|: x + n
  make = |k|: |x|: x * k
  triple = make(3)
  "#{inc(10)}:#{add(3, 4)}:#{cap(5)}:#{triple(7)}:#{(|x|: x * x)(6)}"

probe()
# => "11:7:105:21:36"
```

Here `cap` captures `n`, and `make` returns a lambda — a closure over `k`. A
leading `|` always starts a lambda, so the syntax is unambiguous and never
clashes with the infix bitwise `|`.

## Passing a block with `&`

A function accepts a block as an explicit `&blk` parameter and forwards it as
`&blk`. Inside the body `blk` is an ordinary callable; `blk == null` when no
block was passed:

```amber
def each(xs, &blk):
  i = 0
  n = xs.length
  while i < n:
    blk(xs[i])
    i = i + 1
  xs

def my_map(xs, &blk):
  out = []
  each(xs) |x|:
    out.push!(blk(x))
  out

def probe():
  doubled = my_map([1, 2, 3]) |x|: x * 2
  total = 0
  each(doubled) |x|:
    total = total + x
  "#{doubled[0]}:#{doubled[1]}:#{doubled[2]}:sum=#{total}"

probe()
# => "2:4:6:sum=12"
```

> [!note]
> `&blk` is a block as a value. Ordinary positional arguments and the block
> channel never mix: a call has exactly one block, and it always comes last.

Next: [functions, methods and classes](guide:functions) use blocks as the
building material for collection APIs.
