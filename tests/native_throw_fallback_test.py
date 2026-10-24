#!/usr/bin/env python3
"""Keep source throw/catch in one VM scope when native coverage is incomplete."""

import json
import os
from pathlib import Path
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
    "uncaught": "throw :missing, 1\n",
}

with tempfile.TemporaryDirectory(prefix="sputnik-native-throw-") as temporary:
    for name, text in programs.items():
        source = Path(temporary) / (name + ".s")
        source.write_text(text)
        vm = run(compiler, "run", source)
        executable = Path(temporary) / name
        built = run(compiler, "build", source, "--target", "native", "--entry", "init",
                    "-o", executable, timeout=300)
        assert built.returncode == 0, built.stdout + built.stderr
        report = json.loads(built.stdout)
        assert report["bytecode_fallback"] and not report["native_entry"], report
        native = run(executable)
        assert (native.returncode, native.stdout, native.stderr) == (
            vm.returncode, vm.stdout, vm.stderr), (name, vm, native)
        if name == "effect_before_catch":
            assert native.returncode == 0 and native.stdout == "before\n2\n42\n", native
        elif name == "iterator_catch":
            assert native.returncode == 0 and "40:caught:[1, 99, 3]" in native.stdout, native
        else:
            assert native.returncode != 0 and "UncaughtThrowError" in native.stderr, native

print("native_throw_fallback_test: dynamic catch, single effect and uncaught parity passed")
