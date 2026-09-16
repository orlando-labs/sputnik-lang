# Amber for Vim

Syntax highlighting and filetype detection for Amber source files (`.am`).
Works as a native runtime package in Vim 8+ and Neovim, without external
dependencies or a compiler installation.

The rules follow the VS Code grammar in `../vscode/syntaxes/amber.tmLanguage.json`:
keywords and contextual declarations, methods and callable references (`&fn`,
`&Type.method`, `&Type#method`), properties, `@instance` / `@@class` variables,
placeholders, symbols, numeric literals, operators, Unicode identifiers,
strings, tagged text blocks, nested interpolation and macro splices. Tagged
single-quoted strings also interpolate; raw `r` strings do not.

Highlight groups link to standard Vim groups, so the active colorscheme controls
the colors. The filetype plugin sets `comments` and `commentstring` for `#`
comments and leaves indentation settings to your configuration.

## Installation

From the repository root, install a Vim package:

```sh
mkdir -p ~/.vim/pack/amber/start
ln -s "$(pwd)/editors/vim" ~/.vim/pack/amber/start/amber
```

For Neovim, use its data directory instead (default path shown):

```sh
mkdir -p ~/.local/share/nvim/site/pack/amber/start
ln -s "$(pwd)/editors/vim" ~/.local/share/nvim/site/pack/amber/start/amber
```

Enable syntax and filetype plugins in your `vimrc` (or Neovim `init.vim`):

```vim
syntax enable
filetype plugin on
```

Restart the editor and open a `.am` file. `:set filetype?` should report
`filetype=amber`; `:set syntax?` should report `syntax=amber`.
For a buffer without the `.am` extension, use `:setfiletype amber`.
The detector overrides Vim's built-in `.am` mapping to ELF and preserves the
Automake detection for `Makefile.am`.

Alternatively, add the package directory to `runtimepath` before enabling
filetype detection:

```vim
set runtimepath+=/absolute/path/to/amber-lang/editors/vim
syntax enable
filetype plugin on
```

## Verification

Run the headless regression checks from the repository root:

```sh
vim -Nu NONE -i NONE -n -es -S editors/vim/test/syntax.vim
# Or with Neovim:
nvim --headless -u NONE -n -S editors/vim/test/syntax.vim
```

The checks inspect actual syntax groups, interpolation boundaries, contextual
words, filetype detection, colorscheme links and filetype plugin cleanup. A
failure prints diagnostics and exits with a nonzero status.
