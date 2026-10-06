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

from benchmark_support import (
    balanced_orders,
    file_provenance,
    git_provenance,
    host_provenance,
    relevant_environment,
    sha256_file,
    summary_stats,
    tree_provenance,
)


ROOT = Path(__file__).resolve().parents[2]
WORKSPACE = ROOT.parent
EMBER = WORKSPACE / "ember"
BUILD = ROOT / "bench" / "polyglot" / "build" / "http-rps"
RESULTS = ROOT / "bench" / "polyglot" / "results"
SPUTNIK_SERVER_NAMES = {
    "raw": "sputnik.bench.polyglot.raw_http_rps_server",
    "ember": "sputnik.bench.polyglot.http_rps_server",
}
SPUTNIK_CLIENT_NAME = "ember.example.soak.client"
PINNED_CLIENT = BUILD / "sputnik-client-pinned" / SPUTNIK_CLIENT_NAME
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


def refresh_client_pin(compiler: Path) -> None:
    client_out = PINNED_CLIENT.parent
    client_out.mkdir(parents=True, exist_ok=True)
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


def build_all(
    compiler: Path,
    client: Path,
    stack: str,
    languages: Sequence[str],
    sputnik_execution: str,
) -> Dict[str, Path]:
    sputnik_out = BUILD / f"sputnik-{stack}-server-{sputnik_execution}"
    go_out = BUILD / "go-server"
    rust_out = BUILD / "rust-server"
    for directory in (sputnik_out, go_out, rust_out):
        directory.mkdir(parents=True, exist_ok=True)

    if "sputnik" in languages and sputnik_execution == "native":
        sputnik_build = [
            str(compiler),
            "build",
            f"bench/polyglot/sputnik/{stack}_http_rps_server.build.yaml"
            if stack == "raw"
            else "bench/polyglot/sputnik/http_rps_server.build.yaml",
            "--target",
            "native",
            "--out-dir",
            str(sputnik_out),
            "--cache-dir",
            str(sputnik_out / "cache"),
        ]
        sputnik_build.append("--require-full-native")
        sputnik_build += [
            "--grant",
            "net.listen",
            "--grant",
            "random.secure",
            "--grant",
            "ffi",
        ]
        command(sputnik_build)
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
        "sputnik": (
            sputnik_out / SPUTNIK_SERVER_NAMES[stack]
            if sputnik_execution == "native"
            else (
                ROOT / f"bench/polyglot/sputnik/{stack}_http_rps_server.build.yaml"
                if stack == "raw"
                else ROOT / "bench/polyglot/sputnik/http_rps_server.build.yaml"
            )
        ),
        "compiler": compiler,
        "client": client,
        "go": go_out / "http-rps-server",
        "rust": rust_out / "http-rps-server",
        "python": ROOT / "bench/polyglot/python/http_rps_server.py",
        "rails": ROOT / "bench/polyglot/rails/config.ru",
    }


def built_paths(
    compiler: Path, client: Path, stack: str, sputnik_execution: str
) -> Dict[str, Path]:
    return {
        "sputnik": (
            BUILD
            / f"sputnik-{stack}-server-{sputnik_execution}"
            / SPUTNIK_SERVER_NAMES[stack]
            if sputnik_execution == "native"
            else (
                ROOT / f"bench/polyglot/sputnik/{stack}_http_rps_server.build.yaml"
                if stack == "raw"
                else ROOT / "bench/polyglot/sputnik/http_rps_server.build.yaml"
            )
        ),
        "compiler": compiler,
        "client": client,
        "go": BUILD / "go-server" / "http-rps-server",
        "rust": BUILD / "rust-server" / "http-rps-server",
        "python": ROOT / "bench/polyglot/python/http_rps_server.py",
        "rails": ROOT / "bench/polyglot/rails/config.ru",
    }


def rails_ruby() -> Path:
    override = os.environ.get("SPUTNIK_BENCH_RUBY")
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
        "set SPUTNIK_BENCH_RUBY to a suitable Ruby executable"
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


