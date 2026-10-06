---
id: running
title: Running and building with amberc
summary: How to run a program, build a native binary, and inspect the compilation stages.
category: toolchain
order: 15
prerequisites: []
related: [overview, functions]
status: draft
---

# Running and building with amberc

`amberc` is the Amber compiler and runner. One command executes a program;
another builds a standalone native executable. Here is the working loop, on
verified examples.

## Build the toolchain

The compiler and runtime build with a single `make`. You need `clang++` (the
default), on Linux or macOS; the compiler build has no external dependencies.

```sh
make build/amberc     # build only amberc
make build            # build the whole toolchain (amberc, ambertest, tests)
```

The binary lands at `build/amberc`.

## Run a program

`amberc <file.am>` executes the module and prints the value of the last
top-level expression in its canonical form (repr).

```amber
def greet(name):
  "Hello, #{name}!"

greet("Amber")
```

```sh
build/amberc hello.am
# => "Hello, Amber!"
```

> [!note]
> The output string is shown quoted — that is the value's repr, exactly as the
> runtime prints it. An integer `42` prints as `42`, a string as `"..."`.

## Build a native executable

`amberc build <file.am> -o <path>` compiles the program into a standalone native
binary. A JSON build report is printed to stdout, the generated C++ is saved
alongside (`<path>.native.cpp`), and the executable is written to the given path.

```sh
build/amberc build hello.am -o hello
./hello
# => "Hello, Amber!"
```

## Build targets and options

| Option | Values | Purpose |
| --- | --- | --- |
| `--target` | `native`, `native-debug`, `bytecode-wrapper` | Artifact form: native code, native with debug info, or a wrapper over bytecode. |
| `--entry` | `auto`, `init`, `main`, `main-only` | Which entry runs: module initialization and/or `main`. `auto` picks based on whether a `main` exists. |
| `--out-dir` | `<dir>` | Where to place artifacts. |
| `--grant` | `<cap[=target]>` | Grant a capability (e.g. `net.connect`, `fs.read`) to the built binary. |

```sh
# a bytecode wrapper instead of native code
build/amberc build hello.am --target bytecode-wrapper -o hello.bc
```

## Multi-file project builds

A project uses an `amber.build.json` manifest describing sources, targets and a
cache. `amberc build` accepts the manifest instead of a single file.

```sh
build/amberc build amber.build.json \
  --out-dir build/out \
  --cache-dir build/cache
```

## Compilation stages and inspection tools

Useful for seeing what the syntax lowers into. Each stage is a subcommand that
prints that form of the program; they do not run it.

```sh
build/amberc lex   source.am   # tokens
build/amberc parse source.am   # parse tree
build/amberc hir   source.am   # high-level IR (the explicit lowering shows here)
build/amberc mir   source.am   # mid-level IR
build/amberc bc    source.am   # bytecode
build/amberc native-dump source.am   # the generated native code
```

`.amberbc` artifacts are data, not trusted code: a verifier checks their
structure before execution.

```sh
build/amberc verify   module.amberbc --json   # verify bytecode
build/amberc metadata module.amberbc --json   # read module metadata
```

## Run the conformance suite

The corpus of verified programs runs through `ambertest`:

```sh
build/ambertest run corpus              # the whole corpus
build/ambertest run corpus --bundle M11 # a single bundle
```

Next: how the programs themselves are shaped — start with the
[language overview](guide:overview) and [functions and classes](guide:functions).
