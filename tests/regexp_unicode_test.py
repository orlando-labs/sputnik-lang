#!/usr/bin/env python3
"""Regexp compiler diagnostics and VM / direct-native equivalence."""
from pathlib import Path
import json
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
compiler = (root / (sys.argv[1] if len(sys.argv) > 1 else "build/sputnik")).resolve()
work = (root / (sys.argv[2] if len(sys.argv) > 2 else "build/regexp-unicode")).resolve()
work.mkdir(parents=True, exist_ok=True)
fixture = root / "tests/fixtures/regexp_unicode/main.s"

def run(*args):
    result = subprocess.run(args, cwd=root, text=True, capture_output=True, timeout=600)
    assert result.returncode == 0, (args, result.stdout, result.stderr)
    return result

baseline = run(str(compiler), str(fixture))
assert "PASS regexp Unicode" in baseline.stdout, baseline.stdout
metadata = json.loads(run(str(compiler), "build", str(fixture), "--entry", "main",
    "--require-full-native", "-o", str(work / "native")).stdout)
assert metadata["native_full_coverage"] and not metadata["bytecode_fallback"], metadata
assert metadata["vm_fallback_code_count"] == 0, metadata
native = run(str(work / "native"))
assert (baseline.stdout, baseline.stderr) == (native.stdout, native.stderr)

failures = {
    "invalid_literal": 'def unused():\n  r"["\n',
    "unknown_flag": 'r"x"z\n',
    "duplicate_flag": 'r"x"ii\n',
    "class_hole": 'x = "a-z"\nr"[#{x}]"\n',
    "quote_hole": 'x = "a"\nr"\\Q#{x}\\E"\n',
    "comment_hole": 'x = "a"\nr"# #{x}"x\n',
    "quantifier_hole": 'x = "2"\nr"a{#{x}}"\n',
    "partial_hex_escape": 'x = "41"\nr"\\x#{x}"\n',
    "partial_quantifier": 'x = "3"\nr"a{2,#{x}}"\n',
    "byte_capture": 'r"\\C"\n',
}
for name, source in failures.items():
    path = work / f"{name}.s"
    path.write_text(source)
    result = subprocess.run([str(compiler), "run", str(path)], cwd=root,
                            text=True, capture_output=True, timeout=30)
    assert result.returncode != 0, (name, result.stdout, result.stderr)
    assert "macro expansion" in result.stderr, (name, result.stderr)
for newline in ("\n", "\r\n"):
    path = work / ("block_location_" + str(len(newline)) + ".s")
    source = 'p = r"""\n    я\\\n    \\qX   \n    """x\n'
    path.write_bytes(source.replace("\n", newline).encode())
    result = subprocess.run([str(compiler), "run", str(path)], cwd=root,
                            text=True, capture_output=True, timeout=30)
    assert result.returncode != 0, result.stdout
    assert f"{path}:3:7" in result.stderr, result.stderr

for op in ("=~", "!~"):
    path = work / ("removed_" + ("match" if op == "=~" else "nonmatch") + ".s")
    path.write_text(f'"x" {op} r"x"\n')
    result = subprocess.run([str(compiler), "run", str(path)], cwd=root,
                            text=True, capture_output=True, timeout=30)
    assert result.returncode != 0, (op, result.stdout, result.stderr)
print("regexp_unicode_test: VM and full native agree; static diagnostics and removed operators verified")
