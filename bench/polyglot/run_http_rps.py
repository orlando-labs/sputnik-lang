#!/usr/bin/env python3
"""Run the Ember soak HTTP contract against raw or framework HTTP servers."""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import platform
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence


ROOT = Path(__file__).resolve().parents[2]
WORKSPACE = ROOT.parent
EMBER = WORKSPACE / "ember"
BUILD = ROOT / "bench" / "polyglot" / "build" / "http-rps"
RESULTS = ROOT / "bench" / "polyglot" / "results"
AMBER_SERVER_NAMES = {
    "raw": "amber.bench.polyglot.raw_http_rps_server",
    "ember": "amber.bench.polyglot.http_rps_server",
}
AMBER_CLIENT_NAME = "ember.example.soak.client"
HTTP = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def command(
    args: Sequence[str],
    cwd: Path = ROOT,
    env: Optional[Dict[str, str]] = None,
) -> None:
    print("+", " ".join(args), flush=True)
    subprocess.run(list(args), cwd=cwd, env=env, check=True)


def captured(args: Sequence[str], cwd: Path = ROOT) -> str:
    result = subprocess.run(
        list(args),
        cwd=cwd,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=True,
    )
    return result.stdout.strip().splitlines()[0]


def build_all(
    compiler: Path,
    stack: str,
    languages: Sequence[str],
) -> Dict[str, Path]:
    amber_out = BUILD / f"amber-{stack}-server"
    client_out = BUILD / "amber-client"
    go_out = BUILD / "go-server"
    rust_out = BUILD / "rust-server"
    for directory in (amber_out, client_out, go_out, rust_out):
        directory.mkdir(parents=True, exist_ok=True)

    if "amber" in languages:
        command(
            [
                str(compiler),
                "build",
                f"bench/polyglot/amber/{stack}_http_rps_server.build.yaml"
                if stack == "raw"
                else "bench/polyglot/amber/http_rps_server.build.yaml",
                "--target",
                "native",
                "--out-dir",
                str(amber_out),
                "--cache-dir",
                str(amber_out / "cache"),
                "--require-full-native",
                "--grant",
                "net.listen",
                "--grant",
                "random.secure",
            ]
        )
    command(
        [
            str(compiler),
            "build",
            "examples/soak/client.build.yaml",
            "--target",
            "native",
            "--out-dir",
            str(client_out),
            "--cache-dir",
            str(client_out / "cache"),
            "--require-native-body-coverage",
            "--grant",
            "net.connect",
        ],
        cwd=EMBER,
    )
    if "go" in languages:
        go_env = os.environ.copy()
        go_env["GOCACHE"] = str(BUILD / "go-cache")
        command(
            [
                "go",
                "build",
                "-o",
                str(go_out / "http-rps-server"),
                "bench/polyglot/go/http_rps_server.go",
            ],
            env=go_env,
        )
    if "rust" in languages:
        command(
            [
                "rustc",
                "--edition=2021",
                "-O",
                "-o",
                str(rust_out / "http-rps-server"),
                "bench/polyglot/rust/http_rps_server.rs",
            ]
        )
    return {
        "amber": amber_out / AMBER_SERVER_NAMES[stack],
        "client": client_out / AMBER_CLIENT_NAME,
        "go": go_out / "http-rps-server",
        "rust": rust_out / "http-rps-server",
        "python": ROOT / "bench/polyglot/python/http_rps_server.py",
        "rails": ROOT / "bench/polyglot/rails/config.ru",
    }


def built_paths(stack: str) -> Dict[str, Path]:
    return {
        "amber": BUILD / f"amber-{stack}-server" / AMBER_SERVER_NAMES[stack],
        "client": BUILD / "amber-client" / AMBER_CLIENT_NAME,
        "go": BUILD / "go-server" / "http-rps-server",
        "rust": BUILD / "rust-server" / "http-rps-server",
        "python": ROOT / "bench/polyglot/python/http_rps_server.py",
        "rails": ROOT / "bench/polyglot/rails/config.ru",
    }


