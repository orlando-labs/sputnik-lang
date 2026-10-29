import assert from 'node:assert/strict'
import { mkdtemp, mkdir, readFile, writeFile, rm, symlink, realpath } from 'node:fs/promises'
import { spawnSync } from 'node:child_process'
import { tmpdir } from 'node:os'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const sdk = fileURLToPath(new URL('../', import.meta.url))
const compiler = path.join(sdk, 'build/sputnik')
const root = await realpath(await mkdtemp(path.join(tmpdir(), 'sputnik-path-deps-')))
const env = { ...process.env, SPUTNIK_PKG_HOME: path.join(root, 'home'), SPUTNIK_VM_CACHE: path.join(root, 'vm'), SPUTNIK_NATIVE_RT_CACHE: path.join(sdk, 'build/interop-runtime-cache') }
async function write(file, content) { await mkdir(path.dirname(file), { recursive: true }); await writeFile(file, content) }
function run(args, error) {
  const result = spawnSync(compiler, args, { cwd: root, env, encoding: 'utf8', timeout: 600_000, maxBuffer: 10 * 1024 * 1024 })
  const text = result.stdout + result.stderr
  if (error) { assert.notEqual(result.status, 0, text); assert.match(text, error); return text }
  assert.equal(result.status, 0, text)
  return result.stdout
}
function manifest(name, modules, dependencies = {}) {
  return JSON.stringify({name, version: '1.0.0', files: [...modules.map(name => `${name}.s`), ...(name === 'views' ? ['page.text.sv'] : [])], dependencies})
}
const app = path.join(root, 'app'), words = path.join(root, 'words'), views = path.join(root, 'views')
const build = path.join(app, 'sputnik.build.json')
const config = {
  entry: 'backend/main.s',
  dependencies: { views: { path: '../views' } },
  source_formats: [{ suffix: '.text.sv', module: 'demo.words', macro: 'text' }],
}
const originalWords = (await readFile(path.join(sdk, 'tests/fixtures/external_templates/text.s'), 'utf8')).replaceAll('app.text', 'demo.words')
async function wordSource(number) { await write(path.join(words, 'demo.words.s'), originalWords + `\nexport macro number\nmacro def number(): return Ast.lift(${number})\n`) }
const buildArgs = [build, '--target', 'bytecode', '--out-dir', path.join(root, 'out'), '--cache-dir', path.join(root, 'cache')]
try {
  const plain = path.join(root, 'plain');
  const plainEntry = path.join(plain, 'main.s');
  await write(plainEntry, 'package demo.main\nfrom demo.greeting import message\nexport main\ndef main(): print message()\n');
  await write(path.join(plain, 'greeting.s'), 'package demo.greeting\nfrom demo.nested.value import word\nexport message\ndef message(): return word()\n');
  await write(path.join(plain, 'nested/value.s'), 'package demo.nested.value\nexport macro word\nmacro def word(): return Ast.lift("no manifest")\n');
  await write(path.join(plain, 'unused.s'), 'this unused file is deliberately invalid (((\n');
  assert.equal(run(['run', plainEntry]).trim(), 'no manifest', 'local imports and macros need no manifest and ignore unreachable files');
  run(['pkg', 'lock', plainEntry]);
  assert.match(await readFile(path.join(plain, 'sputnik.lock'), 'utf8'), /sputnik.packages.lock.v1/);
  await write(path.join(plain, 'greeting.spu'), 'package demo.greeting\n');
  run(['run', plainEntry], /ambiguous local module/);
  await rm(path.join(plain, 'greeting.spu'));
  await write(path.join(plain, 'sputnik.build.json'), '{}');
  assert.equal(run(['run', plainEntry]).trim(), 'no manifest', 'optional empty settings preserve local import behavior');
  await write(path.join(plain, 'script.s'), 'print "script settings"\n');
  assert.equal(run(['run', path.join(plain, 'script.s')]).trim(), 'script settings', 'configured scripts need no package header');
  await write(path.join(plain, 'sputnik.build.yaml'), 'dependencies: {}\n');
  run(['run', plainEntry], /ambiguous build configuration/);
  await rm(path.join(plain, 'sputnik.build.yaml'));
  await rm(path.join(plain, 'sputnik.build.json'));
  await write(path.join(words, 'sputnik.build.yaml'), manifest('words', ['demo.words']))
  await wordSource(7)
  await write(path.join(views, 'sputnik.build.yaml'), manifest('views', ['demo.views'], { words: { path: '../words' } }))
  await write(path.join(views, 'demo.views.s'), 'package demo.views\nexport page\ntemplate "./page.text.sv"\ndef page(name)\n')
  await write(path.join(views, 'page.text.sv'), 'hello #{name}')
  await write(path.join(app, 'backend/main.s'), 'package app\nfrom demo.views import page\nfrom demo.words import number\nexport main\ndef main():\n  value = number()\n  print page("Sputnik") + ":" + value.to_str\n')
  await write(build, JSON.stringify(config))
  let summary = JSON.parse(run(['build', ...buildArgs]))
  assert.equal(summary.artifacts.length, 3)
  assert(summary.inputs.includes(path.join(views, 'page.text.sv')))
  assert.equal(run(['run', build]).trim(), 'hello Sputnik:7')
  const settingsOnly = { ...config }; delete settingsOnly.entry;
  await write(build, JSON.stringify(settingsOnly));
  assert.equal(run(['run', path.join(app, 'backend/main.s')]).trim(), 'hello Sputnik:7', 'source entry finds parent settings without modules, sources or entry');
  run(['pkg', 'lock', path.join(app, 'backend/main.s')]);
  await write(build, JSON.stringify(config));
  const lock = await readFile(path.join(app, 'sputnik.lock'), 'utf8')
  assert.match(lock, /name = "words"/)
  assert.match(lock, /source_kind = "path"/)
  await wordSource(9)
  assert.equal(run(['run', build]).trim(), 'hello Sputnik:9', 'macro edits invalidate consumer expansion cache')
  await write(path.join(views, 'page.text.sv'), 'welcome #{name}')
  assert.equal(run(['run', build]).trim(), 'welcome Sputnik:9', 'package external template edits invalidate graph cache')
  assert.equal(await readFile(path.join(app, 'sputnik.lock'), 'utf8'), lock, 'source edits do not rewrite the lock')
  const withoutDependencies = { ...config }; delete withoutDependencies.dependencies
  await write(build, JSON.stringify(withoutDependencies))
  assert.equal(run(['run', build]).trim(), 'welcome Sputnik:9', 'lock locates local packages without repeated declarations')
  await write(build, JSON.stringify(config))
  const inputs = JSON.parse(await readFile(path.join(root, 'vm/inputs.json'), 'utf8')).inputs
  assert(inputs.includes(path.join(words, 'demo.words.s')) && inputs.includes(path.join(views, 'sputnik.build.yaml')))
  await symlink(words, path.join(root, 'words-link'), 'dir')
  await write(build, JSON.stringify({ ...config, dependencies: { ...config.dependencies, words: { path: '../words-link' } } }))
  assert.equal(run(['run', build]).trim(), 'welcome Sputnik:9', 'symlink aliases preserve canonical package identity')
  await write(path.join(root, 'other-words/sputnik.build.yaml'), manifest('words', ['demo.words']))
  await write(path.join(root, 'other-words/demo.words.s'), originalWords)
  await write(build, JSON.stringify({ ...config, dependencies: { ...config.dependencies, words: { path: '../other-words' } } }))
  run(['build', ...buildArgs], /conflicting path dependencies/)
  await write(build, JSON.stringify({ ...config, dependencies: { wrong: { path: '../views' } } }))
  run(['build', ...buildArgs], /name mismatch/)
  await write(build, JSON.stringify({ ...config, dependencies: { views: { path: '../views', version: '2.0.0' } } }))
  run(['build', ...buildArgs], /version mismatch/)
  await write(build, JSON.stringify(config))
  await write(path.join(views, 'sputnik.build.yaml'), manifest('views', ['demo.views']))
  run(['run', build], /differs from sputnik.lock/)
  await write(path.join(views, 'sputnik.build.yaml'), manifest('views', ['demo.views'], { words: { path: '../words' } }))
  const distribution = path.join(root, 'views.sputnikpkg')
  run(['pkg', 'build', views, '--out', distribution])
  const extracted = path.join(root, 'packed-views')
  run(['pkg', 'extract', distribution, extracted])
  const packedManifest = JSON.parse(await readFile(path.join(extracted, 'sputnik.build.json'), 'utf8'))
  assert.deepEqual(packedManifest.dependencies, { words: '1.0.0' }, 'packing pins local dependency versions')
  assert(packedManifest.files.includes('page.text.sv'), 'template resources are explicit package files')

  // Installed imports remain usable without dependencies, then respect a lock.
  const installed = path.join(root, 'installed'), installedEntry = path.join(installed, 'main.s')
  await write(installedEntry, 'package installed\nfrom demo.words import number\nexport main\ndef main(): print number()\n')
  async function install(version, number) {
    await write(path.join(words, 'sputnik.build.yaml'), manifest('words', ['demo.words']).replace('1.0.0', version))
    await wordSource(number)
    const artifact = path.join(root, `words-${version}.sputnikpkg`)
    run(['pkg', 'build', words, '--out', artifact])
    run(['pkg', 'install', artifact])
  }
  await install('1.0.0', 10)
  await install('1.1.0', 11)
  assert.equal(run(['run', installedEntry]).trim(), '11', 'imports discover installed packages without any manifest')
  run(['pkg', 'lock', installedEntry])
  await install('1.2.0', 12)
  assert.equal(run(['run', installedEntry]).trim(), '11', 'installed package versions stay locked')
  // Restore the live package's identity for the native graph check.
  await write(path.join(words, 'sputnik.build.yaml'), manifest('words', ['demo.words']))
  await wordSource(9)
  if (process.argv.includes('--native')) {
    const plainBinary = path.join(root, 'plain-bin');
    const plainSummary = JSON.parse(run(['build', plainEntry, '-o', plainBinary, '--require-full-native']));
    assert(plainSummary.native_full_coverage && plainSummary.native_vm_independent && !plainSummary.bytecode_fallback);
    assert.equal(spawnSync(plainBinary, [], { encoding: 'utf8' }).stdout.trim(), 'no manifest');
    await write(build, JSON.stringify(settingsOnly));
    const appBinary = path.join(root, 'app-bin');
    const appSummary = JSON.parse(run(['build', path.join(app, 'backend/main.s'), '-o', appBinary, '--require-full-native']));
    assert(appSummary.native_full_coverage && appSummary.native_vm_independent && !appSummary.bytecode_fallback);
    assert.equal(spawnSync(appBinary, [], { encoding: 'utf8' }).stdout.trim(), 'welcome Sputnik:9');
    await write(build, JSON.stringify(config));
    summary = JSON.parse(run(['build', build, '--target', 'native', '--require-full-native', '--out-dir', path.join(root, 'native'), '--cache-dir', path.join(root, 'native-cache')]))
    assert(summary.native_graph_full_coverage && summary.native_graph_vm_independent)
    assert(!summary.native_bytecode_fallback && !summary.native_graph_runtime_bridge)
    const executed = spawnSync(path.join(root, 'native/app'), [], { cwd: root, encoding: 'utf8' })
    assert.equal(executed.status, 0, executed.stderr)
    assert.equal(executed.stdout.trim(), 'welcome Sputnik:9')
  }
  console.log('PASS path dependencies, live sources, lock graph, macros/templates and installed imports')
} finally {
  if (process.env.SPUTNIK_KEEP_TEST_FILES) console.log('Fixture:', root)
  else await rm(root, { recursive: true, force: true })
}
