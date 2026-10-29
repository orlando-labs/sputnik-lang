#!/usr/bin/env python3
"""Install once, import from scripts/projects, and reuse prepared native code."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def main():
    binary = str(Path(sys.argv[1] if len(sys.argv) > 1 else "build/sputnik").resolve())
    with tempfile.TemporaryDirectory(prefix="sputnik-pkg-cli-") as temporary:
        root = Path(temporary)
        home, registry, app, elsewhere = (root / name for name in ("home", "registry", "app", "cwd"))
        app.mkdir()
        elsewhere.mkdir()
        env = dict(os.environ, SPUTNIK_PKG_HOME=str(home), SPUTNIK_PKG_REGISTRY=str(registry))
        env["SPUTNIK_VM_CACHE"] = str(root / "vm-cache")
        env["SPUTNIK_NATIVE_EXT_CACHE"] = str(root / "native-cache")

        def cli(*args, ok=True, extra_env=None, timeout=180):
            result = subprocess.run([binary, *map(str, args)], cwd=elsewhere,
                                    env=dict(env, **(extra_env or {})), text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)
            output = result.stdout + result.stderr
            assert (result.returncode == 0) == ok, (args, result.returncode, output)
            return output

        def package(name, version, modules, dependencies=None, native=None):
            directory = root / "sources" / (name + "-" + version)
            directory.mkdir(parents=True)
            entries = []
            manifest = dict(name=name, version=version, files=[], module_names={}, dependencies=dependencies or {})
            for index, (module, source) in enumerate(modules.items()):
                path = f"src/module{index}.s"
                (directory / "src").mkdir(exist_ok=True)
                (directory / path).write_text(f"package {module}\n{source}")
                manifest['files'].append(path)
                manifest['module_names'][path] = module
                entries.append({"name": module, "path": path})
            if native:
                (directory / "native").mkdir()
                (directory / "native/answer.c").write_text(native)
                manifest['files'].append('native/answer.c')
                manifest['profiles'] = dict(required=['core.v1', 'ffi.v1'])
                manifest['native_extensions'] = [dict(name='native_answer',language='c',sources=['native/answer.c'],symbols={'native.answer':'sputnik_package_answer'})]
            (directory / "sputnik.build.yaml").write_text(json.dumps(manifest))
            artifact = root / (name + "-" + version + ".sputnikpkg")
            cli("package-build", directory / "sputnik.build.yaml", artifact)
            cli("package-publish", artifact, registry)
            return directory, artifact

        package("base-library", "1.0.0", {"base.number": "export number\ndef number(): 40\n"})
        directory, old = package("renamed-library", "1.9.0", {
            "calc": "from base.number import number\nexport answer\ndef answer(): number() + 1\n",
            "calc.unused": 'raise "unused module initialized"\n',
        }, {"base-library": "1.0.0"})
        _, latest = package("renamed-library", "1.10.0", {
            "calc": "from base.number import number\nexport answer\ndef answer(): number() + 2\n",
            "calc.unused": 'raise "unused module initialized"\n',
        }, {"base-library": "1.0.0"})
        package("renamed-library", "2.0.0-preview.1", {"calc": "export answer\ndef answer(): 99\n"})
        cli("pkg", "install", "renamed-library")
        assert (home / "renamed-library/1.10.0/renamed-library-1.10.0.sputnikpkg").is_file()
        assert (home / "base-library/1.0.0/base-library-1.0.0.sputnikpkg").is_file()
        cli("pkg", "install", old)
        cli("pkg", "install", "renamed-library")  # idempotent

        script = app / "train.s"
        script.write_text("import calc\ncalc.answer()\n")
        assert cli("run", script).strip() == "42"
        assert not (app / "packages").exists(), "shared imports must not copy packages"

        # Explicit manifest entries beat automatic package discovery.
        (app / "main.s").write_text("package app.main\nfrom calc import answer\ndef main(): answer()\n")
        manifest = app / "sputnik.build.json"
        manifest.write_text(json.dumps({"schema": "sputnik.build.v1", "name": "app", "root": "app.main",
                                       "modules": [{"name": "app.main", "path": "main.s"}]}))
        assert cli("run", manifest).strip() == "42"
        pinned = json.loads(manifest.read_text())
        pinned["dependencies"] = {"renamed-library": "1.9.0"}
        manifest.write_text(json.dumps(pinned))
        assert cli("run", manifest).strip() == "41"
        del pinned["dependencies"]
        manifest.write_text(json.dumps(pinned))
        built = json.loads(cli("build", manifest, "--target", "bytecode", "--out-dir", app / "bytecode"))
        assert cli("run", app / "bytecode/app.main.sputnikbc").strip() == "42", built
        cli("build", script, "--target", "bytecode-wrapper", "-o", app / "train", timeout=600)
        wrapper = subprocess.run([str(app / "train")], env=env, capture_output=True, text=True)
        assert wrapper.returncode == 0 and wrapper.stdout.strip() == "42", wrapper
        cli("build", script, "--require-full-native", "-o", app / "native-train", timeout=600)
        native_app = subprocess.run([str(app / "native-train")], env=env, capture_output=True, text=True)
        assert native_app.returncode == 0 and native_app.stdout.strip() == "42", native_app

        # Package pins and artifact checksums survive newer installations.
        cli("pkg", "lock", script)
        lock = (app / "sputnik.lock").read_text()
        assert 'name = "renamed-library"' in lock and 'name = "base-library"' in lock
        _, newer = package("renamed-library", "1.11.0", {"calc": "export answer\ndef answer(): 43\n"})
        cli("pkg", "install", newer)
        assert cli("run", script).strip() == "42"
        unpinned, _ = package("extra-library", "1.0.0", {"extra": "export number\ndef number(): 5\n"})
        cli("pkg", "install", unpinned)
        additional = app / "extra_consumer.s"
        additional.write_text("from extra import number\nnumber()\n")
        output = cli("run", additional, ok=False)
        assert "missing from sputnik.lock" in output, output
        cli("pkg", "lock", script)
        assert cli("run", script).strip() == "43"
        (app / "sputnik.lock").unlink()

        # Anchor to the entry file, with an entire local package taking priority.
        local = app / "packages" / "custom-checkout"
        local.mkdir(parents=True)
        (local / "src").mkdir()
        (local / "src/calc.s").write_text("package calc\nexport answer\ndef answer(): 7\n")
        (local / "sputnik.build.json").write_text(json.dumps({
            "schema": "sputnik.build.v1", "name": "renamed-library", "root": "calc",
            "modules": [{"name": "calc", "path": "src/calc.s"}],
        }))
        assert cli("run", script).strip() == "7"
        assert cli("run", manifest).strip() == "7", "package content must invalidate graph cache"
        hidden = app / "hidden.s"
        hidden.write_text("import calc.unused\n")
        assert "was not found" in cli("run", hidden, ok=False), "local package hides the entire global build"
        cli("pkg", "lock", script)
        (local / "src/calc.s").write_text("package calc\nexport answer\ndef answer(): 8\n")
        assert cli("run", script).strip() == "8", "local edits stay live under the same dependency graph lock"
        (app / "sputnik.lock").unlink()
        shutil.rmtree(app / "packages")

        # Directory install also produces a reusable package artifact.
        directory, _ = package("directory-only", "1.0.0", {"directory_only": "export answer\ndef answer(): 42\n"})
        cli("pkg", "install", directory)
        # A malformed dependency never becomes a usable import.
        missing = app / "missing.s"
        missing.write_text("import absent_module\n")
        assert "sputnik pkg install" in cli("run", missing, ok=False)
        assert not (home / "absent_module").exists()

        # Native compilation belongs to install. Run works with no compiler.
        native_source = '''#include "runtime/sputnik_ext.h"
SputnikStatus sputnik_package_answer(SputnikCtx *cx, const SputnikValue *args,
    size_t argc, SputnikValue *out) {
  (void)args; (void)argc;
  *out = sputnik_make_int(cx, 42);
  return SPUTNIK_OK;
}
'''
        package("native-package", "1.0.0", {
            "native_calc": 'export answer\nnative def answer() -> Int from "native.answer": 9\n',
            "native_calc.facade": "from native_calc import answer\nexport answer\n",
        }, native=native_source)
        cli("pkg", "install", "native-package", "--allow-native", "--build-from-source")
        native_script = app / "native.s"
        native_script.write_text("from native_calc import answer\nanswer()\n")
        assert cli("run", native_script).strip() == "9"
        assert cli("run", native_script, "--grant", "ffi",
                   extra_env={"SPUTNIK_NATIVE_CXX": "/nonexistent/compiler"}).strip() == "42"
        fixtures = Path(__file__).resolve().parent / "fixtures"
        for fixture, module, expected in (("native_class_demo", "nat.box", "24"),
                                          ("native_ext_demo", "nat.demo", "42")):
            cli("pkg", "install", fixtures / fixture, "--version", "1.0.0")
            entry = app / (fixture + ".s")
            entry.write_text(f"from {module} import main\nmain()\n")
            assert cli("run", entry, "--grant", "ffi",
                       extra_env={"SPUTNIK_NATIVE_CXX": "/nonexistent/compiler"}).strip() == expected
        cli("run", fixtures / "native_class_demo/sputnik.build.json", "--grant", "ffi")
        local_box = app / "packages/custom-box"
        shutil.copytree(fixtures / "native_class_demo", local_box,
                        ignore=shutil.ignore_patterns(".sputnik", "build"))
        mixed = app / "mixed.s"
        mixed.write_text("from nat.box import main\nfrom native_calc import answer\nmain() + answer()\n")
        assert cli("run", mixed, "--grant", "ffi").strip() == "66"
        shutil.rmtree(app / "packages")
        if len(sys.argv) > 2:
            notebook = subprocess.run([str(Path(sys.argv[2]).resolve()), "--native", str(home)],
                                      env=dict(env, SPUTNIK_NATIVE_CXX="/nonexistent/compiler"),
                                      capture_output=True, text=True, timeout=60)
            assert notebook.returncode == 0, notebook.stdout + notebook.stderr
        library_meta = home / "native-package/1.0.0/native-library.meta"
        library = library_meta.parent / library_meta.read_text().splitlines()[1]
        with library.open("ab") as output:
            output.write(b"tampered")
        changed = cli("run", native_script, "--grant", "ffi", ok=False)
        assert "prepared native library changed" in changed and library.name in changed, changed

        # An installed artifact cannot silently change its pinned contents.
        artifact = home / "renamed-library/1.11.0/renamed-library-1.11.0.sputnikpkg"
        text = artifact.read_text()
        artifact.write_text(text.replace("module.0.sha256=sha256:", "module.0.sha256=sha256:0", 1))
        assert "digest does not match" in cli("run", script, ok=False)
    print("package_store_cli_smoke ok")


if __name__ == "__main__":
    main()