def rails_ruby() -> Path:
    override = os.environ.get("AMBER_BENCH_RUBY")
    candidates = [Path(override).expanduser()] if override else []
    candidates += sorted(
        (Path.home() / ".rvm" / "rubies").glob("ruby-*/bin/ruby"),
        reverse=True,
    )
    system = shutil.which("ruby")
    if system:
        candidates.append(Path(system))
    for candidate in candidates:
        if not candidate.is_file():
            continue
        probe = subprocess.run(
            [
                str(candidate),
                "-e",
                'require "rails"; require "puma"; abort unless Rails.version.start_with?("8.")',
            ],
            text=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        if probe.returncode == 0:
            return candidate
    raise RuntimeError(
        "Rails 8 and Puma are required for the Ember framework lane; "
        "set AMBER_BENCH_RUBY to a suitable Ruby executable"
    )


def ensure_paths(paths: Dict[str, Path], languages: Sequence[str]) -> None:
    required = ["client", *languages]
    missing = [str(paths[name]) for name in required if not paths[name].is_file()]
    if missing:
        raise RuntimeError("missing benchmark artifacts: " + ", ".join(missing))


def fetch_status(url: str) -> int:
    request = urllib.request.Request(url, method="GET")
    try:
        with HTTP.open(request, timeout=1.0) as response:
            response.read()
            return response.status
    except urllib.error.HTTPError as error:
        error.read()
        return error.code


def wait_ready(process: subprocess.Popen[Any], base_url: str, timeout: float = 15.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"server exited during startup with status {process.returncode}")
        try:
            if fetch_status(base_url + "/health/ready") == 200:
                return
        except OSError:
            pass
        time.sleep(0.05)
    raise RuntimeError("server did not become ready")


def parse_client_output(output: str, label: str) -> Dict[str, Any]:
    lines = [line.strip() for line in output.splitlines() if line.strip()]
    for line in reversed(lines):
        try:
            result = json.loads(line)
        except json.JSONDecodeError:
            continue
        if result.get("schema") == "ember.soak-client.result.v1":
            return result
    raise RuntimeError(f"{label} did not emit an Ember soak client result: {output[-1000:]}")


def client_command(
    client: Path,
    base_url: str,
    worker: int,
    duration: int = 0,
) -> List[str]:
    args = [
        str(client),
        "--base-url",
        base_url,
        "--iterations",
        "1",
        "--worker",
        str(worker),
        "--mode",
        "mixed",
        "--negative-every",
        "25",
    ]
    if duration:
        args += ["--duration-seconds", str(duration)]
    return args


def server_rss(pid: int) -> Optional[int]:
    result = subprocess.run(
        ["ps", "-o", "rss=", "-p", str(pid)],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
    )
    value = result.stdout.strip()
    return int(value) * 1024 if result.returncode == 0 and value.isdigit() else None


def terminate(process: subprocess.Popen[Any]) -> None:
    if process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=5.0)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5.0)


def run_one(
    name: str,
    paths: Dict[str, Path],
    duration: int,
    clients: int,
    port: int,
    stack: str,
) -> Dict[str, Any]:
    if name == "amber":
        server_args = [
            str(paths[name]),
            "--host",
            "127.0.0.1",
            "--port",
            str(port),
            "--workers",
            "4",
        ]
    elif name in {"go", "rust"}:
        server_args = [str(paths[name]), "--host", "127.0.0.1", "--port", str(port)]
    elif name == "python":
        server_args = [sys.executable, str(paths[name]), "--host", "127.0.0.1", "--port", str(port)]
    elif name == "rails":
        server_args = [
            str(rails_ruby()),
            "-S",
            "puma",
            "--environment",
            "production",
            "--bind",
            f"tcp://127.0.0.1:{port}",
            "--threads",
            "4:4",
            str(paths[name]),
        ]
    else:
        raise RuntimeError(f"unknown server {name}")

    run_dir = BUILD / "runs" / name
    run_dir.mkdir(parents=True, exist_ok=True)
    base_url = f"http://127.0.0.1:{port}"
    print(f"\n== {name}: contract smoke + {duration}s measurement ==", flush=True)
    with (run_dir / "server.stdout.log").open("w") as stdout_log, (
        run_dir / "server.stderr.log"
    ).open("w") as stderr_log:
        server = subprocess.Popen(
            server_args,
            cwd=ROOT,
            stdout=stdout_log,
            stderr=stderr_log,
            text=True,
        )
        try:
            wait_ready(server, base_url)
            smoke = subprocess.run(
                client_command(paths["client"], base_url, 9999),
                cwd=ROOT,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=30.0,
            )
            if smoke.returncode != 0:
                raise RuntimeError(
                    f"{name} contract smoke failed ({smoke.returncode}):\n{smoke.stderr}\n{smoke.stdout}"
                )
            smoke_result = parse_client_output(smoke.stdout, f"{name} smoke")

            started = time.monotonic()
            processes = [
                subprocess.Popen(
                    client_command(paths["client"], base_url, worker, duration),
                    cwd=ROOT,
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                )
                for worker in range(clients)
            ]
            peak_rss = server_rss(server.pid)
            deadline = time.monotonic() + duration + 45.0
            while any(process.poll() is None for process in processes):
                if server.poll() is not None:
                    raise RuntimeError(f"{name} server exited during measurement")
                if time.monotonic() > deadline:
                    raise RuntimeError(f"{name} clients exceeded measurement deadline")
                rss = server_rss(server.pid)
                if rss is not None:
                    peak_rss = max(peak_rss or 0, rss)
                time.sleep(0.25)
            wall_seconds = time.monotonic() - started

            client_results = []
            for worker, process in enumerate(processes):
                output, error = process.communicate()
                (run_dir / f"client-{worker}.stdout.log").write_text(output)
                (run_dir / f"client-{worker}.stderr.log").write_text(error)
                if process.returncode != 0:
                    raise RuntimeError(
                        f"{name} client {worker} failed ({process.returncode}): {error[-2000:]}"
                    )
                client_results.append(parse_client_output(output, f"{name} client {worker}"))

            elapsed = max(float(result["elapsed_seconds"]) for result in client_results)
            requests = sum(int(result["requests"]) for result in client_results)
            valid_requests = sum(int(result["valid_requests"]) for result in client_results)
            invalid_requests = sum(int(result["invalid_requests"]) for result in client_results)
            result = {
                "server": name,
                "stack": stack if name in {"amber", "rails"} else "raw",
                "duration_seconds": duration,
                "client_count": clients,
                "requests": requests,
                "valid_requests": valid_requests,
                "invalid_requests": invalid_requests,
                "client_max_elapsed_seconds": elapsed,
                "wall_seconds": wall_seconds,
                "requests_per_second": requests / elapsed,
                "server_peak_rss_bytes": peak_rss,
                "contract_smoke_requests": smoke_result["requests"],
                "client_results": client_results,
            }
            print(json.dumps({k: v for k, v in result.items() if k != "client_results"}), flush=True)
            return result
        finally:
            terminate(server)


