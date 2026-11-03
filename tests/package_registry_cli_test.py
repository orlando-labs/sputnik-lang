#!/usr/bin/env python3
"""Real SDK, public tar distributions, and a loopback protocol fixture."""
import errno
import hashlib
import http.server
import json
import os
from pathlib import Path
import pty
import select
import subprocess
import sys
import tempfile
import threading
import time

sdk = str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix='sputnik-registry-cli-') as temporary:
    root = Path(temporary)
    env = dict(os.environ, SPUTNIK_PKG_HOME=str(root/'home'), SPUTNIK_PKG_REGISTRY='')
    def cli(*args, ok=True):
        result = subprocess.run([sdk, *args], env=env, capture_output=True, text=True, timeout=90)
        assert (result.returncode == 0) == ok, (args, result.stdout, result.stderr)
        return result.stdout + result.stderr
    assert 'SPUTNIK_PKG_REGISTRY=https://registry.sputnik-lang.online' in cli('pkg', 'env')
    assert cli('pkg', 'list') == 'No packages installed.\n'
    assert not (root/'home').exists()
    packages = {}
    for name in ['leaf', 'base', 'lib']:
        directory = root/name; directory.mkdir()
        (directory/'main.s').write_text(f'package test.{name}\nexport answer\ndef answer(): 42\n')
        dependencies = 'dependencies:\n  \"test/base\": \"1.0.0\"\n' if name == 'lib' else ''
        artifact_dependencies = '  dependencies:\n    \"test/leaf\": \"1.0.0\"\n' if name == 'base' else ''
        (directory/'sputnik.build.yaml').write_text(f'schema: "sputnik.build.v1"\nname: "test/{name}"\nversion: "1.0.0"\nsource_root: "."\nfiles:\n  - "main.s"\nmodule_names:\n  main.s: "test.{name}"\nartifact:\n  variant: "source"\n'+artifact_dependencies+dependencies)
        archive = root/(name+'.sputnikpkg')
        cli('pkg', 'build', str(directory), '--out', str(archive))
        packages[name] = archive.read_bytes()
    manifest = root/'lib/sputnik.build.yaml'
    manifest.write_text(manifest.read_text().replace('version: "1.0.0"', 'version: "1.1.0"'))
    update_archive = root/'lib-update.sputnikpkg'
    cli('pkg', 'build', str(root/'lib'), '--out', str(update_archive))
    update = update_archive.read_bytes()
    state = dict(mode='good', requests=[], update=False)
    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_): pass
        def do_GET(self):
            state['requests'].append(self.path)
            errors = {
                '/api/v1/packages/ololo': [],
                '/api/v1/packages/unscoped': ['test/unscoped', 'other/unscoped', 'test/unscoped-extra'],
                '/api/v1/namespaces/test/packages/absent': None,
            }
            status = 200
            if self.path in errors:
                detail = dict(code='NOT_FOUND')
                if errors[self.path] is not None:
                    detail['suggestions'] = errors[self.path]
                body = json.dumps(dict(error=detail)).encode()
                status = 404
            if status == 404:
                self.send_response(status)
                self.send_header('content-length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return
            name = next((name for name in packages if '/'+name in self.path), 'lib')
            ref = 'test/'+name
            base = '/api/v1/namespaces/test/packages/'+name
            version = '1.1.0' if name == 'lib' and '/versions/1.1.0' in self.path else '1.0.0'
            artifact = base+'/versions/'+version+'/artifacts/source'
            body = update if version == '1.1.0' else packages[name]
            digest = 'sha256:'+hashlib.sha256(body).hexdigest()
            status = 200; headers = {}
            if self.path.endswith('/download'):
                headers['x-sputnik-sha256'] = digest
                if state['mode'] == 'tamper': body = body[:-1] + bytes([body[-1] ^ 1])
                if state['mode'] == 'partial-status': status = 206
            else:
                release = dict(package_ref=ref, version=version, yanked=False,
                    artifacts=[dict(variant='source', quarantined=False, compatibility=dict(sputnik_requirement='>=1.0.0 <2.0.0'))])
                if name == 'lib':
                    release['dependencies'] = [dict(name='test/base', version='1.0.0', source='registry')]
                if name == 'base':
                    release['artifacts'][0]['dependencies'] = [dict(package_ref='test/leaf', version='1.0.0', source='registry')]
                if state['mode'] in ['language-major','language-patch','legacy-language']:
                    item = release['artifacts'][0]
                    if state['mode'] == 'legacy-language': item.pop('compatibility')
                    else: item['compatibility']['sputnik_requirement'] = '>=2.0.0 <3.0.0' if state['mode'] == 'language-major' else '>=1.0.1 <2.0.0'
                if self.path.endswith('/index'):
                    future = dict(release, version='2.0.0', artifacts=[dict(variant='source', quarantined=False, compatibility=dict(sputnik_requirement='>=2.0.0 <3.0.0'))])
                    versions = [future, dict(release, version='9.0.0', yanked=True), dict(release, version='2.0.0-beta.1'), release]
                    if name == 'lib' and state['update']:
                        versions.append(dict(release, version='1.1.0'))
                    data = dict(package_ref=ref, versions=versions, next_cursor=None)
                elif self.path.endswith('/artifacts/source'):
                    data = dict(package_ref=ref, version=version, variant='source', compatibility=dict(sputnik_requirement='>=1.0.0 <2.0.0'), size=len(body), sha256=digest, url=origin+artifact+'/download')
                    if state['mode'] == 'foreign-url': data['url'] = 'https://other.invalid/download'
                    if state['mode'] == 'identity': data['package_ref'] = 'other/lib'
                    if state['mode'] == 'size': data['size'] = 67108865
                elif '/versions/2.0.0' in self.path:
                    data = dict(release, version='2.0.0', artifacts=[dict(variant='source', quarantined=False, compatibility=dict(sputnik_requirement='>=2.0.0 <3.0.0'))])
                elif '/versions/' in self.path: data = release
                else: data = dict(package_ref=ref, requested_ref=name)
                body = json.dumps(data).encode()
                if state['mode'] == 'duplicate' and '/artifacts/source' in self.path:
                    body = body[:-1]+b',"size":1}'
                if state['mode'] == 'redirect': status = 302; headers['location'] = origin+base
            self.send_response(status)
            for key, value in headers.items(): self.send_header(key, value)
            self.send_header('content-length', str(len(body)))
            self.end_headers()
            if state['mode'] == 'slow' and self.path.endswith('/download'):
                midpoint = len(body) // 2
                time.sleep(0.2)
                self.wfile.write(body[:midpoint])
                self.wfile.flush()
                time.sleep(0.2)
                body = body[midpoint:]
            self.wfile.write(body)
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    origin = f'http://127.0.0.1:{server.server_port}'
    thread = threading.Thread(target=server.serve_forever, daemon=True); thread.start()
    try:
        missing = cli('pkg', 'install', 'ololo', '--registry', origin, ok=False)
        assert "Package 'ololo' was not found." in missing and 'HTTP 404' not in missing
        assert state['requests'] == ['/api/v1/packages/ololo']
        scoped = cli('pkg', 'install', 'unscoped', '--registry', origin, ok=False)
        assert 'other/unscoped\n  test/unscoped' in scoped and 'unscoped-extra' not in scoped
        absent = cli('pkg', 'install', 'test/absent', '--registry', origin, ok=False)
        assert "Package 'test/absent' was not found." in absent
        assert state['requests'][-1] == '/api/v1/namespaces/test/packages/absent'
        state['requests'].clear()
        state['requests'].clear()
        missing_install = cli('pkg', 'update', 'lib', '--registry', origin, ok=False)
        assert "Cannot update 'test/lib': package is not installed." in missing_install
        assert state['requests'] == ['/api/v1/packages/lib']
        assert not (root/'home').exists()
        output = cli('pkg', 'install', '--update', 'lib', '--registry', origin)
        assert 'test/lib@1.0.0' in output and 'test/base@1.0.0' in output, output
        assert 'Downloading' not in output
        assert '2.0.0-beta.1' not in ''.join(state['requests'])
        assert any('/versions/2.0.0' in path for path in state['requests'])
        assert not any('/versions/2.0.0/artifacts/' in path for path in state['requests']), state['requests']
        state['requests'].clear()
        state['mode'] = 'slow'
        terminal_env = dict(env, SPUTNIK_PKG_HOME=str(root/'terminal-home'))
        master, terminal = pty.openpty()
        try:
            with subprocess.Popen([sdk, 'pkg', 'install', 'lib', '--registry', origin],
                                  env=terminal_env, stdin=terminal, stderr=terminal,
                                  stdout=subprocess.PIPE) as process:
                os.close(terminal)
                terminal = None
                prompt = b''
                while b'Proceed? [y/N]' not in prompt:
                    if not select.select([master], [], [], 10)[0]:
                        process.kill()
                        raise AssertionError(prompt)
                    prompt += os.read(master, 4096)
                assert b'test/base@1.0.0' in prompt and b'test/leaf@1.0.0' in prompt, prompt
                assert not any(path.endswith('/download') for path in state['requests'])
                os.write(master, b'y\n')
                progress = b''
                while select.select([master], [], [], 10)[0]:
                    try:
                        chunk = os.read(master, 4096)
                    except OSError as error:
                        if error.errno != errno.EIO:
                            raise
                        break
                    if not chunk:
                        break
                    progress += chunk
                stdout, _ = process.communicate(timeout=10)
                assert process.returncode == 0, (stdout, progress)
                for name in packages:
                    size = len(packages[name])
                    percent = (size // 2) * 100 // size
                    assert f'Downloading test/{name}@1.0.0    0%'.encode() in progress, progress
                    assert f'Downloading test/{name}@1.0.0  {percent:3}%'.encode() in progress, progress
                    assert f'Downloading test/{name}@1.0.0  100%'.encode() in progress, progress
        finally:
            os.close(master)
            if terminal is not None:
                os.close(terminal)
        state['mode'] = 'good'
        app = root/'app.s'; app.write_text('package app\nfrom test.lib import answer\nio.stdout.write_line(answer().to_str)\n')
        assert cli('run', str(app)).strip() == '42'
        state['requests'].clear()
        assert 'Already installed test/lib@1.0.0' in cli('pkg', 'install', 'test/lib', '--registry', origin)
        assert not any(path.endswith('/download') for path in state['requests'])
        cli('pkg', 'remove', 'test/leaf', '--version', '1.0.0')
        state['requests'].clear()
        assert 'Already installed test/lib@1.0.0' in cli('pkg', 'install', 'test/lib', '--registry', origin)
        downloads = [path for path in state['requests'] if path.endswith('/download')]
        assert downloads == ['/api/v1/namespaces/test/packages/leaf/versions/1.0.0/artifacts/source/download']
        archive = root/'home/test/lib/1.0.0/source/test-lib-1.0.0.sputnikpkg'
        archive.rename(root/'retained.sputnikpkg')
        assert 'missing installed package artifact' in cli('run', str(app), ok=False)
        (root/'retained.sputnikpkg').rename(archive)
        # Activation checks extracted source as well as the retained archive.
        member = root/'home/test/lib/1.0.0/source/main.s'; original = member.read_bytes()
        member.write_bytes(original+b'\n# changed\n')
        assert 'modified' in cli('run', str(app), ok=False)
        state['requests'].clear()
        assert 'modified' in cli('pkg', 'install', 'test/lib', '--registry', origin, ok=False)
        assert not any(path.endswith('/download') for path in state['requests'])
        assert 'Installed test/lib@1.0.0' in cli('pkg', 'install', 'lib', '--registry', origin, '--pristine')
        assert member.read_bytes() == original
        def interactive(answer, *flags, action='install'):
            master, terminal = pty.openpty()
            try:
                with subprocess.Popen([sdk, 'pkg', action, 'lib', '--registry', origin, *flags],
                                      env=env, stdin=terminal, stdout=subprocess.PIPE,
                                      stderr=subprocess.PIPE, text=True) as process:
                    os.write(master, (answer + '\n').encode())
                    stdout, stderr = process.communicate(timeout=15)
                    assert process.returncode == 0, (stdout, stderr)
                    return stdout + stderr
            finally:
                os.close(master)
                os.close(terminal)
        state['update'] = True
        state['requests'].clear()
        declined = interactive('n', '--update')
        assert 'Installed: test/lib@1.0.0' in declined and 'Update to test/lib@1.1.0' in declined
        assert 'Proceed? [y/N]' in declined and 'Installation cancelled' in declined
        assert not any(path.endswith('/download') for path in state['requests'])
        assert 'Installed test/lib@1.1.0' in interactive('y', action='update')
        state['requests'].clear()
        assert cli('pkg', 'update', 'lib', '--registry', origin) == 'No update available.\n'
        assert cli('pkg', 'update', 'lib', '--registry', origin, '--version', '1.0.0') == 'No update available.\n'
        assert not any(path.endswith('/download') for path in state['requests'])
        assert archive.exists()
        assert cli('pkg', 'list').splitlines() == [
            'test/base@1.0.0 (source)', 'test/leaf@1.0.0 (source)',
            'test/lib@1.0.0 (source)', 'test/lib@1.1.0 (source)',
        ]
        new_directory = root/'home/test/lib/1.1.0/source'
        stale = new_directory/'stale'
        stale.touch()
        declined = interactive('n', '--reinstall')
        assert 'Reinstall test/lib@1.1.0' in declined and stale.exists()
        assert 'Installed test/lib@1.1.0' in interactive('y', '--reinstall')
        assert not stale.exists() and archive.exists()
        cli('pkg', 'remove', 'test/lib', '--version', '1.1.0')
        state['update'] = False
        for mode in ['foreign-url', 'identity', 'size', 'tamper', 'duplicate', 'redirect', 'partial-status']:
            state['mode'] = mode
            env['SPUTNIK_PKG_HOME'] = str(root/('bad-'+mode))
            cli('pkg', 'install', 'lib', '--registry', origin, ok=False)
            assert not (root/('bad-'+mode)/'test/lib/1.0.0').exists(), mode
        for mode in ['language-major','language-patch','legacy-language']:
            state['mode'] = mode
            state['requests'].clear()
            assert 'NO_COMPATIBLE_ARTIFACT' in cli('pkg', 'install', 'lib', '--version', '1.0.0', '--registry', origin, ok=False)
            assert not any(path.endswith('/download') for path in state['requests']), state['requests']
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
