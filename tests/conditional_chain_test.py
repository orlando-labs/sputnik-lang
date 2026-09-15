#!/usr/bin/env python3
"""Exercise conditional-chain semantics in the VM and full native executables."""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    amberc = (root / (sys.argv[1] if len(sys.argv) > 1 else "build/amberc")).resolve()
    work = (root / (sys.argv[2] if len(sys.argv) > 2 else "build/conditional-chain")).resolve()
    work.mkdir(parents=True, exist_ok=True)
    fixture = root / "corpus/run/conditional_chain_segments"
    source = work / "source.am"
    source.write_text((fixture / "source.am").read_text() + "\nprobe()\n")

    def run(*args: str) -> subprocess.CompletedProcess[str]:
        result = subprocess.run(args, cwd=root, text=True, capture_output=True, timeout=300)
        if result.returncode:
            raise AssertionError(f"{' '.join(args)} failed:\n{result.stdout}{result.stderr}")
        return result

    # Validate the optimizer path as well as the bytecode used by native codegen.
    run(str(amberc), "mir-verify", str(source))
    run(str(amberc), "native-verify", str(source))
    results = []
    for target in ("bytecode-wrapper", "native"):
        exe = work / target
        args = [str(amberc), "build", str(source), "--target", target,
                "-o", str(exe), "--out-dir", str(work)]
        if target == "native":
            args.append("--require-full-native")
        build = run(*args)
        (work / f"{target}-build.json").write_text(build.stdout)
        metadata = json.loads(build.stdout)
        if target == "native":
            assert metadata["native_backend"] == "cpp-bytecode-direct-v1", metadata
            assert metadata["native_full_coverage"] is True, metadata
            assert metadata["bytecode_fallback"] is False, metadata
            assert metadata["vm_fallback_code_count"] == 0, metadata
            assert metadata["native_fallback_code_count"] == 0, metadata
        result = run(str(exe))
        expected = json.loads((fixture / "expect.run.json").read_text())["value"]
        assert result.stdout.strip() == expected, (target, result.stdout, result.stderr)
        results.append((result.returncode, result.stdout, result.stderr))
    assert results[0] == results[1], results
    print("conditional_chain_test: VM and full native agree; no fallback")
    return 0


if __name__ == "__main__":
    sys.exit(main())
