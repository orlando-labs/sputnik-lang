#!/usr/bin/env python3
import argparse
from dataclasses import dataclass, replace
import datetime as dt
import json
import os
import platform
import shutil
import shlex
import subprocess
import sys
from pathlib import Path
from typing import Optional
from body_timing import prepare as prepare_body_sources

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
    sputnik_module: str
    sputnik_entry: str
    sputnik_source: str
    python_source: str
    ruby_source: str
    cpp_source: str
    cpp_binary: str
    go_source: str
    go_binary: str
    rust_source: str
    rust_binary: str
    requires_full_sputnik_native: bool = True
    sputnik_grants: tuple[str, ...] = ()


JSON_RECORDS = 20000
JSON_FIXTURE_RELATIVE = Path("bench/polyglot/build/json/events.jsonl")
TOOLS = {}
SOURCE_OVERRIDES = {}
BUILD_RECORDS = []


WORKLOADS = {
    "arithmetic": Workload(
        name="arithmetic",
        expected_checksum="715609516598740",
        sputnik_module="bench.polyglot",
        sputnik_entry="main",
        sputnik_source="main.s",
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
        sputnik_module="bench.polyglot.calls_collections",
        sputnik_entry="__init__",
        sputnik_source="calls_collections.s",
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
        expected_checksum="5512000",
        sputnik_module="bench.polyglot.sha_digest",
        sputnik_entry="__init__",
        sputnik_source="sha_digest.s",
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
        sputnik_module="bench.polyglot.json",
        sputnik_entry="main",
        sputnik_source="json.s",
        python_source="json_workload.py",
        ruby_source="json_workload.rb",
        cpp_source="json_workload.cpp",
        cpp_binary="json_workload",
        go_source="json_workload.go",
        go_binary="json_workload",
        rust_source="json_workload.rs",
        rust_binary="json_workload",
        sputnik_grants=("fs.read=bench/polyglot/build/json/events.jsonl",),
    ),
    "string-ops": Workload(
        name="string-ops",
        expected_checksum="280113",
        sputnik_module="bench.polyglot.string_ops",
        sputnik_entry="main",
        sputnik_source="string_ops.s",
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
        sputnik_module="bench.polyglot.map_words",
        sputnik_entry="main",
        sputnik_source="map_words.s",
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
        sputnik_module="bench.polyglot.codecs",
        sputnik_entry="main",
        sputnik_source="codecs.s",
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
        sputnik_module="bench.polyglot.secure_random",
        sputnik_entry="main",
        sputnik_source="secure_random.s",
        python_source="secure_random.py",
        ruby_source="secure_random.rb",
        cpp_source="secure_random.cpp",
        cpp_binary="secure_random",
        go_source="secure_random.go",
        go_binary="secure_random",
        rust_source="secure_random.rs",
        rust_binary="secure_random",
        sputnik_grants=("random.secure",),
    ),
    "time-flow": Workload(
        name="time-flow",
        expected_checksum="110397732",
        sputnik_module="bench.polyglot.time_flow",
        sputnik_entry="main",
        sputnik_source="time_flow.s",
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
        sputnik_module="bench.polyglot.uuid",
        sputnik_entry="main",
        sputnik_source="uuid.s",
        python_source="uuid_workload.py",
        ruby_source="uuid_workload.rb",
        cpp_source="uuid_workload.cpp",
        cpp_binary="uuid_workload",
        go_source="uuid_workload.go",
        go_binary="uuid_workload",
        rust_source="uuid_workload.rs",
        rust_binary="uuid_workload",
        sputnik_grants=("random.secure",),
    ),
}

WORKLOADS["json-full-api"] = replace(
    WORKLOADS["json"], name="json-full-api", sputnik_module="bench.polyglot.json_full_api",
    sputnik_source="json_full_api.s", sputnik_entry="__init__", python_source="json_full_api.py",
    ruby_source="json_full_api.rb", cpp_source="json_full_api.cpp", cpp_binary="json_full_api",
    go_source="json_full_api.go", go_binary="json_full_api",
    rust_source="json_full_api.rs", rust_binary="json_full_api",
)


def tool(name: str, root: Path):
    return TOOLS.get(name, root / "build" / name)


