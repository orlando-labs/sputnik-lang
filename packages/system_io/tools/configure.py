#!/usr/bin/env python3
"""Stage SDK filesystem/digest/process/package leaves with exact SDK inputs.

System IO calls SDK C++ package inspection in addition to the stable extension
ABI. Its native SDK and dependent runtimes are carried in this verified bundle.
"""
import argparse,hashlib,json,os,shutil,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[4]
p=argparse.ArgumentParser()
p.add_argument('--sdk',type=Path,default=Path(os.environ.get('SPUTNIK_ROOT',ROOT/'sputnik-lang')))
p.add_argument('--package',type=Path)
a=p.parse_args();SDK=a.sdk.resolve();repo=(a.package or SDK/'packages/system_io').resolve();base=repo/'build/release-input'
if base.exists():shutil.rmtree(base)
base.mkdir(parents=True,exist_ok=True)
mac=sys.platform=='darwin'
if not mac: raise SystemExit('Current standalone System IO bundle recipe supports macOS arm64')
libs=base/'lib/vendor';libs.mkdir(parents=True,exist_ok=True)
queue=[Path('/opt/homebrew/opt/'+pkg+'/lib/'+name) for pkg,name in [('libarchive','libarchive.dylib'),('openssl@3','libssl.3.dylib'),('openssl@3','libcrypto.3.dylib'),('libyaml','libyaml.dylib'),('nettle','libhogweed.dylib'),('nettle','libnettle.dylib'),('gmp','libgmp.dylib')]]
images={}
while queue:
 original=queue.pop().resolve()
 if original in images:continue
 target=libs/original.name
 if target.exists() and target.read_bytes()!=original.read_bytes():raise RuntimeError('Library basename collision')
 shutil.copy2(original,target);images[original]=target
 for line in subprocess.check_output(['otool','-L',str(original)],text=True).splitlines()[1:]:
  dep=line.strip().split(' (')[0]
  if dep.startswith('/opt/homebrew/'):queue.append(Path(dep))
