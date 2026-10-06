#!/usr/bin/env python3
"""Process/tag parity, full native coverage, capabilities, and one-worker async."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys


def run(command, root, env, timeout=30):
    result = subprocess.run(command, cwd=root, env=env, text=True,
                            capture_output=True, timeout=timeout)
    if result.returncode:
        raise AssertionError(f"{command}\n{result.stdout}\n{result.stderr}")
    return result.stdout


def main():
    root = Path(__file__).resolve().parents[1]
    compiler = Path(sys.argv[1] if len(sys.argv) > 1 else root / "build/sputnik").resolve()
    work = compiler.parent / "system-backend"
    work.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    env.setdefault("SPUTNIK_NATIVE_RT_CACHE", str(compiler.parent / "native-rt-cache"))
    # Record the real launcher compile command so the scheduler can be tested
    # with one worker without adding a production configuration solely for tests.
    cxx = shutil.which(env.get("SPUTNIK_NATIVE_CXX", env.get("CXX", "clang++")))
    assert cxx, "native C++ compiler is unavailable"
    wrapper = work / "record-cxx"
    command_log = work / "compile-command.json"
    wrapper.write_text(
        f"#!{sys.executable}\nimport json, os, sys\nfrom pathlib import Path\n"
        "if any(a.endswith('.native.cpp') for a in sys.argv[1:]):\n"
        f"    Path({str(command_log)!r}).write_text(json.dumps([{cxx!r}, *sys.argv[1:]]))\n"
        f"os.execv({cxx!r}, [{cxx!r}, *sys.argv[1:]])\n")
    wrapper.chmod(0o755)
    env["SPUTNIK_NATIVE_CXX"] = str(wrapper)
    for name in ("macro_string_tag_oneline", "system_capture", "system_async", "system_errors", "system_control", "system_denied"):
        source = work / f"{name}.s"
        grants = [] if name == "system_denied" else ["--grant", "process.spawn"]
        if name == "system_control":
            grants += ["--grant", "process.signal"]
        if name == "system_denied":
            text = ('from system import cmd\ntry:\n  cmd\'printf forbidden\'.output()\n'
                    'rescue CapabilityError:\n  42\n')
        else:
            text = (root / "corpus/run" / name / "source.s").read_text() + "\nprobe()\n"
        source.write_text(text)
        vm = run([str(compiler), "run", str(source), *grants], root, env)
        assert vm.strip() == "42", (name, "VM", vm)
        executable = work / name
        info = json.loads(run([str(compiler), "build", str(source), "--target", "native",
                               "--entry", "init", "--require-full-native", "-o", str(executable),
                               *grants], root, env, 300))
        assert info["native_vm_independent"] and info["native_full_coverage"], info
        assert not info["native_runtime_bridge"] and not info["bytecode_fallback"], info
        assert info["vm_fallback_code_count"] == info["native_fallback_code_count"] == 0, info
        (work / f"{name}.build.json").write_text(json.dumps(info, indent=2) + "\n")
        output = run([str(executable)], root, env)
        assert output.strip() == "42", (name, "native", output)
        if name in ("system_async", "system_errors", "system_control"):
            command = json.loads(command_log.read_text())
            original = Path(info["native_source"])
            generated = original.read_text()
            needle = "static sputnik::runtime::RuntimeTaskModule runtime;"
            assert generated.count(needle) == 1
            single_source = work / f"{name}.one-worker.cpp"
            single_source.write_text(generated.replace(needle, "static sputnik::runtime::RuntimeTaskModule runtime(1);"))
            command[command.index(str(original))] = str(single_source)
            single_executable = work / f"{name}.one-worker"
            command[command.index("-o") + 1] = str(single_executable)
            run(command, root, env, 180)
            assert run([str(single_executable)], root, env).strip() == "42"
        print(f"{name}: VM + full native passed", flush=True)
    print("system backend tests passed (including native async/cancel with one worker)")


if __name__ == "__main__":
    main()
