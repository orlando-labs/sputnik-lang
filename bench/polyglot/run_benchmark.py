#!/usr/bin/env python3
import argparse
from dataclasses import dataclass
import datetime as dt
import json
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Optional

from benchmark_support import (
    balanced_orders,
    file_provenance,
    git_provenance,
    host_provenance,
    relevant_environment,
    summary_stats,
    tree_provenance,
)


@dataclass(frozen=True)
class Workload:
    name: str
    expected_checksum: str
    amber_module: str
    amber_entry: str
    amber_source: str
    python_source: str
    ruby_source: str
    cpp_source: str
    cpp_binary: str
    go_source: str
    go_binary: str
    rust_source: str
    rust_binary: str
    requires_full_amber_native: bool = True
    amber_grants: tuple[str, ...] = ()


JSON_RECORDS = 20000
JSON_FIXTURE_RELATIVE = Path("bench/polyglot/build/json/events.jsonl")


WORKLOADS = {
    "arithmetic": Workload(
        name="arithmetic",
        expected_checksum="715609516598740",
        amber_module="bench.polyglot",
        amber_entry="main",
        amber_source="main.am",
        python_source="main.py",
        ruby_source="main.rb",
        cpp_source="main.cpp",
        cpp_binary="main",
        go_source="main.go",
        go_binary="main",
        rust_source="main.rs",
        rust_binary="main",
    ),
    "calls-collections": Workload(
        name="calls-collections",
        expected_checksum="2047795430",
        amber_module="bench.polyglot.calls_collections",
        amber_entry="__init__",
        amber_source="calls_collections.am",
        python_source="calls_collections.py",
        ruby_source="calls_collections.rb",
        cpp_source="calls_collections.cpp",
        cpp_binary="calls_collections",
        go_source="calls_collections.go",
        go_binary="calls_collections",
        rust_source="calls_collections.rs",
        rust_binary="calls_collections",
    ),
    "sha-digest": Workload(
        name="sha-digest",
        expected_checksum="5616000",
        amber_module="bench.polyglot.sha_digest",
        amber_entry="__init__",
        amber_source="sha_digest.am",
        python_source="sha_digest.py",
        ruby_source="sha_digest.rb",
        cpp_source="sha_digest.cpp",
        cpp_binary="sha_digest",
        go_source="sha_digest.go",
        go_binary="sha_digest",
        rust_source="sha_digest.rs",
        rust_binary="sha_digest",
    ),
    "json": Workload(
        name="json",
        expected_checksum="1531352227",
        amber_module="bench.polyglot.json",
        amber_entry="main",
        amber_source="json.am",
        python_source="json_workload.py",
        ruby_source="json_workload.rb",
        cpp_source="json_workload.cpp",
        cpp_binary="json_workload",
        go_source="json_workload.go",
        go_binary="json_workload",
        rust_source="json_workload.rs",
        rust_binary="json_workload",
        amber_grants=("fs.read=bench/polyglot/build/json/events.jsonl",),
    ),
    "string-ops": Workload(
        name="string-ops",
        expected_checksum="280113",
        amber_module="bench.polyglot.string_ops",
        amber_entry="main",
        amber_source="string_ops.am",
        python_source="string_ops.py",
        ruby_source="string_ops.rb",
        cpp_source="string_ops.cpp",
        cpp_binary="string_ops",
        go_source="string_ops.go",
        go_binary="string_ops",
        rust_source="string_ops.rs",
        rust_binary="string_ops",
    ),
    "map-words": Workload(
        name="map-words",
        expected_checksum="235174",
        amber_module="bench.polyglot.map_words",
        amber_entry="main",
        amber_source="map_words.am",
        python_source="map_words.py",
        ruby_source="map_words.rb",
        cpp_source="map_words.cpp",
        cpp_binary="map_words",
        go_source="map_words.go",
        go_binary="map_words",
        rust_source="map_words.rs",
        rust_binary="map_words",
    ),
    "codecs": Workload(
        name="codecs",
        expected_checksum="2056190",
        amber_module="bench.polyglot.codecs",
        amber_entry="main",
        amber_source="codecs.am",
        python_source="codecs_workload.py",
        ruby_source="codecs_workload.rb",
        cpp_source="codecs_workload.cpp",
        cpp_binary="codecs_workload",
        go_source="codecs_workload.go",
        go_binary="codecs_workload",
        rust_source="codecs_workload.rs",
        rust_binary="codecs_workload",
    ),
    "secure-random": Workload(
        name="secure-random",
        expected_checksum="296000",
        amber_module="bench.polyglot.secure_random",
        amber_entry="main",
        amber_source="secure_random.am",
        python_source="secure_random.py",
        ruby_source="secure_random.rb",
        cpp_source="secure_random.cpp",
        cpp_binary="secure_random",
        go_source="secure_random.go",
        go_binary="secure_random",
        rust_source="secure_random.rs",
        rust_binary="secure_random",
        amber_grants=("random.secure",),
    ),
    "time-flow": Workload(
        name="time-flow",
        expected_checksum="110397732",
        amber_module="bench.polyglot.time_flow",
        amber_entry="main",
        amber_source="time_flow.am",
        python_source="time_flow.py",
        ruby_source="time_flow.rb",
        cpp_source="time_flow.cpp",
        cpp_binary="time_flow",
        go_source="time_flow.go",
        go_binary="time_flow",
        rust_source="time_flow.rs",
        rust_binary="time_flow",
    ),
    "uuid": Workload(
        name="uuid",
        expected_checksum="1040000",
        amber_module="bench.polyglot.uuid",
        amber_entry="main",
        amber_source="uuid.am",
        python_source="uuid_workload.py",
        ruby_source="uuid_workload.rb",
        cpp_source="uuid_workload.cpp",
        cpp_binary="uuid_workload",
        go_source="uuid_workload.go",
        go_binary="uuid_workload",
        rust_source="uuid_workload.rs",
        rust_binary="uuid_workload",
        amber_grants=("random.secure",),
    ),
}


def repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def run_command(
    command, cwd: Path, *, capture=True, env: Optional[dict] = None
) -> subprocess.CompletedProcess:
    run_env = None
    if env is not None:
        run_env = os.environ.copy()
        run_env.update({key: str(value) for key, value in env.items()})
    completed = subprocess.run(
        [str(part) for part in command],
        cwd=str(cwd),
        env=run_env,
        text=True,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.PIPE if capture else None,
    )
    if completed.returncode != 0:
        stdout = completed.stdout or ""
        stderr = completed.stderr or ""
        raise RuntimeError(
            "command failed with exit code "
            f"{completed.returncode}: {' '.join(map(str, command))}\n"
            f"stdout:\n{stdout}\nstderr:\n{stderr}"
        )
    return completed


def choose_cxx() -> str:
    env_cxx = os.environ.get("CXX")
    if env_cxx:
        return env_cxx
    for candidate in ("clang++", "g++", "c++"):
        path = shutil.which(candidate)
        if path:
            return path
    raise RuntimeError("no C++ compiler found in PATH")


def choose_go() -> Optional[str]:
    return shutil.which("go")


def choose_rust() -> Optional[str]:
    return shutil.which("rustc")


def choose_ruby() -> str:
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
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return str(candidate.resolve())
    raise RuntimeError(
        "ruby was not found; set AMBER_BENCH_RUBY to a Ruby executable"
    )


def ensure_amber_tools(root: Path) -> None:
    # Let make verify source timestamps even when both tools already exist.
    # A merely present iamber may predate the VM changes being benchmarked.
    run_command(["make", "build/amberc", "build/iamber"], root, capture=False)


def prepare_json_fixture(root: Path) -> Path:
    path = root / JSON_FIXTURE_RELATIVE
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as handle:
        for i in range(JSON_RECORDS):
            value = (i * 17) % 100000
            group = i % 8
            name = i % 100
            handle.write(
                f'{{"id":{i},"value":{value},'
                f'"group":{group},"name":"event-{name}"}}\n'
            )
    return path


def amber_artifact_path(build_dir: Path, workload: Workload) -> Path:
    return build_dir / "amber" / "out" / f"{workload.amber_module}.amberbc"


def safe_artifact_name(value: str) -> str:
    return "".join(
        ch if ch.isalnum() or ch in "._-" else "_"
        for ch in value
    ) or "artifact"


def amber_native_path(build_dir: Path, workload: Workload) -> Path:
    return build_dir / "amber" / "native" / safe_artifact_name(workload.amber_module)