for original,target in images.items():
 args=['install_name_tool','-id','@rpath/'+target.name]
 for line in subprocess.check_output(['otool','-L',str(original)],text=True).splitlines()[1:]:
  dep=line.strip().split(' (')[0]
  if dep.startswith('/opt/homebrew/'):args+=['-change',dep,'@loader_path/'+images[Path(dep).resolve()].name]
 subprocess.run(args+[str(target)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
 subprocess.run(['codesign','--force','--sign','-',str(target)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
headers=base/'include';headers.mkdir(exist_ok=True)
for directory in ['runtime','package','profile','bytecode','frontend','frozen','optimizer','buildsys','third_party']:
 for src in (SDK/directory).rglob('*.h'):
  dest=headers/src.relative_to(SDK);dest.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(src,dest)
for prefix in ['libarchive','openssl@3']:
 include=Path('/opt/homebrew/opt')/prefix/'include'
 for src in include.rglob('*'):
  if src.is_file():
   dest=headers/src.relative_to(include);dest.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(src.resolve(),dest)
static=base/'lib/libsputnik.a';shutil.copy2(SDK/'build/libsputnik.a',static)
# Add the SDK and runtime closure as declared package assets, never host paths.
files=[p.relative_to(base).as_posix() for p in sorted(base.rglob('*')) if p.is_file()]
runtime_id='sha256:'+hashlib.sha256(''.join(p.name+hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(libs.iterdir())).encode()).hexdigest()
sdk_id='sha256:'+hashlib.sha256(static.read_bytes()).hexdigest()
version='0.1.0';abi='sputnik-sdk-'+hashlib.sha256((SDK/'build/sputnik').read_bytes()).hexdigest()
runtime=dict(id='system-io-runtime',kind='runtime',version=version,abi=abi,build_id=runtime_id,library_dirs=['vendor/lib/vendor'],link_libraries=['archive','ssl','crypto','yaml','hogweed','nettle','gmp','z'],libraries=[dict(path='vendor/'+p.relative_to(base).as_posix(),role='dependency') for p in sorted(libs.iterdir())])
provider=dict(id='sputnik-sdk',kind='sdk',version=version,abi=abi,build_id=sdk_id,runtime_build_id=runtime_id,include_dirs=['vendor/include'],files=['vendor/'+f for f in files if f.startswith('include/') or f=='lib/libsputnik.a'])
requirements=[dict(id=runtime['id'],kind='runtime',version=version,abi=abi,build_id=runtime_id,provider='bundled'),dict(id=provider['id'],kind='sdk',version=version,abi=abi,build_id=sdk_id,provider='bundled',runtime=runtime['id'])]
# The manifest's runtime provider supplies dynamic link names; static SDK is
# linked through a package-relative library directory by the source extension.
extension=dict(name='system_io',language='c++',sources=['native/system_io.cpp'],requirements=[r['id'] for r in requirements],include_dirs=['vendor/include'],library_dirs=['vendor/lib'],link_libraries=['sputnik'],defines=['SPUTNIK_VALUE_REPR_TAGGED'])
cfg=dict(name='system-io',version=version,root='sdk.fs',sources=['src/*.s'],native=[extension],native_requirements=requirements,native_providers=[runtime,provider],assets={str(base):'vendor'},bundle='sdk',build_include_dirs=[str(headers)],build_library_dirs=[str(base/'lib'),str(libs)],build_link_libraries=['sputnik','archive','ssl','crypto','yaml','hogweed','nettle','gmp','z'])
names=[Path(l['path']).name[3:-6] for l in runtime['libraries']]
runtime['link_libraries']=names+['z'];cfg['build_link_libraries']=['sputnik']+names+['z']
(repo/'build/release-config.json').write_text(json.dumps(cfg,indent=2)+'\n')
print(repo/'build/release-config.json')
# Standalone releases keep SDK/native runtimes external. Bind the verified
# installed SDK locally instead of redistributing a private SDK snapshot.
Q=json.dumps
bindings=repo/'build/release-providers'
if bindings.exists():shutil.rmtree(bindings)
rt=bindings/'runtime';sd=bindings/'sdk'
for d in [rt,sd]:d.mkdir(parents=True,exist_ok=True)
shutil.copytree(base/'lib',rt/'lib',dirs_exist_ok=True)
shutil.copytree(base/'include',sd/'include',dirs_exist_ok=True)
rt_files=[p.relative_to(rt).as_posix() for p in sorted(rt.rglob('*')) if p.is_file() and p.name not in ['sputnik.build.json','checksums.sha256']]
sd_files=[p.relative_to(sd).as_posix() for p in sorted(sd.rglob('*')) if p.is_file() and p.name not in ['sputnik.build.json','checksums.sha256']]
runtime=dict(id='system-io-runtime',kind='runtime',version=version,abi=abi,build_id=runtime_id,library_dirs=['lib','lib/vendor'],link_libraries=['sputnik']+names+['z'],files=rt_files,libraries=[dict(path='lib/vendor/'+p.name,role='dependency') for p in sorted(libs.iterdir())])
provider=dict(id='sputnik-sdk',kind='sdk',version=version,abi=abi,build_id=sdk_id,runtime_build_id=runtime_id,include_dirs=['include'],files=sd_files)
for d,item in [(rt,runtime),(sd,provider)]:
 descriptor=dict(schema='sputnik.build.v1',kind='native',name='local-'+item['id'],version='0.1.0',
                 files=item['files'],target=dict(os='macos',arch='aarch64'),native_providers=[item])
 (d/'sputnik.build.json').write_text(json.dumps(descriptor,indent=2)+'\n')
 files=item['files']+['sputnik.build.json']
 (d/'checksums.sha256').write_text(''.join(hashlib.sha256((d/f).read_bytes()).hexdigest()+'  '+f+'\n' for f in sorted(files)))
for r in requirements:r['provider']='external'
requirements[0]['libraries']=[Path(p['path']).name for p in runtime['libraries']]
extension.update(include_dirs=[],library_dirs=[],link_libraries=[])
cfg.update(bundle='none',assets={},native_providers=[],native_requirements=requirements,native=[extension])
(repo/'build/release-config.json').write_text(json.dumps(cfg,indent=2)+'\n')
print('SPUTNIK_NATIVE_PROVIDERS='+str(rt)+':'+str(sd))

# The package uses the same build manifest as consumers and the packer.
manifest=dict(schema='sputnik.build.v1',name='system_io',version=version,source_root='src',
              files=sorted([p.relative_to(repo).as_posix() for p in (repo/'src').glob('*.s')]+['native/system_io.cpp','README.md']),
              profiles=dict(required=['core.v1','ffi.v1']),native_extensions=[extension],native_requirements=requirements)
manifest['module_names']={p.relative_to(repo).as_posix(): 'sdk.'+p.stem for p in (repo/'src').glob('*.s')}
(repo/'sputnik.build.yaml').write_text(''.join(key+': '+Q(value)+'\n' for key,value in manifest.items()))