def source_path(root: Path, workload: Workload, language: str) -> Path:
    if language in SOURCE_OVERRIDES:
        return SOURCE_OVERRIDES[language]
    base = root / "bench/polyglot" / language
    if language == "sputnik": base /= "src"
    return base / getattr(workload, f"{language}_source")


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
    BUILD_RECORDS.append({"command": list(map(str, command)), "cwd": str(cwd),
                          "environment": {k: str(v) for k, v in (env or {}).items()}, "returncode": completed.returncode,
                          "stdout": completed.stdout, "stderr": completed.stderr})
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
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return str(candidate.resolve())
    raise RuntimeError(
        "ruby was not found; set SPUTNIK_BENCH_RUBY to a Ruby executable"
    )


def ensure_sputnik_tools(root: Path) -> None:
    # Let make verify source timestamps even when both tools already exist.
    # A merely present isputnik may predate the VM changes being benchmarked.
    if "sputnik" not in TOOLS and "isputnik" not in TOOLS:
        run_command(["make", "build/sputnik", "build/isputnik"], root, capture=False)


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


def sputnik_artifact_path(build_dir: Path, workload: Workload) -> Path:
    return build_dir / "sputnik" / "out" / f"{workload.sputnik_module}.sputnikbc"


def safe_artifact_name(value: str) -> str:
    return "".join(
        ch if ch.isalnum() or ch in "._-" else "_"
        for ch in value
    ) or "artifact"


def sputnik_native_path(build_dir: Path, workload: Workload) -> Path:
    return build_dir / "sputnik" / "native" / safe_artifact_name(workload.sputnik_module)