def runtime_versions(languages: Sequence[str]) -> Dict[str, str]:
    versions = {"amber": captured([str(ROOT / "build/amberc"), "--version"])}
    if "go" in languages:
        versions["go"] = captured(["go", "version"])
    if "rust" in languages:
        versions["rust"] = captured(["rustc", "--version"])
    if "python" in languages:
        versions["python"] = platform.python_version()
    if "rails" in languages:
        versions["rails"] = captured(
            [
                str(rails_ruby()),
                "-e",
                'require "rails"; require "puma"; '
                'puts "Ruby #{RUBY_VERSION}; Rails #{Rails.version}; Puma #{Puma::Const::PUMA_VERSION}"',
            ]
        )
    return versions


def markdown_report(payload: Dict[str, Any]) -> str:
    rows = payload["results"]
    stack = payload["stack"]
    amber_rps = next(row["requests_per_second"] for row in rows if row["server"] == "amber")
    relative = {
        row["server"]: row["requests_per_second"] / amber_rps
        for row in rows
    }
    labels = {
        "amber": (
            "Amber `net.http` (full native; VM-independent server)"
            if stack == "raw"
            else "Amber + Ember (full native; VM-independent server)"
        ),
        "go": "Go `net/http`",
        "rust": "Rust `std::net`",
        "python": "Python `ThreadingHTTPServer`",
        "rails": "Rails API 8 + Puma",
    }
    lines = [
        (
            "# Polyglot raw HTTP RPS benchmark"
            if stack == "raw"
            else "# Ember request-flow HTTP RPS benchmark"
        ),
        "",
        f"Date: `{payload['timestamp']}`<br>",
        f"Host: `{payload['host']}`<br>",
        "Client: the unchanged native-body-covered Amber client from "
        "`ember/examples/soak/client.am` (`vm-stdlib-send-v1` client bridge)<br>",
        f"Stack: `{stack}`<br>",
        f"Load: `{payload['client_count']}` concurrent clients, `{payload['duration_seconds']}` seconds per server, `mixed`, negative suite every 25 iterations.",
        "",
        "| Server | RPS | Requests | Valid | Invalid | Peak server RSS | vs Amber |",
        "|---|---:|---:|---:|---:|---:|---:|",
    ]
    for row in rows:
        rss = row["server_peak_rss_bytes"]
        rss_text = f"{rss / 1024 / 1024:.1f} MiB" if rss is not None else "n/a"
        lines.append(
            "| {label} | {rps:.2f} | {requests:,} | {valid:,} | {invalid:,} | {rss} | {relative:.2f}× |".format(
                label=labels[row["server"]],
                rps=row["requests_per_second"],
                requests=row["requests"],
                valid=row["valid_requests"],
                invalid=row["invalid_requests"],
                rss=rss_text,
                relative=row["requests_per_second"] / amber_rps,
            )
        )
    competitors = [
        f"{labels[name]} is {relative[name]:.2f}x"
        for name in (("go", "rust", "python") if stack == "raw" else ("rails",))
        if name in relative
    ]
    comparison = (
        ", ".join(competitors) + f" the Amber {stack} throughput"
        if competitors
        else "This run contains only the Amber/Ember baseline"
    )
    lines += [
        "",
        "## Reading",
        "",
        (
            f"On this workload, {comparison}. The raw lane bypasses Ember and isolates language/runtime, HTTP, JSON, validation, and in-memory-store costs."
            if stack == "raw"
            else f"On this workload, {comparison}. This lane intentionally measures the complete Ember request flow; compare it with similarly featured framework servers, not the manual raw servers."
        ),
        "",
        "## Method",
        "",
        "This is a server-side polyglot comparison, not a replacement-client microbenchmark. Every row is driven by the same compiled Amber executable and therefore performs the same persistent-connection CRUD cycle, JSON checks, schema failures, model-validation failures, optimistic-lock conflicts, method/Host rejection, and oversized-body case.",
        "",
        "All benchmark servers use a process-local, mutex-protected in-memory store so the table compares HTTP routing, JSON/schema handling, validation, and concurrency rather than unrelated database drivers. The production Ember qualification soak remains the separate SQLite-backed workload.",
        "",
        "Before each timed row, the runner executes one complete mixed Amber-client iteration (76 requests) and rejects the row on any contract mismatch. Timed RPS is total requests divided by the maximum elapsed time reported by the concurrent Amber clients. Server and clients share the same host, so client CPU is part of the available-machine budget; results are comparative for this machine, not universal language rankings.",
        "",
        "## Runtime versions",
        "",
    ]
    for name, version in payload["versions"].items():
        lines.append(f"- {name}: `{version}`")
    lines += [
        "",
        "## Reproduce",
        "",
        "```sh",
        f"python3 bench/polyglot/run_http_rps.py --stack {stack} --duration 60 --clients 4",
        "```",
        "",
        f"Machine-readable result: `{payload['json_result']}`.",
        "",
    ]
    return "\n".join(lines)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--duration", type=int, default=60)
    parser.add_argument("--clients", type=int, default=4)
    parser.add_argument("--port", type=int, default=3340)
    parser.add_argument(
        "--languages",
        help="comma-separated servers (default: raw=amber,go,rust,python; ember=amber,rails)",
    )
    parser.add_argument("--stack", choices=("raw", "ember"), default="raw")
    parser.add_argument("--compiler", type=Path, default=ROOT / "build/amberc")
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--output", type=Path)
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    if args.duration <= 0 or args.clients <= 0:
        raise RuntimeError("duration and clients must be positive")
    language_text = args.languages or (
        "amber,go,rust,python" if args.stack == "raw" else "amber,rails"
    )
    languages = [name.strip() for name in language_text.split(",") if name.strip()]
    supported = (
        {"amber", "go", "rust", "python"}
        if args.stack == "raw"
        else {"amber", "rails"}
    )
    unknown = sorted(set(languages) - supported)
    if unknown:
        raise RuntimeError("unknown languages: " + ", ".join(unknown))
    if "amber" not in languages:
        raise RuntimeError("the Amber baseline must be included")
    compiler = args.compiler.resolve()
    paths = (
        built_paths(args.stack)
        if args.skip_build
        else build_all(compiler, args.stack, languages)
    )
    ensure_paths(paths, languages)
    results = [
        run_one(
            name,
            paths,
            args.duration,
            args.clients,
            args.port + index,
            args.stack,
        )
        for index, name in enumerate(languages)
    ]

    RESULTS.mkdir(parents=True, exist_ok=True)
    stamp = dt.datetime.now().astimezone().strftime("%Y-%m-%d-%H%M%S-%z")
    result_stem = f"{args.stack}-http-rps-{stamp}"
    json_path = RESULTS / f"{result_stem}.json"
    markdown_path = (
        args.output.resolve() if args.output else RESULTS / f"{result_stem}.md"
    )
    payload = {
        "schema": "amber.polyglot.http-rps.v3",
        "stack": args.stack,
        "timestamp": dt.datetime.now().astimezone().isoformat(),
        "host": f"{platform.system()} {platform.release()} / {platform.machine()} / {platform.processor()}",
        "duration_seconds": args.duration,
        "client_count": args.clients,
        "languages": languages,
        "versions": runtime_versions(languages),
        "results": results,
        "json_result": str(json_path.relative_to(ROOT)),
    }
    json_path.write_text(json.dumps(payload, indent=2) + "\n")
    markdown_path.parent.mkdir(parents=True, exist_ok=True)
    markdown_path.write_text(markdown_report(payload))
    print(f"\nJSON: {json_path}")
    print(f"Markdown: {markdown_path}")


if __name__ == "__main__":
    main()