def loaded_native_extensions(pid: int) -> List[str]:
    try:
        result = subprocess.run(
            ["/usr/sbin/lsof", "-Fn", "-p", str(pid)],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            timeout=5.0,
        )
    except (OSError, subprocess.TimeoutExpired):
        return []
    if result.returncode != 0:
        return []
    return sorted(
        {
            line[1:]
            for line in result.stdout.splitlines()
            if line.startswith("n")
            and line.endswith("sputnik_manifest_extensions.dylib")
        }
    )


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
    sputnik_execution: str,
    sputnik_pool_size: int,
    sample_seconds: int,
    server_max_requests_per_connection: int | None,
    series_id: str,
    repeat_index: int,
    order_position: int,
) -> Dict[str, Any]:
    if name == "sputnik":
        program_args = [
            "--host", "127.0.0.1", "--port", str(port), "--workers", "4"
        ]
        if stack == "ember":
            program_args += ["--pool-size", str(sputnik_pool_size)]
        if stack == "raw" and server_max_requests_per_connection is not None:
            program_args += [
                "--max-requests-per-connection",
                str(server_max_requests_per_connection),
            ]
        server_args = (
            [str(paths[name]), *program_args]
            if sputnik_execution == "native"
            else [
                str(paths["compiler"]),
                "run",
                str(paths[name]),
                "--grant",
                "net.listen",
                "--grant",
                "random.secure",
                "--grant",
                "ffi",
                "--",
                *program_args,
            ]
        )
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

    run_dir = (
        BUILD
        / "runs"
        / series_id
        / f"repeat-{repeat_index + 1:02d}-position-{order_position + 1:02d}-{name}"
    )
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
            native_extensions = (
                loaded_native_extensions(server.pid)
                if name == "sputnik" and sputnik_execution == "vm"
                else []
            )
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
            sample_path = run_dir / "server.sample.txt"
            sampler = (
                subprocess.Popen(
                    [
                        "/usr/bin/sample",
                        str(server.pid),
                        str(min(sample_seconds, duration)),
                        "1",
                        "-file",
                        str(sample_path),
                    ],
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.PIPE,
                    text=True,
                )
                if sample_seconds
                else None
            )
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
            if sampler is not None:
                _, sample_error = sampler.communicate(timeout=10.0)
                if sampler.returncode != 0:
                    raise RuntimeError(
                        f"{name} sample failed ({sampler.returncode}): {sample_error}"
                    )

            elapsed = max(float(result["elapsed_seconds"]) for result in client_results)
            requests = sum(int(result["requests"]) for result in client_results)
            valid_requests = sum(int(result["valid_requests"]) for result in client_results)
            invalid_requests = sum(int(result["invalid_requests"]) for result in client_results)
            result = {
                "server": name,
                "repeat_index": repeat_index + 1,
                "order_position": order_position + 1,
                "stack": stack if name in {"sputnik", "rails"} else "raw",
                "execution": sputnik_execution if name == "sputnik" else "native",
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
                "sputnik_pool_size": (
                    sputnik_pool_size
                    if name == "sputnik" and stack == "ember"
                    else None
                ),
                "sample_profile": str(sample_path) if sampler is not None else None,
                "loaded_native_extensions": native_extensions,
                "server_max_requests_per_connection": (
                    server_max_requests_per_connection
                    if name == "sputnik" and stack == "raw"
                    else None
                ),
                "client_results": client_results,
            }
            print(json.dumps({k: v for k, v in result.items() if k != "client_results"}), flush=True)
            return result
        finally:
            terminate(server)


