#!/usr/bin/env python3
"""Native output lifecycle, standalone C++ export, and known-call parity."""
import concurrent.futures
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
compiler = Path(sys.argv[1] if len(sys.argv) > 1 else root / 'build/sputnik').resolve()

with tempfile.TemporaryDirectory(prefix='sputnik-build-cli-') as temporary:
    work = Path(temporary)
    scratch = work / 'scratch'
    scratch.mkdir()
    env = dict(os.environ, TMPDIR=str(scratch),
               SPUTNIK_NATIVE_RT_CACHE=str(work / 'runtime-cache'),
               SPUTNIK_NATIVE_RUNTIME_ROOT=str(root))

    def run(*args, environment=None, ok=True):
        result = subprocess.run(list(map(str, args)), cwd=work,
                                env=env if environment is None else environment,
                                capture_output=True, text=True, timeout=300)
        assert (result.returncode == 0) == ok, (args, result.stdout, result.stderr)
        return result

    def clean(directory, expected):
        # Apple's compiler creates its own xcrun lookup database in TMPDIR.
        actual = [p.name for p in directory.iterdir()
                  if directory != scratch or p.name != 'xcrun_db']
        assert sorted(actual) == sorted(expected), list(directory.iterdir())
        assert not list(scratch.glob('.sputnik-build-*')), list(scratch.iterdir())

    source = work / 'Ohai space,comma.spu'
    source.write_text('print("Ohai")\n')
    binary = source.with_suffix('')
    built = run(compiler, 'build', source)
    metadata = json.loads(built.stdout)
    assert metadata['native_source'] == '' and metadata['native_full_coverage'], metadata
    assert built.stderr == '', built.stderr
    assert run(binary).stdout == 'Ohai\n'
    assert not (work / 'runtime-cache').exists(), 'bundled runtime was rebuilt'
    clean(work, [source.name, binary.name, 'scratch'])

    # Export is independent of the C++ toolchain and never creates a binary.
    cpp = work / 'export' / 'hello.cpp'
    no_cxx = dict(env, SPUTNIK_NATIVE_CXX=str(work / 'missing-cxx'))
    run(compiler, 'emit-cpp', source, '-o', cpp, environment=no_cxx)
    assert 'int main(' in cpp.read_text()
    assert 'native_call_print({' in cpp.read_text()
    clean(cpp.parent, ['hello.cpp'])
    run(compiler, 'emit-cpp', source, environment=no_cxx)
    assert source.with_suffix('.cpp').exists()

    # Manifest builds default to a single executable; bytecode is opt-in.
    manifest = root / 'tests/fixtures/w14_build/sputnik.build.json'
    out = work / 'manifest'
    report = json.loads(run(compiler, 'build', manifest, '--out-dir', out).stdout)
    assert report['target'] == 'native' and report['native_source'] == '', report
    clean(out, ['demo.main'])
    assert run(out / 'demo.main').stdout == '42\n'
    exported = work / 'manifest-export' / 'main.cpp'
    run(compiler, 'emit-cpp', manifest, '-o', exported, environment=no_cxx)
    clean(exported.parent, ['main.cpp'])
    both = work / 'both'
    run(compiler, 'build', manifest, '--target', 'both', '--out-dir', both)
    assert (both / 'demo.main.sputnikbc').exists() and (both / 'demo.main').exists()
    assert not list(both.glob('*.cpp'))

    configured = work / 'configured'
    configured.mkdir()
    (configured / 'main.spu').write_text('print("configured")\n')
    (configured / 'sputnik.build.json').write_text(json.dumps({'entry': 'main.spu'}))
    run(compiler, 'build', configured / 'main.spu')
    clean(configured, ['main.spu', 'main', 'sputnik.build.json'])
    assert run(configured / 'main').stdout == 'configured\n'
    run(compiler, 'emit-cpp', configured / 'main.spu', environment=no_cxx)
    clean(configured, ['main.spu', 'main', 'main.cpp', 'sputnik.build.json'])

    # Native C objects, symbol-check dumps and failed build outputs are scratch.
    ffi = root / 'tests/fixtures/native_call_buffers/sputnik.build.json'
    ffi_out = work / 'ffi'
    run(compiler, 'build', ffi, '--out-dir', ffi_out, '--require-full-native', '--grant', 'ffi')
    clean(ffi_out, ['test.call_buffers'])
    assert 'PASS native call buffers' in run(ffi_out / 'test.call_buffers').stdout

    # A compiler wrapper fails after writing a partial output. The old binary
    # must survive, diagnostics must remain visible, and temporary files vanish.
    wrapper = work / 'fail-cxx'
    wrapper.write_text('''#!/usr/bin/env python3
import os, pathlib, sys
if '--version' in sys.argv:
    os.execvp('clang++', ['clang++', '--version'])
if '-o' in sys.argv:
    pathlib.Path(sys.argv[sys.argv.index('-o') + 1]).write_text('partial')
sys.stderr.write('intentional C++ failure\\n')
sys.exit(1)
''')
    wrapper.chmod(0o755)
    key = run(compiler, '--native-runtime-key', wrapper).stdout.strip()
    cached = work / 'runtime-cache' / key
    cached.mkdir(parents=True)
    shutil.copyfile(compiler.parent / 'libsputnik_rt.a', cached / 'libsputnik_rt.a')
    previous = binary.read_bytes()
    failure = run(compiler, 'build', source,
                  environment=dict(env, SPUTNIK_NATIVE_CXX=str(wrapper)), ok=False)
    assert 'intentional C++ failure' in failure.stderr, failure
    assert binary.read_bytes() == previous
    assert not list(work.glob('.sputnik-build-*'))
    clean(scratch, [])

    # Concurrent invocations cannot collide on intermediate object names.
    outputs = [work / 'concurrent' / name for name in ['one', 'two']]
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        list(pool.map(lambda output: run(compiler, 'build', source, '-o', output), outputs))
    clean(outputs[0].parent, ['one', 'two'])
    assert [run(path).stdout for path in outputs] == ['Ohai\n', 'Ohai\n']

    # Known print calls, aliases, changing callees, joins, capture mutation and
    # exceptional paths must preserve VM semantics and diagnostic categories.
    programs = {
        'values': 'print("A", [1, 2], null, true)\nprint()\n',
        'alias': 'p = &print\np("alias")\n',
        'shadow': 'def print(value): value + 1\nprint(41)\n',
        'reassignment': 'p = &print\np("before")\np = |x|: x + 1\np(41)\n',
        'join': 'p = &print\nif [1].size == 1:\n  p = |x|: x + 1\np(41)\n',
        'capture': 'p = &print\ndef change():\n  p = |x|: x + 1\nchange()\np(41)\n',
        'invalid': 'print("bad", unknown: 1)\n',
        'handler': 'try:\n  print("try")\n  raise ValueError("expected")\nrescue ValueError:\n  print("rescued")\n',
        'task': 'job = task.spawn: 41\nprint(job.wait() + 1)\n',
    }
    for name, text in programs.items():
        case = work / (name + '.spu')
        case.write_text(text)
        vm = run(compiler, 'run', case, ok=name != 'invalid')
        executable = work / (name + '-native')
        run(compiler, 'build', case, '-o', executable, '--require-full-native')
        native = run(executable, ok=name != 'invalid')
        assert native.returncode == vm.returncode and native.stdout == vm.stdout, (name, vm, native)
        if name == 'invalid':
            assert 'TypeError' in native.stderr and 'TypeError' in vm.stderr, (vm, native)
        clean(scratch, [])

print('native_build_cli_test: clean builds, export, errors, FFI, concurrency and VM parity passed')
