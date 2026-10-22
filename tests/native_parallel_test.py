#!/usr/bin/env python3
"""Compare serial/partitioned native semantics and exercise automatic selection.

Linux, Clang 19+ and matching LLVM/LLD tools are required. This is an opt-in
toolchain test; it deliberately verifies real compilation, not mocked commands.
"""
import json
import os
from pathlib import Path
import subprocess
import sys


root = Path(__file__).resolve().parents[1]
compiler = Path(sys.argv[1] if len(sys.argv) > 1 else root / "build/sputnik").resolve()
work = Path(sys.argv[2] if len(sys.argv) > 2 else root / "build/native-parallel-test").resolve()
work.mkdir(parents=True, exist_ok=True)


def build(source, name, jobs):
    environment = dict(os.environ, SPUTNIK_NATIVE_JOBS=str(jobs))
    output = work / name
    command = [str(compiler), "build", str(source), "--target", "native",
               "--require-full-native", "--out-dir", str(output), "--grant", "ffi"]
    result = subprocess.run(command, cwd=root, env=environment, capture_output=True,
                            text=True, timeout=600)
    (work / (name + ".json")).write_text(result.stdout)
    (work / (name + ".log")).write_text(result.stderr)
    assert result.returncode == 0, result.stderr[-6000:] + result.stdout[-3000:]
    metadata = json.loads(result.stdout)
    executable = metadata.get("native_output", metadata.get("output"))
    report = json.loads(Path(metadata["native_source"] + ".compile.json").read_text())
    assert report["optimization"] == "O2", report
    actual = subprocess.run([executable], cwd=root, capture_output=True, timeout=120)
    assert actual.returncode == 0, actual.stderr.decode(errors="replace")
    return actual.stdout, report


fixture = root / "tests/fixtures/native_call_buffers/sputnik.build.json"
serial, serial_report = build(fixture, "serial", 1)
parallel, parallel_report = build(fixture, "parallel space,comma", 2)
assert b"PASS native call buffers" in serial
assert serial == parallel, (serial, parallel)
assert serial_report["mode"] == "serial", serial_report
assert parallel_report["mode"] == "thinlto" and parallel_report["partitions"] == 4, parallel_report

# Hundreds of emitted functions reproduce the large-graph selection without
# checking a wall-clock threshold dependent on the machine running the test.
source = work / "large.s"
functions = "\n".join(f"def f{i}(value): value + {i}\n" for i in range(600))
source.write_text("package native.parallel_large\nexport main\n" + functions
                  + '\ndef main():\n  total = f599(1)\n'
                    '  raise ValueError("parallel checksum") unless total == 600\n'
                    '  print "define available_externally linkonce_odr payload"\n'
                    '  print "PASS parallel large graph"\n  0\n')
large, large_report = build(source, "large", 0)
large_metadata = json.loads((work / "large.json").read_text())
assert large_metadata["native_code_count"] >= 512, large_metadata
assert b"PASS parallel large graph" in large
assert b"define available_externally linkonce_odr payload\n" in large
assert large_report["mode"] == "thinlto" and large_report["jobs"] > 1, large_report
print(json.dumps({"passed": True, "serial": serial_report,
                  "parallel": parallel_report, "large": large_report}, indent=2))
