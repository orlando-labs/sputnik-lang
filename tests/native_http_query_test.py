#!/usr/bin/env python3
"""Full-native net.http QUERY smoke test against a loopback server."""

from __future__ import annotations

import json
import socket
import subprocess
import sys
import threading
from pathlib import Path


def fail(message: str) -> None:
    raise SystemExit(f"native HTTP QUERY test failed: {message}")


def receive_request(connection: socket.socket) -> bytes:
    request = bytearray()
    content_length: int | None = None
    while True:
        chunk = connection.recv(4096)
        if not chunk:
            break
        request.extend(chunk)
        marker = request.find(b"\r\n\r\n")
        if marker < 0:
            continue
        if content_length is None:
            for line in request[:marker].split(b"\r\n")[1:]:
                name, separator, value = line.partition(b":")
                if separator and name.strip().lower() == b"content-length":
                    content_length = int(value.strip())
                    break
            if content_length is None:
                content_length = 0
        if len(request) >= marker + 4 + content_length:
            break
    return bytes(request)


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    amberc = Path(sys.argv[1]) if len(sys.argv) > 1 else root / "build/amberc"
    work = Path(sys.argv[2]) if len(sys.argv) > 2 else root / "build/native-http-query"
    if not amberc.is_absolute():
        amberc = (root / amberc).resolve()
    if not work.is_absolute():
        work = (root / work).resolve()
    work.mkdir(parents=True, exist_ok=True)

    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", 0))
    listener.listen(4)
    listener.settimeout(120)
    port = listener.getsockname()[1]
    received: list[bytes] = []
    server_error: list[BaseException] = []

    def serve() -> None:
        try:
            for index in range(4):
                connection, _ = listener.accept()
                with connection:
                    connection.settimeout(10)
                    received.append(receive_request(connection))
                    body = f"native-query-ok-{index + 1}".encode()
                    status = 207 + index
                    connection.sendall(
                        f"HTTP/1.1 {status} Multi-Status\r\n".encode()
                        + f"Content-Length: {len(body)}\r\n".encode()
                        + b"Content-Type: text/plain\r\nConnection: close\r\n\r\n"
                        + body
                    )
        except BaseException as error:  # surfaced on the main test thread
            server_error.append(error)

    server = threading.Thread(target=serve, daemon=True)
    server.start()

    source = work / "query.am"
    source.write_text(
        "package native.http_query\n\n"
        "import task\n"
        "from net.http import Client, Request, RequestBody, Server, ServerRequest, ServerResponse\n\n"
        "from sync import Mutex\n\n"
        "export main\n\n"
        "def main():\n"
        "  client = Client()\n"
        "  server_types = [Server, ServerRequest, ServerResponse]\n"
        "  types_ok = server_types.count() == 3\n"
        f"  first = client.query(\"http://127.0.0.1:{port}/search?active=1\",\n"
        "    headers: {\"content-type\": \"application/sql\", \"x-native-map\": \"preserved\"},\n"
        "    body: \"SELECT 1\")\n"
        "  first_ok = first.status() == 207 and first.body_text() == \"native-query-ok-1\"\n"
        f"  second = client.query(\"http://127.0.0.1:{port}/search?active=1\",\n"
        "    headers: {\"content-type\": \"application/sql\"},\n"
        "    body: \"SELECT 2\") |response|:\n"
        "    if response.status() == 208:\n"
        "      response.body_text()\n"
        "    else:\n"
        "      \"unexpected-scoped-status\"\n"
        "  payload = RequestBody.stream(length: 18) |writer|:\n"
        "    writer.write_all!(\"native-stream-body\".bytes)\n"
        f"  third = client.post(\"http://127.0.0.1:{port}/upload\", body: payload)\n"
        "  third_ok = third.status() == 209 and third.body_text() == \"native-query-ok-3\"\n"
        f"  request = Request(method: :get, url: \"http://127.0.0.1:{port}/request\", headers: {{\"x-native-request\": \"preserved\"}})\n"
        "  fourth = client.send(request)\n"
        "  fourth_ok = fourth.status() == 210 and fourth.body_text() == \"native-query-ok-4\"\n"
        "  internal_server = Server(host: \"127.0.0.1\", port: 0, workers: 1)\n"
        "  internal_endpoint = internal_server.local_endpoint()\n"
        "  endpoint_ok = internal_endpoint.host() == \"127.0.0.1\" and internal_endpoint.family() == :inet\n"
        "  internal_port = internal_server.port()\n"
        "  runner = task.spawn:\n"
        "    internal_server.serve(max_requests: 1) |request|:\n"
        "      request_ok = request.method() == \"GET\" and request.target() == \"/stream\" and request.path() == \"/stream\" and request.query() == null and request.headers().first(\"host\") != null and request.body_stream() != null and request.local_endpoint().port() == internal_port and request.remote_endpoint().port() > 0\n"
        "      ServerResponse.stream(\n"
        "        headers: {\"x-native-one\": \"one\", \"x-native-two\": if request_ok then \"two\" else \"bad-request\"},\n"
        "        trailers: [\"x-native-done\"]) |writer|:\n"
        "          writer.write(\"native-stream-response\")\n"
        "          writer.close()\n"
        "          writer.trailer(\"x-native-done\", \"complete\")\n"
        "          writer.finish()\n"
        "  internal = client.get(\"http://127.0.0.1:#{internal_port}/stream\")\n"
        "  internal_body = internal.body_text()\n"
        "  internal_trailer = internal.body().trailers().first(\"x-native-done\")\n"
        "  runner.wait()\n"
        "  internal_ok = internal.headers().first(\"transfer-encoding\") == \"chunked\" and internal.headers().first(\"x-native-one\") == \"one\" and internal.headers().first(\"x-native-two\") == \"two\" and internal_body == \"native-stream-response\" and internal_trailer == \"complete\"\n"
        "  concurrent_server = Server(host: \"127.0.0.1\", port: 0, workers: 4)\n"
        "  concurrent_port = concurrent_server.port()\n"
        "  response_gate = Mutex.new()\n"
        "  concurrent_runner = task.spawn:\n"
        "    concurrent_server.serve(max_requests: 4) |request|:\n"
        "      response_gate.synchronize:\n"
        "        task.sleep(20)\n"
        "        ServerResponse(body: \"coordinator-ok\")\n"
        "  concurrent_one = task.spawn:\n"
        "    Client().get(\"http://127.0.0.1:#{concurrent_port}/one\").body_text()\n"
        "  concurrent_two = task.spawn:\n"
        "    Client().get(\"http://127.0.0.1:#{concurrent_port}/two\").body_text()\n"
        "  concurrent_three = task.spawn:\n"
        "    Client().get(\"http://127.0.0.1:#{concurrent_port}/three\").body_text()\n"
        "  concurrent_four = task.spawn:\n"
        "    Client().get(\"http://127.0.0.1:#{concurrent_port}/four\").body_text()\n"
        "  concurrent_ok = concurrent_one.wait() == \"coordinator-ok\" and concurrent_two.wait() == \"coordinator-ok\" and concurrent_three.wait() == \"coordinator-ok\" and concurrent_four.wait() == \"coordinator-ok\"\n"
        "  concurrent_runner.wait()\n"
        "  if types_ok and first_ok and second == \"native-query-ok-2\" and third_ok and fourth_ok and endpoint_ok and internal_ok and concurrent_ok:\n"
        "    \"native-query-ok\"\n"
        "  else:\n"
        "    \"unexpected-query-result\"\n"
    )
    executable = work / "query"
    build = subprocess.run(
        [
            str(amberc),
            "build",
            str(source),
            "--target",
            "native",
            "--entry",
            "main-only",
            "--require-native-body-coverage",
            "--grant",
            "net.connect",
            "--grant",
            "net.listen",
            "-o",
            str(executable),
            "--out-dir",
            str(work),
        ],
        cwd=root,
        capture_output=True,
        text=True,
        timeout=240,
    )
    if build.returncode != 0:
        listener.close()
        fail(build.stderr or build.stdout)
    result = json.loads(build.stdout)
    bytecode_fallback = result.get(
        "native_bytecode_fallback", result.get("bytecode_fallback")
    )
    if bytecode_fallback is not False:
        listener.close()
        fail(f"build retained bytecode fallback: {result}")
    if result.get("native_body_coverage_full") is not True:
        listener.close()
        fail(f"build did not report complete native body coverage: {result}")
    if result.get("native_full_coverage") is not False:
        listener.close()
        fail(f"HTTP bridge was mislabeled VM-independent native: {result}")
    if result.get("native_runtime_bridge_kind") != "vm-stdlib-send-v1":
        listener.close()
        fail(f"HTTP bridge kind was not reported honestly: {result}")

    run = subprocess.run(
        [str(executable)], capture_output=True, text=True, timeout=15, cwd=root
    )
    server.join(timeout=15)
    listener.close()
    if server.is_alive():
        fail("loopback server did not finish")
    if server_error:
        fail(f"loopback server error: {server_error[0]}")
    if run.returncode != 0:
        fail(f"native executable exited {run.returncode}: {run.stderr}")
    if run.stdout != '"native-query-ok"\n':
        fail(f"unexpected stdout: {run.stdout!r}")
    if len(received) != 4:
        fail("server did not receive exactly four requests")

    for index, request in enumerate(received):
        head, separator, body = request.partition(b"\r\n\r\n")
        if not separator:
            fail("request head was incomplete")
        lines = head.split(b"\r\n")
        expected_line = (
            b"QUERY /search?active=1 HTTP/1.1"
            if index < 2
            else (
                b"POST /upload HTTP/1.1"
                if index == 2
                else b"GET /request HTTP/1.1"
            )
        )
        if lines[0] != expected_line:
            fail(f"wrong request line: {lines[0]!r}")
        headers = {}
        for line in lines[1:]:
            name, colon, value = line.partition(b":")
            if colon:
                headers[name.strip().lower()] = value.strip().lower()
        if index < 2 and headers.get(b"content-type") != b"application/sql":
            fail(f"wrong Content-Type: {headers.get(b'content-type')!r}")
        if index == 0 and headers.get(b"x-native-map") != b"preserved":
            fail(f"native Map bridge dropped a string key: {headers!r}")
        if index == 3 and headers.get(b"x-native-request") != b"preserved":
            fail(f"native Request bridge dropped a string key: {headers!r}")
        expected_body = (
            f"SELECT {index + 1}".encode()
            if index < 2
            else (b"native-stream-body" if index == 2 else b"")
        )
        if body != expected_body:
            fail(f"wrong request body: {body!r}")

    print("native HTTP QUERY test: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
