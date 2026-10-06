---
id: overview
title: Language overview
summary: What Sputnik is, how to read this Guide, and how the knowledge graph works.
category: intro
order: 10
prerequisites: []
related: [blocks, functions]
status: draft
spec_refs:
  - anchor: 1-дизайн-якоря-языка
    label: Spec §1
---

# Language overview

Sputnik is a compact language for a no-GIL VM. It takes Python readability, Ruby
plasticity, pattern matching and modern data-flow syntax, then lowers them into
bytecode with an explicit lowering model: nice to write, predictable to compile.

This **Guide** is a teaching layer on top of the formal
[specification](spec:1-дизайн-якоря-языка). It uncovers the language step by
step, on real, verifiable examples. Every code block is a working Sputnik program,
and a `# =>` comment shows its actual result.

> [!note]
> The Guide is meant to become a self-contained document. For now it links into
> the formal spec through "Spec §N" chips — these are kept separate and will be
> removed once the Guide covers a topic in full.

## How to read the examples

The unit of code in Sputnik is the expression. `if`, `case`, loops and blocks all
return values, so programs often end with a result expression.

```sputnik
def probe():
  greeting = "Sputnik"
  "#{greeting}:#{2 + 3}"

probe()
# => "Sputnik:5"
```

String interpolation `#{...}` evaluates an expression and splices in its string
form. Note that the resulting string is shown quoted in the output — that is its
canonical representation (repr), exactly as the runtime prints it.

## The knowledge graph

The Guide is organized as a graph, not a linear book. Every page has:

- **prerequisites** — topics worth reading first;
- **related topics** — where to go next.

Start with blocks and functions — the load-bearing shapes of Sputnik syntax — then
move on to pattern matching.

> [!tip]
> The panel on the right shows the current page's prerequisites and related
> topics. That *is* the graph navigation: follow the edges, not a flat table of
> contents.
