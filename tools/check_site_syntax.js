#!/usr/bin/env node
// Lexical regressions and lossless code rendering, without browser dependencies.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const root = path.resolve(__dirname, '..');
const context = vm.createContext({window: {}});
for (const file of ['syntax-highlight.js', 'md-render.js']) {
  vm.runInContext(fs.readFileSync(path.join(root, 'site', file), 'utf8'), context);
}
const {highlight} = context.window.SputnikSyntax;
const {render} = context.window.SputnikMarkdown;
const textOf = (html) => html.replace(/<\/?span\b[^>]*>/g, '')
  .replaceAll('&quot;', '"').replaceAll('&lt;', '<').replaceAll('&gt;', '>').replaceAll('&amp;', '&');

const cases = [
  ['sputnik', 'f = &User#full_name\nf(user) # def false 123 "literal"'],
  ['sputnik', 'x = 0xFF // 2 + 0b1010 + 0o755 + 1_000 + 1.25e-3\nr = 1..5:2'],
  ['sputnik', 'users.map: $it1 * 2 .select: _1 > @limit\nstate = :ready\nname = @@name'],
  ['sputnik', '"Hello, #{name}! #{lookup({key: "x", other: {n: 2}})}"'],
  ['sputnik', '"\\#{literal}"\n\'#{literal}\'\nraw"#{literal}"\ntag\'#{value}\''],
  ['sputnik', '"""multiline\n#{2 ** 10}\n"""\ndef имя(x): x\n"unterminated'],
  ['sputnik', 'x = "<script>alert(1)</script>" # <span>& "quote"'],
  ['sh', 'git clone https://example.org/code.git\nprintf \'hello\\n\' > hello.s\nbuild/sputnik --target native -o hello # compile'],
  ['json', '{"name": "Ada", "enabled": true, "count": 1.5e-3, "value": null}'],
  ['toml', '[project]\nname = "orbit"\nworkers = 4 # threads\nactive = true'],
  ['ebnf', 'call ::= identifier, "(", [arguments], ")" ;\nend = EOF ;'],
  ['text', 'Worker → Strand → Task\n  └── <plain> & output']
];
for (const [language, source] of cases) {
  const html = highlight(source, language);
  assert.equal(textOf(html), source, `${language}: highlighting changed code text`);
  assert.ok(!html.includes('<script>'), 'Source HTML must remain escaped');
}
const reference = highlight('&User#method // 2 # def "x" 42', 'sputnik');
assert.equal((reference.match(/tok-comment/g) || []).length, 1);
assert.match(reference, /tok-op">#<\/span><span class="tok-fn">method/);
assert.match(reference, /tok-op">\/\//);
assert.match(reference, /tok-comment"># def &quot;x&quot; 42<\/span>$/);
for (const source of ['"\\#{literal}"', "'#{literal}'", 'raw"#{literal}"']) {
  assert.ok(!highlight(source, 'sputnik').includes('tok-interp'), 'Literal strings must not gain interpolation');
}
assert.ok(highlight('"#{2 ** 10}"', 'sputnik').includes('tok-interp'));
assert.ok(!highlight(cases.at(-1)[1], 'text').includes('<span'), 'Diagrams must remain plain text');

const docs = ['cheat-sheet.md', 'cheat-sheet.ru.md'];
const codeBlocks = docs.map(name => {
  const markdown = fs.readFileSync(path.join(root, 'docs', name), 'utf8');
  return [...render(markdown).html.matchAll(/<pre><code[^>]*>([\s\S]*?)<\/code><\/pre>/g)].map(match => match[1]);
});
assert.equal(codeBlocks[0].length, 11);
assert.equal(codeBlocks[1].length, codeBlocks[0].length);
const executableText = (html) => textOf(html.replace(/<span class="tok-comment">[\s\S]*?<\/span>/g, ''))
  .split('\n').map(line => line.trimEnd()).join('\n');
codeBlocks[0].forEach((html, index) => {
  assert.equal(executableText(codeBlocks[1][index]), executableText(html), `Translated example ${index + 1} changed executable code`);
});
console.log(`Syntax checks passed: ${cases.length} lexical cases, ${codeBlocks[0].length} matching RU/EN examples`);
