#!/usr/bin/env python3
"""One manifest for exact inventories, module naming, capabilities and installs."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile

binary = str(Path(sys.argv[1] if len(sys.argv) > 1 else 'build/sputnik').resolve())
with tempfile.TemporaryDirectory(prefix='sputnik-manifest-') as temporary:
    root = Path(temporary)
    registry = root / 'registry'
    env = dict(os.environ, SPUTNIK_PKG_HOME=str(root / 'home'), SPUTNIK_PKG_REGISTRY=str(registry),
               SPUTNIK_VM_CACHE=str(root / 'vm'), SPUTNIK_NATIVE_EXT_CACHE=str(root / 'native'))

    def cli(*args, ok=True, environment=None):
        result = subprocess.run([binary, *map(str, args)], cwd=root, env=environment or env,
                                text=True, capture_output=True, timeout=180)
        output = result.stdout + result.stderr
        assert (result.returncode == 0) == ok, (args, output)
        return output

    def source(name, module, body, dependencies=None):
        directory = root / name
        path = 'src/' + module.replace('.', '/') + '.s'
        (directory / path).parent.mkdir(parents=True)
        (directory / path).write_text(f'package {module}\n{body}\n')
        (directory / 'LICENSE').write_text('fixture\n')
        (directory / 'unlisted.s').write_text('deliberately invalid; never packed')
        data = dict(name=name, version='1.0.0', source_root='src', files=[path, 'LICENSE'],
                    dependencies=dependencies or {}, capabilities={'fs.read': ['./data'], 'time.now': True})
        # Actual YAML, with JSON flow values for nested maps/arrays.
        (directory / 'sputnik.build.yaml').write_text(''.join(k + ': ' + json.dumps(v) + '\n' for k, v in data.items()))
        return directory, data

    leaf, _ = source('leaf', 'demo.leaf', 'export number\ndef number(): 40')
    middle, middle_data = source('middle', 'demo.middle',
        'from demo.leaf import number\nexport number_plus_one\ndef number_plus_one(): number() + 1',
        {'leaf': {'path': '../leaf'}})
    top, _ = source('top', 'demo.top',
        'from demo.middle import number_plus_one\nexport answer\ndef answer(): number_plus_one() + 1',
        {'middle': '1.0.0'})
    for directory in [leaf, middle, top]:
        output = registry / directory.name / '1.0.0' / (directory.name + '-1.0.0.sputnikpkg')
        output.parent.mkdir(parents=True)
        cli('pkg', 'build', directory, '--out', output)
        duplicate = root / 'repeat.sputnikpkg'
        cli('pkg', 'build', directory, '--out', duplicate)
        assert hashlib.sha256(output.read_bytes()).digest() == hashlib.sha256(duplicate.read_bytes()).digest()
        with tarfile.open(output) as archive:
            names = archive.getnames()
            assert names[:2] == ['sputnik.build.json', 'checksums.sha256']
            assert 'unlisted.s' not in names and not any(name.endswith('.toml') for name in names)
            metadata = json.load(archive.extractfile('sputnik.build.json'))
            assert 'modules' not in metadata
            assert metadata['source_root'] == 'src'
            assert metadata['capabilities'] == {'fs.read': ['./data'], 'time.now': True}
            if directory == middle:
                assert metadata['dependencies'] == {'leaf': '1.0.0'}
        cli('pkg', 'verify', output)
    cli('pkg', 'install', 'top', '--version', '1.0.0', '--registry', registry)
    script = root / 'consumer.s'
    script.write_text('from demo.top import answer\nprint(answer())\n')
    assert cli('run', script).strip() == '42'
    cli('pkg', 'lock', script)
    lock = (root / 'sputnik.lock').read_text()
    assert all(f'name = "{name}"' in lock for name in ['leaf', 'middle', 'top'])
    (root / 'sputnik.lock').unlink()

    local_env = dict(env, SPUTNIK_PKG_HOME=str(root / 'local-home'), SPUTNIK_PKG_REGISTRY=str(root / 'absent-registry'))
    cli('pkg', 'install', middle, environment=local_env)
    shutil.rmtree(leaf)
    shutil.rmtree(middle)
    script.write_text('from demo.middle import number_plus_one\nprint(number_plus_one())\n')
    assert cli('run', script, environment=local_env).strip() == '41'

    custom, data = source('custom', 'unusual.name', 'export answer\ndef answer(): 7')
    original = custom / 'src/unusual/name.s'
    original.rename(custom / 'src/odd.s')
    data.update(files=['src/odd.s', 'LICENSE'], module_names={'src/odd.s': 'unusual.name'})
    manifest = custom / 'sputnik.build.json'
    (custom / 'sputnik.build.yaml').unlink()
    manifest.write_text(json.dumps(data))
    archive = root / 'custom.sputnikpkg'
    cli('pkg', 'build', custom, '--modules', 'bytecode', '--out', archive)
    with tarfile.open(archive) as packed:
        names = packed.getnames()
        assert 'bytecode/unusual.name.sputnikbc' in names and 'src/odd.s' not in names
    cli('pkg', 'install', archive)
    script.write_text('from unusual.name import answer\nprint(answer())\n')
    assert cli('run', script).strip() == '7'
    for change, expected in [
        ({'files': ['../outside.s']}, 'invalid or duplicate'),
        ({'files': ['src/odd.s', 'src/odd.s']}, 'invalid or duplicate'),
        ({'files': ['src/odd.s', 'src']}, 'regular files'),
        ({'module_names': {'missing.s': 'demo'}}, 'absent from files'),
        ({'capabilities': {'unknown.capability': True}}, 'unknown capability'),
    ]:
        manifest.write_text(json.dumps(dict(data, **change)))
        output = cli('pkg', 'build', custom, '--out', archive, ok=False)
        assert expected in output, (change, output)
    without_version = dict(data); without_version.pop('version')
    manifest.write_text(json.dumps(without_version))
    cli('pkg', 'build', manifest, '--version', '2.0.0', '--out', archive)
    with tarfile.open(archive) as packed:
        assert json.load(packed.extractfile('sputnik.build.json'))['version'] == '2.0.0'
    manifest.write_text('{"name":"a","name":"b"}')
    assert 'duplicate object key' in cli('pkg', 'build', custom, '--out', archive, ok=False)
print('package_manifest_workflow_test: PASS')
