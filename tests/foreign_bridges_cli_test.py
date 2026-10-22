#!/usr/bin/env python3
"""Verify installed independent bridges and nested calls in VM and full native."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

compiler = str(Path(sys.argv[1] if len(sys.argv) > 1 else 'build/sputnik').resolve())
language = Path(__file__).resolve().parents[1]
workspace = language.parent
for name in ['pycall', 'rbcall']:
    assert (workspace / name / 'sputnik.toml').is_file(), 'configure ' + name + ' first'
with tempfile.TemporaryDirectory(prefix='sputnik-foreign-bridges-') as directory:
    root = Path(directory)
    env = dict(os.environ, SPUTNIK_PKG_HOME=str(root / 'packages'),
               SPUTNIK_NATIVE_EXT_CACHE=str(root / 'native'))
    env.setdefault('SPUTNIK_NATIVE_RT_CACHE', str(language / 'build/interop-runtime-cache'))

    def cli(*args):
        result = subprocess.run([compiler, *map(str, args)], env=env, cwd=root,
                                text=True, capture_output=True, timeout=600)
        assert result.returncode == 0, result.stdout + result.stderr
        return result.stdout

    for name in ['pycall', 'rbcall']:
        cli('pkg', 'install', workspace / name)
    source = root / 'main.s'
    source.write_text((language / 'tests/fixtures/foreign_bridges/main.s').read_text())
    assert 'PASS both foreign runtimes' in cli('run', source, '--grant', 'ffi')
    summary = json.loads(cli('build', source, '--require-full-native', '-o', root / 'native-app'))
    assert summary['bytecode_fallback'] is False, summary
    result = subprocess.run([str(root / 'native-app')], env=env, cwd=root,
                            text=True, capture_output=True, timeout=90)
    assert result.returncode == 0, result.stdout + result.stderr
    assert 'PASS both foreign runtimes' in result.stdout, result.stdout
print('foreign_bridges_cli_test: VM and full native passed')
