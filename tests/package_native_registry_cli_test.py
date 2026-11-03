#!/usr/bin/env python3
"""Native selection, variant coexistence, exact locks and compiler-free install."""
import hashlib
import http.server
import json
import os
from pathlib import Path
import platform
import pty
import shutil
import subprocess
import sys
import tempfile
import threading

sdk = str(Path(sys.argv[1]).resolve())
source_root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='sputnik-native-registry-') as temporary:
    root = Path(temporary)
    env = dict(os.environ, SPUTNIK_PKG_HOME=str(root/'home'))
    env.pop('SPUTNIK_PKG_CREDENTIALS', None)
    def cli(*args, ok=True):
        result = subprocess.run([sdk, *map(str,args)], env=env, capture_output=True, text=True, timeout=90)
        assert (result.returncode == 0) == ok, (args,result.stdout,result.stderr)
        return result.stdout+result.stderr
    mac = sys.platform == 'darwin'
    arch = {'arm64':'aarch64','amd64':'x86_64'}.get(platform.machine(),platform.machine())
    target = dict(os='macos' if mac else 'linux',arch=arch,compute_backend='cpu',cpu_features=[],compute_backends=[])
    if mac: target['min_os'] = platform.mac_ver()[0]
    else: target.update(libc='gnu',min_libc='2.28')
    stage = root/'stage'; (stage/'lib').mkdir(parents=True)
    cpp=stage/'answer.cpp'
    cpp.write_text('#include "runtime/sputnik_ext.h"\nextern "C" const char *sputnik_language_version(){return SPUTNIK_VERSION;}\n'
                   'extern "C" SputnikStatus native_answer(SputnikCtx*c,const SputnikValue*,size_t,SputnikValue*out){*out=sputnik_make_int(c,42);return SPUTNIK_OK;}\n')
    library='lib/answer.'+('dylib' if mac else 'so')
    flags=['-dynamiclib','-Wl,-undefined,dynamic_lookup'] if mac else ['-shared','-fPIC']
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-O2',*flags,'-I'+str(source_root),str(cpp),'-o',str(stage/library)],check=True)
    (stage/'main.s').write_text('package test.native_test\nexport answer\nnative def answer() -> Int from "native_answer"\n')
    (stage/'configure.s').write_text('raise "prebuilt configure must not run"\n')
    common=dict(name='test/native',version='1.0.0',files=['main.s',library,'configure.s'],module_names={'main.s':'test.native_test'},profiles=dict(required=['core.v1','ffi.v1']))
    module=dict(name='answer',kind='prebuilt',library=library,sputnik_version="1.0.0")
    artifacts={}
    for variant in ['cpu-a','cpu-b','foreign']:
        actual=dict(target)
        if variant=='foreign': actual['arch']='x86_64' if arch=='aarch64' else 'aarch64'
        # Foreign binary architecture is validated by the packer, so use a
        # valid archive and advertise a different target only for pre-download selection.
        text=dict(common,artifact=dict(variant=variant),target=target,native_extensions=[module])
        (stage/'sputnik.build.yaml').write_text(json.dumps(text))
        file=root/(variant+'.sputnikpkg')
        cli('pkg','build',stage,'--out',file)
        body=file.read_bytes()
        artifacts[variant]=dict(body=body,target=actual,sha256='sha256:'+hashlib.sha256(body).hexdigest())
    state=dict(variants=['cpu-a'],bad_digest=False,downloads=0)
    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self,*_): pass
        def do_GET(self):
            base='/api/v1/namespaces/test/packages/native'
            def metadata(v):
                a=artifacts.get(v, artifacts['cpu-a'])
                item = dict(package_ref='test/native',version='1.0.0',variant=v,kind='library',bundle='none',native_mode='prebuilt',native_payload=True,
                    target=a['target'],compatibility=dict(sputnik_requirement=">=1.0.0 <2.0.0",sputnik_versions=["1.0.0"]),quarantined=False,size=len(a['body']),sha256='sha256:'+'0'*64 if state['bad_digest'] else a['sha256'],
                    url=origin+base+'/versions/1.0.0/artifacts/'+v+'/download')
                if v == 'native-source': item['native_mode'] = 'source'
                if v == 'source-runtime': item.update(native_mode='source', bundle='runtime')
                if v == 'source':
                    item.update(native_mode='none', native_payload=False, target=dict(os='any', arch='any', compute_backend='none'))
                if v == 'cpu-mps': item['target'] = dict(a['target'], compute_backends=['cpu', 'mps'])
                if v == 'wrong-abi': item['compatibility']['sputnik_versions'] = ['999.0.0']
                if state.get('large'): item['size'] = 70 * 1024 * 1024
                return item
            if '/artifacts/' in self.path:
                v=self.path.split('/artifacts/')[1].split('/')[0]
                if self.path.endswith('/download'):
                    state['downloads']+=1
                    body=artifacts[v]['body']; headers={'x-sputnik-sha256':artifacts[v]['sha256']}
                else: body=json.dumps(metadata(v)).encode(); headers={}
            else:
                release=dict(package_ref='test/native',version='1.0.0',yanked=False,artifacts=[metadata(v) for v in state['variants']])
                if self.path.endswith('/index'): data=dict(package_ref='test/native',versions=[release],revision=1,next_cursor=None)
                elif '/versions/' in self.path: data=release
                else: data=dict(package_ref='test/native')
                body=json.dumps(data).encode(); headers={}
            self.send_response(200)
            for key,value in headers.items(): self.send_header(key,value)
            self.send_header('content-length',str(len(body))); self.end_headers(); self.wfile.write(body)
    server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler)
    origin='http://127.0.0.1:'+str(server.server_port)
    thread=threading.Thread(target=server.serve_forever,daemon=True); thread.start()
    try:
        def install(*args,ok=True): return cli('pkg','install','test/native','--registry',origin,*args,ok=ok)
        def interactive_install(answer, *args, ok=True):
            master, terminal = pty.openpty()
            try:
                with subprocess.Popen([sdk, 'pkg', 'install', 'test/native', '--registry', origin, *args],
                                      env=env, stdin=terminal, stdout=subprocess.PIPE,
                                      stderr=subprocess.PIPE, text=True) as process:
                    os.write(master, (answer + '\n').encode())
                    stdout, stderr = process.communicate(timeout=90)
                    assert (process.returncode == 0) == ok, (args, stdout, stderr)
                    return stdout + stderr
            finally:
                os.close(master)
                os.close(terminal)
        def inspect():
            return json.loads(cli('pkg', 'variants', 'test/native@1.0.0', '--registry', origin, '--json', '--check-host'))
        state['variants'] = ['source']
        inspected = inspect()
        assert inspected['recommended_variant'] == 'source'
        selection = inspected['artifacts'][0]['host_selection']
        assert selection['compatible'] and not selection['allow_native'] and selection['backend'] == 'cpu'
        state['variants'] = ['native-source', 'cpu-a', 'foreign', 'wrong-abi']
        inspected = inspect()
        assert inspected['recommended_variant'] == 'cpu-a'
        selections = {a['variant']: a['host_selection'] for a in inspected['artifacts']}
        assert selections['native-source']['compatible'] and selections['native-source']['build_from_source']
        assert not selections['foreign']['compatible'] and 'architecture' in selections['foreign']['reason']
        assert not selections['wrong-abi']['compatible'] and 'LANGUAGE_VERSION_MISMATCH' in selections['wrong-abi']['reason']
        assert state['downloads'] == 0 and not (root/'home').exists()
        state['variants'] = ['foreign']
        assert inspect()['recommended_variant'] is None
        state['variants'] = ['cpu-mps']
        selection = inspect()['artifacts'][0]['host_selection']
        assert selection['compatible'] and selection['backend'] == ('mps' if mac else 'cpu')
        state['variants'] = ['cpu-a']
        denied = install(ok=False)
        assert 'NO_COMPATIBLE_ARTIFACT: test/native@1.0.0' in denied
        assert 'native package requires --allow-native' in denied
        assert state['downloads']==0
        declined = interactive_install('')
        assert 'Install test/native@1.0.0 (cpu-a,' in declined
        assert 'Contains native code' in declined and 'Installation cancelled' in declined
        assert state['downloads'] == 0 and not (root/'home').exists()
        state['large'] = True
        declined = interactive_install('n')
        assert '70.0 MiB (73400320 bytes)' in declined
        assert 'Exceeds the 64 MiB download limit' in declined
        assert 'Exceeds the 64 MiB download limit' not in interactive_install('n', '--allow-native-large')
        assert state['downloads'] == 0
        state['large'] = False
        denied = interactive_install('y', '--backend', 'mps', '--variant', 'cpu-a', ok=False)
        assert 'different compute backend' in denied and state['downloads'] == 0
        state['variants'] = ['native-source']
        declined = interactive_install('n', '--build-from-source')
        assert '(native-source,' in declined and 'Builds native code from source' in declined
        assert state['downloads'] == 0
        state['variants'] = ['cpu-b', 'cpu-a']
        env['SPUTNIK_NATIVE_CXX']='/no/compiler'
        accepted = interactive_install('y')
        assert 'Install test/native@1.0.0 (cpu-a,' in accepted
        assert 'Installed test/native@1.0.0' in accepted and state['downloads'] == 1
        reused = interactive_install('')
        assert 'Already installed test/native@1.0.0 (cpu-a)' in reused
        assert 'No update available.' in reused
        assert 'Proceed? [y/N]' not in reused and state['downloads'] == 1
        pinned = interactive_install('', '--version', '1.0.0')
        assert 'Already installed test/native@1.0.0 (cpu-a)' in pinned
        assert 'No update available.' not in pinned and state['downloads'] == 1
        explicit = interactive_install('n', '--version', '1.0.0', '--variant', 'cpu-b',
                                       '--backend', 'cpu', '--bundle', 'none', '--allow-native')
        assert 'Install test/native@1.0.0 (cpu-b,' in explicit and state['downloads'] == 1
        app=root/'app.s'; app.write_text('package app\nfrom test.native_test import answer\nio.stdout.write_line(answer().to_str)\n')
        assert cli('run',app,'--grant','ffi').strip()=='42'
        if len(sys.argv)>2:
            subprocess.run([str(Path(sys.argv[2]).resolve()),'--native-registry',env['SPUTNIK_PKG_HOME']],env=env,check=True,timeout=90)
        cli('pkg','lock',app)
        lock=root/'sputnik.lock'; original_lock=lock.read_text()
        assert 'variant = "cpu-a"' in original_lock and artifacts['cpu-a']['sha256'] in original_lock and origin in original_lock
        state['variants']=['cpu-a','cpu-b','foreign']
        assert 'AMBIGUOUS' in install('--allow-native',ok=False)
        assert inspect()['recommended_variant'] == 'cpu-a'
        before=state['downloads']
        assert 'NO_COMPATIBLE_ARTIFACT' in install('--allow-native','--variant','foreign',ok=False)
        assert state['downloads']==before
        install('--allow-native','--variant','cpu-b')
        assert cli('run',app,'--grant','ffi').strip()=='42'  # lock keeps cpu-a
        for v in ['cpu-a','cpu-b']: assert (root/'home/test/native/1.0.0'/v).is_dir()
        assert cli('pkg', 'list').splitlines() == [
            'test/native@1.0.0 (cpu-a)', 'test/native@1.0.0 (cpu-b)',
        ]
        lock.write_text(original_lock.replace(artifacts['cpu-a']['sha256'],'sha256:'+'0'*64))
        assert 'differs from sputnik.lock' in cli('run',app,'--grant','ffi',ok=False)
        lock.write_text(original_lock)
        env['SPUTNIK_PKG_HOME']=str(root/'restore')
        install('--allow-native','--locked',lock)
        assert (root/'restore/test/native/1.0.0/cpu-a').is_dir()
        assert not (root/'restore/test/native/1.0.0/cpu-b').exists()
        state['bad_digest']=True
        env['SPUTNIK_PKG_HOME']=str(root/'bad-restore')
        assert 'differs from sputnik.lock' in install('--allow-native','--locked',lock,ok=False)
        env['SPUTNIK_PKG_HOME']=str(root/'home')
        cli('pkg','remove','test/native','--version','1.0.0','--variant','cpu-b')
        assert (root/'home/test/native/1.0.0/cpu-a').is_dir()
        assert not (root/'home/test/native/1.0.0/cpu-b').exists()
        # Source adapters must keep their bundled runtime after the staging
        # directory is atomically renamed to its final installed location.
        env['SPUTNIK_PKG_HOME']=str(root/'source-home')
        env.pop('SPUTNIK_NATIVE_CXX',None)
        runtime=stage/'runtime.cpp'
        runtime.write_text('extern "C" int fixture_value(){return 42;}\n')
        runtime_library='lib/libfixture.'+('1.dylib' if mac else 'so.13')
        runtime_flags=['-dynamiclib','-Wl,-install_name,@rpath/libfixture.1.dylib'] if mac else ['-shared','-fPIC','-Wl,-soname,libfixture.so.13']
        subprocess.run([os.environ.get('CXX','c++'),*runtime_flags,str(runtime),'-o',str(stage/runtime_library)],check=True)
        if not mac: shutil.copyfile(stage/runtime_library,stage/'lib/libfixture.so')
        cpp.write_text('#include "runtime/sputnik_ext.h"\n#include "generated.h"\nextern "C" int fixture_value();\n'
                       'extern "C" SputnikStatus native_answer(SputnikCtx*c,const SputnikValue*,size_t,SputnikValue*out){*out=sputnik_make_int(c,fixture_value() + CONFIGURE_OFFSET);return SPUTNIK_OK;}\n')
        (stage/'configure.s').write_text(
            'print("published configure ran")\n'
            'fs.File.open("generated.h", :write, create: true, exclusive: true) |file|:\n'
            '  file.write_all!("#define CONFIGURE_OFFSET 1\\n".bytes)\n')
        native_source=dict(name='answer',kind='source',language='c++',sources=['answer.cpp'],requirements=['fixture'])
        identity=dict(kind='runtime',version='1.0.0',abi='fixture-1',build_id='sha256:'+'a'*64)
        requirement=dict(identity,id='fixture',provider='bundled')
        libraries=[dict(path=name,role='dependency',abi='fixture-1') for name in [runtime_library]+([] if mac else ['lib/libfixture.so'])]
        provider=dict(identity,id='fixture',library_dirs=['lib'],link_libraries=['fixture.1' if mac else 'fixture'],libraries=libraries)
        manifest=dict(common,files=['main.s','answer.cpp','configure.s']+[l['path'] for l in libraries],artifact=dict(variant='source-runtime',bundle='runtime'),target=target,native_extensions=[native_source],native_requirements=[requirement],native_providers=[provider])
        (stage/'sputnik.build.yaml').write_text(json.dumps(manifest))
        source_artifact=root/'source.sputnikpkg'
        cli('pkg','build',stage,'--out',source_artifact)
        body = source_artifact.read_bytes()
        artifacts['source-runtime'] = dict(body=body, target=target,
                                           sha256='sha256:'+hashlib.sha256(body).hexdigest())
        state['variants'] = ['source-runtime']
        build_flags = ('--allow-native', '--bundle', 'runtime', '--build-from-source')
        grant = ('--grant', 'fs.write=generated.h')
        state['bad_digest'] = True
        failure = install(*build_flags, *grant, ok=False)
        assert 'published configure ran' not in failure
        state['bad_digest'] = False
        failure = install(*build_flags, ok=False)
        assert 'CapabilityError' in failure and 'package configure failed' in failure
        installed = root/'source-home/test/native/1.0.0/source-runtime'
        assert not installed.exists()
        assert not list((root/'source-home').glob('.install-*'))
        assert install(*build_flags, *grant).splitlines().count('published configure ran') == 1
        assert (installed/'test-native-1.0.0.sputnikpkg').read_bytes() == body
        assert not (installed/'generated.h').exists()
        assert 'configure' not in (installed/'modules.index').read_text()
        assert not list((root/'source-home').glob('.install-*'))
        app.unlink(); lock.unlink()
        app.write_text('from test.native_test import answer\nio.stdout.write_line(answer().to_str)\n')
        env['SPUTNIK_NATIVE_CXX']='/no/compiler'
        assert 'published configure ran' not in install(*build_flags, *grant)
        assert cli('run',app,'--grant','ffi').strip()=='43'
        failure = install(*build_flags, '--pristine', ok=False)
        assert 'package configure failed' in failure
        assert cli('run',app,'--grant','ffi').strip()=='43'
        assert not list((root/'source-home').glob('.install-*'))
        # The manifest is only an assertion: the loaded library must report
        # its own language version. Legacy binaries must not inherit the host's
        # version entry point through dynamic symbol lookup.
        for label, handshake, expected in [
            ('future-language', 'extern "C" const char *sputnik_language_version(){return "999.0.0";}\n', '999.0.0'),
            ('legacy-language', 'extern "C" uint32_t sputnik_ext_abi_version(){return 1;}\n', 'sputnik_language_version'),
        ]:
            env['SPUTNIK_PKG_HOME'] = str(root/label)
            cpp.write_text('#include "runtime/sputnik_ext.h"\n' + handshake +
                'extern "C" SputnikStatus native_answer(SputnikCtx*c,const SputnikValue*,size_t,SputnikValue*out){*out=sputnik_make_int(c,42);return SPUTNIK_OK;}\n')
            bad_library = 'lib/'+label+('.dylib' if mac else '.so')
            subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-O2',*flags,'-I'+str(source_root),str(cpp),'-o',str(stage/bad_library)],check=True)
            bad_manifest = dict(common, name='test/'+label, files=['main.s',bad_library], target=target,
                native_extensions=[dict(name='answer',kind='prebuilt',library=bad_library,sputnik_version='1.0.0')])
            (stage/'sputnik.build.yaml').write_text(json.dumps(bad_manifest))
            bad_artifact = root/(label+'.sputnikpkg')
            cli('pkg','build',stage,'--out',bad_artifact)
            cli('pkg','install',bad_artifact,'--allow-native')
            failure = cli('run',app,'--grant','ffi',ok=False)
            assert expected in failure, (label,failure)
    finally:
        server.shutdown(); server.server_close(); thread.join()
print('package_native_registry_cli_test: PASS')
