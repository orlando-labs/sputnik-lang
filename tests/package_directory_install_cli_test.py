#!/usr/bin/env python3
"""Install local directories independently of the registry and working directory."""

import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def main():
    binary = str(Path(sys.argv[1] if len(sys.argv) > 1 else "build/sputnik").resolve())
    with tempfile.TemporaryDirectory(prefix="sputnik-directory-install-") as temporary:
        root = Path(temporary)
        source = root / "arbitrary source directory"
        cwd = root / "unrelated working directory"
        cwd.mkdir()
        source.mkdir()
        (source / "src").mkdir()
        (source / "src/api.s").write_text(
            "package directory_calc\nfrom directory_calc.helper import increment\n"
            "export answer\ndef answer(): increment(41)\n")
        (source / "src/helper.s").write_text(
            "package directory_calc.helper\nexport increment\ndef increment(n): n + 1\n")
        (source / "src/unused.s").write_text(
            'package directory_calc.unused\nraise "install must not evaluate modules"\n')
        modules = [{"name": "directory_calc", "path": "src/api.s"},
                   {"name": "directory_calc.helper", "path": "src/helper.s"},
                   {"name": "directory_calc.unused", "path": "src/unused.s"}]
        manifest = dict(name='directory-library', version='1.2.0', files=[m['path'] for m in modules],
                        module_names={m['path']:m['name'] for m in modules})
        (source / "sputnik.build.yaml").write_text(json.dumps(manifest))
        baseline = {str(p.relative_to(source)): p.read_bytes()
                    for p in source.rglob("*") if p.is_file()}

        environment = dict(os.environ, HOME=str(root / "user home"))
        environment.pop("SPUTNIK_PKG_HOME", None)
        environment.pop("SPUTNIK_PKG_REGISTRY", None)
        environment["SPUTNIK_NATIVE_EXT_CACHE"] = str(root / "native cache")
        environment.setdefault("SPUTNIK_NATIVE_RT_CACHE",
                               str(Path(binary).parent / "interop-runtime-cache"))

        def cli(*args, env=None, ok=True, timeout=90):
            result = subprocess.run([binary, *map(str, args)], cwd=cwd,
                                    env=environment if env is None else env,
                                    text=True, capture_output=True, timeout=timeout)
            output = result.stdout + result.stderr
            assert (result.returncode == 0) == ok, (args, result.returncode, output)
            return output

        # An absolute source path installs into the fallback home, with no registry.
        cli("pkg", "install", source)
        default_home = Path(environment["HOME"]) / ".sputnik/packages"
        relative_artifact = Path("directory-library/1.2.0/source/directory-library-1.2.0.sputnikpkg")
        installed = default_home / relative_artifact
        assert installed.is_file(), installed
        assert (installed.parent / "modules.index").is_file()
        digest = hashlib.sha256(installed.read_bytes()).digest()
        cli("pkg", "install", Path("..") / source.name)
        assert hashlib.sha256(installed.read_bytes()).digest() == digest
        alias = root / "source symlink"
        alias.symlink_to(source, target_is_directory=True)
        cli("pkg", "install", alias)
        assert hashlib.sha256(installed.read_bytes()).digest() == digest

        # The override is an installation home, not a directory searched for sources.
        custom_home = root / "shared package home"
        custom_env = dict(environment, SPUTNIK_PKG_HOME=str(custom_home))
        cli("pkg", "install", source, env=custom_env)
        assert (custom_home / relative_artifact).is_file()
        assert hashlib.sha256((custom_home / relative_artifact).read_bytes()).digest() == digest
        empty_override = dict(environment, SPUTNIK_PKG_HOME="")
        cli("pkg", "install", source, env=empty_override)
        assert not (cwd / "packages").exists()
        assert baseline == {str(p.relative_to(source)): p.read_bytes()
                            for p in source.rglob("*") if p.is_file()}, "installation must not write into source directory"

        # Build-manifest directories use the same installation home.
        build_only = root / "package with build manifest"
        shutil.copytree(source, build_only)
        (build_only / "sputnik.build.yaml").unlink()
        (build_only / "sputnik.build.yaml").write_text(
            "schema: sputnik.build.v1\nname: directory-build\nroot: directory_calc\n"
            "modules:\n" + "".join(f'  - name: {m["name"]}\n    path: {m["path"]}\n' for m in modules))
        assert "requires version or --version" in cli("pkg", "install", build_only, ok=False)
        cli("pkg", "install", build_only, "--version", "2.0.0")
        build_artifact = default_home / "directory-build/2.0.0/source/directory-build-2.0.0.sputnikpkg"
        assert build_artifact.is_file()

        # The hook runs from the source root before packaging generated inputs.
        configured = root / "configure's source directory"
        configured.mkdir()
        (configured / "sputnik.build.json").write_text(json.dumps(dict(
            name="configured", version="1.0.0", files=["generated.s"],
            module_names={"generated.s": "generated"})))
        hook_code = (
            'fs.File.open("generated.s", :write, create: true) |file|:\n'
            '  file.write_all!("package generated\\nexport answer\\ndef answer(): 42\\n".bytes)\n'
            'print("configure ran")\n'
        )
        for suffix in (".sp", ".spu", ".s", ".sputnik"):
            hook = configured / ("configure" + suffix)
            hook.write_text(hook_code)
            output = cli("pkg", "install", configured, "--grant", "fs.write=generated.s")
            assert output.splitlines().count("configure ran") == 1
            assert (configured / "generated.s").is_file()
            hook.unlink()
            (configured / "generated.s").unlink()
        assert not (cwd / "generated.s").exists()

        def source_with_dependencies(name, dependencies):
            directory = root / name
            directory.mkdir()
            (directory / "api.s").write_text(f"package {name}\n")
            (directory / "sputnik.build.json").write_text(json.dumps(dict(
                name=name, version="1.0.0", files=["api.s"],
                module_names={"api.s": name}, dependencies={
                    dep_name: {"path": "../" + dep_directory.name}
                    for dep_name, dep_directory in dependencies.items()
                })))
            return directory

        hook = configured / "configure.s"
        hook.write_text(hook_code)
        left = source_with_dependencies("left", {"configured": configured})
        right = source_with_dependencies("right", {"configured": configured})
        diamond = source_with_dependencies("diamond", {"left": left, "right": right})
        output = cli("pkg", "install", diamond, "--grant", "fs.write=generated.s")
        assert output.splitlines().count("configure ran") == 1

        failed_home = root / "failed configure home"
        failed_env = dict(environment, SPUTNIK_PKG_HOME=str(failed_home))
        hook.write_text('raise "configure stopped"\n')
        failure = cli("pkg", "install", configured, env=failed_env, ok=False)
        assert "package configure failed" in failure and "configure stopped" in failure
        assert not failed_home.exists()
        (configured / "configure.sp").write_text(hook_code)
        assert "multiple configure scripts" in cli("pkg", "install", configured, ok=False)

        native = root / "native configure directory"
        native.mkdir()
        (native / "sputnik.build.json").write_text(json.dumps(dict(
            name="configured-native", version="1.0.0", configure_native="adapter.so",
            files=["generated.s", "adapter.c"],
            profiles=dict(required=["core.v1", "ffi.v1"]),
            native_extensions=[dict(name="adapter", kind="source", language="c",
                                    sources=["adapter.c"])])))
        (native / "generated.s").write_text("package generated\n")
        (native / "adapter.c").write_text("/* configure fixture */\n")
        (native / "configure.sputnik").write_text(
            'import system\nprint system.cmd"/usr/bin/printf configured-native-hook".output\n')
        denied = cli("pkg", "install", native, "--allow-native", ok=False)
        assert "requires --allow-native --build-from-source" in denied, denied
        assert "configured-native-hook" not in denied
        missing = cli("pkg", "install", native, "--allow-native", "--build-from-source", ok=False)
        assert "configured-native-hook" in missing and "adapter.so" in missing, missing

        # Missing and invalid local paths must not become registry requests.
        assert "local package directory or artifact" in cli("pkg", "install", root / "missing", ok=False)
        assert "local package directory or artifact" in cli("pkg", "install", "./missing", ok=False)
        empty = root / "empty package"
        empty.mkdir()
        assert "no package manifest" in cli("pkg", "install", empty, ok=False)
        assert not (default_home / "missing").exists()

        # The installed package remains importable after its checkout disappears.
        shutil.rmtree(source)
        shutil.rmtree(build_only)
        program = cwd / "train.s"
        program.write_text("import directory_calc\ndirectory_calc.answer()\n")
        assert cli("run", program, env=custom_env).strip() == "42"
        # Avoid the deliberately installed alternate provider in the default home.
        shutil.rmtree(default_home / "directory-build")
        assert cli("run", program).strip() == "42"

        # Imported macros must expand before standalone lowering, including
        # macros used inside a transitive installed source module.
        macros = root / "installed macros"
        macros.mkdir()
        (macros / "macros.s").write_text(
            "package acme.macros\nexport macro twice\n"
            "macro def twice(value as Ast):\n  return quote:\n    #{value} + #{value}\n")
        (macros / "sputnik.build.yaml").write_text(
            'schema: "sputnik.build.v1"\nname: "acme/macros"\nversion: "1.0.0"\nsource_root: "."\nfiles:\n  - "macros.s"\nmodule_names:\n  macros.s: "acme.macros"\n')
        macro_archive = root / "macros.sputnikpkg"
        cli("pkg", "build", macros, "--out", macro_archive)
        cli("pkg", "install", macro_archive, env=custom_env)
        calculation = root / "macro consumer"
        calculation.mkdir()
        (calculation / "calc.s").write_text(
            "package acme.calc\nfrom acme.macros import twice\n"
            "export answer\ndef answer(): twice(21)\n")
        (calculation / "sputnik.build.yaml").write_text(
            'schema: "sputnik.build.v1"\nname: "acme/calc"\nversion: "1.0.0"\nsource_root: "."\nfiles:\n  - "calc.s"\nmodule_names:\n  calc.s: "acme.calc"\ndependencies:\n  "acme/macros": "1.0.0"\n')
        calculation_archive = root / "calc.sputnikpkg"
        cli("pkg", "build", calculation, "--out", calculation_archive)
        cli("pkg", "install", calculation_archive, env=custom_env)
        shutil.rmtree(macros)
        shutil.rmtree(calculation)
        program.write_text("from acme.macros import twice\nprint(twice(21))\n")
        assert cli("run", program, env=custom_env).strip() == "42"
        program.write_text("from acme.calc import answer\nprint(answer())\n")
        assert cli("run", program, env=custom_env).strip() == "42"
        executable = cwd / "macro-native"
        cli("build", program, "--require-full-native", "-o", executable,
            env=custom_env, timeout=600)
        native = subprocess.run([str(executable)], capture_output=True, text=True, timeout=30)
        assert native.returncode == 0 and native.stdout.strip() == "42", native.stderr

        # Native directory installation also survives removing the checkout.
        fixtures = Path(__file__).resolve().parent / "fixtures"
        native_source = root / "native source directory"
        shutil.copytree(fixtures / "native_class_demo", native_source,
                        ignore=shutil.ignore_patterns(".sputnik", "build"))
        cli("pkg", "install", native_source, "--version", "1.0.0", "--allow-native", "--build-from-source", env=custom_env)
        shutil.rmtree(native_source)
        program.write_text("from nat.box import main\nmain()\n")
        no_compiler = dict(custom_env, SPUTNIK_NATIVE_CXX="/nonexistent/compiler")
        assert cli("run", program, "--grant", "ffi", env=no_compiler).strip() == "24"

    print("package_directory_install_cli_test ok")


if __name__ == "__main__":
    main()
