#!/usr/bin/env python3
"""Native selection, variant coexistence, exact locks and compiler-free install."""
import hashlib
import http.server
import json
import os
from pathlib import Path
import platform
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
    cpp.write_text('#include "runtime/sputnik_ext.h"\nextern "C" uint32_t sputnik_ext_abi_version(){return SPUTNIK_EXT_ABI_VERSION;}\n'
                   'extern "C" SputnikStatus native_answer(SputnikCtx*c,const SputnikValue*,size_t,SputnikValue*out){*out=sputnik_make_int(c,42);return SPUTNIK_OK;}\n')
    library='lib/answer.'+('dylib' if mac else 'so')
    flags=['-dynamiclib','-Wl,-undefined,dynamic_lookup'] if mac else ['-shared','-fPIC']
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-O2',*flags,'-I'+str(source_root),str(cpp),'-o',str(stage/library)],check=True)
    (stage/'main.s').write_text('package test.native_test\nexport answer\nnative def answer() -> Int from "native_answer"\n')
    common='schema="sputnik.package-manifest.v1"\n[package]\nname="test/native"\nversion="1.0.0"\nroot="test.native_test"\n'
    module='\n[[modules]]\nname="test.native_test"\nsource="main.s"\n[[native]]\nname="answer"\nkind="prebuilt"\nlibrary="'+library+'"\nsputnik_ext_abi=1\n'
    artifacts={}
    for variant in ['cpu-a','cpu-b','foreign']:
        actual=dict(target)
        if variant=='foreign': actual['arch']='x86_64' if arch=='aarch64' else 'aarch64'
        # Foreign binary architecture is validated by the packer, so use a
        # valid archive and advertise a different target only for pre-download selection.
        text=common+'[artifact]\nvariant='+json.dumps(variant)+'\n[target]\n'+''.join(k+'='+json.dumps(v)+'\n' for k,v in target.items())+module
        (stage/'sputnik.toml').write_text(text)
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
                a=artifacts[v]
                return dict(package_ref='test/native',version='1.0.0',variant=v,kind='library',bundle='none',native_mode='prebuilt',native_payload=True,
                    target=a['target'],compatibility=dict(sputnik_ext_abi=1),quarantined=False,size=len(a['body']),sha256='sha256:'+'0'*64 if state['bad_digest'] else a['sha256'],
                    url=origin+base+'/versions/1.0.0/artifacts/'+v+'/download')
            if '/artifacts/' in self.path:
                v=self.path.split('/artifacts/')[1].split('/')[0]
                if self.path.endswith('/download'):
                    state['downloads']+=1
                    body=artifacts[v]['body']; headers={'x-sputnik-sha256':artifacts[v]['sha256']}
                else: body=json.dumps(metadata(v)).encode(); headers={}
            else:
                release=dict(package_ref='test/native',version='1.0.0',yanked=False,artifacts=[metadata(v) for v in state['variants']])
                if self.path.endswith('/index'): data=dict(package_ref='test/native',versions=[release],next_cursor=None)
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
        assert 'NO_COMPATIBLE_ARTIFACT' in install(ok=False)
        assert state['downloads']==0
        env['SPUTNIK_NATIVE_CXX']='/no/compiler'
        install('--allow-native')
        app=root/'app.s'; app.write_text('package app\nfrom test.native_test import answer\nio.stdout.write_line(answer().to_str)\n')
        assert cli('run',app,'--grant','ffi').strip()=='42'
        if len(sys.argv)>2:
            subprocess.run([str(Path(sys.argv[2]).resolve()),'--native-registry',env['SPUTNIK_PKG_HOME']],env=env,check=True,timeout=90)
        cli('pkg','lock',app)
        lock=root/'sputnik.lock'; original_lock=lock.read_text()
        assert 'variant = "cpu-a"' in original_lock and artifacts['cpu-a']['sha256'] in original_lock and origin in original_lock
        state['variants']=['cpu-a','cpu-b','foreign']
        assert 'AMBIGUOUS' in install('--allow-native',ok=False)
        before=state['downloads']
        assert 'NO_COMPATIBLE_ARTIFACT' in install('--allow-native','--variant','foreign',ok=False)
        assert state['downloads']==before
        install('--allow-native','--variant','cpu-b')
        assert cli('run',app,'--grant','ffi').strip()=='42'  # lock keeps cpu-a
        for v in ['cpu-a','cpu-b']: assert (root/'home/test/native/1.0.0'/v).is_dir()
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
        cpp.write_text('#include "runtime/sputnik_ext.h"\nextern "C" int fixture_value();\n'
                       'extern "C" SputnikStatus native_answer(SputnikCtx*c,const SputnikValue*,size_t,SputnikValue*out){*out=sputnik_make_int(c,fixture_value());return SPUTNIK_OK;}\n')
        native_source='\n[[modules]]\nname="test.native_test"\nsource="main.s"\n[[native]]\nname="answer"\nkind="source"\nlanguage="c++"\nsources=["answer.cpp"]\nrequirements=["fixture"]\n'
        identity='kind="runtime"\nversion="1.0.0"\nabi="fixture-1"\nbuild_id="sha256:'+'a'*64+'"\n'
        providers='\n[[native_requirements]]\nid="fixture"\n'+identity+'provider="bundled"\n\n[[native_providers]]\nid="fixture"\n'+identity+'library_dirs=["lib"]\nlink_libraries=["'+('fixture.1' if mac else 'fixture')+'"]\n'
        for name in [runtime_library]+([] if mac else ['lib/libfixture.so']):
            providers+='\n[[native_providers.libraries]]\npath='+json.dumps(name)+'\nrole="dependency"\nabi="fixture-1"\n'
        (stage/'sputnik.toml').write_text(common.replace('test/native','test/source')+'[artifact]\nvariant="source-runtime"\nbundle="runtime"\n[target]\n'+''.join(k+'='+json.dumps(v)+'\n' for k,v in target.items())+native_source+providers)
        source_artifact=root/'source.sputnikpkg'
        cli('pkg','build',stage,'--out',source_artifact)
        cli('pkg','install',source_artifact,'--allow-native','--bundle','runtime','--build-from-source')
        app.unlink(); lock.unlink()
        app.write_text('from test.native_test import answer\nio.stdout.write_line(answer().to_str)\n')
        env['SPUTNIK_NATIVE_CXX']='/no/compiler'
        assert cli('run',app,'--grant','ffi').strip()=='42'
    finally:
        server.shutdown(); server.server_close(); thread.join()
print('package_native_registry_cli_test: PASS')
