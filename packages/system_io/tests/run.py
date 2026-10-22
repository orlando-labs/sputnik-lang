#!/usr/bin/env python3
"""Development harness for the SDK API; creates adversarial filesystem fixtures."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile

p = argparse.ArgumentParser()
p.add_argument("--sdk", type=Path, default=Path(__file__).resolve().parents[3])
p.add_argument("--native", action="store_true")
a = p.parse_args()
sdk = a.sdk.resolve()
package = sdk / "packages/system_io"
work = Path(tempfile.mkdtemp(prefix="system-io-", dir=sdk / "build"))
root = work / "root"
outside = work / "outside"
root.mkdir(); outside.mkdir()
(outside / "secret").write_text("must survive")
(root / "folder").mkdir()
(root / "linked").symlink_to(outside, target_is_directory=True)
(root / "symlink").symlink_to(outside / "secret")
fixture = work / "package"
fixture.mkdir()
(fixture / "main.s").write_text("package tests.file\nexport value\ndef value(): 42\n")
(fixture / "sputnik.toml").write_text('schema="sputnik.package-manifest.v1"\n[package]\nname="tests/file"\nversion="1.0.0"\nroot="tests.file"\n[[modules]]\nname="tests.file"\nsource="main.s"\n')
subprocess.run([str(sdk / "build/sputnik"), "pkg", "build", str(fixture), "--out", str(root / "archive.sputnikpkg")], check=True, capture_output=True)
subprocess.run([str(sdk / "build/sputnik"), "pkg", "build", str(fixture), "--modules", "bytecode", "--out", str(root / "archive-bytecode.sputnikpkg")], check=True, capture_output=True)
manifest = json.loads((package / "sputnik.build.json").read_text())
manifest["root"] = "system_io.selftest"
manifest["modules"].append({"name": "system_io.selftest", "path": "tests/selftest.s"})
for module in manifest["modules"]:
    module["path"] = str(package / module["path"])
for extension in manifest["native_extensions"]:
    for key in ["sources", "include_dirs", "library_dirs"]:
        extension[key] = [str((package / value).resolve()) if not value.startswith("/") else value for value in extension[key]]
build = work / "test.build.json"
build.write_text(json.dumps(manifest))
env = dict(os.environ, SYSTEM_IO_SELFTEST="working")
env.setdefault("SPUTNIK_NATIVE_RUNTIME_ROOT", str(sdk))
env.setdefault("SPUTNIK_NATIVE_RT_CACHE", str(sdk / "build/native-runtime"))
env.setdefault("SPUTNIK_NATIVE_RT_JOBS", "2")
command = [str(sdk / "build/sputnik")]
grants = ["--grant", "ffi", "--grant", "fs.read", "--grant", "fs.write", "--grant", "fs.metadata"]
if a.native:
    report = subprocess.run(command + ["build", str(build), "--target", "native", "--require-full-native", "--out-dir", str(work / "native")] + grants, env=env, capture_output=True, text=True)
    (work / "build.json").write_text(report.stdout)
    if report.returncode: raise RuntimeError(report.stderr + report.stdout)
    command = [json.loads(report.stdout)["native_output"]]
else:
    command += ["run", str(build)] + grants + ["--"]
subprocess.run(command + [str(root)], env=env, check=True)
assert (outside / "secret").read_text() == "must survive"
print(json.dumps({"backend": "native" if a.native else "vm", "work": str(work), "passed": True}))