def build_amber_bytecode(root: Path, build_dir: Path) -> None:
    out_dir = build_dir / "amber" / "out"
    cache_dir = build_dir / "amber" / "cache"
    out_dir.mkdir(parents=True, exist_ok=True)
    cache_dir.mkdir(parents=True, exist_ok=True)
    run_command(
        [
            root / "build" / "amberc",
            "build",
            root / "bench" / "polyglot" / "amber" / "amber.build.json",
            "--out-dir",
            out_dir,
            "--cache-dir",
            cache_dir,
            "--target",
            "bytecode",
        ],
        root,
    )
    for workload in WORKLOADS.values():
        artifact = amber_artifact_path(build_dir, workload)
        if not artifact.exists():
            raise RuntimeError(
                f"expected Amber bytecode artifact is missing: {artifact}"
            )


def build_amber_native_executable(
    root: Path, build_dir: Path, workload: Workload
) -> Path:
    output = amber_native_path(build_dir, workload)
    output.parent.mkdir(parents=True, exist_ok=True)
    source = root / "bench" / "polyglot" / "amber" / "src" / workload.amber_source
    entry = "init" if workload.amber_entry == "__init__" else "main-only"
    command = [
        root / "build" / "amberc",
        "build",
        source,
        "-o",
        output,
        "--entry",
        entry,
    ]
    for grant in workload.amber_grants:
        command.extend(["--grant", grant])
    completed = run_command(command, root)
    build_result = json.loads(completed.stdout)
    if build_result.get("status") != "ok":
        raise RuntimeError(f"Amber native build failed: {completed.stdout}")
    if not workload.requires_full_amber_native:
        if not output.exists():
            raise RuntimeError(f"expected Amber built executable is missing: {output}")
        return output
    if build_result.get("native_backend") != "cpp-bytecode-direct-v1":
        raise RuntimeError(
            "Amber built benchmark expected native backend "
            f"cpp-bytecode-direct-v1, got {build_result.get('native_backend')!r}"
        )
    if build_result.get("native_entry") is not True:
        raise RuntimeError(
            "Amber built benchmark entry is not native: "
            f"{build_result.get('native_fallback_reason')}"
        )
    native_code_count = build_result.get("native_code_count")
    bytecode_code_count = build_result.get("bytecode_code_count")
    if native_code_count != bytecode_code_count:
        raise RuntimeError(
            "Amber built benchmark requires full native coverage, got "
            f"{native_code_count}/{bytecode_code_count} code objects: "
            f"{build_result.get('native_fallback_reason')}"
        )
    if not output.exists():
        raise RuntimeError(f"expected Amber native executable is missing: {output}")
    return output


def compile_cpp_program(
    root: Path, build_dir: Path, cxx: str, workload: Workload
) -> Path:
    output = build_dir / "cpp" / workload.cpp_binary
    output.parent.mkdir(parents=True, exist_ok=True)
    sources = [root / "bench" / "polyglot" / "cpp" / workload.cpp_source]
    extra_args = []
    if workload.name == "sha-digest":
        sources.append(root / "runtime" / "digest.cpp")
        extra_args.extend(["-I", root])
    run_command(
        [
            cxx,
            "-std=c++17",
            "-O2",
            "-pipe",
            *extra_args,
            *sources,
            "-o",
            output,
        ],
        root,
    )
    return output


def compile_go_program(root: Path, build_dir: Path, go: str, workload: Workload) -> Path:
    output = build_dir / "go" / workload.go_binary
    cache_dir = build_dir / "go-cache"
    output.parent.mkdir(parents=True, exist_ok=True)
    cache_dir.mkdir(parents=True, exist_ok=True)
    run_command(
        [
            go,
            "build",
            "-o",
            output,
            root / "bench" / "polyglot" / "go" / workload.go_source,
        ],
        root,
        env={"GOCACHE": cache_dir},
    )
    return output


def compile_rust_program(
    root: Path, build_dir: Path, rustc: str, workload: Workload
) -> Path:
    output = build_dir / "rust" / workload.rust_binary
    output.parent.mkdir(parents=True, exist_ok=True)
    run_command(
        [
            rustc,
            "-O",
            "--edition",
            "2021",
            "-o",
            output,
            root / "bench" / "polyglot" / "rust" / workload.rust_source,
        ],
        root,
    )
    return output


