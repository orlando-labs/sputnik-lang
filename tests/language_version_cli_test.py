#!/usr/bin/env python3
"""Language requirements survive publication and gate transitive execution."""
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile

binary = str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix='sputnik-language-version-') as temporary:
    root = Path(temporary)
    env = dict(os.environ, SPUTNIK_PKG_HOME=str(root/'home'),
               SPUTNIK_PKG_REGISTRY=str(root/'registry'), SPUTNIK_VM_CACHE=str(root/'cache'),
               SPUTNIK_NATIVE_EXT_CACHE=str(root/'native'))

    def cli(*args, ok=True):
        result = subprocess.run([binary, *map(str, args)], cwd=root, env=env,
                                capture_output=True, text=True, timeout=90)
        output = result.stdout + result.stderr
        assert (result.returncode == 0) == ok, (args, result.returncode, output)
        return output

    def source(name, requirement='>=1.0.0 <2.0.0', dependencies=None):
        directory = root/name
        directory.mkdir()
        (directory/'main.s').write_text(f'package {name}\nexport answer\ndef answer(): 42\n')
        data = dict(name=name, version='1.0.0', files=['main.s'], module_names={'main.s':name},
                    requires=dict(sputnik=requirement), dependencies=dependencies or {})
        (directory/'sputnik.build.json').write_text(json.dumps(data))
        return directory, data

    def manifest(directory, data):
        (directory/'sputnik.build.json').write_text(json.dumps(data))

    def rewrite_archive(source, destination, transform):
        with tarfile.open(source) as archive:
            files = {item.name: archive.extractfile(item).read() for item in archive}
        data = json.loads(files['sputnik.build.json'])
        transform(data)
        files['sputnik.build.json'] = json.dumps(data).encode()
        # Inventory excludes itself; it also authenticates the manifest.
        files['checksums.sha256'] = ''.join(
            hashlib.sha256(contents).hexdigest()+'  '+name+'\n'
            for name, contents in sorted(files.items()) if name != 'checksums.sha256').encode()
        with tarfile.open(destination, 'w:gz') as archive:
            for name in ['sputnik.build.json','checksums.sha256'] + sorted(n for n in files if n not in ['sputnik.build.json','checksums.sha256']):
                item = tarfile.TarInfo(name); item.size = len(files[name]); item.mode = 0o644
                archive.addfile(item, io.BytesIO(files[name]))

    assert cli('--version').strip() == 'sputnik 1.0.0'
    package, data = source('version_leaf')
    source_archive = root/'source.sputnikpkg'
    bytecode_archive = root/'bytecode.sputnikpkg'
    cli('pkg','build',package,'--out',source_archive)
    cli('pkg','build',package,'--modules','bytecode','--out',bytecode_archive)
    for archive in [source_archive, bytecode_archive]:
        with tarfile.open(archive) as packed:
            published = json.load(packed.extractfile('sputnik.build.json'))
            assert published['requires'] == {'sputnik':'>=1.0.0 <2.0.0'}
            if archive == bytecode_archive:
                metadata = json.load(packed.extractfile('meta/build.json'))
                assert all(m['language'] == [1,0,0] and m['format'] == [1,2] for m in metadata['bytecode'].values())
    for requirement in ['>=2.0.0 <3.0.0', '>=1.0.1 <2.0.0']:
        manifest(package, dict(data, requires=dict(sputnik=requirement)))
        assert 'LANGUAGE_VERSION_MISMATCH' in cli('pkg','build',package,'--out',root/'bad.sputnikpkg',ok=False)
    for requirement in ['*','>=1.0.0','1.0','1.0.0 ||']:
        manifest(package, dict(data, requires=dict(sputnik=requirement)))
        assert 'SemVer' in cli('pkg','build',package,'--out',root/'bad.sputnikpkg',ok=False)
    manifest(package, data)
    legacy = root/'legacy.sputnikpkg'
    rewrite_archive(source_archive, legacy, lambda m: m.pop('requires'))
    assert 'LANGUAGE_REQUIREMENT_MISSING' in cli('pkg','verify',legacy,ok=False)
    incompatible = root/'incompatible.sputnikpkg'
    rewrite_archive(source_archive,incompatible,lambda m: m.update(requires=dict(sputnik='>=2.0.0 <3.0.0')))
    assert 'LANGUAGE_VERSION_MISMATCH' in cli('pkg','install',incompatible,ok=False)
    cli('pkg','install',bytecode_archive)
    consumer = root/'consumer.s'
    consumer.write_text('from version_leaf import answer\nprint(answer())\n')
    assert cli('run',consumer).strip() == '42'
    cli('pkg','lock',consumer)
    lock = root/'sputnik.lock'
    original = lock.read_text()
    assert 'sputnik_version = "1.0.0"' in original
    for changed in [original.replace('sputnik_version = "1.0.0"','sputnik_version = "2.0.0"'),
                    original.replace('sputnik_version = "1.0.0"\n','')]:
        lock.write_text(changed)
        assert 'LOCK_LANGUAGE_MISMATCH' in cli('run',consumer,ok=False)
    cli('pkg','lock',consumer)
    assert lock.read_text() == original
    lock.unlink()
    # A direct dependency that imports another local package must validate that
    # transitive package even when the application has no explicit requirement.
    middle, middle_data = source('version_middle', dependencies={'version_leaf':{'path':'../version_leaf'}})
    (middle/'main.s').write_text('package version_middle\nfrom version_leaf import answer\nexport answer\n')
    app = root/'app'; app.mkdir()
    app_manifest = dict(name='application', source_root='.', files=['main.s'], module_names={'main.s':'application'}, dependencies={'version_middle':{'path':'../version_middle'}})
    (app/'sputnik.build.json').write_text(json.dumps(app_manifest))
    (app/'main.s').write_text('package application\nfrom version_middle import answer\nprint(answer())\n')
    manifest(package, dict(data, requires=dict(sputnik='>=2.0.0 <3.0.0')))
    result = cli('run',app/'main.s',ok=False)
    assert 'LANGUAGE_VERSION_MISMATCH' in result and 'version_leaf' in result, result
    manifest(package, data)
    assert cli('run',app/'main.s').strip() == '42'
print('language_version_cli_test: PASS')
