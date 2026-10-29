#!/usr/bin/env python3
"""Run the shared membership conformance cases in VM and full native mode."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
compiler = Path(sys.argv[1] if len(sys.argv) > 1 else root / "build/sputnik").resolve()
env = dict(os.environ)
env.setdefault("SPUTNIK_NATIVE_RT_CACHE", str(root / "build/interop-runtime-cache"))


def run(*args):
    result = subprocess.run(list(map(str, args)), capture_output=True, text=True,
                            env=env, timeout=600)
    assert result.returncode == 0, (args, result.stdout, result.stderr)
    return result.stdout


with tempfile.TemporaryDirectory(prefix="sputnik-membership-") as temporary:
    directory = Path(temporary)
    source = directory / "membership.s"
    source.write_text((root / "corpus/run/membership/source.s").read_text()
                      + '\nmain()\nprint "PASS membership"\n0\n')
    vm = run(compiler, "run", source)
    binary = directory / "native-membership"
    report = json.loads(run(compiler, "build", source, "--entry", "init",
                            "--require-full-native", "-o", binary))
    assert report["native_full_coverage"] is True, report
    assert report["native_vm_independent"] is True, report
    assert report["native_fallback_code_count"] == 0, report
    assert report["bytecode_fallback"] is False, report
    native = run(binary)
    assert native == vm == "PASS membership\n0\n", (native, vm)
print("native_membership_test: VM and full native passed")
