#!/usr/bin/env python3
"""Exact BigInt arithmetic and VM-independent native regression coverage."""
import json
import os
from pathlib import Path
import random
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
compiler = Path(sys.argv[1] if len(sys.argv) > 1 else root / 'build/sputnik').resolve()
env = dict(os.environ)
env.setdefault('SPUTNIK_NATIVE_RT_CACHE', str(root / 'build/interop-runtime-cache'))

def run(*args):
    result = subprocess.run(list(map(str, args)), capture_output=True, text=True,
                            env=env, timeout=300)
    assert result.returncode == 0, (args, result.stdout, result.stderr)
    return result.stdout

# An independent oracle checks signed multi-limb division and carries, beyond
# merely comparing two backends that intentionally share the numeric kernel.
rng = random.Random(1305)
operands = [(2**64 - 1, 2**64 + 1), (-(2**63), -1), (0, -3),
            (2**256 - 1, 2**128 + 1), (-(2**256 - 1), 2**128 + 1)]
for _ in range(24):
    left = rng.getrandbits(rng.choice([65, 130, 256, 512]))
    right = rng.getrandbits(rng.choice([32, 64, 129, 255])) or 1
    operands.append((left * rng.choice([-1, 1]), right * rng.choice([-1, 1])))
rows = []
for left, right in operands:
    quotient = abs(left) // abs(right)
    if (left < 0) != (right < 0):
        quotient = -quotient
    rows.append(list(map(str, [left, right, left + right, left - right,
                               left * right, quotient, left // right, left % right])))

with tempfile.TemporaryDirectory(prefix='sputnik-bigint-') as temporary:
    directory = Path(temporary)
    source = directory / 'bigint.s'
    fixture = (root / 'tests/fixtures/native_bigint/main.s').read_text()
    source.write_text(fixture + '\nrows = ' + json.dumps(rows) + '''
rows.each |(left, right, sum, difference, product, quotient, floor, remainder)|:
  a = BigInt(left)
  b = BigInt(right)
  check((a + b).str == sum, "oracle addition")
  check((a - b).str == difference, "oracle subtraction")
  check((a * b).str == product, "oracle multiplication")
  check((a / b).str == quotient, "oracle truncating division")
  check((a // b).str == floor, "oracle floor division")
  check((a % b).str == remainder, "oracle floor modulo")
print "PASS BigInt arithmetic oracle"
0
''')
    vm = run(compiler, 'run', source)
    binary = directory / 'native-bigint'
    report = json.loads(run(compiler, 'build', source, '--entry', 'init',
                            '--require-full-native', '-o', binary))
    assert report['native_full_coverage'] is True, report
    assert report['bytecode_fallback'] is False, report
    assert report['native_vm_independent'] is True, report
    assert report['native_fallback_code_count'] == 0, report
    generated = binary.with_suffix('.native.cpp').read_text()
    assert 'invoke_native_stdlib_send' not in generated
    assert 'sputnik_vm_fallback_call' not in generated
    native = run(binary)
    assert native == vm, (native, vm)
    assert 'PASS native BigInt' in native and 'PASS BigInt arithmetic oracle' in native, native
print('native_bigint_test: VM/full native and arithmetic oracle passed')
