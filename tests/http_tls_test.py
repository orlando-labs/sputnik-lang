#!/usr/bin/env python3
"""Offline HTTPS conformance: the same client/server API in VM and full native."""
from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import socket
import ssl
import subprocess
import sys
import threading
import time


def run(command, *, cwd, env=None, timeout=240):
    result = subprocess.run(command, cwd=cwd, env=env, capture_output=True, text=True, timeout=timeout)
    if result.returncode:
        raise AssertionError(f"{command}:\n{result.stdout}\n{result.stderr}")
    return result


def certificates(work):
    openssl = shutil.which("openssl")
    if openssl is None:
        raise AssertionError("openssl CLI is required for local test certificates")
    def cmd(*args):
        return run([openssl, *args], cwd=work)
    (work / "ca-req.cnf").write_text("""[req]
distinguished_name=dn
x509_extensions=ca_extensions
[dn]
[ca_extensions]
basicConstraints=critical,CA:true
keyUsage=critical,keyCertSign,cRLSign
subjectKeyIdentifier=hash
authorityKeyIdentifier=keyid:always
""")
    cmd("req", "-config", "ca-req.cnf", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "2", "-subj", "/CN=Amber test CA",
        "-keyout", "ca.key", "-out", "ca.pem")
    for name, san in [("good", "DNS:localhost,IP:127.0.0.1"), ("wrong", "DNS:wrong.invalid"), ("client", "DNS:client")]:
        cmd("req", "-new", "-newkey", "rsa:2048", "-nodes", "-subj", f"/CN={name}",
            "-keyout", f"{name}.key", "-out", f"{name}.csr")
        (work / f"{name}.ext").write_text(f"basicConstraints=critical,CA:false\nkeyUsage=critical,digitalSignature,keyEncipherment\nsubjectKeyIdentifier=hash\nauthorityKeyIdentifier=keyid:always\nsubjectAltName={san}\nextendedKeyUsage={'clientAuth' if name == 'client' else 'serverAuth'}\n")
        cmd("x509", "-req", "-in", f"{name}.csr", "-CA", "ca.pem", "-CAkey", "ca.key", "-CAcreateserial",
            "-days", "2", "-extfile", f"{name}.ext", "-out", f"{name}.pem")
    (work / "index").write_text("")
    (work / "serial").write_text("1000\n")
    (work / "ca.cnf").write_text("""[ca]
default_ca=local
[local]
database=index
serial=serial
new_certs_dir=.
certificate=ca.pem
private_key=ca.key
default_md=sha256
policy=policy
x509_extensions=extensions
[policy]
commonName=supplied
[extensions]
basicConstraints=critical,CA:false
keyUsage=critical,digitalSignature,keyEncipherment
subjectKeyIdentifier=hash
authorityKeyIdentifier=keyid:always
subjectAltName=DNS:localhost,IP:127.0.0.1
extendedKeyUsage=serverAuth
""")
    cmd("ca", "-batch", "-config", "ca.cnf", "-in", "good.csr", "-out", "expired.pem",
        "-startdate", "200101000000Z", "-enddate", "200102000000Z")


