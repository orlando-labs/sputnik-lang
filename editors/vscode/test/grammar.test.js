"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const oniguruma = require("vscode-oniguruma");
const textmate = require("vscode-textmate");

const extensionRoot = path.resolve(__dirname, "..");
const grammarPath = path.join(
  extensionRoot,
  "syntaxes",
  "amber.tmLanguage.json",
);

function onigLib() {
  return {
    createOnigScanner(patterns) {
      return new oniguruma.OnigScanner(patterns);
    },
    createOnigString(value) {
      return new oniguruma.OnigString(value);
    },
  };
}

async function loadGrammar() {
  const wasm = fs.readFileSync(
    require.resolve("vscode-oniguruma/release/onig.wasm"),
  );
  await oniguruma.loadWASM(
    wasm.buffer.slice(wasm.byteOffset, wasm.byteOffset + wasm.byteLength),
  );

  const registry = new textmate.Registry({
    onigLib: Promise.resolve(onigLib()),
    loadGrammar: async (scopeName) => {
      if (scopeName !== "source.amber") {
        return null;
      }
      return textmate.parseRawGrammar(
        fs.readFileSync(grammarPath, "utf8"),
        grammarPath,
      );
    },
  });
  return registry.loadGrammar("source.amber");
}

function tokenize(grammar, source) {
  let ruleStack = textmate.INITIAL;
  return source.split("\n").map((line) => {
    const result = grammar.tokenizeLine(line, ruleStack);
    ruleStack = result.ruleStack;
    return { line, tokens: result.tokens };
  });
}

function tokenFor(lines, lineIndex, text, occurrence = 0) {
  const { line, tokens } = lines[lineIndex];
  let from = 0;
  let index = -1;
  for (let count = 0; count <= occurrence; ++count) {
    index = line.indexOf(text, from);
    assert.notEqual(
      index,
      -1,
      `could not find ${JSON.stringify(text)} on line ${lineIndex + 1}`,
    );
    from = index + text.length;
  }
  const token = tokens.find(
    (candidate) =>
      candidate.startIndex <= index &&
      candidate.endIndex >= index + text.length,
  );
  assert.ok(
    token,
    `no single token covers ${JSON.stringify(text)} on line ${lineIndex + 1}`,
  );
  return token;
}

function assertScope(lines, lineIndex, text, scope, occurrence = 0) {
  const token = tokenFor(lines, lineIndex, text, occurrence);
  assert.ok(
    token.scopes.includes(scope),
    `${JSON.stringify(text)} on line ${lineIndex + 1} lacks ${scope}; got ${token.scopes.join(", ")}`,
  );
}

function assertNoScopePrefix(
  lines,
  lineIndex,
  text,
  scopePrefix,
  occurrence = 0,
) {
  const token = tokenFor(lines, lineIndex, text, occurrence);
  assert.ok(
    !token.scopes.some((scope) => scope.startsWith(scopePrefix)),
    `${JSON.stringify(text)} on line ${lineIndex + 1} unexpectedly has ${scopePrefix}; got ${token.scopes.join(", ")}`,
  );
}

