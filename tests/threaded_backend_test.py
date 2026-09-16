#!/usr/bin/env python3
"""VM/native parity for threaded collections and native-only failure boundaries."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys


def main():
    root = Path(__file__).resolve().parents[1]
    compiler = Path(sys.argv[1] if len(sys.argv) > 1 else "build/amberc").resolve()
    work = compiler.parent / "threaded-backend"
    work.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    env.setdefault("AMBER_NATIVE_RT_CACHE", str(compiler.parent / "native-rt-cache"))
    # Keep the real compile command to exercise the same native launcher with
    # one scheduler worker, without adding a production-only testing switch.
    cxx = shutil.which(env.get("AMBER_NATIVE_CXX", env.get("CXX", "clang++")))
    assert cxx, "native C++ compiler is unavailable"
    wrapper = work / "record-cxx"
    command_log = work / "compile-command.json"
    wrapper.write_text(
        f"#!{sys.executable}\nimport json, os, sys\nfrom pathlib import Path\n"
        "if any(a.endswith('.native.cpp') for a in sys.argv[1:]):\n"
        f"    Path({str(command_log)!r}).write_text(json.dumps([{cxx!r}, *sys.argv[1:]]))\n"
        f"os.execv({cxx!r}, [{cxx!r}, *sys.argv[1:]])\n")
    wrapper.chmod(0o755)
    env["AMBER_NATIVE_CXX"] = str(wrapper)

    def run(args, timeout=180, success=True):
        result = subprocess.run(args, cwd=root, env=env, text=True,
                                capture_output=True, timeout=timeout)
        if success and result.returncode:
            raise AssertionError(f"{args}\n{result.stdout}\n{result.stderr}")
        return result

    def build(name, source):
        path = work / f"{name}.am"
        path.write_text(source)
        executable = work / name
        result = run([str(compiler), "build", str(path), "--target", "native",
                      "--entry", "init", "--require-full-native", "--grant",
                      "process.spawn", "-o", str(executable)], timeout=300)
        info = json.loads(result.stdout)
        assert info["native_full_coverage"] and info["native_vm_independent"], info
        assert not info["native_runtime_bridge"] and not info["bytecode_fallback"], info
        assert info["vm_fallback_code_count"] == info["native_fallback_code_count"] == 0, info
        (work / f"{name}.build.json").write_text(result.stdout)
        return path, executable

    fixture = (root / "corpus/run/threaded_native/source.am").read_text() + "\nprobe()\n"
    source, executable = build("collections", fixture)
    vm = run([str(compiler), "run", str(source)]).stdout
    native = run([str(executable)]).stdout
    assert native == vm == "42\n", (vm, native)
    print("threaded collections: VM + full native passed", flush=True)

    stress = ('import system\ncmd = system.cmd"/usr/bin/printf output"\n'
              '10_000.times.threaded(500).map: cmd.output .group: $it .transform_values: $it.size\n'
              'print $_\n')
    source, executable = build("process_stress", stress)
    vm = run([str(compiler), "run", str(source), "--grant", "process.spawn"]).stdout
    native = run([str(executable)]).stdout
    assert native == vm == "{output: 10000}\n", (vm, native)
    print("10,000 subprocesses / 500 workers: VM + full native passed", flush=True)

    errors = '''
from sync import Channel
score = 0
try:
  [1, 2].threaded(2).map: raise ValueError.new("worker")
rescue ValueError:
  score += 1
try:
  [[1]].threaded(1).map: $it
rescue IsolationError:
  score += 1
try:
  [1].threaded(1).map: [$it]
rescue IsolationError:
  score += 1
try:
  [1].threaded(-1)
rescue TypeError:
  score += 1
try:
  [1].threaded(1, scatter: :invalid)
rescue TypeError:
  score += 1
try:
  [1].threaded(1).flat_map: 7
rescue TypeError:
  score += 1
work = task.async:
  [1, 2].threaded(2).map:
    task.sleep(5.0)
    1
task.sleep(0.03)
work.cancel()
try:
  work.wait()
rescue CancelledError:
  score += 1
job = task.async:
  channel = Channel.new(capacity: 0)
  producer = task.async: channel.send(41, timeout: 2.0)
  values = [1].threaded(1).map: channel.recv(timeout: 2.0) + $it
  producer.wait()
  values[0]
if job.wait() == 42:
  score += 1
if score == 8: 42 else: score
'''
    _, executable = build("errors", errors)
    assert run([str(executable)], timeout=15).stdout == "42\n"
    command = json.loads(command_log.read_text())
    info = json.loads((work / "errors.build.json").read_text())
    original = Path(info["native_source"])
    generated = original.read_text()
    needle = "static amber::runtime::RuntimeTaskModule runtime;"
    assert generated.count(needle) == 1
    single_source = work / "errors.one-worker.cpp"
    single_source.write_text(generated.replace(
        needle, "static amber::runtime::RuntimeTaskModule runtime(1);"))
    command[command.index(str(original))] = str(single_source)
    single_executable = work / "errors.one-worker"
    command[command.index("-o") + 1] = str(single_executable)
    run(command)
    assert run([str(single_executable)], timeout=15).stdout == "42\n"
    print("threaded native errors, isolation, async, and cancellation passed "
          "(including one scheduler worker)", flush=True)


if __name__ == "__main__":
    main()
