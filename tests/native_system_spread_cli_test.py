#!/usr/bin/env python3
"""System sends with positional/keyword spreads must work in strict native."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

language = Path(__file__).resolve().parents[1]
compiler = str(Path(sys.argv[1] if len(sys.argv) > 1 else language / 'build/sputnik').resolve())
source = language / 'tests/fixtures/native_system_spread/main.s'
env = dict(os.environ)
env.setdefault('SPUTNIK_NATIVE_RT_CACHE', str(language / 'build/interop-runtime-cache'))
with tempfile.TemporaryDirectory(prefix='sputnik-system-spread-', dir='/tmp') as temporary:
    def cli(*args):
        result = subprocess.run([compiler, *map(str, args)], capture_output=True, text=True, env=env, timeout=180)
        assert result.returncode == 0, result.stdout + result.stderr
        return result.stdout
    assert 'PASS native system spread' in cli('run', source, '--grant', 'process.spawn')
    binary = Path(temporary) / 'native'
    summary = json.loads(cli('build', source, '--grant', 'process.spawn', '--require-full-native', '-o', binary))
    assert summary['bytecode_fallback'] is False, summary
    result = subprocess.run([str(binary)], capture_output=True, text=True, env=env, timeout=30)
    assert result.returncode == 0 and 'PASS native system spread' in result.stdout, result.stdout + result.stderr
print('native_system_spread_cli_test: VM and full native passed')