def aggregate_http_results(
    samples: Sequence[Dict[str, Any]], languages: Sequence[str]
) -> List[Dict[str, Any]]:
    rows = []
    for name in languages:
        selected = [sample for sample in samples if sample["server"] == name]
        if not selected:
            continue
        rows.append(
            {
                "server": name,
                "stack": selected[0]["stack"],
                "execution": selected[0]["execution"],
                "runs": len(selected),
                "rps": summary_stats(
                    [sample["requests_per_second"] for sample in selected]
                ),
                "peak_rss_bytes": summary_stats(
                    [
                        sample["server_peak_rss_bytes"]
                        for sample in selected
                        if sample["server_peak_rss_bytes"] is not None
                    ]
                ),
                "requests": sum(sample["requests"] for sample in selected),
                "valid_requests": sum(
                    sample["valid_requests"] for sample in selected
                ),
                "invalid_requests": sum(
                    sample["invalid_requests"] for sample in selected
                ),
                "contract_smoke_requests": sum(
                    sample["contract_smoke_requests"] for sample in selected
                ),
            }
        )
    return rows


def paired_comparisons(
    samples: Sequence[Dict[str, Any]], languages: Sequence[str]
) -> List[Dict[str, Any]]:
    by_repeat: Dict[int, Dict[str, float]] = {}
    for sample in samples:
        by_repeat.setdefault(sample["repeat_index"], {})[
            sample["server"]
        ] = sample["requests_per_second"]
    comparisons = []
    for name in languages:
        if name == "sputnik":
            continue
        ratios = []
        deltas = []
        for repeat_index in sorted(by_repeat):
            repeat = by_repeat[repeat_index]
            if "sputnik" not in repeat or name not in repeat:
                continue
            ratio = repeat[name] / repeat["sputnik"]
            ratios.append(ratio)
            deltas.append((ratio - 1.0) * 100.0)
        comparisons.append(
            {
                "baseline": "sputnik",
                "competitor": name,
                "paired_ratio": summary_stats(ratios),
                "paired_delta_percent": summary_stats(deltas),
            }
        )
    return comparisons


def benchmark_provenance(
    paths: Dict[str, Path], languages: Sequence[str], stack: str,
    sputnik_execution: str, samples: Sequence[Dict[str, Any]],
) -> Dict[str, Any]:
    dependency_roots = {
        "sputnik": ROOT,
        "ember": EMBER,
        "sputnik_orm": EMBER / ".build/dependencies/sputnik-orm",
        "sqlite3_sputnik": EMBER / ".build/dependencies/sqlite3-sputnik",
    }
    repositories = {
        name: git_provenance(path)
        for name, path in dependency_roots.items()
        if path.exists()
    }
    artifacts = {
        "compiler": file_provenance(paths["compiler"]),
        "client": file_provenance(paths["client"]),
    }
    for name in languages:
        path = paths[name]
        if path.is_file():
            artifacts[f"server_{name}"] = file_provenance(path)
    extension_paths = sorted(
        {
            extension
            for sample in samples
            for extension in sample.get("loaded_native_extensions", [])
        }
    )
    for index, extension in enumerate(extension_paths, start=1):
        artifacts[f"sputnik_vm_extension_{index}"] = file_provenance(
            Path(extension)
        )
    source_paths = [
        ROOT / "bench/polyglot/run_http_rps.py",
        ROOT / "bench/polyglot/benchmark_support.py",
        EMBER / "examples/soak/client.s",
        EMBER / "examples/soak/client.build.yaml",
        EMBER / "sputnik.lock",
        EMBER / "release/dependencies.lock.json",
    ]
    if stack == "ember":
        source_paths.extend(
            [
                ROOT / "bench/polyglot/sputnik/http_rps_server.s",
                ROOT / "bench/polyglot/sputnik/http_rps_server.build.yaml",
                ROOT / "bench/polyglot/rails/config.ru",
                ROOT / "bench/polyglot/rails/http_rps_app.rb",
                EMBER / "src/ember.s",
                EMBER / "src/ember/telemetry.s",
                EMBER / "packages/ember-orm/src",
                EMBER / ".build/dependencies/sputnik-orm/src",
                EMBER / ".build/dependencies/sqlite3-sputnik/src",
                EMBER / ".build/dependencies/sqlite3-sputnik/native/sqlite3_ext.c",
            ]
        )
    else:
        source_paths.extend(
            [
                ROOT / "bench/polyglot/sputnik/raw_http_rps_server.s",
                ROOT / "bench/polyglot/sputnik/raw_http_rps_server.build.yaml",
                ROOT / "bench/polyglot/go/http_rps_server.go",
                ROOT / "bench/polyglot/rust/http_rps_server.rs",
                ROOT / "bench/polyglot/python/http_rps_server.py",
            ]
        )
    runtime_bundles = {}
    if "sputnik" in languages and sputnik_execution == "vm":
        sputnik_state = ROOT / "bench/polyglot/sputnik/.sputnik"
        runtime_bundles = {
            "sputnik_vm_bytecode": tree_provenance(
                [sputnik_state / "vm/out"], relative_to=ROOT
            ),
        }
    return {
        "host": host_provenance(),
        "repositories": repositories,
        "artifacts": artifacts,
        "runtime_bundles": runtime_bundles,
        "source_tree": tree_provenance(source_paths, relative_to=WORKSPACE),
        "environment": relevant_environment(
            ["SPUTNIK_BENCH_RUBY", "CXX", "CC", "RUSTFLAGS", "GOFLAGS"]
        ),
        "argv": sys.argv,
    }


