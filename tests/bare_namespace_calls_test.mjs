import { spawnSync } from 'node:child_process'
import { mkdtemp, rm } from 'node:fs/promises'
import os from 'node:os'
import path from 'node:path'
import assert from 'node:assert/strict'

const compiler = path.resolve(process.argv.slice(2).find(value => !value.startsWith('--')) || 'build/sputnik')
const source = path.resolve('tests/fixtures/bare_namespace_calls/main.s')
const root = await mkdtemp(path.join(os.tmpdir(), 'sputnik-bare-namespace-'))
const env = { ...process.env, SPUTNIK_PKG_HOME: path.join(root, 'packages'), SPUTNIK_VM_CACHE: path.join(root, 'cache') }
const expected = 'PASS bare namespace calls, references, defaults, blocks, classes and shadowing'
function run(command, args) {
  const result = spawnSync(command, args, { env, encoding: 'utf8', maxBuffer: 16 * 1024 * 1024 })
  assert.equal(result.status, 0, result.error?.message || result.stdout + result.stderr)
  return result.stdout.trim()
}
try {
  assert.equal(run(compiler, ['run', source]), expected)
  console.log('PASS VM bare namespace calls')
  if (process.argv.includes('--native')) {
    const binary = path.join(root, 'native')
    const report = JSON.parse(run(compiler, ['build', source, '--target', 'native', '--require-full-native', '-o', binary]))
    assert.equal(report.native_full_coverage, true)
    assert.equal(report.native_vm_independent, true)
    assert.equal(run(binary, []), expected)
    console.log('PASS full-native bare namespace calls, no fallback')
  }
} finally {
  await rm(root, { recursive: true, force: true })
}
