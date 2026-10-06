# Changelog

## Unreleased

- Align syntax highlighting with the current language grammar, including inline
  `if ... then ... else`, logical word operators, callable references, current
  symbolic operators, properties, symbols, special variables, tagged text blocks,
  and contextual keywords.
- Treat `#` as a comment opener only at the start of a line or after whitespace,
  preserving `&Type#method` unbound instance-method references.
- Distinguish decimal integers from floats and stop classifying `pass` / `noop` as
  keywords.
- Add TextMate/Oniguruma regression tests for the language grammar.

## 0.1.0

- Initial release.
- Syntax highlighting for Sputnik (`.s`): keywords, `@`/`@@` variables, numeric literals,
  single/double-quoted strings with `#{ }` interpolation, comments, `def`/`class`/`mixin`
  definitions, and operators.
- Language configuration: `#` comments, bracket matching, auto-closing pairs, and
  indentation for `:`-led blocks.
- **Sputnik: Run File** and **Sputnik: Build File** commands and `sputnik` task type.