def compile_amberbc_runner(root: Path, build_dir: Path, cxx: str) -> Path:
    output = build_dir / "amberbc_run"
    output.parent.mkdir(parents=True, exist_ok=True)
    sources = [
        root / "bench" / "polyglot" / "tools" / "amberbc_run.cpp",
        root / "bytecode" / "format.cpp",
        root / "frontend" / "lexer" / "token.cpp",
        root / "profile" / "capabilities.cpp",
        root / "profile" / "effects.cpp",
        root / "profile" / "replay.cpp",
        root / "profile" / "data.cpp",
        root / "profile" / "wasm_accel.cpp",
        root / "profile" / "modern.cpp",
        root / "package" / "package.cpp",
        root / "runtime" / "context.cpp",
        root / "runtime" / "text.cpp",
        root / "runtime" / "io.cpp",
        root / "runtime" / "digest.cpp",
        root / "runtime" / "vm.cpp",
        root / "runtime" / "stdlib_registry.cpp",
        root / "runtime" / "stdlib_bool.cpp",
        root / "runtime" / "stdlib_math.cpp",
        root / "runtime" / "stdlib_json.cpp",
        root / "runtime" / "stdlib_codecs.cpp",
        root / "runtime" / "stdlib_digest.cpp",
        root / "runtime" / "stdlib_secure_random.cpp",
        root / "runtime" / "stdlib_uuid.cpp",
        root / "runtime" / "stdlib_time.cpp",
    ]
    run_command(
        [
            cxx,
            "-std=c++17",
            "-O2",
            "-pipe",
            "-pthread",
            "-I",
            root,
            *sources,
            "-o",
            output,
        ],
        root,
    )
    return output