class Server:
    def __init__(self, context=None, *, redirect=None, stall=False):
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(32)
        self.listener.settimeout(0.2)
        self.port = self.listener.getsockname()[1]
        self.context = context
        self.redirect = redirect
        self.stall = stall
        self.requests = []
        self.request_sessions = []
        self.connections = 0
        self.sni = []
        self.alpn = []
        self.errors = []
        self.stopped = threading.Event()
        if context:
            context.set_servername_callback(lambda sock, name, ctx: self.sni.append(name))
        self.thread = threading.Thread(target=self.serve, daemon=True)
        self.thread.start()

    def serve(self):
        while not self.stopped.is_set():
            try:
                connection, _ = self.listener.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            self.connections += 1
            threading.Thread(target=self.handle, args=(connection,), daemon=True).start()

    def handle(self, connection):
        try:
            connection.settimeout(5)
            if self.stall:
                time.sleep(0.3)
                return
            if self.context:
                try:
                    connection = self.context.wrap_socket(connection, server_side=True)
                    self.alpn.append(connection.selected_alpn_protocol())
                except ssl.SSLError:
                    return  # Expected certificate/mTLS failures.
            pending = b""
            while not self.stopped.is_set():
                while b"\r\n\r\n" not in pending:
                    data = connection.recv(65536)
                    if not data:
                        return
                    pending += data
                head, pending = pending.split(b"\r\n\r\n", 1)
                lines = head.split(b"\r\n")
                method, path, _ = lines[0].split(b" ", 2)
                headers = {}
                for line in lines[1:]:
                    key, _, value = line.partition(b":")
                    headers[key.lower()] = value.strip()
                length = int(headers.get(b"content-length", b"0"))
                while len(pending) < length:
                    data = connection.recv(65536)
                    if not data:
                        raise AssertionError("incomplete request body")
                    pending += data
                body, pending = pending[:length], pending[length:]
                self.requests.append((method, path, headers, body))
                self.request_sessions.append((path, id(connection)))
                if self.redirect:
                    connection.sendall(f"HTTP/1.1 302 Found\r\nLocation: {self.redirect}\r\nContent-Length: 0\r\n\r\n".encode())
                elif path == b"/chunked":
                    connection.sendall(b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nTrailer: x-done\r\n\r\n3\r\nabc\r\n")
                    connection.sendall(b"3\r\ndef\r\n0\r\nx-done: yes\r\n\r\n")
                elif path == b"/json":
                    connection.sendall(b'HTTP/1.1 200 OK\r\nContent-Length: 15\r\n\r\n{"secure":true}')
                elif path == b"/truncated":
                    connection.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nshort")
                    return  # No close_notify: must not look like clean EOF.
                elif path == b"/eof":
                    connection.sendall(b"HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nclean-eof")
                    connection = connection.unwrap()
                    return
                else:
                    payload = body if method == b"POST" else b"tls-ok"
                    connection.sendall(b"HTTP/1.1 200 OK\r\nX-TLS: yes\r\nContent-Length: " + str(len(payload)).encode() + b"\r\n\r\n" + payload)
        except (ConnectionError, ssl.SSLError):
            pass  # Early close/discard tests intentionally abandon connections.
        except BaseException as error:
            self.errors.append(error)
        finally:
            connection.close()

    def close(self):
        self.stopped.set()
        self.listener.close()
        self.thread.join(2)


def context(work, name="good", version=None, mtls=False):
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(work / f"{name}.pem", work / ("good.key" if name == "expired" else f"{name}.key"))
    if version:
        ctx.minimum_version = ctx.maximum_version = version
    ctx.set_alpn_protocols(["http/1.1"])
    if mtls:
        ctx.load_verify_locations(work / "ca.pem")
        ctx.verify_mode = ssl.CERT_REQUIRED
    return ctx


def build(amberc, root, work, source, env):
    executable = work / source.stem
    result = run([str(amberc), "build", str(source), "--target", "native", "--entry", "main-only",
                  "--require-full-native", "--grant", "net.connect", "--grant", "net.listen",
                  "-o", str(executable), "--out-dir", str(work)], cwd=root, env=env, timeout=600)
    metadata = json.loads(result.stdout)
    assert metadata["native_full_coverage"] is True, metadata
    assert metadata["native_runtime_bridge"] is False, metadata
    assert not metadata["native_runtime_bridge_kind"], metadata
    assert metadata.get("native_bytecode_fallback", metadata.get("bytecode_fallback")) is False, metadata
    generated = source.with_suffix(".native.cpp")
    if generated.exists():
        assert "amber_native_bridge_world()" not in generated.read_text()
    return executable


def main():
    root = Path(__file__).resolve().parents[1]
    amberc = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else root / "build/amberc"
    work = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else root / "build/http-tls"
    work.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env.setdefault("AMBER_NATIVE_RT_CACHE", str(root / "build/native-rt-cache"))
    certificates(work)
    servers = []
    try:
        good = Server(context(work)); servers.append(good)
        wrong = Server(context(work, "wrong")); servers.append(wrong)
        expired = Server(context(work, "expired")); servers.append(expired)
        tls12 = Server(context(work, version=ssl.TLSVersion.TLSv1_2)); servers.append(tls12)
        tls13 = tls12
        if ssl.HAS_TLSv1_3:
            tls13 = Server(context(work, version=ssl.TLSVersion.TLSv1_3)); servers.append(tls13)
        else:
            print("TLS 1.3 fixture skipped: this Python SSL backend lacks TLS 1.3", flush=True)
        mtls = Server(context(work, mtls=True)); servers.append(mtls)
        stall = Server(stall=True); servers.append(stall)
        redirect = Server(redirect=f"https://localhost:{good.port}/redirected"); servers.append(redirect)
        ca = json.dumps(str(work / "ca.pem"))
        good_url = f"https://localhost:{good.port}"
        source = work / "client.am"
        source.write_text(f'''package test.http_tls_client
from net.http import Client, Server
import net
export main

def main():
  defaults = net.http.Client()
  namespace_ok = not defaults.closed?()
  defaults.close!()
  client = Client(timeout: 2, tls_ca_file: {ca})
  first = client.get("{good_url}/one")
  ok = namespace_ok and first.status() == 200 and first.headers().first("x-tls") == "yes" and first.body_text() == "tls-ok"
  ok = ok and client.post("{good_url}/post", body: "encrypted-body").body_text() == "encrypted-body"
  chunked = client.get("{good_url}/chunked")
  ok = ok and chunked.body_text() == "abcdef" and chunked.body().trailers().first("x-done") == "yes"
  streamed = client.get("{good_url}/chunked").body()
  streamed_payload = ""
  streamed.each_chunk(size: 2) |chunk|:
    streamed_payload += chunk.to_str()
  ok = ok and streamed_payload == "abcdef"
  readable = client.get("{good_url}/chunked").body()
  ok = ok and readable.read(max_bytes: 2).to_str() == "ab"
  readable.close!()
  scoped = client.get("{good_url}/one") |result|:
    result.body_text()
  ok = ok and scoped == "tls-ok"
  json_response = client.get("{good_url}/json")
  ok = ok and json_response.json()["secure"] == true
  ok = ok and client.get("https://127.0.0.1:{good.port}/ip").body_text() == "tls-ok"
  ok = ok and client.get("https://localhost:{tls12.port}/tls12").body_text() == "tls-ok"
  ok = ok and client.get("https://localhost:{tls13.port}/tls13").body_text() == "tls-ok"
  try:
    client.get("https://localhost:{wrong.port}/wrong", headers: {{"host": "localhost"}})
    ok = false
  rescue TlsCertificateError:
    null
  try:
    client.get("https://localhost:{expired.port}/expired")
    ok = false
  rescue TlsCertificateError:
    null
  try:
    Client(timeout: 2).get("{good_url}/untrusted")
    ok = false
  rescue TlsCertificateError:
    null
  try:
    Client(timeout: 2, tls_ca_file: "/amber/no-such-ca.pem").get("{good_url}/invalid-ca")
    ok = false
  rescue TlsError:
    null
  try:
    client.get("https://localhost:{stall.port}/stall", timeout: 0.05)
    ok = false
  rescue ConnectionError:
    null
  try:
    client.get("{good_url}/truncated").body_text()
    ok = false
  rescue TlsError:
    null
  mutual = Client(timeout: 2, tls_ca_file: {ca}, tls_cert_file: {json.dumps(str(work / "client.pem"))}, tls_key_file: {json.dumps(str(work / "client.key"))})
  ok = ok and mutual.get("https://localhost:{mtls.port}/mutual").body_text() == "tls-ok"
  redirected = Client(timeout: 2, tls_ca_file: {ca}, redirects: :safe).get("http://127.0.0.1:{redirect.port}/start", headers: {{"authorization": "secret", "cookie": "secret", "host": "caller.invalid"}})
  ok = ok and redirected.body_text() == "tls-ok" and redirected.redirects().count == 1
  ok = ok and client.get("{good_url}/eof").body_text() == "clean-eof"
  try:
    Server(tls_cert_file: {json.dumps(str(work / "good.pem"))}, tls_key_file: {json.dumps(str(work / "wrong.key"))})
    ok = false
  rescue TlsError:
    null
  client.close!()
  mutual.close!()
  if ok then "tls-client-ok" else "tls-client-failed"
''')
        # VM and full native execute precisely the same Amber client source.
        vm = run([str(amberc), "run", str(source), "--grant", "net.connect", "--grant", "net.listen"], cwd=root, env=env)
        assert '"tls-client-ok"' in vm.stdout, vm.stdout
        print("HTTPS client VM: ok", flush=True)
        executable = build(amberc, root, work, source, env)
        native = run([str(executable)], cwd=root, env=env, timeout=30)
        assert native.stdout == '"tls-client-ok"\n', native.stdout
        print("HTTPS client full native: ok", flush=True)
        assert good.sni.count("localhost") > 0 and None in good.sni, good.sni
        assert good.alpn and all(protocol == "http/1.1" for protocol in good.alpn), good.alpn
        # First three requests must use one TLS session in each execution.
        pooled = [(path, session) for path, session in good.request_sessions if path in (b"/one", b"/post", b"/chunked")]
        starts = [index for index, (path, _) in enumerate(pooled) if path == b"/post"]
        assert len(starts) == 2, pooled
        for index in starts:
            assert len({session for _, session in pooled[index - 1:index + 2]}) == 1, pooled
        assert sum(path == b"/post" and body == b"encrypted-body" for _, path, _, body in good.requests) == 2
        for _, path, headers, _ in good.requests:
            if path == b"/redirected":
                assert b"authorization" not in headers and b"cookie" not in headers
                assert headers[b"host"] == f"localhost:{good.port}".encode()
        server_source = work / "server.am"
        probe = socket.socket(); probe.bind(("127.0.0.1", 0)); port = probe.getsockname()[1]; probe.close()
        server_source.write_text(f'''package test.http_tls_server
from net.http import Server, ServerResponse
export main

def main():
  server = Server(port: {port}, tls_cert_file: {json.dumps(str(work / "good.pem"))}, tls_key_file: {json.dumps(str(work / "good.key"))}, read_timeout: 0.7)
  server.serve(max_requests: 2) |request|:
    ServerResponse(status: 200, body: request.body_text())
  "tls-server-ok"
''')
        server_executable = build(amberc, root, work, server_source, env)
        client_context = ssl.create_default_context(cafile=str(work / "ca.pem"))
        client_context.set_alpn_protocols(["http/1.1"])
        for mode, command in [("VM", [str(amberc), "run", str(server_source), "--grant", "net.listen"]),
                              ("full native", [str(server_executable)])]:
            process = subprocess.Popen(command, cwd=root, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            try:
                deadline = time.monotonic() + 10
                while True:
                    try:
                        connection = socket.create_connection(("127.0.0.1", port), timeout=2)
                        break
                    except ConnectionRefusedError:
                        if process.poll() is not None or time.monotonic() > deadline:
                            raise AssertionError(process.communicate(timeout=5))
                        time.sleep(0.02)
                # A stalled TLS handshake must park its strand and keep the
                # original deadline while the sole server worker serves a peer.
                stalled = connection
                time.sleep(0.03)
                started = time.monotonic()
                connection = socket.create_connection(("127.0.0.1", port), timeout=2)
                with client_context.wrap_socket(connection, server_hostname="localhost") as secured:
                    assert secured.selected_alpn_protocol() == "http/1.1"
                    secured.sendall(b"POST /encrypted HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\nContent-Length: 12\r\n\r\nserver-echo!")
                    response = b""
                    while True:
                        data = secured.recv(65536)
                        if not data: break
                        response += data
                assert response.startswith(b"HTTP/1.1 200") and response.endswith(b"server-echo!"), response
                assert time.monotonic() - started < 0.5, "TLS handshake blocked the sole server worker"
                stdout, stderr = process.communicate(timeout=10)
                stalled.close()
                assert process.returncode == 0 and '"tls-server-ok"' in stdout, (stdout, stderr)
                print(f"HTTPS server {mode}: ok", flush=True)
            finally:
                if process.poll() is None:
                    process.kill(); process.communicate()
        assert not any(server.errors for server in servers), [s.errors for s in servers]
        print("HTTP TLS conformance: ok")
    finally:
        for server in servers: server.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
