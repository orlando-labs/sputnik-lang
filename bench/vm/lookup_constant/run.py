#!/usr/bin/env python3
"""Optional sequential before/after VM benchmark; no package dependency."""
import argparse
import hashlib
import json
from pathlib import Path
import statistics
import subprocess

ROOT = Path(__file__).resolve().parent
CASES = {"local_function", "qualified_function", "amber_class", "builtin_type",
         "error_class", "task_module"}
PREFIX = "LOOKUP_ATOM "


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def run(binary, iterations, repeats, log):
    command = [str(binary), "run", str(ROOT / "manifest.json"), "--",
               "--iterations", str(iterations), "--repeats", str(repeats)]
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True,
                            timeout=300)
    log.write_text(result.stdout + result.stderr)
    result.check_returncode()
    rows = [json.loads(line[len(PREFIX):]) for line in result.stdout.splitlines()
            if line.startswith(PREFIX)]
    if len(rows) != len(CASES) or {row["name"] for row in rows} != CASES:
        raise ValueError("Missing or duplicate lookup cases")
    for row in rows:
        if (row["iterations"] != iterations or row["valid"] is not True or
                len(row["nanoseconds"]) != repeats or
                any(t <= 0 for t in row["nanoseconds"])):
            raise ValueError("Invalid lookup result")
    return dict(command=command, rows=rows)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--before", type=Path, required=True)
    parser.add_argument("--after", type=Path, default=ROOT.parents[2] / "build/amberc")
    parser.add_argument("--iterations", type=int, default=300000)
    parser.add_argument("--repeats", type=int, default=5)
    parser.add_argument("--processes", type=int, default=5)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if min(args.iterations, args.repeats, args.processes) <= 0:
        parser.error("counts must be positive")
    binaries = {"before": args.before.resolve(), "after": args.after.resolve()}
    inputs = [ROOT / name for name in ("main.am", "provider.am", "manifest.json", "run.py")]
    hashes = {str(path): sha256(path) for path in [*inputs, *binaries.values()]}
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    envelope = dict(config={key: str(value) if isinstance(value, Path) else value
                            for key, value in vars(args).items()}, sha256=hashes, samples=[])
    for lane, binary in binaries.items():
        run(binary, 2, 1, output / f"prepare-{lane}.log")
    # Verify that this benchmark really executes the instruction under study.
    disassembly = subprocess.run(
        [str(binaries["after"]), "amberbc-disasm",
         str(ROOT / ".amber/vm/out/bench.vm.lookup_constant.provider.amberbc")],
        capture_output=True, text=True, check=True).stdout
    if "LOOKUP_CONST" not in disassembly or "bench.vm.lookup_constant.provider" not in disassembly:
        raise ValueError("Benchmark does not contain LOOKUP_CONST")
    (output / "bytecode.txt").write_text(disassembly)
    for repeat in range(args.processes):
        lanes = ("before", "after") if repeat % 2 == 0 else ("after", "before")
        for lane in lanes:
            print(f"process {repeat}: {lane}", flush=True)
            sample = run(binaries[lane], args.iterations, args.repeats,
                         output / f"{repeat}-{lane}.log")
            envelope["samples"].append(dict(lane=lane, repeat=repeat, **sample))
    if hashes != {path: sha256(path) for path in hashes}:
        raise ValueError("Benchmark inputs changed during measurement")
    summary = {}
    for lane in binaries:
        summary[lane] = {name: [statistics.median(row["nanoseconds"]) / args.iterations
                               for sample in envelope["samples"] if sample["lane"] == lane
                               for row in sample["rows"] if row["name"] == name]
                         for name in sorted(CASES)}
    envelope["process_medians_ns"] = summary
    lines = ["# LOOKUP_CONST before/after", "",
             "ns/op, including the same while loop; median of process medians. Block/method entry is outside the loop.", "",
             "| Case | Before | After | Speedup |", "| --- | ---: | ---: | ---: |"]
    for name in sorted(CASES):
        before = statistics.median(summary["before"][name])
        after = statistics.median(summary["after"][name])
        lines.append(f"| {name} | {before:.2f} | {after:.2f} | {before / after:.3f}x |")
    envelope["qualified_minus_local_ns"] = {
        lane: [a - b for a, b in zip(rows["qualified_function"], rows["local_function"])]
        for lane, rows in summary.items()}
    lines += ["", "Qualified − local function, paired within each process (not a pure instruction timer):"]
    for lane, values in envelope["qualified_minus_local_ns"].items():
        lines.append(f"- {lane}: {statistics.median(values):.2f} ns/op")
    (output / "results.json").write_text(json.dumps(envelope, indent=2) + "\n")
    rendered = "\n".join(lines) + "\n"
    (output / "report.md").write_text(rendered)
    print(rendered)


if __name__ == "__main__":
    main()