def build_sputnik_bytecode(root: Path, build_dir: Path) -> None:
    out_dir = build_dir / "sputnik" / "out"
    cache_dir = build_dir / "sputnik" / "cache"
    out_dir.mkdir(parents=True, exist_ok=True)
    cache_dir.mkdir(parents=True, exist_ok=True)
    run_command(
        [
            tool("sputnik", root),
            "build",
            root / "bench" / "polyglot" / "sputnik" / "sputnik.build.json",
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
        artifact = sputnik_artifact_path(build_dir, workload)
        if not artifact.exists():
            raise RuntimeError(
                f"expected Sputnik bytecode artifact is missing: {artifact}"
            )


def build_sputnik_native_executable(
    root: Path, build_dir: Path, workload: Workload
) -> Path:
    output = sputnik_native_path(build_dir, workload)
    output.parent.mkdir(parents=True, exist_ok=True)
    source = source_path(root, workload, "sputnik")
    entry = "init" if workload.sputnik_entry == "__init__" or "sputnik" in SOURCE_OVERRIDES else "main-only"
    command = [
        tool("sputnik", root),
        "build",
        source,
        "-o",
        output,
        "--entry",
        entry,
    ]
    for grant in workload.sputnik_grants:
        command.extend(["--grant", grant])
    completed = run_command(command, root)
    build_result = json.loads(completed.stdout)
    if build_result.get("status") != "ok":
        raise RuntimeError(f"Sputnik native build failed: {completed.stdout}")
    if not workload.requires_full_sputnik_native:
        if not output.exists():
            raise RuntimeError(f"expected Sputnik built executable is missing: {output}")
        return output
    if build_result.get("native_backend") != "cpp-bytecode-direct-v1":
        raise RuntimeError(
            "Sputnik built benchmark expected native backend "
            f"cpp-bytecode-direct-v1, got {build_result.get('native_backend')!r}"
        )
    if build_result.get("native_entry") is not True:
        raise RuntimeError(
            "Sputnik built benchmark entry is not native: "
            f"{build_result.get('native_fallback_reason')}"
        )
    native_code_count = build_result.get("native_code_count")
    bytecode_code_count = build_result.get("bytecode_code_count")
    if native_code_count != bytecode_code_count:
        raise RuntimeError(
            "Sputnik built benchmark requires full native coverage, got "
            f"{native_code_count}/{bytecode_code_count} code objects: "
            f"{build_result.get('native_fallback_reason')}"
        )
    if not output.exists():
        raise RuntimeError(f"expected Sputnik native executable is missing: {output}")
    return output


def compile_cpp_program(
    root: Path, build_dir: Path, cxx: str, workload: Workload, *, reference=None
) -> Path:
    output = build_dir / "cpp" / workload.cpp_binary
    if reference: output = output.with_name(output.name + "-" + reference)
    output.parent.mkdir(parents=True, exist_ok=True)
    sources = [source_path(root, workload, "cpp")]
    extra_args = []
    if reference == "checked": extra_args.append("-DSPUTNIK_BENCH_CHECKED_INTS")
    if reference == "generic": extra_args.append("-DSPUTNIK_BENCH_GENERIC_ARRAYS")
    if workload.name == "sha-digest":
        sources.append(root / "runtime" / "digest.cpp")
        extra_args.extend(["-I", root])
    json_libs = []
    if workload.name == "json-full-api":
        extra_args += shlex.split(run_command(["pkg-config", "--cflags", "json-c"], root).stdout)
        json_libs = shlex.split(run_command(["pkg-config", "--libs", "json-c"], root).stdout)
    run_command(
        [
            cxx,
            "-std=c++17",
            "-O2",
            "-pipe",
            *extra_args,
            *sources,
            *json_libs,
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
            source_path(root, workload, "go"),
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
    if workload.name == "json-full-api":
        crate = build_dir / "rust-crate"
        crate.mkdir(parents=True, exist_ok=True)
        original = root / "bench/polyglot/rust/json-full-api"
        manifest = (original / "Cargo.toml").read_text().replace(
            'path = "../json_full_api.rs"', 'path = ' + json.dumps(str(source_path(root, workload, "rust"))))
        (crate / "Cargo.toml").write_text(manifest)
        shutil.copyfile(original / "Cargo.lock", crate / "Cargo.lock")
        cargo = TOOLS.get("cargo") or shutil.which("cargo")
        if not cargo: raise RuntimeError("json-full-api requires cargo and locked serde_json dependencies")
        run_command([cargo, "build", "--release", "--locked", "--offline", "--manifest-path", crate / "Cargo.toml",
                     "--target-dir", build_dir / "rust-target"], root,
                    env={"RUSTC": rustc})
        shutil.copy2(build_dir / "rust-target/release/json_full_api", output)
        return output
    run_command(
        [
            rustc,
            "-O",
            "--edition",
            "2021",
            "-o",
            output,
            source_path(root, workload, "rust"),
        ],
        root,
    )
    return output


def compile_sputnikbc_runner(root: Path, build_dir: Path, cxx: str) -> Path:
    output = build_dir / "sputnikbc_run"
    output.parent.mkdir(parents=True, exist_ok=True)
    sources = [
        root / "bench" / "polyglot" / "tools" / "sputnikbc_run.cpp",
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
    if name == "sputnik-interpreted":
        for line in lines:
            if line.startswith("=> "):
                return line[3:].strip()
    if lines:
        return lines[-1]
    return ""


def extract_record(stdout: str, marker: str) -> dict:
    records = [json.loads(line[len(marker):]) for line in stdout.splitlines() if line.startswith(marker)]
    if len(records) != 1:
        raise RuntimeError(f"expected exactly one {marker.strip()} record, got {len(records)}")
    return records[0]


def validate_sample(sample: dict, workload: Workload, timing: str, iterations: int) -> None:
    if sample["returncode"] != 0:
        raise RuntimeError(f"program failed: {sample['stdout']}\n{sample['stderr']}")
    if workload.name == "json-full-api":
        record = extract_record(sample["stdout"], "BENCH_WORK ")
        expected = {"checksum": int(workload.expected_checksum), "documents": 1000, "pretty": 20, "rows": JSON_RECORDS}
        if record != expected:
            raise RuntimeError(f"full JSON operation/shape admission failed: {record!r}")
        sample["work"] = record
    if timing == "body":
        record = extract_record(sample["stdout"], "BENCH_BODY ")
        if record.get("iterations") != iterations or record.get("warmups") != 1 or record.get("elapsed_ns", 0) <= 0:
            raise RuntimeError(f"invalid body timing record: {record!r}")
        sample["body_s"] = record["elapsed_ns"] / 1e9 / iterations
        sample["body_record"] = record
        sample["outside_body_s"] = sample["elapsed_s"] - record["elapsed_ns"] / 1e9


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
    result = {
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
    if good and "body_s" in good[0]:
        result["body_timing_s"] = summary_stats([sample["body_s"] for sample in good])
        result["outside_body_s"] = summary_stats([sample["outside_body_s"] for sample in good])
    return result


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
    for baseline in ("sputnik-interpreted", "sputnik-built"):
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
            if baseline_samples and "body_s" in baseline_samples[0]:
                body_baseline = {s["repeat_index"]: s["body_s"] for s in baseline_samples}
                comparisons[-1]["paired_body_ratio"] = summary_stats(
                    [body_baseline[s["repeat_index"]] / s["body_s"] for s in samples_by_name[name]])
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
        "sputnik": file_provenance(Path(tool("sputnik", root))),
        "isputnik": file_provenance(Path(tool("isputnik", root))),
    }
    for name, command in programs:
        for part in command:
            path = Path(str(part))
            if path.is_file():
                artifacts[f"{name}:{path.name}"] = file_provenance(path)
    sources = [
        root / "bench/polyglot/run_benchmark.py",
        root / "bench/polyglot/benchmark_support.py",
        root / "bench/polyglot/body_timing.py",
        root / "bench/polyglot/sputnik/sputnik.build.json",
        root / "bench/polyglot/sputnik/src" / workload.sputnik_source,
        root / "bench/polyglot/python" / workload.python_source,
        root / "bench/polyglot/ruby" / workload.ruby_source,
        root / "bench/polyglot/cpp" / workload.cpp_source,
        root / "bench/polyglot/go" / workload.go_source,
        root / "bench/polyglot/rust" / workload.rust_source,
    ]
    sources += list(SOURCE_OVERRIDES.values())
    for name, executable in (("cxx", cxx), ("go", go), ("rustc", rust), ("ruby", ruby),
                             ("python", TOOLS.get("python", sys.executable)), ("cargo", TOOLS.get("cargo"))):
        if executable: artifacts[f"tool:{name}"] = file_provenance(Path(shutil.which(str(executable)) or executable))
    if workload.name == "json-full-api":
        sources += [root / "bench/polyglot/rust/json-full-api/Cargo.toml", root / "bench/polyglot/rust/json-full-api/Cargo.lock",
                    root / JSON_FIXTURE_RELATIVE]
        libdir = run_command(["pkg-config", "--variable=libdir", "json-c"], root).stdout.strip()
        library = Path(libdir) / ("libjson-c.dylib" if sys.platform == "darwin" else "libjson-c.so")
        artifacts["json-c"] = file_provenance(library)
    return {
        "host": host_provenance(),
        "repositories": {"sputnik": git_provenance(root)},
        "artifacts": artifacts,
        "source_tree": tree_provenance(sources, relative_to=root),
        "compiler_runtime_tree": tree_provenance([root / name for name in
            ("runtime", "frontend", "bytecode", "optimizer", "profile", "package", "tools/sputnik", "tools/isputnik", "mk", "Makefile")], relative_to=root),
        "build_commands": BUILD_RECORDS,
        "build_dir": str(build_dir.resolve()),
        "versions": {
            "sputnik": version_line([tool("sputnik", root), "--version"], root),
            "python": version_line([TOOLS.get("python", sys.executable), "--version"], root),
            "ruby": version_line([ruby, "--version"], root),
            "cxx": version_line([cxx, "--version"], root),
            "go": version_line([go, "version"], root) if go else None,
            "rust": version_line([rust, "--version"], root) if rust else None,
            "json-c": version_line(["pkg-config", "--modversion", "json-c"], root) if workload.name == "json-full-api" else None,
        },
        "environment": relevant_environment(
            ["CXX", "CC", "RUSTFLAGS", "GOFLAGS", "SPUTNIK_BENCH_RUBY", "SPUTNIK_NATIVE_RT_CACHE", "CARGO_HOME"]
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
        "| Program | Median, s | Mean, s | Stdev | CV | Mean 95% CI | Peak RSS | vs Sputnik VM | vs Sputnik native |",
        "|---|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for row in payload["results"]:
        if not row["available"]:
            lines.append(
                f"| {row['name']} | n/a | n/a | n/a | n/a | n/a | n/a | n/a | n/a |"
            )
            continue
        timing = row["timing_s"]
        vm = comparisons.get(("sputnik-interpreted", row["name"]))
        native = comparisons.get(("sputnik-built", row["name"]))
        vm_text = (
            "1.000×"
            if row["name"] == "sputnik-interpreted"
            else (
                f"{vm['paired_throughput_ratio']['median']:.3f}×"
                if vm is not None
                else "n/a"
            )
        )
        native_text = (
            "1.000×"
            if row["name"] == "sputnik-built"
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
    if payload.get("timing_mode") == "body":
        lines += ["", "## In-process body (per invocation)", "",
                  "One in-process warmup is outside the timed batch. Process wall includes this warmup, imports/init, printing and teardown; outside-body time is not startup alone.", "",
                  "| Program | Body median, ms | CV | Process median, ms | Outside timed batch, ms |",
                  "|---|---:|---:|---:|---:|"]
        for row in payload["results"]:
            if row["available"]:
                body = row["body_timing_s"]
                lines.append(f"| {row['name']} | {body['median']*1000:.4f} | {body['cv_percent']:.2f}% | {row['median_s']*1000:.4f} | {row['outside_body_s']['median']*1000:.4f} |")
    for index, order in enumerate(payload["run_orders"], start=1):
        lines.append(f"- repeat {index}: `{' → '.join(order)}`")
    lines += ["", "## Provenance", ""]
    provenance = payload["provenance"]
    repository = provenance["repositories"]["sputnik"]
    dirty = "dirty" if repository.get("tracked_dirty") else "clean"
    lines.append(
        f"- Sputnik: `{repository.get('commit')}` ({dirty}, `{repository['path']}`)"
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
        description="Run the Sputnik/Python/Ruby/C++/Go/Rust polyglot benchmark."
    )
    parser.add_argument("--repeats", type=int, default=5)
    for name in ("sputnik", "isputnik", "python", "ruby", "cxx", "go", "rustc", "cargo"):
        parser.add_argument("--" + name, help="explicit executable path (recorded and hashed)")
    parser.add_argument("--timing", choices=("process", "body"), default="process")
    parser.add_argument("--body-iterations", type=int, default=5)
    parser.add_argument("--prepare-only", action="store_true", help="build and save admission/provenance without timing")
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
        help="reuse existing generated binaries and Sputnik bytecode artifacts",
    )
    parser.add_argument(
        "--build-dir",
        type=Path,
        default=None,
        help="directory for generated binaries and Sputnik bytecode artifacts",
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
    for name in ("sputnik", "isputnik", "python", "ruby", "cxx", "go", "rustc", "cargo"):
        value = getattr(args, name)
        if value:
            path = Path(shutil.which(value) or value).expanduser().resolve()
            if not path.is_file() or not os.access(path, os.X_OK): raise RuntimeError(f"invalid --{name}: {value}")
            TOOLS[name] = str(path)
    if args.body_iterations < 1: raise RuntimeError("--body-iterations must be positive")
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
    if workload.name in ("json", "json-full-api"):
        prepare_json_fixture(root)
    if args.timing == "body":
        SOURCE_OVERRIDES.update(prepare_body_sources(root, build_dir, workload, args.body_iterations))

    cxx = TOOLS.get("cxx") or choose_cxx()
    go = TOOLS.get("go") or choose_go()
    rust = TOOLS.get("rustc") or choose_rust()
    go_unavailable_reason = None
    rust_unavailable_reason = None
    if args.no_build:
        sputnik_built = sputnik_native_path(build_dir, workload)
        sputnik_built_command = [sputnik_built]
        if not sputnik_built.exists():
            raise RuntimeError(f"Sputnik native executable missing: {sputnik_built}")
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
        ensure_sputnik_tools(root)
        build_sputnik_bytecode(root, build_dir)
        sputnik_built = build_sputnik_native_executable(root, build_dir, workload)
        sputnik_built_command = [sputnik_built]
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

    ruby = TOOLS.get("ruby") or choose_ruby()

    all_programs = [
        (
            "sputnik-interpreted",
            [
                tool("isputnik", root),
                "--eval-file",
                source_path(root, workload, "sputnik"),
            ],
        ),
        ("sputnik-built", sputnik_built_command),
        (
            "python",
            [
                TOOLS.get("python", sys.executable),
                source_path(root, workload, "python"),
            ],
        ),
        (
            "ruby",
            [
                ruby,
                "--disable=gems",
                source_path(root, workload, "ruby"),
            ],
        ),
        ("cpp", [cpp_binary]),
    ]
    if go_unavailable_reason is None:
        all_programs.append(("go", [go_binary]))
    if rust_unavailable_reason is None:
        all_programs.append(("rust", [rust_binary]))
    reference = {"arithmetic": "checked", "calls-collections": "generic"}.get(workload.name)
    if reference:
        reference_binary = build_dir / "cpp" / (workload.cpp_binary + "-" + reference)
        if not args.no_build: compile_cpp_program(root, build_dir, cxx, workload, reference=reference)
        if not reference_binary.is_file(): raise RuntimeError(f"missing C++ control: {reference_binary}")
        all_programs.append(("cpp-" + reference, [reference_binary]))

    provenance_before = micro_provenance(root, build_dir, workload, all_programs, cxx, go, rust, ruby)
    admission_path = build_dir / f"{workload.name}-{args.timing}-admission.json"
    identity = {"timing_mode": args.timing, "body_iterations": args.body_iterations,
                "source_tree": provenance_before["source_tree"], "artifacts": provenance_before["artifacts"]}
    if args.no_build:
        saved = json.loads(admission_path.read_text())
        for key in ("timing_mode", "body_iterations"):
            if saved[key] != identity[key]: raise RuntimeError(f"saved build {key} mismatch")
        if saved["source_tree"]["sha256"] != identity["source_tree"]["sha256"]:
            raise RuntimeError("benchmark sources changed since build; rebuild required")
        for key, artifact in identity["artifacts"].items():
            if saved["artifacts"].get(key, {}).get("sha256") != artifact["sha256"]:
                raise RuntimeError(f"saved executable/dependency changed: {key}")
    else:
        identity["build_commands"] = BUILD_RECORDS.copy()
        admission_path.write_text(json.dumps(identity, indent=2))
    if args.prepare_only:
        print(f"Prepared {workload.name}/{args.timing}: {admission_path}")
        return 0

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
                validate_sample(sample, workload, args.timing, args.body_iterations)
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
            validate_sample(sample, workload, args.timing, args.body_iterations)
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
            "sputnik-interpreted", "sputnik-built", "python", "ruby",
            "cpp", "go", "rust",
            "cpp-checked", "cpp-generic",
        )
        if name in results_by_name
    ]

    comparisons = micro_comparisons(samples_by_name, program_names)
    provenance = micro_provenance(
        root, build_dir, workload, all_programs, cxx, go, rust, ruby
    )
    for key, artifact in provenance_before["artifacts"].items():
        if provenance["artifacts"][key]["sha256"] != artifact["sha256"]:
            raise RuntimeError(f"executable/dependency changed during measurement: {key}")
    if provenance["source_tree"]["sha256"] != provenance_before["source_tree"]["sha256"]:
        raise RuntimeError("benchmark sources changed during measurement")
    if provenance["compiler_runtime_tree"]["sha256"] != provenance_before["compiler_runtime_tree"]["sha256"]:
        raise RuntimeError("compiler/runtime source tree changed during measurement")
    provenance["build_commands"] = json.loads(admission_path.read_text()).get("build_commands", [])
    provenance["identity_verified_before_after"] = True
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
        "schema": "sputnik.polyglot.micro.v3",
        "timestamp": dt.datetime.now().astimezone().isoformat(),
        "workload": workload.name,
        "timing_mode": args.timing,
        "body_iterations": args.body_iterations if args.timing == "body" else None,
        "work_contract": "generic-json-dom-v1" if workload.name == "json-full-api" else "legacy-checksum-v1",
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
