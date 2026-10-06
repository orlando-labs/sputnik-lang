# Sputnik for Visual Studio Code

Editor support for the [Sputnik](https://github.com/orlando-labs/amber-lang) programming language (`.s`, `.spu`, and `.sputnik` files).

## Features (v1)

- **Syntax highlighting** — control/declaration keywords (including contextual `then`,
  `as`, and `next` forms), callable references (`&fn`, `&Type.method`, `&Type#method`),
  properties, instance/class variables (`@x`, `@@x`), placeholders and `$_`, symbols,
  numeric forms, operators, strings and tagged text blocks. Double-quoted strings support
  nested `#{ }` interpolation. Unicode identifiers (e.g. `α`, `@масса`) are supported.
- **Run / Build tasks** — run or compile the current file via `sputnik`:
  - **Sputnik: Run File** — `sputnik <file>` (output streams to the integrated terminal).
  - **Sputnik: Build File** — `sputnik build <file> -o <outDir>/<stem> --target <target>`.
  - Both are also exposed as VS Code tasks (type `sputnik`) so you can bind keys or use
    *Tasks: Run Task*.

Diagnostics, outline, hover, go-to-definition and rename are planned follow-ups — the Sputnik
compiler already emits the JSON needed for them (`sputnik.diag.v1`, `sputnik.agent_tooling.v1`,
`sputnik.explain.v1`).

## Requirements

The `sputnik` compiler must be available. The extension resolves it in this order:

1. `sputnik.compilerPath` setting (default `"sputnik"`), if it is absolute or found on `PATH`.
2. Fallback to `<workspace>/build/sputnik` (the in-repo build output).

Build it from the repository root with `make build`.

## Settings

| Setting | Default | Description |
| --- | --- | --- |
| `sputnik.compilerPath` | `"sputnik"` | Path to the `sputnik` executable. |
| `sputnik.build.target` | `"native"` | `--target` for `sputnik build`. |
| `sputnik.build.outDir` | `"build"` | Output directory for build artifacts. |
| `sputnik.run.args` | `[]` | Extra args appended to `sputnik <file>`. |

## Developing

```sh
cd editors/vscode
npm install
npm run compile      # or: npm run watch
npm test             # TextMate/Oniguruma grammar regression tests
```

Press **F5** to launch an Extension Development Host. Package a `.vsix` with
`npx vsce package`, then install via `code --install-extension sputnik-lang-*.vsix`.
