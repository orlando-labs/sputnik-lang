#!/usr/bin/env python3
"""Import-as selects a fork throughout a manifest-free application graph."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    binary = str(Path(sys.argv[1] if len(sys.argv) > 1 else "build/sputnik").resolve())
    with tempfile.TemporaryDirectory(prefix="sputnik-import-replacement-") as temporary:
        root = Path(temporary)
        home = root / "home"
        app = root / "app"
        app.mkdir()
        env = dict(os.environ, SPUTNIK_PKG_HOME=str(home),
                   SPUTNIK_VM_CACHE=str(root / "vm-cache"))

        def cli(*args, ok=True):
            result = subprocess.run([binary, *map(str, args)], env=env, cwd=root,
                                    capture_output=True, text=True, timeout=180)
            output = result.stdout + result.stderr
            assert (result.returncode == 0) == ok, (args, output)
            return output.strip()

        def package(name, modules):
            directory = root / name
            directory.mkdir()
            manifest = f'[package]\nname = "{name}"\nversion = "1.0.0"\nroot = "{next(iter(modules))}"\n'
            for index, (module, source) in enumerate(modules.items()):
                path = f"module{index}.s"
                (directory / path).write_text(f"package {module}\n{source}")
                manifest += f'\n[[modules]]\nname = "{module}"\npath = "{path}"\n'
            (directory / "sputnik.toml").write_text(manifest)
            artifact = root / f"{name}.sputnikpkg"
            cli("package-build", directory / "sputnik.toml", artifact)
            cli("pkg", "install", artifact)

        package("fork", {
            "my.orm": 'print "fork initialized"\nexport answer, Box\ndef answer(): 42\nclass Box:\n  def init()\n',
            "my.orm.utils": "export extra\ndef extra(): 7\n",
        })
        package("original", {
            name: 'raise "original must not initialize"\n'
            for name in ("orm", "vendor.orm", "orm.utils", "vendor.orm.utils")
        })
        # These modules are compiled before the application's replacement
        # exists, then installed as bytecode. Their imports must be redirected
        # at graph linking, including a renamed re-export and a submodule.
        package("consumers", {
            f"client{index}": f"import {alias} as db\nfrom {alias} import answer, Box\n"
            f"from {alias}.utils import extra\nexport through_dependency, Box, answer as forwarded\n"
            "def through_dependency(): db.answer() + answer() + extra()\n"
            for index, alias in enumerate(("orm", "vendor.orm"))
        })
        cli("pkg", "uninstall", "original")

        script = app / "main.s"
        expected = 'fork initialized\n[42, 42, 91, 42, true, true, 7, "orm.answer"]'
        for index, alias in enumerate(("orm", "vendor.orm")):
            script.write_text(
                f"import my.orm as {alias}\nimport {alias}\n"
                f"from {alias} import answer, Box\nfrom my.orm import answer as direct, Box as DirectBox\n"
                f"from {alias}.utils import extra\n"
                f"from client{index} import through_dependency, Box as ClientBox, forwarded\n"
                '[orm.answer(), direct(), through_dependency(), forwarded(), '
                'Box == DirectBox, Box == ClientBox, extra(), "orm.answer"]\n'
            )
            assert cli("run", script) == expected
            assert cli("run", script) == expected, "cached graph must retain replacements"
            assert not (app / "sputnik.toml").exists()
            assert not (app / "sputnik.build.json").exists()
            assert not (app / "sputnik.lock").exists()

        executable = app / "native"
        cli("build", script, "--require-full-native", "-o", executable)
        native = subprocess.run([str(executable)], env=env, capture_output=True,
                                text=True, timeout=180)
        assert native.returncode == 0 and native.stdout.strip() == expected, native

        if len(sys.argv) > 2:
            notebook = subprocess.run([str(Path(sys.argv[2]).resolve()),
                                       "--import-replacements", str(home)], env=env,
                                      capture_output=True, text=True, timeout=180)
            assert notebook.returncode == 0, notebook.stdout + notebook.stderr
            assert "import_replacement_notebook: PASS" in notebook.stdout

        # Source distributions use the same resolver; adjacent prefixes are
        # distinct and must not be rewritten by a root alias.
        source = app / "packages" / "client-source"
        source.mkdir(parents=True)
        (source / "sputnik.toml").write_text(
            '[package]\nname = "source-client"\nversion = "1.0.0"\nroot = "source_client"\n'
            '[[modules]]\nname = "source_client"\npath = "main.s"\n')
        (source / "main.s").write_text(
            "package source_client\nfrom orm import answer\nexport calculate\ndef calculate(): answer()\n")
        script.write_text("import my.orm as orm\nfrom source_client import calculate\ncalculate()\n")
        assert cli("run", script) == "fork initialized\n42"

        # Macro staging uses the same canonical paths. An explicit import-as
        # must survive macro expansion even when its local namespace is unused
        # and only a transitive dependency observes the replacement.
        macros = app / "packages" / "macros"
        macros.mkdir()
        (macros / "sputnik.toml").write_text(
            '[package]\nname = "macro-fork"\nversion = "1.0.0"\nroot = "my.macros"\n'
            '[[modules]]\nname = "my.macros"\npath = "main.s"\n'
            '[[modules]]\nname = "macro_client0"\npath = "client0.s"\n'
            '[[modules]]\nname = "macro_client1"\npath = "client1.s"\n')
        (macros / "main.s").write_text(
            'package my.macros\nexport macro identity\nexport answer\n'
            'macro def identity(value):\n  return value\ndef answer(): 42\n')
        for index, alias in enumerate(("macros", "vendor.macros")):
            (macros / f"client{index}.s").write_text(
                f"package macro_client{index}\nfrom {alias} import answer\n"
                "export forwarded\ndef forwarded(): answer()\n")
        for index, alias in enumerate(("macros", "vendor.macros")):
            script.write_text(
                f"import my.macros as {alias}\nimport {alias}\n"
                f"from {alias} import identity\nfrom my.macros import identity\n"
                "macros.identity(identity(42))\n")
            assert cli("run", script) == "42"
            script.write_text(
                f"import my.macros as {alias}\nfrom macro_client{index} import forwarded\nforwarded()\n")
            assert cli("run", script) == "42"

        script.write_text("import my.orm as orm\nimport different.orm as orm\n")
        assert "conflicting import replacement" in cli("run", script, ok=False)
        script.write_text("import a as b\nimport b as a\n")
        assert "cyclic import replacement" in cli("run", script, ok=False)
        script.write_text("import absent.orm as orm\nimport orm\n")
        assert "absent.orm" in cli("run", script, ok=False)
        print("import_replacement_cli: PASS")


if __name__ == "__main__":
    main()