def measure(command, cwd: Path) -> dict:
    helper = r"""
import json
import resource
import subprocess
import sys
import time

command = sys.argv[1:]
start = time.perf_counter()
completed = subprocess.run(
    command,
    text=True,
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
)
elapsed = time.perf_counter() - start
rss = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
rss_mb = rss / (1024 * 1024) if sys.platform == "darwin" else rss / 1024
print(json.dumps({
    "returncode": completed.returncode,
    "stdout": completed.stdout,
    "stderr": completed.stderr,
    "elapsed_s": elapsed,
    "peak_rss_mb": rss_mb,
}))
"""
    completed = subprocess.run(
        [sys.executable, "-c", helper, *[str(part) for part in command]],
        cwd=str(cwd),
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if completed.returncode != 0:
        raise RuntimeError(
            "measurement helper failed\n"
            f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
        )
    return json.loads(completed.stdout)


def extract_checksum(name: str, stdout: str) -> str:
    lines = [line.strip() for line in stdout.splitlines() if line.strip()]
    if name == "amber-interpreted":
        for line in lines:
            if line.startswith("=> "):
                return line[3:].strip()
    if lines:
        return lines[-1]
    return ""


def aggregate(
    name: str, command, samples: list[dict], expected_checksum: str
) -> dict:
    good = [sample for sample in samples if sample["returncode"] == 0]
    if len(good) != len(samples):
        failed = next(sample for sample in samples if sample["returncode"] != 0)
        raise RuntimeError(
            f"{name} failed with exit code {failed['returncode']}\n"
            f"stdout:\n{failed['stdout']}\nstderr:\n{failed['stderr']}"
        )
    checksums = [extract_checksum(name, sample["stdout"]) for sample in good]
    mismatches = [
        checksum for checksum in checksums if checksum != expected_checksum
    ]
    if mismatches:
        raise RuntimeError(
            f"{name} produced unexpected checksum {mismatches[0]}, "
            f"expected {expected_checksum}"
        )
    elapsed = [sample["elapsed_s"] for sample in good]
    rss_values = [
        sample["peak_rss_mb"]
        for sample in good
        if sample["peak_rss_mb"] is not None
    ]
    timing = summary_stats(elapsed)
    rss = summary_stats(rss_values)
    return {
        "name": name,
        "command": [str(part) for part in command],
        "available": True,
        "runs": len(good),
        "checksum": checksums[0] if checksums else "",
        "elapsed_s": elapsed,
        "timing_s": timing,
        "rss_mb": rss,
        "mean_s": timing["mean"],
        "median_s": timing["median"],
        "stdev_s": timing["stdev"],
        "cv_percent": timing["cv_percent"],
        "best_s": timing["min"],
        "peak_rss_mb": rss["max"],
    }


def unavailable_result(name: str, reason: str) -> dict:
    return {
        "name": name,
        "command": [],
        "available": False,
        "error": reason,
        "runs": 0,
        "checksum": "",
        "elapsed_s": [],
        "timing_s": summary_stats([]),
        "rss_mb": summary_stats([]),
        "mean_s": None,
        "median_s": None,
        "stdev_s": None,
        "cv_percent": None,
        "best_s": None,
        "peak_rss_mb": None,
    }


def print_table(results: list[dict]) -> None:
    print(
        f"{'program':<19} {'runs':>4} {'median_s':>10} {'mean_s':>10} "
        f"{'stdev_s':>10} {'cv_%':>8} {'best_s':>10} "
        f"{'peak_rss_mb':>12} {'checksum':>18}"
    )
    print("-" * 124)
    for result in results:
        mean_s = (
            f"{result['mean_s']:.4f}"
            if result["mean_s"] is not None
            else "n/a"
        )
        best_s = (
            f"{result['best_s']:.4f}"
            if result["best_s"] is not None
            else "n/a"
        )
        median_s = (
            f"{result['median_s']:.4f}"
            if result["median_s"] is not None
            else "n/a"
        )
        stdev_s = (
            f"{result['stdev_s']:.4f}"
            if result["stdev_s"] is not None
            else "n/a"
        )
        cv = (
            f"{result['cv_percent']:.2f}"
            if result["cv_percent"] is not None
            else "n/a"
        )
        rss = (
            f"{result['peak_rss_mb']:.1f}"
            if result["peak_rss_mb"] is not None
            else "n/a"
        )
        checksum = result["checksum"] or result.get("error", "n/a")
        print(
            f"{result['name']:<19} {result['runs']:>4} "
            f"{median_s:>10} {mean_s:>10} {stdev_s:>10} "
            f"{cv:>8} {best_s:>10} "
            f"{rss:>12} {checksum:>18}"
        )


def micro_comparisons(
    samples_by_name: dict[str, list[dict]], program_names: list[str]
) -> list[dict]:
    comparisons = []
    for baseline in ("amber-interpreted", "amber-built"):
        baseline_samples = samples_by_name.get(baseline, [])
        if not baseline_samples:
            continue
        baseline_by_repeat = {
            sample["repeat_index"]: sample["elapsed_s"]
            for sample in baseline_samples
        }
        for name in program_names:
            if name == baseline or name not in samples_by_name:
                continue
            ratios = []
            for sample in samples_by_name[name]:
                baseline_elapsed = baseline_by_repeat.get(sample["repeat_index"])
                if baseline_elapsed is None or sample["elapsed_s"] <= 0.0:
                    continue
                ratios.append(baseline_elapsed / sample["elapsed_s"])
            comparisons.append(
                {
                    "baseline": baseline,
                    "program": name,
                    "paired_throughput_ratio": summary_stats(ratios),
                }
            )
    return comparisons


def version_line(command: list[object], root: Path) -> Optional[str]:
    try:
        completed = subprocess.run(
            [str(part) for part in command],
            cwd=str(root),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=10.0,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    if completed.returncode != 0:
        return None
    lines = [line.strip() for line in completed.stdout.splitlines() if line.strip()]
    return lines[0] if lines else None


def micro_provenance(
    root: Path,
    build_dir: Path,
    workload: Workload,
    programs: list[tuple[str, list[object]]],
    cxx: str,
    go: Optional[str],
    rust: Optional[str],
    ruby: str,
) -> dict:
    artifacts = {
        "amberc": file_provenance(root / "build/amberc"),
        "iamber": file_provenance(root / "build/iamber"),
    }
    for name, command in programs:
        for part in command:
            path = Path(str(part))
            if path.is_file():
                artifacts[f"{name}:{path.name}"] = file_provenance(path)
    sources = [
        root / "bench/polyglot/run_benchmark.py",
        root / "bench/polyglot/benchmark_support.py",
        root / "bench/polyglot/amber/amber.build.json",
        root / "bench/polyglot/amber/src" / workload.amber_source,
        root / "bench/polyglot/python" / workload.python_source,
        root / "bench/polyglot/ruby" / workload.ruby_source,
        root / "bench/polyglot/cpp" / workload.cpp_source,
        root / "bench/polyglot/go" / workload.go_source,
        root / "bench/polyglot/rust" / workload.rust_source,
    ]
    return {
        "host": host_provenance(),
        "repositories": {"amber": git_provenance(root)},
        "artifacts": artifacts,
        "source_tree": tree_provenance(sources, relative_to=root),
        "build_dir": str(build_dir.resolve()),
        "versions": {
            "amberc": version_line([root / "build/amberc", "--version"], root),
            "python": platform.python_version(),
            "ruby": version_line([ruby, "--version"], root),
            "cxx": version_line([cxx, "--version"], root),
            "go": version_line([go, "version"], root) if go else None,
            "rust": version_line([rust, "--version"], root) if rust else None,
        },
        "environment": relevant_environment(
            ["CXX", "CC", "RUSTFLAGS", "GOFLAGS", "AMBER_BENCH_RUBY"]
        ),
        "argv": sys.argv,
    }


def markdown_report(payload: dict) -> str:
    comparisons = {
        (row["baseline"], row["program"]): row
        for row in payload["paired_comparisons"]
    }
    lines = [
        f"# Polyglot microbenchmark: {payload['workload']}",
        "",
        f"Date: `{payload['timestamp']}`<br>",
        f"Host: `{payload['provenance']['host']['platform']}`<br>",
        f"Repeats: `{payload['repeats']}` measured, `{payload['warmups']}` warmup; balanced rotation seed `{payload['order_seed']}`.",
        "",
        "| Program | Median, s | Mean, s | Stdev | CV | Mean 95% CI | Peak RSS | vs Amber VM | vs Amber native |",
        "|---|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for row in payload["results"]:
        if not row["available"]:
            lines.append(
                f"| {row['name']} | n/a | n/a | n/a | n/a | n/a | n/a | n/a | n/a |"
            )
            continue
        timing = row["timing_s"]
        vm = comparisons.get(("amber-interpreted", row["name"]))
        native = comparisons.get(("amber-built", row["name"]))
        vm_text = (
            "1.000×"
            if row["name"] == "amber-interpreted"
            else (
                f"{vm['paired_throughput_ratio']['median']:.3f}×"
                if vm is not None
                else "n/a"
            )
        )
        native_text = (
            "1.000×"
            if row["name"] == "amber-built"
            else (
                f"{native['paired_throughput_ratio']['median']:.3f}×"
                if native is not None
                else "n/a"
            )
        )
        lines.append(
            "| {name} | {median:.6f} | {mean:.6f} | {stdev:.6f} | {cv:.2f}% | {low:.6f}…{high:.6f} | {rss:.1f} MiB | {vm} | {native} |".format(
                name=row["name"],
                median=timing["median"],
                mean=timing["mean"],
                stdev=timing["stdev"],
                cv=timing["cv_percent"],
                low=timing["ci95_mean_low"],
                high=timing["ci95_mean_high"],
                rss=row["peak_rss_mb"],
                vm=vm_text,
                native=native_text,
            )
        )
    lines += ["", "## Measured run order", ""]
    for index, order in enumerate(payload["run_orders"], start=1):
        lines.append(f"- repeat {index}: `{' → '.join(order)}`")
    lines += ["", "## Provenance", ""]
    provenance = payload["provenance"]
    repository = provenance["repositories"]["amber"]
    dirty = "dirty" if repository.get("tracked_dirty") else "clean"
    lines.append(
        f"- Amber: `{repository.get('commit')}` ({dirty}, `{repository['path']}`)"
    )
    for name, artifact in provenance["artifacts"].items():
        lines.append(
            f"- {name}: SHA-256 `{artifact['sha256']}`, {artifact['size_bytes']} bytes (`{artifact['path']}`)"
        )
    lines.append(
        f"- benchmark source tree: SHA-256 `{provenance['source_tree']['sha256']}` over {provenance['source_tree']['file_count']} files"
    )
    lines += [
        "",
        f"Machine-readable result: `{payload['json_result']}`.",
        "",
    ]
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run the Amber/Python/Ruby/C++/Go/Rust polyglot benchmark."
    )
    parser.add_argument("--repeats", type=int, default=5)
    parser.add_argument(
        "--warmups",
        type=int,
        default=1,
        help="unmeasured process runs per program before the measured series",
    )
    parser.add_argument(
        "--continue-on-program-error",
        action="store_true",
        help=(
            "record a program that fails warmup as unavailable and continue "
            "measuring the remaining implementations"
        ),
    )
    parser.add_argument(
        "--order-seed",
        type=int,
        default=0,
        help="deterministic seed for balanced program-order rotation",
    )
    parser.add_argument(
        "--workload",
        choices=sorted(WORKLOADS.keys()),
        default="arithmetic",
        help="benchmark program to run",
    )
    parser.add_argument(
        "--no-build",
        action="store_true",
        help="reuse existing generated binaries and Amber bytecode artifacts",
    )
    parser.add_argument(
        "--build-dir",
        type=Path,
        default=None,
        help="directory for generated binaries and Amber bytecode artifacts",
    )
    parser.add_argument(
        "--json-out",
        type=Path,
        default=None,
        help="write raw aggregate results to this JSON file",
    )
    parser.add_argument(
        "--markdown-out",
        type=Path,
        default=None,
        help="write a Markdown statistical report",
    )
    args = parser.parse_args()
    if args.repeats < 1:
        raise RuntimeError("--repeats must be at least 1")
    if args.warmups < 0:
        raise RuntimeError("--warmups must be non-negative")
    if args.continue_on_program_error and args.warmups < 1:
        raise RuntimeError(
            "--continue-on-program-error requires at least one warmup"
        )

    root = repo_root()
    workload = WORKLOADS[args.workload]
    build_dir = args.build_dir or (root / "bench" / "polyglot" / "build")
    if not build_dir.is_absolute():
        build_dir = root / build_dir
    build_dir.mkdir(parents=True, exist_ok=True)
    if workload.name == "json":
        prepare_json_fixture(root)

    cxx = choose_cxx()
    go = choose_go()
    rust = choose_rust()
    go_unavailable_reason = None
    rust_unavailable_reason = None
    if args.no_build:
        amber_built = amber_native_path(build_dir, workload)
        amber_built_command = [amber_built]
        if not amber_built.exists():
            raise RuntimeError(f"Amber native executable missing: {amber_built}")
        cpp_binary = build_dir / "cpp" / workload.cpp_binary
        go_binary = build_dir / "go" / workload.go_binary
        if not go_binary.exists():
            if go is None:
                go_unavailable_reason = "go not found in PATH"
            else:
                go_unavailable_reason = f"go binary missing: {go_binary}"
        rust_binary = build_dir / "rust" / workload.rust_binary
        if not rust_binary.exists():
            if rust is None:
                rust_unavailable_reason = "rustc not found in PATH"
            else:
                rust_unavailable_reason = f"rust binary missing: {rust_binary}"
    else:
        ensure_amber_tools(root)
        build_amber_bytecode(root, build_dir)
        amber_built = build_amber_native_executable(root, build_dir, workload)
        amber_built_command = [amber_built]
        cpp_binary = compile_cpp_program(root, build_dir, cxx, workload)
        if go is None:
            go_binary = None
            go_unavailable_reason = "go not found in PATH"
        else:
            go_binary = compile_go_program(root, build_dir, go, workload)
        if rust is None:
            rust_binary = None
            rust_unavailable_reason = "rustc not found in PATH"
        else:
            rust_binary = compile_rust_program(root, build_dir, rust, workload)

    ruby = choose_ruby()

    all_programs = [
        (
            "amber-interpreted",
            [
                root / "build" / "iamber",
                "--eval-file",
                root
                / "bench"
                / "polyglot"
                / "amber"
                / "src"
                / workload.amber_source,
            ],
        ),
        ("amber-built", amber_built_command),
        (
            "python",
            [
                sys.executable,
                root / "bench" / "polyglot" / "python" / workload.python_source,
            ],
        ),
        (
            "ruby",
            [
                ruby,
                "--disable=gems",
                root / "bench" / "polyglot" / "ruby" / workload.ruby_source,
            ],
        ),
        ("cpp", [cpp_binary]),
    ]
    if go_unavailable_reason is None:
        all_programs.append(("go", [go_binary]))
    if rust_unavailable_reason is None:
        all_programs.append(("rust", [rust_binary]))

    program_map = {name: command for name, command in all_programs}
    initial_program_names = [name for name, _command in all_programs]
    warmup_orders = (
        balanced_orders(initial_program_names, args.warmups, args.order_seed + 1)
        if args.warmups
        else []
    )
    program_errors = {}
    for warmup_index, order in enumerate(warmup_orders):
        print(
            f"warmup {warmup_index + 1}/{args.warmups}: "
            + " -> ".join(order),
            flush=True,
        )
        for name in order:
            if name in program_errors:
                continue
            try:
                sample = measure(program_map[name], root)
                aggregate(
                    name,
                    program_map[name],
                    [sample],
                    workload.expected_checksum,
                )
            except RuntimeError as error:
                if not args.continue_on_program_error:
                    raise
                program_errors[name] = str(error)
                print(f"marking {name} unavailable: {error}", flush=True)

    programs = [
        (name, command)
        for name, command in all_programs
        if name not in program_errors
    ]
    program_names = [name for name, _command in programs]

    run_orders = balanced_orders(program_names, args.repeats, args.order_seed)
    samples_by_name = {name: [] for name in program_names}
    for repeat_index, order in enumerate(run_orders):
        print(
            f"repeat {repeat_index + 1}/{args.repeats}: "
            + " -> ".join(order),
            flush=True,
        )
        for order_position, name in enumerate(order):
            sample = measure(program_map[name], root)
            sample["repeat_index"] = repeat_index + 1
            sample["order_position"] = order_position + 1
            samples_by_name[name].append(sample)

    results_by_name = {
        name: aggregate(
            name,
            command,
            samples_by_name[name],
            workload.expected_checksum,
        )
        for name, command in programs
    }
    if go_unavailable_reason is not None:
        results_by_name["go"] = unavailable_result(
            "go", go_unavailable_reason
        )
    if rust_unavailable_reason is not None:
        results_by_name["rust"] = unavailable_result(
            "rust", rust_unavailable_reason
        )
    for name in initial_program_names:
        if name in program_errors:
            results_by_name[name] = unavailable_result(
                name, program_errors[name]
            )
    results = [
        results_by_name[name]
        for name in (
            "amber-interpreted", "amber-built", "python", "ruby",
            "cpp", "go", "rust",
        )
        if name in results_by_name
    ]

    comparisons = micro_comparisons(samples_by_name, program_names)
    provenance = micro_provenance(
        root, build_dir, workload, all_programs, cxx, go, rust, ruby
    )
    stamp = dt.datetime.now().astimezone().strftime("%Y-%m-%d-%H%M%S-%z")
    results_dir = root / "bench/polyglot/results"
    results_dir.mkdir(parents=True, exist_ok=True)
    default_stem = f"micro-{workload.name}-r{args.repeats}-{stamp}"
    json_out = (args.json_out or (results_dir / f"{default_stem}.json")).resolve()
    markdown_out = (
        args.markdown_out or (results_dir / f"{default_stem}.md")
    ).resolve()
    try:
        json_result = str(json_out.relative_to(root))
    except ValueError:
        json_result = str(json_out)
    payload = {
        "schema": "amber.polyglot.micro.v2",
        "timestamp": dt.datetime.now().astimezone().isoformat(),
        "workload": workload.name,
        "expected_checksum": workload.expected_checksum,
        "repeats": args.repeats,
        "warmups": args.warmups,
        "order_seed": args.order_seed,
        "warmup_orders": warmup_orders,
        "run_orders": run_orders,
        "program_errors": program_errors,
        "provenance": provenance,
        "samples": samples_by_name,
        "results": results,
        "paired_comparisons": comparisons,
        "json_result": json_result,
    }
    print_table(results)
    json_out.parent.mkdir(parents=True, exist_ok=True)
    json_out.write_text(json.dumps(payload, indent=2), encoding="utf-8")
    markdown_out.parent.mkdir(parents=True, exist_ok=True)
    markdown_out.write_text(markdown_report(payload), encoding="utf-8")
    print(f"\nWrote JSON results to {json_out}")
    print(f"Wrote Markdown report to {markdown_out}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"benchmark error: {error}", file=sys.stderr)
        raise SystemExit(1)