async function main() {
  const grammar = await loadGrammar();
  assert.ok(grammar, "Amber grammar failed to load");

  const conditionals = tokenize(
    grammar,
    [
      "@bytes += if Bytes === data then data.count else data.to_str.bytesize",
      "then = 1",
      "if_ready? = 2",
    ].join("\n"),
  );
  assertScope(
    conditionals,
    0,
    "if",
    "keyword.control.conditional.amber",
  );
  assertScope(
    conditionals,
    0,
    "then",
    "keyword.control.conditional.amber",
  );
  assertScope(
    conditionals,
    0,
    "else",
    "keyword.control.conditional.amber",
  );
  assertScope(conditionals, 0, "Bytes", "entity.name.type.amber");
  assertScope(conditionals, 0, "===", "keyword.operator.amber");
  assertNoScopePrefix(conditionals, 1, "then", "keyword.control");
  assertNoScopePrefix(conditionals, 2, "if_ready?", "keyword.control");

  const logical = tokenize(
    grammar,
    "ready and enabled or fallback\nnot empty? and item in items",
  );
  assertScope(logical, 0, "and", "keyword.operator.logical.amber");
  assertScope(logical, 0, "or", "keyword.operator.logical.amber");
  assertScope(logical, 1, "not", "keyword.operator.logical.amber");
  assertScope(logical, 1, "in", "keyword.operator.relational.amber");

  const controls = tokenize(
    grammar,
    [
      "if true:",
      ' raise "nope"',
      "else:",
      " return null",
      "value = catch(:done):",
      " throw :done, 42",
    ].join("\n"),
  );
  assertScope(controls, 0, "if", "keyword.control.amber");
  assertScope(controls, 1, "raise", "keyword.control.exception.amber");
  assertScope(controls, 2, "else", "keyword.control.amber");
  assertScope(controls, 3, "return", "keyword.control.amber");
  assertScope(controls, 4, "catch", "keyword.control.exception.amber");
  assertScope(controls, 5, "throw", "keyword.control.exception.amber");

  const references = tokenize(
    grammar,
    [
      'get "/v1/items/:id", to: &Items#index # route handler',
      "ready = &liveness",
      "callable = &Users.find",
      "foo#bar",
      "foo # ordinary comment",
    ].join("\n"),
  );
  assertScope(
    references,
    0,
    "get",
    "entity.name.function.call.amber",
  );
  assertScope(
    references,
    0,
    "&",
    "keyword.operator.reference.amber",
  );
  assertScope(
    references,
    0,
    "Items",
    "entity.name.type.receiver.amber",
  );
  assertScope(
    references,
    0,
    "#",
    "punctuation.accessor.unbound.amber",
  );
  assertScope(
    references,
    0,
    "index",
    "entity.name.function.reference.amber",
  );
  assertNoScopePrefix(references, 0, "index", "comment");
  assertScope(
    references,
    0,
    "route handler",
    "comment.line.number-sign.amber",
  );
  assertScope(
    references,
    1,
    "liveness",
    "entity.name.function.reference.amber",
  );
  assertScope(
    references,
    2,
    "find",
    "entity.name.function.reference.amber",
  );
  assertNoScopePrefix(references, 3, "#", "comment");
  assertScope(
    references,
    4,
    "ordinary comment",
    "comment.line.number-sign.amber",
  );

  const literals = tokenize(
    grammar,
    [
      "values = [42, 1_000, 1.5, 2e3, 0xFF, 0b1010, 0o755]",
      "state = :ready",
      "call(mode::fast)",
      "last = $_",
      "items.map: _1 + _2",
      "case value:",
      " when _:",
      "  null",
    ].join("\n"),
  );
  assertScope(
    literals,
    0,
    "42",
    "constant.numeric.integer.decimal.amber",
  );
  assertScope(
    literals,
    0,
    "1_000",
    "constant.numeric.integer.decimal.amber",
  );
  assertScope(literals, 0, "1.5", "constant.numeric.float.amber");
  assertScope(literals, 0, "2e3", "constant.numeric.float.amber");
  assertScope(literals, 0, "0xFF", "constant.numeric.hex.amber");
  assertScope(literals, 1, "ready", "constant.other.symbol.amber");
  assertScope(literals, 2, "fast", "constant.other.symbol.amber");
  assertScope(literals, 3, "$_", "variable.language.last-value.amber");
  assertScope(
    literals,
    4,
    "_1",
    "variable.parameter.placeholder.amber",
  );
  assertScope(literals, 6, "_", "variable.language.wildcard.amber");

  const operators = tokenize(
    grammar,
    [
      "total += 1",
      "a ** b // c % d <=> e",
      "a =~ pattern or a !~ pattern",
      "a << 2 >> 1",
      "first..last",
      "first...last",
      "value.?.member()",
      "items?[0]",
      "def <=>(other): 0",
      "def []=(key, value): value",
    ].join("\n"),
  );
  assertScope(operators, 0, "+=", "keyword.operator.amber");
  for (const operator of ["**", "//", "%", "<=>"]) {
    assertScope(operators, 1, operator, "keyword.operator.amber");
  }
  assertScope(operators, 2, "=~", "keyword.operator.amber");
  assertScope(operators, 2, "!~", "keyword.operator.amber");
  assertScope(operators, 3, "<<", "keyword.operator.amber");
  assertScope(operators, 3, ">>", "keyword.operator.amber");
  assertScope(operators, 4, "..", "keyword.operator.range.amber");
  assertScope(operators, 5, "...", "keyword.operator.range.amber");
  assertScope(
    operators,
    6,
    ".?.",
    "keyword.operator.navigation.safe.amber",
  );
  assertScope(
    operators,
    6,
    "member",
    "entity.name.function.call.member.amber",
  );
  assertScope(
    operators,
    7,
    "?",
    "keyword.operator.optional-access.amber",
  );
  assertScope(operators, 8, "<=>", "entity.name.function.amber");
  assertScope(operators, 9, "[]=", "entity.name.function.amber");

  const declarations = tokenize(
    grammar,
    [
      "native def read(fd as Int) -> Bytes",
      'native class Handle from "ext.Handle" owned:',
      "prop name:",
      " get: @name",
      " set(value): @name = value",
      "attr var email from @raw_email",
      "pass = noop",
      "import app.models as models",
      "next item",
      "next = item",
    ].join("\n"),
  );
  assertScope(declarations, 0, "native", "storage.modifier.native.amber");
  assertScope(declarations, 0, "read", "entity.name.function.amber");
  assertScope(
    declarations,
    0,
    "as",
    "keyword.operator.type.annotation.amber",
  );
  assertScope(declarations, 1, "Handle", "entity.name.type.amber");
  assertScope(declarations, 1, "owned", "storage.modifier.ownership.amber");
  assertScope(declarations, 2, "prop", "storage.type.property.amber");
  assertScope(
    declarations,
    2,
    "name",
    "entity.name.function.property.amber",
  );
  assertScope(declarations, 3, "get", "storage.modifier.property.amber");
  assertScope(declarations, 4, "set", "storage.modifier.property.amber");
  assertScope(declarations, 5, "var", "storage.modifier.property.amber");
  assertNoScopePrefix(declarations, 6, "pass", "keyword");
  assertNoScopePrefix(declarations, 6, "noop", "keyword");
  assertScope(
    declarations,
    7,
    "as",
    "keyword.operator.type.annotation.amber",
  );
  assertScope(declarations, 8, "next", "keyword.control.amber");
  assertNoScopePrefix(declarations, 9, "next", "keyword.control");

  const strings = tokenize(
    grammar,
    [
      'message = "hello #{if ok then {name: user.name} else null}"',
      'x = r"\\d+#not-a-comment"',
      'query = sql"""',
      " SELECT * FROM users WHERE id = #{id}",
      ' """',
    ].join("\n"),
  );
  assertScope(strings, 0, "message", "source.amber");
  assertScope(
    strings,
    0,
    "then",
    "keyword.control.conditional.amber",
  );
  assertScope(strings, 1, "r", "entity.name.function.macro.string-tag.amber");
  assertNoScopePrefix(strings, 1, "#not-a-comment", "comment");
  assertScope(
    strings,
    2,
    "sql",
    "entity.name.function.macro.string-tag.amber",
  );
  assertScope(strings, 3, "id", "meta.interpolation.amber", 1);

  const macros = tokenize(
    grammar,
    [
      "macro def assert(check):",
      " if not #{check}:",
      '  raise "failed"',
      "# outside macro",
    ].join("\n"),
  );
  assertScope(macros, 0, "macro", "storage.modifier.macro.amber");
  assertScope(macros, 0, "assert", "entity.name.function.macro.amber");
  assertScope(macros, 1, "#{", "punctuation.section.embedded.begin.amber");
  assertNoScopePrefix(macros, 1, "#{", "comment");
  assertScope(macros, 3, "outside macro", "comment.line.number-sign.amber");

  console.log("grammar tests: ok");
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
