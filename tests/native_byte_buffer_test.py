#!/usr/bin/env python3
"""ByteBuffer constructors/operations and Map transform: VM/native parity."""
import json
import os
from pathlib import Path
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

with tempfile.TemporaryDirectory(prefix='sputnik-byte-buffer-') as temporary:
    for fixture, message in [('native_byte_buffer', 'PASS native byte buffer'),
                             ('map_transform_names', 'PASS map transform names')]:
        source = root / 'tests/fixtures' / fixture / 'main.s'
        vm = run(compiler, 'run', source)
        assert message in vm, vm
        binary = Path(temporary) / fixture
        report = json.loads(run(compiler, 'build', source, '--entry', 'init',
                                '--require-full-native', '-o', binary))
        assert report['native_full_coverage'] is True, report
        assert report['bytecode_fallback'] is False, report
        assert report['native_vm_independent'] is True, report
        generated = binary.with_suffix('.native.cpp').read_text()
        assert 'invoke_native_stdlib_send' not in generated
        native = run(binary)
        assert native == vm, (native, vm)
    # Renamed builtins do not remain as aliases; user-defined names are unaffected.
    probe = Path(temporary) / 'old-map-method.s'
    for old in ('transform', 'transform_keys', 'transform_values'):
        probe.write_text('try:\n  {a: 1}.' + old + ' |k, v|: (k, v)\n'
                         'rescue NoMethodError:\n  print "removed"\n')
        assert run(compiler, 'run', probe).strip() == 'removed'
    for name in ('transformed', 'transformed_keys', 'transformed_values',
                 'transform!', 'transform_keys!', 'transform_values!'):
        probe.write_text('{a: 1}.' + name + ': break 2\n')
        rejected = subprocess.run([str(compiler), 'bc', str(probe)],
                                  capture_output=True, text=True, env=env, timeout=300)
        assert rejected.returncode != 0 and 'break' in rejected.stderr, rejected.stderr
print('native_byte_buffer_test: VM and full native passed')
