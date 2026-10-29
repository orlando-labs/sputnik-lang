#!/usr/bin/env python3
"""Real SDK, public tar distributions, and a loopback protocol fixture."""
import hashlib
import http.server
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading

sdk = str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix='sputnik-registry-cli-') as temporary:
    root = Path(temporary)
    env = dict(os.environ, SPUTNIK_PKG_HOME=str(root/'home'), SPUTNIK_PKG_REGISTRY='')
    def cli(*args, ok=True):
        result = subprocess.run([sdk, *args], env=env, capture_output=True, text=True, timeout=90)
        assert (result.returncode == 0) == ok, (args, result.stdout, result.stderr)
        return result.stdout + result.stderr
    assert 'SPUTNIK_PKG_REGISTRY=https://registry.sputnik-lang.online' in cli('pkg', 'env')
    packages = {}
    for name in ['base', 'lib']:
        directory = root/name; directory.mkdir()
        (directory/'main.s').write_text(f'package test.{name}\nexport answer\ndef answer(): 42\n')
        dependencies = 'dependencies:\n  \"test/base\": \"1.0.0\"\n' if name == 'lib' else ''
        (directory/'sputnik.build.yaml').write_text(f'schema: "sputnik.build.v1"\nname: "test/{name}"\nversion: "1.0.0"\nsource_root: "."\nfiles:\n  - "main.s"\nmodule_names:\n  main.s: "test.{name}"\nartifact:\n  variant: "source"\n'+dependencies)
        archive = root/(name+'.sputnikpkg')
        cli('pkg', 'build', str(directory), '--out', str(archive))
        packages[name] = archive.read_bytes()
    state = dict(mode='good', requests=[])
    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_): pass
        def do_GET(self):
            state['requests'].append(self.path)
            name = 'base' if '/base' in self.path else 'lib'
            ref = 'test/'+name
            base = '/api/v1/namespaces/test/packages/'+name
            artifact = base+'/versions/1.0.0/artifacts/source'
            body = packages[name]
            digest = 'sha256:'+hashlib.sha256(body).hexdigest()
            status = 200; headers = {}
            if self.path.endswith('/download'):
                headers['x-sputnik-sha256'] = digest
                if state['mode'] == 'tamper': body = body[:-1] + bytes([body[-1] ^ 1])
                if state['mode'] == 'partial-status': status = 206
            else:
                release = dict(package_ref=ref, version='1.0.0', yanked=False,
                    artifacts=[dict(variant='source', quarantined=False)])
                if self.path.endswith('/index'):
                    versions = [dict(release, version='9.0.0', yanked=True), dict(release, version='2.0.0-beta.1'), release]
                    data = dict(package_ref=ref, versions=versions, next_cursor=None)
                elif self.path.endswith('/artifacts/source'):
                    data = dict(package_ref=ref, version='1.0.0', variant='source', size=len(body), sha256=digest, url=origin+artifact+'/download')
                    if state['mode'] == 'foreign-url': data['url'] = 'https://other.invalid/download'
                    if state['mode'] == 'identity': data['package_ref'] = 'other/lib'
                    if state['mode'] == 'size': data['size'] = 67108865
                elif '/versions/' in self.path: data = release
                else: data = dict(package_ref=ref, requested_ref=name)
                body = json.dumps(data).encode()
                if state['mode'] == 'duplicate' and '/artifacts/source' in self.path:
                    body = body[:-1]+b',"size":1}'
                if state['mode'] == 'redirect': status = 302; headers['location'] = origin+base
            self.send_response(status)
            for key, value in headers.items(): self.send_header(key, value)
            self.send_header('content-length', str(len(body)))
            self.end_headers(); self.wfile.write(body)
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    origin = f'http://127.0.0.1:{server.server_port}'
    thread = threading.Thread(target=server.serve_forever, daemon=True); thread.start()
    try:
        output = cli('pkg', 'install', 'lib', '--registry', origin)
        assert 'test/lib@1.0.0' in output and 'test/base@1.0.0' in output, output
        assert '2.0.0-beta.1' not in ''.join(state['requests'])
        app = root/'app.s'; app.write_text('package app\nfrom test.lib import answer\nio.stdout.write_line(answer().to_str)\n')
        assert cli('run', str(app)).strip() == '42'
        cli('pkg', 'install', 'test/lib', '--registry', origin)
        archive = root/'home/test/lib/1.0.0/source/test-lib-1.0.0.sputnikpkg'
        archive.rename(root/'retained.sputnikpkg')
        assert 'missing installed package artifact' in cli('run', str(app), ok=False)
        (root/'retained.sputnikpkg').rename(archive)
        # Activation checks extracted source as well as the retained archive.
        member = root/'home/test/lib/1.0.0/source/main.s'; original = member.read_bytes()
        member.write_bytes(original+b'\n# changed\n')
        assert 'modified' in cli('run', str(app), ok=False)
        member.write_bytes(original)
        for mode in ['foreign-url', 'identity', 'size', 'tamper', 'duplicate', 'redirect', 'partial-status']:
            state['mode'] = mode
            env['SPUTNIK_PKG_HOME'] = str(root/('bad-'+mode))
            cli('pkg', 'install', 'lib', '--registry', origin, ok=False)
            assert not (root/('bad-'+mode)/'test/lib/1.0.0').exists(), mode
        state['mode'] = 'good'
        credentials = root/'credentials.json'; credentials.write_text(json.dumps(dict(origin=origin, token='spreg_example')))
        credentials.chmod(0o644)
        assert 'private' in cli('pkg', 'install', 'lib', '--registry', origin, '--credentials', str(credentials), ok=False)
        credentials.chmod(0o600)
        credentials.write_text(json.dumps(dict(origin='https://wrong.invalid', token='spreg_example')))
        assert 'origin differs' in cli('pkg', 'install', 'lib', '--registry', origin, '--credentials', str(credentials), ok=False)
        env['SPUTNIK_PKG_HOME'] = str(root/'home')
        namespace = root/'home/test'
        namespace.rename(root/'outside-namespace')
        namespace.symlink_to(root/'outside-namespace', target_is_directory=True)
        assert 'symlinked namespace' in cli('pkg', 'remove', 'test/lib', ok=False)
        assert (root/'outside-namespace/lib/1.0.0').exists()
        namespace.unlink(); (root/'outside-namespace').rename(namespace)
        cli('pkg', 'remove', 'test/lib', '--version', '1.0.0')
        assert not (root/'home/test/lib/1.0.0').exists()
    finally:
        server.shutdown(); server.server_close(); thread.join()
print('package_registry_cli_test ok')
