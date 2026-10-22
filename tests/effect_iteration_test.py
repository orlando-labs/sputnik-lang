#!/usr/bin/env python3
"""Effect iteration and search: VM/native parity and source diagnostics."""
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
COMPILER = Path(sys.argv[1] if len(sys.argv) > 1 else ROOT / 'build/sputnik').resolve()
WORK = COMPILER.parent / 'effect-iteration'
WORK.mkdir(exist_ok=True)
ENV = dict(os.environ)
ENV.setdefault('SPUTNIK_NATIVE_RT_CACHE', str(COMPILER.parent / 'native-rt-cache'))


def run(args, ok=True, timeout=300):
    result = subprocess.run([str(a) for a in args], cwd=ROOT, env=ENV,
                            text=True, capture_output=True, timeout=timeout)
    if ok and result.returncode:
        raise AssertionError(f'{args}\n{result.stdout}\n{result.stderr}')
    return result


source = ROOT / 'tests/fixtures/effect_iteration/main.s'
vm = run([COMPILER, source]).stdout
assert vm.splitlines() == ['PASS effect iteration and indexed search', '0'], vm
control = run([COMPILER, ROOT / 'tests/fixtures/effect_iteration/vm_control.s']).stdout
assert control.strip() == 'PASS lazy indexed search', control

for name, text in {
    'for-value': 'for x in [1]:\n  break x\n',
    'for-null': 'for x in [1]:\n  break null\n',
    'effect-value': '[1].for_each |x|: break 42\n',
    'indexed-value': '[1].for_each.with_index |x, i|: break i\n',
    'times-value': '10.times |i|: break i\n',
}.items():
    path = WORK / f'{name}.s'
    path.write_text(text)
    result = run([COMPILER, 'bc', path], ok=False)
    assert result.returncode and 'break value is forbidden in effect iteration' in result.stdout + result.stderr, result

for name, text, error in [
    ('arity', 'for a, b in [(1, 2)]:\n  null\n', 'ArgumentError'),
    ('pattern', 'for (a, b) in [1]:\n  null\n', 'MatchError'),
    ('each-break', '[1].each |x|: break\n', 'break is forbidden'),
    ('missing-block', '[].for_each()\n', 'ArgumentError'),
]:
    path = WORK / f'{name}.s'
    path.write_text(text)
    result = run([COMPILER, path], ok=False)
    assert result.returncode and error in result.stdout + result.stderr, result

binary = WORK / 'native'
info = json.loads(run([COMPILER, 'build', source, '--entry', 'init',
                       '--require-full-native', '-o', binary]).stdout)
assert info['native_full_coverage'] and info['native_vm_independent'], info
assert not info['native_runtime_bridge'] and not info['bytecode_fallback'], info
assert info['vm_fallback_code_count'] == 0, info
native = run([binary]).stdout
assert native == vm, (vm, native)
print('effect iteration: VM + full native, lazy search, arity/pattern and break diagnostics passed')