def runtime_versions(
    languages: Sequence[str], compiler: Path
) -> Dict[str, str]:
    versions = {"sputnik": captured([str(compiler), "--version"])}
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
    sputnik_execution = payload["sputnik_execution"]
    sputnik_pool_size = payload["sputnik_pool_size"]
    comparisons = {
        row["competitor"]: row for row in payload["paired_comparisons"]
    }
    labels = {
        "sputnik": (
            (
                "Sputnik `net.http` (full native; VM-independent server)"
                if stack == "raw"
                else "Sputnik + Ember (full native; VM-independent server)"
            )
            if sputnik_execution == "native"
            else (
                "Sputnik `net.http` (interpreted bytecode VM)"
                if stack == "raw"
                else "Sputnik + Ember (interpreted bytecode VM)"
            )
        ),
        "go": "Go `net/http`",
        "rust": "Rust `std::net`",
        "python": "Python `ThreadingHTTPServer`",
        "rails": "Rails API 8 + Puma",
    }
    storage_method = (
        "The Ember and Rails framework servers persist the catalog through ORM "
        "models backed by a process-local shared in-memory SQLite database. "
        "The raw lane keeps its mutex-protected in-memory store. This makes the "
        "framework table include model lifecycle, SQL generation, connection-pool, "
        "and SQLite costs while keeping the database local and deterministic. "
        f"The Sputnik pool contains {sputnik_pool_size} connection(s) for this run."
        if stack == "ember"
        else "All raw benchmark servers use a process-local, mutex-protected "
        "in-memory store so the table compares HTTP routing, JSON/schema handling, "
        "validation, and concurrency rather than unrelated database drivers."
    )
    lines = [
        (
            "# Polyglot raw HTTP RPS benchmark"
            if stack == "raw"
            else "# Ember request-flow HTTP RPS benchmark"
        ),
        "",
        f"Date: `{payload['timestamp']}`<br>",
        f"Host: `{payload['host']}`<br>",
        "Client: pinned native-body-covered Sputnik client from "
        f"`ember/examples/soak/client.s`; SHA-256 `{payload['client']['sha256']}` "
        "(`vm-stdlib-send-v1` client bridge)<br>",
        f"Stack: `{stack}`<br>",
        f"Sputnik execution: `{sputnik_execution}`<br>",
        (
            f"Sputnik SQLite pool size: `{sputnik_pool_size}`<br>"
            if stack == "ember"
            else ""
        ),
        f"Load: `{payload['client_count']}` concurrent clients, `{payload['duration_seconds']}` seconds per server, `mixed`, negative suite every 25 iterations.",
        f"Statistics: `{payload['repeats']}` paired repeats, balanced rotation seed `{payload['order_seed']}`; "
        + (
            "throughput samples are unprofiled."
            if not payload["sample_seconds"]
            else f"this diagnostic run samples each server for `{payload['sample_seconds']}` seconds."
        ),
        "",
        "| Server | Median RPS | Mean RPS | Stdev | CV | Mean 95% CI | Median peak RSS | Paired vs Sputnik |",
        "|---|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for row in rows:
        rps = row["rps"]
        rss = row["peak_rss_bytes"]["median"]
        rss_text = f"{rss / 1024 / 1024:.1f} MiB" if rss is not None else "n/a"
        paired = (
            "1.000×"
            if row["server"] == "sputnik"
            else f"{comparisons[row['server']]['paired_ratio']['median']:.3f}×"
        )
        lines.append(
            "| {label} | {median:.2f} | {mean:.2f} | {stdev:.2f} | {cv:.2f}% | {low:.2f}…{high:.2f} | {rss} | {paired} |".format(
                label=labels[row["server"]],
                median=rps["median"],
                mean=rps["mean"],
                stdev=rps["stdev"],
                cv=rps["cv_percent"],
                low=rps["ci95_mean_low"],
                high=rps["ci95_mean_high"],
                rss=rss_text,
                paired=paired,
            )
        )
    competitor_text = []
    for name in (("go", "rust", "python") if stack == "raw" else ("rails",)):
        comparison = comparisons.get(name)
        if comparison is None:
            continue
        ratio = comparison["paired_ratio"]["median"]
        delta = comparison["paired_delta_percent"]["median"]
        competitor_text.append(
            f"{labels[name]}: {ratio:.3f}× Sputnik ({delta:+.2f}%)"
        )
    lines += [
        "",
        "## Reading",
        "",
        (
            "; ".join(competitor_text) + ". "
            if competitor_text
            else "This run contains only the Sputnik baseline. "
        )
        + (
            "The raw lane bypasses Ember and isolates language/runtime, HTTP, JSON, validation, and in-memory-store costs."
            if stack == "raw"
            else "The framework lane includes routing, Strong Parameters-equivalent schema handling, controllers, ORM models, pooling, and SQLite."
        ),
        "",
        "## Method",
        "",
        "This is a server-side polyglot comparison, not a replacement-client microbenchmark. Every row is driven by the same compiled Sputnik executable and therefore performs the same persistent-connection CRUD cycle, JSON checks, schema failures, model-validation failures, optimistic-lock conflicts, method/Host rejection, and oversized-body case.",
        "",
        storage_method,
        "",
        "Before every timed sample, the runner starts a fresh server and executes one complete mixed Sputnik-client iteration (76 requests), rejecting the sample on any contract mismatch. Each repeat rotates server order so every implementation occupies different thermal/cache positions. Timed RPS is total requests divided by the maximum elapsed time reported by the concurrent Sputnik clients. The table reports per-server distributions and paired competitor/Sputnik ratios from the same repeat.",
        "",
        "Profiler samples are forbidden in multi-repeat throughput mode. Server and clients share the same host, so client CPU is part of the available-machine budget; results are comparative for this machine, not universal language rankings.",
        "",
        "## Run order",
        "",
    ]
    for index, order in enumerate(payload["run_orders"], start=1):
        lines.append(f"- repeat {index}: `{' → '.join(order)}`")
    lines += [
        "",
        "## Runtime versions",
        "",
    ]
    for name, version in payload["versions"].items():
        lines.append(f"- {name}: `{version}`")
    lines += ["", "## Provenance", ""]
    provenance = payload["provenance"]
    for name, repository in provenance["repositories"].items():
        if not repository.get("available"):
            lines.append(f"- {name}: not a Git checkout (`{repository['path']}`)")
            continue
        dirty = "dirty" if repository["tracked_dirty"] else "clean"
        lines.append(
            f"- {name}: `{repository['commit']}` ({dirty}, `{repository['path']}`)"
        )
    for name, artifact in provenance["artifacts"].items():
        lines.append(
            f"- {name}: SHA-256 `{artifact['sha256']}`, {artifact['size_bytes']} bytes (`{artifact['path']}`)"
        )
    for name, bundle in provenance["runtime_bundles"].items():
        lines.append(
            f"- {name}: SHA-256 `{bundle['sha256']}` over {bundle['file_count']} files"
        )
    lines.append(
        f"- benchmark source tree: SHA-256 `{provenance['source_tree']['sha256']}` over {provenance['source_tree']['file_count']} files"
    )
    lines += [
        "",
        "## Reproduce",
        "",
        "```sh",
        "python3 bench/polyglot/run_http_rps.py "
        f"--stack {stack} --sputnik-execution {sputnik_execution} "
        f"--duration {payload['duration_seconds']} --clients {payload['client_count']} "
        f"--repeats {payload['repeats']} --order-seed {payload['order_seed']} "
        f"--sputnik-pool-size {sputnik_pool_size} "
        f"--languages {','.join(payload['languages'])} "
        f"--compiler {payload['provenance']['artifacts']['compiler']['path']} "
        f"--client {payload['client']['path']}",
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
    parser.add_argument(
        "--repeats",
        type=int,
        default=5,
        help="number of paired, order-rotated throughput repeats",
    )
    parser.add_argument(
        "--order-seed",
        type=int,
        default=0,
        help="deterministic seed for the initial server order",
    )
    parser.add_argument(
        "--sputnik-pool-size",
        type=int,
        default=1,
        help="SQLite pool size for the Sputnik/Ember workload",
    )
    parser.add_argument("--port", type=int, default=3340)
    parser.add_argument(
        "--languages",
        help="comma-separated servers (default: raw=sputnik,go,rust,python; ember=sputnik,rails)",
    )
    parser.add_argument("--stack", choices=("raw", "ember"), default="raw")
    parser.add_argument(
        "--sputnik-execution", choices=("native", "vm"), default="native"
    )
    parser.add_argument("--compiler", type=Path, default=ROOT / "build/sputnik")
    parser.add_argument(
        "--client",
        type=Path,
        default=PINNED_CLIENT,
        help="prebuilt native Sputnik load client; never rebuilt implicitly",
    )
    parser.add_argument(
        "--refresh-client-pin",
        action="store_true",
        help="explicitly rebuild the default pinned client before running",
    )
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--sample-seconds", type=int, default=0)
    parser.add_argument(
        "--server-max-requests-per-connection",
        type=int,
        help="override the raw Sputnik server keep-alive rotation limit",
    )
    parser.add_argument("--output", type=Path)
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    if (
        args.duration <= 0
        or args.clients <= 0
        or args.repeats <= 0
        or args.sputnik_pool_size <= 0
        or args.sample_seconds < 0
    ):
        raise RuntimeError(
            "duration, clients, repeats, and sputnik-pool-size must be positive"
        )
    if args.sample_seconds and args.repeats != 1:
        raise RuntimeError(
            "profiled runs require --repeats 1; never mix sampling with "
            "multi-repeat throughput statistics"
        )
    if (
        args.server_max_requests_per_connection is not None
        and args.server_max_requests_per_connection <= 0
    ):
        raise RuntimeError(
            "server-max-requests-per-connection must be positive"
        )
    language_text = args.languages or (
        "sputnik,go,rust,python" if args.stack == "raw" else "sputnik,rails"
    )
    languages = [name.strip() for name in language_text.split(",") if name.strip()]
    supported = (
        {"sputnik", "go", "rust", "python"}
        if args.stack == "raw"
        else {"sputnik", "rails"}
    )
    unknown = sorted(set(languages) - supported)
    if unknown:
        raise RuntimeError("unknown languages: " + ", ".join(unknown))
    if len(set(languages)) != len(languages):
        raise RuntimeError("languages must not contain duplicates")
    if "sputnik" not in languages:
        raise RuntimeError("the Sputnik baseline must be included")
    compiler = args.compiler.resolve()
    client = args.client.resolve()
    if args.refresh_client_pin:
        if client != PINNED_CLIENT.resolve():
            raise RuntimeError(
                "--refresh-client-pin only updates the default pinned client"
            )
        refresh_client_pin(compiler)
    paths = (
        built_paths(compiler, client, args.stack, args.sputnik_execution)
        if args.skip_build
        else build_all(
            compiler, client, args.stack, languages, args.sputnik_execution
        )
    )
    ensure_paths(paths, languages)
    client_sha256 = sha256_file(paths["client"])
    print(
        f"Pinned client: {paths['client']} (sha256:{client_sha256})",
        flush=True,
    )
    RESULTS.mkdir(parents=True, exist_ok=True)
    stamp = dt.datetime.now().astimezone().strftime("%Y-%m-%d-%H%M%S-%z")
    series_id = (
        f"{args.stack}-{args.sputnik_execution}-r{args.repeats}-{stamp}"
    )
    run_orders = balanced_orders(languages, args.repeats, args.order_seed)
    ports = {name: args.port + index for index, name in enumerate(languages)}
    samples = []
    for repeat_index, order in enumerate(run_orders):
        print(
            f"\n## repeat {repeat_index + 1}/{args.repeats}: "
            + " -> ".join(order),
            flush=True,
        )
        for order_position, name in enumerate(order):
            samples.append(
                run_one(
                    name,
                    paths,
                    args.duration,
                    args.clients,
                    ports[name],
                    args.stack,
                    args.sputnik_execution,
                    args.sputnik_pool_size,
                    args.sample_seconds,
                    args.server_max_requests_per_connection,
                    series_id,
                    repeat_index,
                    order_position,
                )
            )
    results = aggregate_http_results(samples, languages)
    comparisons = paired_comparisons(samples, languages)
    provenance = benchmark_provenance(
        paths, languages, args.stack, args.sputnik_execution, samples
    )
    pool_suffix = (
        f"-pool{args.sputnik_pool_size}" if args.stack == "ember" else ""
    )
    result_stem = (
        f"{args.stack}-http-rps-{args.sputnik_execution}{pool_suffix}"
        f"-r{args.repeats}-{stamp}"
    )
    json_path = RESULTS / f"{result_stem}.json"
    markdown_path = (
        args.output.resolve() if args.output else RESULTS / f"{result_stem}.md"
    )
    payload = {
        "schema": "sputnik.polyglot.http-rps.v5",
        "stack": args.stack,
        "sputnik_execution": args.sputnik_execution,
        "timestamp": dt.datetime.now().astimezone().isoformat(),
        "host": provenance["host"]["platform"],
        "duration_seconds": args.duration,
        "client_count": args.clients,
        "repeats": args.repeats,
        "order_seed": args.order_seed,
        "run_orders": run_orders,
        "sputnik_pool_size": args.sputnik_pool_size,
        "languages": languages,
        "client": {
            "path": str(paths["client"]),
            "sha256": client_sha256,
        },
        "versions": runtime_versions(languages, compiler),
        "measurement_mode": (
            "profiled" if args.sample_seconds else "throughput"
        ),
        "sample_seconds": args.sample_seconds,
        "provenance": provenance,
        "samples": samples,
        "results": results,
        "paired_comparisons": comparisons,
        "json_result": str(json_path.relative_to(ROOT)),
    }
    json_path.write_text(json.dumps(payload, indent=2) + "\n")
    markdown_path.parent.mkdir(parents=True, exist_ok=True)
    markdown_path.write_text(markdown_report(payload))
    print(f"\nJSON: {json_path}")
    print(f"Markdown: {markdown_path}")


if __name__ == "__main__":
    main()
