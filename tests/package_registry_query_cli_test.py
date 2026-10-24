#!/usr/bin/env python3
"""Real discovery CLI against a paginated, authenticated protocol fixture."""
import hashlib
import http.server
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
from urllib.parse import parse_qs, urlsplit

sdk = str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix='sputnik-registry-query-') as temporary:
    root = Path(temporary)
    env = dict(os.environ, HOME=str(root), SPUTNIK_PKG_HOME=str(root/'packages'),
               SPUTNIK_PKG_REGISTRY='', SPUTNIK_NATIVE_CXX='/no/compiler')
    env.pop('SPUTNIK_PKG_CREDENTIALS', None)
    state = dict(mode='good', requests=[], starts=0, auth=False)
    canonical = 'test/example'
    base = '/api/v1/namespaces/test/packages/example'
    cursor = 'page+/=2'
    revision = 9007199254740993123
    artifacts = [
        dict(variant='linux-cu130', quarantined=False, bundle='runtime',
             target=dict(os='linux', arch='x86_64', compute_backend='cuda', cuda_version='13.0'),
             size=2147483648, sha256='sha256:'+'1'*64,
             compatibility=dict(native_requirements=[dict(id='libtorch', version='2.14.0')],
                                note='пакет\n"quoted"\t\\')),
        dict(variant='quarantined', quarantined=True),
        dict(variant='cpu', quarantined=False),
    ]

    def release(version, yanked=False):
        return dict(schema='sputnik.registry.release.v1', package_ref=canonical,
                    version=version, yanked=yanked, artifacts=artifacts)

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_): pass

        def do_GET(self):
            state['requests'].append((self.path, self.headers.get('authorization')))
            url, mode = urlsplit(self.path), state['mode']
            route = url.path.replace('/api/v1/packages/example', base, 1)
            if state['auth'] and self.headers.get('authorization') != 'Bearer spreg_fixture':
                status, data = 401, dict(error=dict(code='UNAUTHORIZED'))
            elif mode == 'not_found':
                status, data = 404, dict(error=dict(code='NOT_FOUND'))
            elif route == base:
                status, data = 200, dict(package_ref=canonical)
            elif route == base+'/index':
                following = parse_qs(url.query).get('cursor')
                if not following: state['starts'] += 1
                if following and mode in ['changed_once', 'always_changed'] and (
                        mode == 'always_changed' or state['starts'] == 1):
                    status, data = 409, dict(error=dict(code='INDEX_CHANGED'))
                else:
                    assert not following or following == [cursor], following
                    status = 200
                    entries = ([release('1.9.0'), release('1.10.0')] if following else
                               [release('2.0.0-rc.10'), release('3.0.0', yanked=True)])
                    if mode == 'changed_once' and state['starts'] == 1: entries = [release('99.0.0')]
                    if following and mode == 'duplicate': entries = [release('3.0.0')]
                    if mode == 'bad_release': entries[0]['package_ref'] = 'other/example'
                    if mode == 'bad_boolean': entries[0]['yanked'] = 'false'
                    data = dict(schema='sputnik.registry.index.v1', package_ref=canonical,
                                revision=revision+(state['starts'] if mode == 'changed_once' else 0),
                                versions=entries, next_cursor=cursor if not following or mode == 'loop' else None)
                    if following and mode == 'bad_revision': data['revision'] += 1
                    if following and mode == 'alias_changed': data['package_ref'] = 'other/example'
                    if mode == 'bad_cursor': data['next_cursor'] = 'https://other.invalid'
                    if mode == 'empty': data.update(versions=[], next_cursor=None)
            elif route == base+'/versions/1.10.0':
                status, data = 200, release('1.10.0', yanked=mode == 'yanked')
                if mode == 'bad_release': data['version'] = '9.0.0'
                if mode == 'bad_variant': data['artifacts'] = [dict(variant='unsafe\nname', quarantined=False)]
                if mode == 'duplicate_variant': data['artifacts'] = [artifacts[0], artifacts[0]]
                if mode == 'empty': data['artifacts'] = []
            else:
                status, data = 404, dict(error=dict(code='NOT_FOUND'))
            if status == 200 and mode == 'bad_alias': data['package_ref'] = 'other/example'
            body = json.dumps(data, ensure_ascii=True).encode()
            self.send_response(status)
            self.send_header('content-length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    origin = f'http://127.0.0.1:{server.server_port}'
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()

    def cli(*args, ok=True, explicit_registry=True):
        command = [sdk, 'pkg', *args]
        if explicit_registry: command += ['--registry', origin]
        result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=15)
        assert (result.returncode == 0) == ok, (command, result.stdout, result.stderr)
        if not ok: assert result.stdout == '', result.stdout
        return result

    try:
        expected = ['3.0.0 (yanked)', '2.0.0-rc.10', '1.10.0', '1.9.0']
        assert cli('versions', 'example').stdout.splitlines() == expected
        assert len(state['requests']) == 2  # Index pages, with no name-resolution GET.
        assert any('cursor=page%2B%2F%3D2' in path for path, _ in state['requests'])
        indexed = json.loads(cli('versions', canonical, '--json').stdout)
        assert indexed['package_ref'] == canonical and indexed['revision'] == revision
        assert len(indexed['versions']) == 4 and indexed['next_cursor'] is None
        assert indexed['versions'][0]['yanked'] is True
        assert all('/download' not in path for path, _ in state['requests'])
        assert not (root/'packages').exists()
        before = len(state['requests'])
        assert cli('variants', 'example@1.10.0').stdout == 'cpu\nlinux-cu130\n'
        assert len(state['requests']) == before+1
        assert state['requests'][-1][0] == '/api/v1/packages/example/versions/1.10.0'
        details = json.loads(cli('variants', canonical+'@1.10.0', '--json').stdout)
        assert details['package_ref'] == canonical and details['version'] == '1.10.0'
        assert details['artifacts'] == [artifacts[2], artifacts[0]]
        state.update(mode='yanked')
        assert 'is yanked' in cli('variants', 'example@1.10.0').stderr
        state.update(mode='empty')
        assert cli('versions', 'example').stdout == ''
        assert cli('variants', 'example@1.10.0').stdout == ''
        assert json.loads(cli('variants', 'example@1.10.0', '--json').stdout)['artifacts'] == []
        state.update(mode='changed_once', starts=0)
        assert cli('versions', 'example').stdout.splitlines() == expected
        assert state['starts'] == 2
        for mode, fragment in [('always_changed', 'kept changing'), ('loop', 'pagination cursor'),
                               ('duplicate', 'duplicate release'), ('bad_revision', 'revision changed'),
                               ('bad_release', 'release identity'), ('bad_boolean', 'boolean'),
                               ('bad_cursor', 'pagination cursor'), ('not_found', 'HTTP 404 NOT_FOUND')]:
            state.update(mode=mode, starts=0)
            assert fragment in cli('versions', canonical, ok=False).stderr
            if mode == 'always_changed': assert state['starts'] == 3
        state.update(mode='bad_alias')
        assert 'alias identity' in cli('versions', canonical, ok=False).stderr
        state.update(mode='alias_changed')
        assert 'package identity changed' in cli('versions', 'example', ok=False).stderr
        for mode in ['bad_release', 'bad_variant', 'duplicate_variant']:
            state.update(mode=mode)
            cli('variants', 'example@1.10.0', ok=False)
        state.update(mode='good', auth=True)
        assert 'use pkg login' in cli('versions', 'example', ok=False).stderr
        credentials = root/'credentials.json'
        credentials.write_text(json.dumps(dict(origin=origin, token='spreg_fixture')))
        credentials.chmod(0o600)
        cli('versions', 'example', '--credentials', str(credentials))
        env['SPUTNIK_PKG_CREDENTIALS'] = str(credentials)
        cli('variants', 'example@1.10.0')
        env.pop('SPUTNIK_PKG_CREDENTIALS')
        saved = root/'.sputnik/credentials'/(hashlib.sha256(origin.encode()).hexdigest()+'.json')
        saved.parent.mkdir(parents=True)
        saved.write_bytes(credentials.read_bytes()); saved.chmod(0o600)
        cli('versions', 'example')
        env['SPUTNIK_PKG_REGISTRY'] = origin
        cli('variants', 'example@1.10.0', explicit_registry=False)
        env['SPUTNIK_PKG_REGISTRY'] = 'https://wrong.invalid'
        cli('variants', 'example@1.10.0')
        env['SPUTNIK_PKG_REGISTRY'] = ''
        count = len(state['requests'])
        credentials.write_text(json.dumps(dict(origin='https://wrong.invalid', token='spreg_fixture')))
        assert 'origin differs' in cli('versions', 'example', '--credentials', str(credentials), ok=False).stderr
        assert len(state['requests']) == count
        for args in [('versions',), ('variants',), ('versions', 'example@1.0.0'),
                     ('variants', 'example'), ('variants', 'example@latest'),
                     ('variants', 'example@1.0.0@2.0.0'), ('variants', 'example@'),
                     ('variants', 'example', '--version', '1.0.0'),
                     ('versions', 'UPPER'), ('versions', 'a/b/c'),
                     ('versions', 'example', '--json', '--json'),
                     ('versions', 'example', '--registry', '--json'),
                     ('versions', 'example', '--check-host', '--json'),
                     ('variants', 'example@1.10.0', '--check-host'),
                     ('versions', 'example', '--unknown'), ('versions', 'example', 'extra')]:
            cli(*args, ok=False)
        assert len(state['requests']) == count
    finally:
        server.shutdown(); server.server_close(); thread.join()
print('package_registry_query_cli_test ok')
