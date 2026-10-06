---
id: running
title: Running and building with sputnik
summary: How to run a program, build a native binary, and inspect the compilation stages.
category: toolchain
order: 15
prerequisites: []
related: [overview, functions]
status: draft
---

# Running and building with sputnik

`sputnik` is the Sputnik compiler and runner. One command executes a program;
another builds a standalone native executable. Here is the working loop, on
verified examples.

## Build the toolchain

The compiler and runtime build with a single `make`. You need `clang++` (the
default), on Linux or macOS; the compiler build has no external dependencies.

```sh
make build/sputnik     # build only sputnik
make build            # build the whole toolchain (sputnik, sputniktest, tests)
```

The binary lands at `build/sputnik`.

## Run a program

`sputnik <file.s>` executes the module and prints the value of the last
top-level expression in its canonical form (repr).

```sputnik
def greet(name):
  "Hello, #{name}!"

greet("Sputnik")
```

```sh
build/sputnik hello.s
# => "Hello, Sputnik!"
```

> [!note]
> The output string is shown quoted — that is the value's repr, exactly as the
> runtime prints it. An integer `42` prints as `42`, a string as `"..."`.

## Build a native executable

`sputnik build <file.s> -o <path>` compiles the program into a standalone native
binary. A JSON build report is printed to stdout, the generated C++ is saved
alongside (`<path>.native.cpp`), and the executable is written to the given path.

```sh
build/sputnik build hello.s -o hello
./hello
# => "Hello, Sputnik!"
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
build/sputnik build hello.s --target bytecode-wrapper -o hello.bc
```

## Multi-file project builds

A project uses an `sputnik.build.json` manifest describing sources, targets and a
cache. `sputnik build` accepts the manifest instead of a single file.

```sh
build/sputnik build sputnik.build.json \
  --out-dir build/out \
  --cache-dir build/cache
```

## Compilation stages and inspection tools

Useful for seeing what the syntax lowers into. Each stage is a subcommand that
prints that form of the program; they do not run it.

```sh
build/sputnik lex   source.s   # tokens
build/sputnik parse source.s   # parse tree
build/sputnik hir   source.s   # high-level IR (the explicit lowering shows here)
build/sputnik mir   source.s   # mid-level IR
build/sputnik bc    source.s   # bytecode
build/sputnik native-dump source.s   # the generated native code
```

`.sputnikbc` artifacts are data, not trusted code: a verifier checks their
structure before execution.

```sh
build/sputnik verify   module.sputnikbc --json   # verify bytecode
build/sputnik metadata module.sputnikbc --json   # read module metadata
```

## Run the conformance suite

The corpus of verified programs runs through `sputniktest`:

```sh
build/sputniktest run corpus              # the whole corpus
build/sputniktest run corpus --bundle M11 # a single bundle
```

Next: how the programs themselves are shaped — start with the
[language overview](guide:overview) and [functions and classes](guide:functions).
