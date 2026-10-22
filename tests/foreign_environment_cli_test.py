#!/usr/bin/env python3
"""Exercise worker-selected venv/Bundler initialization in VM and full native."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

workspace = Path(__file__).resolve().parents[2]
compiler = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else workspace / 'sputnik-lang/build/sputnik'
python_config = json.loads((workspace / 'pycall/sputnik.build.json').read_text())
python = json.loads(next(d.split('=', 1)[1] for d in python_config['native_extensions'][0]['defines']
                         if d.startswith('SPUTNIK_PYTHON_EXECUTABLE=')))
ruby_config = json.loads((workspace / 'rbcall/sputnik.build.json').read_text())
ruby = os.environ.get('SPUTNIK_TEST_RUBY', str(Path(ruby_config['native_extensions'][0]['library_dirs'][0]).parent / 'bin/ruby'))
with tempfile.TemporaryDirectory(prefix='sputnik-foreign-env-', dir='/tmp') as temporary:
    root = Path(temporary).resolve()
    env = dict(os.environ, SPUTNIK_NATIVE_RT_CACHE=str(workspace / 'sputnik-lang/build/interop-runtime-cache'),
               BUNDLE_USER_HOME=str(root / 'bundler-home'))
    subprocess.run([python, '-m', 'venv', '--without-pip', str(root / '.venv')], check=True, env=env)
    site = Path(subprocess.check_output([str(root / '.venv/bin/python'), '-c',
                  'import sysconfig; print(sysconfig.get_path("purelib"))'], env=env, text=True).strip())
    (site / 'sputnik_env_probe.py').write_text('VALUE = 123\n')
    (root / 'Gemfile').write_text('source "https://rubygems.org"\n')
    env.update(BUNDLE_GEMFILE=str(root / 'Gemfile'), BUNDLE_APP_CONFIG=str(root / '.bundle'))
    subprocess.run([ruby, '-S', 'bundle', 'lock', '--local'], cwd=root, env=env, check=True,
                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    env.update(SPUTNIK_PYTHON_EXECUTABLE=str(root / '.venv/bin/python'),
               SPUTNIK_RBCALL_BUNDLE_GEMFILE=str(root / 'Gemfile'), BUNDLE_FROZEN='true')

    def cli(*args, success=True):
        result = subprocess.run([str(compiler), *map(str, args)], cwd=root, env=env,
                                capture_output=True, text=True, timeout=600)
        if success:
            assert result.returncode == 0, result.stdout + result.stderr
        return result

    for package, source in [
        ('pycall', 'import pycall.sys as sys\nimport pycall.sputnik_env_probe as probe\n'
         'raise ValueError("wrong venv") unless sys.prefix == ' + json.dumps(str(root / '.venv')) + '\n'
         'raise ValueError("package absent") unless probe.VALUE == 123\nexport main\ndef main(): 0\n'),
        ('rbcall', 'import rbcall\nraise ValueError("wrong bundle") unless rbcall.eval("Bundler.root.to_s") == '
         + json.dumps(str(root)) + '\nexport main\ndef main(): 0\n')]:
        directory = workspace / package
        manifest = json.loads((directory / 'sputnik.build.json').read_text())
        for module in manifest['modules']:
            module['path'] = str(directory / module['path'])
        for extension in manifest['native_extensions']:
            for key in ['sources', 'headers', 'include_dirs', 'library_dirs', 'runtime_library_dirs']:
                if key in extension:
                    extension[key] = [str(directory / path) for path in extension[key]]
        entry = root / (package + '.s')
        entry.write_text('package test.' + package + '\n' + source)
        manifest['root'] = manifest['name'] = 'test.' + package
        manifest['modules'].append(dict(name=manifest['root'], path=str(entry)))
        path = root / (package + '.build.json')
        path.write_text(json.dumps(manifest))
        cli('run', path, '--grant', 'ffi')
        output = root / (package + '-native')
        summary = json.loads(cli('build', path, '--target', 'native', '--require-full-native', '--out-dir', output).stdout)
        assert summary['native_graph_vm_fallback_code_count'] == 0
        subprocess.run([str(output / manifest['root'])], cwd=root, env=env, check=True, timeout=60)
        print(package + ': environment selected in VM and full native', flush=True)
        if package == 'pycall':
            cfg = root / '.venv/pyvenv.cfg'
            original = cfg.read_text()
            try:
                cfg.write_text('version = 0.1.0\n')
                failed = cli('run', path, '--grant', 'ffi', success=False)
                assert failed.returncode != 0 and 'must match pycall' in failed.stderr + failed.stdout
            finally:
                cfg.write_text(original)
