#!/usr/bin/env python3
"""Build/install the adjacent source packages in an isolated filesystem registry.

Configure pycall/rbcall and system_io providers first. No public registry is used.
Torch has its separate CMake package matrix and does not depend on this harness.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--sputnik', type=Path, default=Path(__file__).resolve().parents[1]/'build/sputnik')
parser.add_argument('--workspace', type=Path, default=Path(__file__).resolve().parents[2])
parser.add_argument('--out', type=Path)
parser.add_argument('--native', action='store_true')
args = parser.parse_args()
binary, workspace = args.sputnik.resolve(), args.workspace.resolve()
output = args.out.resolve() if args.out else Path(tempfile.mkdtemp(prefix='sputnik-workspace-packages-'))
if args.out:
    output.mkdir(parents=True, exist_ok=False)
registry, home = output/'registry', output/'home'
registry.mkdir()
roots = [workspace/name for name in ['sqlite3','orm','orbit','plot','pycall','rbcall']]
roots += sorted((workspace/'orbit/packages').glob('*'))
roots += [workspace/'sputnik-notebook/packages/notebook-core']
roots += sorted((workspace/'sputnik-lang/packages').glob('*'))
roots = [root for root in roots if root.is_dir()]
assert roots, 'no workspace packages found'
for root in roots:
    assert (root/'sputnik.build.yaml').is_file(), str(root)+': missing manifest; configure the package first'
providers = [workspace/repo/'build/native-providers'/kind for repo in ['pycall','rbcall'] for kind in ['runtime','sdk']]
providers += [workspace/'sputnik-lang/packages/system_io/build/release-providers'/kind for kind in ['runtime','sdk']]
providers = [path for path in providers if (path/'sputnik.build.json').is_file()]
environment = dict(os.environ, SPUTNIK_PKG_HOME=str(home), SPUTNIK_PKG_REGISTRY=str(registry),
                   SPUTNIK_NATIVE_PROVIDERS=':'.join(map(str, providers)),
                   SPUTNIK_NATIVE_EXT_CACHE=str(output/'native'), SPUTNIK_VM_CACHE=str(output/'vm'))
environment.setdefault('SPUTNIK_NATIVE_RT_CACHE', str(binary.parent/'interop-runtime-cache'))
report = dict(schema='sputnik.workspace-packages.check.v1', registry=str(registry), home=str(home), packages=[], checks=[])
with (output/'commands.log').open('w') as log:
    def cli(*arguments, env=None):
        command = [str(binary), *map(str, arguments)]
        log.write('$ '+' '.join(command)+'\n'); log.flush()
        result = subprocess.run(command, cwd=output, env=env or environment, text=True,
                                capture_output=True, timeout=600)
        log.write(result.stdout+result.stderr); log.flush()
        if result.returncode:
            raise AssertionError((arguments, result.returncode, result.stdout[-1500:], result.stderr[-3000:]))
        return result.stdout

    for root in roots:
        assert not (root/'sputnik.toml').exists(), root
        metadata = json.loads(cli('package-manifest', root/'sputnik.build.yaml'))
        name, version = metadata['name'], metadata['version']
        artifact = registry/name/version/(name.rsplit('/',1)[-1]+'-'+version+'.sputnikpkg')
        artifact.parent.mkdir(parents=True)
        first = json.loads(cli('pkg','build',root,'--out',artifact))
        repeat = output/'repeat.sputnikpkg'
        second = json.loads(cli('pkg','build',root,'--out',repeat))
        assert first['sha256'] == second['sha256'], name+' is not reproducible'
        cli('pkg','verify',artifact)
        report['packages'].append(dict(name=name, version=version, source=str(root), artifact=str(artifact), sha256=first['sha256']))
        print('PASS build', name, flush=True)
    for package in report['packages']:
        cli('pkg','install',package['name'],'--version',package['version'],'--registry',registry,
            '--allow-native','--build-from-source')
        artifact = home/package['name']/package['version']/'source'/Path(package['artifact']).name
        assert artifact.is_file()
        assert hashlib.sha256(artifact.read_bytes()).hexdigest() == package['sha256'].removeprefix('sha256:')
        print('PASS install', package['name'], flush=True)
    consumers = output/'consumers'; consumers.mkdir()
    no_sdk = dict(environment); no_sdk.pop('SPUTNIK_NATIVE_PROVIDERS', None)
    no_compiler = dict(no_sdk, SPUTNIK_NATIVE_CXX='/no/compiler')
    names = {package['name'] for package in report['packages']}
    if {'pycall','rbcall'} <= names:
        source = consumers/'foreign.s'
        shutil.copyfile(workspace/'sputnik-lang/tests/fixtures/foreign_bridges/main.s',source)
        assert 'PASS both foreign runtimes' in cli('run',source,'--grant','ffi',env=no_compiler)
        report['checks'].append('installed Python/Ruby VM without SDK bindings or compiler')
        if args.native:
            executable = consumers/'foreign-native'
            summary = json.loads(cli('build',source,'--require-full-native','-o',executable,env=no_sdk))
            assert summary['native_full_coverage'] and not summary['bytecode_fallback']
            result = subprocess.run([str(executable)],env=no_compiler,text=True,capture_output=True,timeout=60)
            assert result.returncode == 0 and 'PASS both foreign runtimes' in result.stdout, result.stderr
            report['checks'].append('installed Python/Ruby full native without SDK bindings')
    if {'sqlite3','system_io'} <= names:
        source = consumers/'sdk.s'
        source.write_text('package check.installed_sdk\nfrom sdk.digest import SHA256\nimport sqlite3\nexport main\n'
            'def main():\n  hash = SHA256()\n  hash.update("abc".bytes)\n'
            '  raise ValueError("digest") unless hash.finish() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"\n'
            '  hash.destroy!()\n  sqlite3.memory() |db|:\n    raise ValueError("SQLite") unless db.scalar("select 42") == 42\n'
            '  print "PASS installed SDK and SQLite"\n  0\n')
        assert 'PASS installed SDK and SQLite' in cli('run',source,'--grant','ffi',env=no_compiler)
        report['checks'].append('installed System IO/SQLite without SDK bindings or compiler')
(output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
print('workspace_packages_test: PASS ('+str(len(roots))+' packages); '+str(output),flush=True)
