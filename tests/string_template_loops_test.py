#!/usr/bin/env python3
"""Structured tag loops: runtime/native parity and compile-time diagnostics."""
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


def run(*args, ok=True):
    result = subprocess.run([str(compiler), *map(str, args)], cwd=root,
                            env=env, capture_output=True, text=True, timeout=300)
    assert (result.returncode == 0) == ok, (args, result.stdout, result.stderr)
    return result


fixture = root / "tests/fixtures/string_template_loops/main.s"
with tempfile.TemporaryDirectory(prefix="sputnik-template-loops-") as temporary:
    directory = Path(temporary)
    vm = run("run", fixture).stdout
    assert vm.strip() == "PASS string template loops", vm
    executable = directory / "loops"
    info = json.loads(run("build", fixture, "--entry", "init", "--require-full-native",
                          "-o", executable).stdout)
    assert info["native_full_coverage"] and info["native_vm_independent"], info
    assert not info["bytecode_fallback"] and not info["native_runtime_bridge"], info
    native = subprocess.run([str(executable)], capture_output=True, text=True, timeout=30)
    assert native.returncode == 0 and native.stdout == vm, (native.stdout, native.stderr)
    generated = executable.with_suffix(".native.cpp").read_text()
    assert "invoke_native_stdlib_send" not in generated

    probe = directory / "bad.s"
    for body in ["%for x in [1, 2]\n  #{x}\n", "%for x in []:\n",
                 "%while true:\n  x\n", "%for x in []:\n    x\n  y\n"]:
        probe.write_text('from system import cmd\ncmd"""\n' + body + '"""\n')
        result = run("run", probe, ok=False)
        assert "STRING_TEMPLATE_CONTROL" in result.stderr, result.stderr
        assert str(probe) in result.stderr
    for body in ["echo '\n%for x in [1]:\n  #{x}\n'\n",
                 "echo\n%for x in [1]:\n  '#{x}\nend'\n"]:
        probe.write_text('from system import cmd\ncmd"""\n' + body + '"""\n')
        assert "CMD_TEMPLATE_CONTROL" in run("run", probe, ok=False).stderr

    # The provider checks HTML context around the entire repeated region.
    orbit = root.parent / "orbit"
    if not (orbit / "src/orbit/html.s").is_file():
        print("Orbit sibling absent; HTML provider diagnostics skipped")
    else:
        manifest = directory / "html.build.json"
        manifest.write_text(json.dumps({
            "schema": "sputnik.build.v1", "name": "app.bad_html", "root": "app.bad_html",
            "profiles": {"required": ["core.v1", "macro.v1"]},
            "modules": [
                {"name": "orbit", "path": str(orbit / "src/orbit.s")},
                {"name": "orbit.html", "path": str(orbit / "src/orbit/html.s")},
                {"name": "app.bad_html", "path": str(probe)},
            ],
        }))
        for body in ['<div title="\n%for x in [1]:\n  #{x}\n">ok</div>\n',
                     '<script>\n%for x in [1]:\n  #{x}\n</script>\n',
                     '<!--\n%for x in [1]:\n  #{x}\n-->\n',
                     '%for x in [1]:\n  <a href="\nend\n',
                     '%for x in [1]:\n  <script>\nend\n']:
            probe.write_text('package app.bad_html\nimport orbit\n'
                             'from orbit.html import html, escape\n'
                             'export main\ndef main():\n  html"""\n' +
                             ''.join('    ' + line + '\n' for line in body.splitlines()) +
                             '    """\n')
            result = run("build", manifest, "--target", "bytecode", "--no-cache",
                         "--out-dir", directory / "out", "--cache-dir", directory / "cache", ok=False)
            assert "ORBIT_HTML_CONTROL_CONTEXT" in result.stderr + result.stdout, (result.stdout, result.stderr)

print("string_template_loops_test: VM, full native and diagnostics passed")
