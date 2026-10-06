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
  "sputnik.tmLanguage.json",
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
      if (scopeName !== "source.sputnik") {
        return null;
      }
      return textmate.parseRawGrammar(
        fs.readFileSync(grammarPath, "utf8"),
        grammarPath,
      );
    },
  });
  return registry.loadGrammar("source.sputnik");
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
  assert.ok(grammar, "Sputnik grammar failed to load");

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
    "keyword.control.conditional.sputnik",
  );
  assertScope(
    conditionals,
    0,
    "then",
    "keyword.control.conditional.sputnik",
  );
  assertScope(
    conditionals,
    0,
    "else",
    "keyword.control.conditional.sputnik",
  );
  assertScope(conditionals, 0, "Bytes", "entity.name.type.sputnik");
  assertScope(conditionals, 0, "===", "keyword.operator.sputnik");
  assertNoScopePrefix(conditionals, 1, "then", "keyword.control");
  assertNoScopePrefix(conditionals, 2, "if_ready?", "keyword.control");

  const logical = tokenize(
    grammar,
    "ready and enabled or fallback\nnot empty? and item in items",
  );
  assertScope(logical, 0, "and", "keyword.operator.logical.sputnik");
  assertScope(logical, 0, "or", "keyword.operator.logical.sputnik");
  assertScope(logical, 1, "not", "keyword.operator.logical.sputnik");
  assertScope(logical, 1, "in", "keyword.operator.relational.sputnik");

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
  assertScope(controls, 0, "if", "keyword.control.sputnik");
  assertScope(controls, 1, "raise", "keyword.control.exception.sputnik");
  assertScope(controls, 2, "else", "keyword.control.sputnik");
  assertScope(controls, 3, "return", "keyword.control.sputnik");
  assertScope(controls, 4, "catch", "keyword.control.exception.sputnik");
  assertScope(controls, 5, "throw", "keyword.control.exception.sputnik");

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
    "entity.name.function.call.sputnik",
  );
  assertScope(
    references,
    0,
    "&",
    "keyword.operator.reference.sputnik",
  );
  assertScope(
    references,
    0,
    "Items",
    "entity.name.type.receiver.sputnik",
  );
  assertScope(
    references,
    0,
    "#",
    "punctuation.accessor.unbound.sputnik",
  );
  assertScope(
    references,
    0,
    "index",
    "entity.name.function.reference.sputnik",
  );
  assertNoScopePrefix(references, 0, "index", "comment");
  assertScope(
    references,
    0,
    "route handler",
    "comment.line.number-sign.sputnik",
  );
  assertScope(
    references,
    1,
    "liveness",
    "entity.name.function.reference.sputnik",
  );
  assertScope(
    references,
    2,
    "find",
    "entity.name.function.reference.sputnik",
  );
  assertNoScopePrefix(references, 3, "#", "comment");
  assertScope(
    references,
    4,
    "ordinary comment",
    "comment.line.number-sign.sputnik",
  );

  const literals = tokenize(
    grammar,
    [
      "values = [42, 1_000, 1.5, 2e3, 0xFF, 0b1010, 0o755]",
      "state = :ready",
      "call(mode::fast)",
      "last = $_",
      "items.map: $it + $it1 + $it2 + _1 + _2",
      "case value:",
      " when _:",
      "  null",
    ].join("\n"),
  );
  assertScope(
    literals,
    0,
    "42",
    "constant.numeric.integer.decimal.sputnik",
  );
  assertScope(
    literals,
    0,
    "1_000",
    "constant.numeric.integer.decimal.sputnik",
  );
  assertScope(literals, 0, "1.5", "constant.numeric.float.sputnik");
  assertScope(literals, 0, "2e3", "constant.numeric.float.sputnik");
  assertScope(literals, 0, "0xFF", "constant.numeric.hex.sputnik");
  assertScope(literals, 1, "ready", "constant.other.symbol.sputnik");
  assertScope(literals, 2, "fast", "constant.other.symbol.sputnik");
  assertScope(literals, 3, "$_", "variable.language.last-value.sputnik");
  assertScope(
    literals,
    4,
    "$it",
    "variable.parameter.placeholder.sputnik",
  );
  assertScope(
    literals,
    4,
    "$it1",
    "variable.parameter.placeholder.sputnik",
  );
  assertScope(
    literals,
    4,
    "$it2",
    "variable.parameter.placeholder.sputnik",
  );
  assertScope(
    literals,
    4,
    "_1",
    "variable.parameter.placeholder.sputnik",
  );
  assertScope(literals, 6, "_", "variable.language.wildcard.sputnik");

  const placeholderOperators = tokenize(
    grammar,
    "$it!=0\n$it1!~pattern\n$it2[?0]",
  );
  for (const [line, alias] of ["$it", "$it1", "$it2"].entries()) {
    assertScope(
      placeholderOperators,
      line,
      alias,
      "variable.parameter.placeholder.sputnik",
    );
  }
  for (const invalid of [
    "$it0", "$it01", "$item", "$it_1", "$it2x",
    "$it?", "$it!", "$it1?", "$it?[0]", "$itя", "$it1é",
  ]) {
    const [{ tokens }] = tokenize(grammar, invalid);
    assert.ok(
      tokens.every(
        (token) => !token.scopes.includes("variable.parameter.placeholder.sputnik"),
      ),
      `${invalid} must not be highlighted as a placeholder`,
    );
  }

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
  assertScope(operators, 0, "+=", "keyword.operator.sputnik");
  for (const operator of ["**", "//", "%", "<=>"]) {
    assertScope(operators, 1, operator, "keyword.operator.sputnik");
  }
  assertScope(operators, 2, "=~", "keyword.operator.sputnik");
  assertScope(operators, 2, "!~", "keyword.operator.sputnik");
  assertScope(operators, 3, "<<", "keyword.operator.sputnik");
  assertScope(operators, 3, ">>", "keyword.operator.sputnik");
  assertScope(operators, 4, "..", "keyword.operator.range.sputnik");
  assertScope(operators, 5, "...", "keyword.operator.range.sputnik");
  assertScope(
    operators,
    6,
    ".?.",
    "keyword.operator.navigation.safe.sputnik",
  );
  assertScope(
    operators,
    6,
    "member",
    "entity.name.function.call.member.sputnik",
  );
  assertScope(
    operators,
    7,
    "?",
    "keyword.operator.optional-access.sputnik",
  );
  assertScope(operators, 8, "<=>", "entity.name.function.sputnik");
  assertScope(operators, 9, "[]=", "entity.name.function.sputnik");

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
  assertScope(declarations, 0, "native", "storage.modifier.native.sputnik");
  assertScope(declarations, 0, "read", "entity.name.function.sputnik");
  assertScope(
    declarations,
    0,
    "as",
    "keyword.operator.type.annotation.sputnik",
  );
  assertScope(declarations, 1, "Handle", "entity.name.type.sputnik");
  assertScope(declarations, 1, "owned", "storage.modifier.ownership.sputnik");
  assertScope(declarations, 2, "prop", "storage.type.property.sputnik");
  assertScope(
    declarations,
    2,
    "name",
    "entity.name.function.property.sputnik",
  );
  assertScope(declarations, 3, "get", "storage.modifier.property.sputnik");
  assertScope(declarations, 4, "set", "storage.modifier.property.sputnik");
  assertScope(declarations, 5, "var", "storage.modifier.property.sputnik");
  assertNoScopePrefix(declarations, 6, "pass", "keyword");
  assertNoScopePrefix(declarations, 6, "noop", "keyword");
  assertScope(
    declarations,
    7,
    "as",
    "keyword.operator.type.annotation.sputnik",
  );
  assertScope(declarations, 8, "next", "keyword.control.sputnik");
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
  assertScope(strings, 0, "message", "source.sputnik");
  assertScope(
    strings,
    0,
    "then",
    "keyword.control.conditional.sputnik",
  );
  assertScope(strings, 1, "r", "entity.name.function.macro.string-tag.sputnik");
  assertNoScopePrefix(strings, 1, "#not-a-comment", "comment");
  assertScope(
    strings,
    2,
    "sql",
    "entity.name.function.macro.string-tag.sputnik",
  );
  assertScope(strings, 3, "id", "meta.interpolation.sputnik", 1);

  const macros = tokenize(
    grammar,
    [
      "macro def assert(check):",
      " if not #{check}:",
      '  raise "failed"',
      "# outside macro",
    ].join("\n"),
  );
  assertScope(macros, 0, "macro", "storage.modifier.macro.sputnik");
  assertScope(macros, 0, "assert", "entity.name.function.macro.sputnik");
  assertScope(macros, 1, "#{", "punctuation.section.embedded.begin.sputnik");
  assertNoScopePrefix(macros, 1, "#{", "comment");
  assertScope(macros, 3, "outside macro", "comment.line.number-sign.sputnik");

  console.log("grammar tests: ok");
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
