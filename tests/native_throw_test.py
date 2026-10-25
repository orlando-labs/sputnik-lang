#!/usr/bin/env python3
"""Require VM-independent native throw/catch and compare observable VM behavior."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


root = Path(__file__).resolve().parents[1]
compiler = Path(sys.argv[1] if len(sys.argv) > 1 else root / "build/sputnik").resolve()
env = dict(os.environ)
env.setdefault("SPUTNIK_NATIVE_RT_CACHE", str(compiler.parent / "interop-runtime-cache"))


def run(*command, timeout=60):
    return subprocess.run(list(map(str, command)), capture_output=True, text=True,
                          env=env, timeout=timeout)


programs = {
    "iterator_catch": (root / "corpus/run/block_nonlocal_exit/source.s").read_text() + "\nprobe()\n",
    "effect_before_catch": '''print "before"
value = catch(:done):
  [1, 2].each |x|:
    if x == 2: throw :done, x
  -1
print value
42
''',
    "unwind": '''def require(value):
  raise ValueError("native throw/catch regression") unless value

def deep():
  throw :done, ["payload", 42]

def forward():
  deep()

value = catch :done: forward()
require(value == ["payload", 42])
value = catch :empty_payload: throw :empty_payload
require(value == null)
value = catch "finished": throw "finished", 7
require(value == 7)
value = catch 5: throw 5, 13
require(value == 13)
value = catch null: throw null, 15
require(value == 15)
tag = :done
value = catch tag:
  tag = :changed
  throw :done, 21
require(value == 21)
value = catch :outer:
  catch :inner:
    throw :outer, 33
require(value == 33)
value = catch :done:
  try:
    throw :done, 17
  rescue:
    99
require(value == 17)
events = []
value = catch :done:
  try:
    throw :done, 11
  ensure:
    events.push!(5)
require(value == 11 and events == [5])
value = catch :replacement:
  try:
    throw :original, 1
  ensure:
    throw :replacement, 23
require(value == 23)
value = catch :done:
  try:
    raise ValueError("original")
  rescue ValueError:
    throw :done, 19
require(value == 19)
value = catch :done:
  try:
    raise ValueError("original")
  ensure:
    throw :done, 31
require(value == 31)
replacement = try:
  try:
    throw :unused, 1
  ensure:
    raise ValueError("replacement")
rescue ValueError:
  37
require(replacement == 37)
print "PASS unwind"
42
''',
    "task_boundary": '''result = try:
  catch :child:
    child = task.spawn: throw :child, 1
    child.wait()
rescue UncaughtThrowError:
  42
result
''',
    "uncaught_threaded_boundary": "catch :worker:\n  [1].threaded(1).map: throw :worker, 1\n",
    "uncaught": "throw :missing, 1\n",
    "uncaught_deep": '''def deep():
  throw "missing", 1
def forward():
  deep()
forward()
''',
    "uncaught_iterator": "[1].each: throw :missing, 1\n",
    "uncaught_ensure": '''def deep():
  throw :missing, 1
try:
  deep()
ensure:
  print "cleanup"
''',
    "uncaught_mismatch": '''def deep():
  throw :missing, 1
catch :other:
  deep()
''',
}

with tempfile.TemporaryDirectory(prefix="sputnik-native-throw-") as temporary:
    for name, text in programs.items():
        source = Path(temporary) / (name + ".s")
        source.write_text(text)
        vm = run(compiler, "run", source)
        executable = Path(temporary) / name
        built = run(compiler, "build", source, "--target", "native", "--entry", "init",
                    "--require-full-native", "-o", executable, timeout=600)
        assert built.returncode == 0, built.stdout + built.stderr
        report = json.loads(built.stdout)
        assert report["native_entry"] and report["native_full_coverage"], report
        assert report["native_vm_independent"] and not report["bytecode_fallback"], report
        generated = Path(report["native_source"]).read_text()
        assert "run_vm_entry" not in generated and "sputnik_vm_fallback_call" not in generated
        native = run(executable)
        assert (native.returncode, native.stdout, native.stderr) == (
            vm.returncode, vm.stdout, vm.stderr), (name, vm, native)
        if name == "effect_before_catch":
            assert native.returncode == 0 and native.stdout == "before\n2\n42\n", native
        elif name == "iterator_catch":
            assert native.returncode == 0 and "40:caught:[1, 99, 3]" in native.stdout, native
        elif name.startswith("uncaught"):
            assert native.returncode != 0 and "UncaughtThrowError" in native.stderr, native
        else:
            assert native.returncode == 0 and native.stdout.endswith("42\n"), native
        print(name + ": full native, VM parity", flush=True)

    # Exercise an uncaught throw at the C callback boundary as well as the
    # caught blocking callback covered by test-native-blocking.
    fixture = root / "tests/fixtures/native_blocking"
    ffi = Path(temporary) / "ffi"
    ffi.mkdir()
    for filename in ("vm_control.build.json", "bindings.s", "bridge.cpp"):
        shutil.copyfile(fixture / filename, ffi / filename)
    (ffi / "vm_control.s").write_text('''package test.blocking.vm_control
from test.blocking.bindings import batch
export main
def main():
  batch(| |: throw :missing, 9)
''')
    manifest = ffi / "vm_control.build.json"
    vm = run(compiler, "run", manifest, "--grant", "ffi")
    built = run(compiler, "build", manifest, "--target", "native",
                "--require-full-native", "--out-dir", ffi / "out", timeout=600)
    assert built.returncode == 0, built.stdout + built.stderr
    report = json.loads(built.stdout)
    assert report["native_graph_full_coverage"] and report["native_graph_vm_independent"], report
    assert not report["native_bytecode_fallback"], report
    generated = Path(report["native_source"]).read_text()
    assert "run_vm_entry" not in generated and "sputnik_vm_fallback_call" not in generated
    native = run(report["native_output"])
    assert (native.returncode, native.stdout, native.stderr) == (
        vm.returncode, vm.stdout, vm.stderr), ("uncaught_ffi", vm, native)
    assert native.returncode != 0 and "UncaughtThrowError" in native.stderr, native
    print("uncaught_ffi: full native, VM parity", flush=True)

print("native_throw_test: all programs passed without VM fallback")
