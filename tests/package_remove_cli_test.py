#!/usr/bin/env python3
"""Remove installed packages and versions without touching checkouts or peers."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    binary = str(Path(sys.argv[1] if len(sys.argv) > 1 else "build/sputnik").resolve())
    with tempfile.TemporaryDirectory(prefix="sputnik-remove-") as temporary:
        root = Path(temporary)
        cwd = root / "working directory"
        cwd.mkdir()
        environment = dict(os.environ, HOME=str(root / "user home"))
        environment.pop("SPUTNIK_PKG_HOME", None)
        environment.pop("SPUTNIK_PKG_REGISTRY", None)
        default_home = Path(environment["HOME"]) / ".sputnik/packages"
        custom_home = root / "custom package home"
        custom_env = dict(environment, SPUTNIK_PKG_HOME=str(custom_home))

        def cli(*args, env=None, ok=True):
            result = subprocess.run([binary, *map(str, args)], cwd=cwd,
                                    env=environment if env is None else env,
                                    capture_output=True, text=True, timeout=90)
            output = result.stdout + result.stderr
            assert (result.returncode == 0) == ok, (args, result.returncode, output)
            return output

        def source(name, version, module, body):
            path = root / "checkouts" / f"{name}-{version}"
            path.mkdir(parents=True)
            (path / "source.s").write_text(f"package {module}\n{body}")
            (path / "sputnik.toml").write_text(
                f'[package]\nname = "{name}"\nversion = "{version}"\nroot = "{module}"\n'
                f'\n[[modules]]\nname = "{module}"\npath = "source.s"\n')
            return path

        v1 = source("remove-library", "1.0.0", "remove_calc", "export answer\ndef answer(): 41\n")
        v2 = source("remove-library", "2.0.0", "remove_calc", "export answer\ndef answer(): 42\n")
        peer = source("peer-library", "1.0.0", "peer", "export answer\ndef answer(): 7\n")
        cli("pkg", "install", v1)
        cli("pkg", "install", v2)
        cli("pkg", "install", peer)
        cli("pkg", "install", v2, env=custom_env)
        checkout_bytes = {p: p.read_bytes() for p in (root / "checkouts").rglob("*") if p.is_file()}
        program = cwd / "train.s"
        program.write_text("from remove_calc import answer\nanswer()\n")
        assert cli("run", program).strip() == "42"

        output = cli("pkg", "uninstall", "remove-library", "--version", "2.0.0")
        assert "Removed remove-library@2.0.0" in output
        assert not (default_home / "remove-library/2.0.0").exists()
        assert (default_home / "remove-library/1.0.0").is_dir()
        assert (default_home / "peer-library/1.0.0").is_dir()
        assert (custom_home / "remove-library/2.0.0").is_dir()
        assert cli("run", program).strip() == "41", "resolver must fall back to the remaining version"

        # CLI parsing must finish before any removal, including malformed versions.
        for args in (("--version",), ("--version", ""), ("--version", "../1.0.0"), ("--version", "9.0.0"),
                     ("--version", "1.0.0", "--version", "2.0.0"), ("--unknown",)):
            cli("pkg", "remove", "remove-library", *args, ok=False)
            assert (default_home / "remove-library/1.0.0").is_dir()
        for name in (".", "..", "../peer-library", str(v1)):
            assert "invalid package name/version" in cli("pkg", "remove", name, ok=False)

        # Remove by installed package identity even when the cwd contains that name.
        local = cwd / "remove-library"
        local.mkdir()
        (local / "keep.s").write_text("preserve local checkout")
        output = cli("pkg", "remove", "remove-library")
        assert "Removed remove-library@1.0.0" in output
        assert not (default_home / "remove-library").exists()
        assert (local / "keep.s").read_text() == "preserve local checkout"
        assert "was not found" in cli("run", program, ok=False)
        assert cli("run", program, env=custom_env).strip() == "42"
        assert "package is not installed" in cli("pkg", "remove", "remove-library", ok=False)

        # The alias removes every installed version and preserves other metadata.
        cli("pkg", "install", v1, env=custom_env)
        notes = custom_home / "remove-library/notes.txt"
        notes.write_text("keep unrelated metadata")
        outside = root / "outside data"
        outside.mkdir()
        (outside / "keep.txt").write_text("outside")
        (custom_home / "remove-library/2.0.0/external").symlink_to(outside, target_is_directory=True)
        output = cli("pkg", "remove", "remove-library", env=custom_env)
        assert "remove-library@1.0.0" in output and "remove-library@2.0.0" in output
        assert not (custom_home / "remove-library/1.0.0").exists()
        assert not (custom_home / "remove-library/2.0.0").exists()
        assert notes.read_text() == "keep unrelated metadata"
        assert (outside / "keep.txt").read_text() == "outside"
        assert not list(custom_home.glob(".remove-*"))

        # Corrupted package bytes must not prevent uninstalling an installation.
        artifact = default_home / "peer-library/1.0.0/peer-library-1.0.0.sputnikpkg"
        artifact.write_text("corrupted package")
        cli("pkg", "uninstall", "peer-library", "--version", "1.0.0")
        assert not (default_home / "peer-library").exists()

        # Directory links into external checkouts are never recursively removed.
        (default_home / "linked-library").symlink_to(v1, target_is_directory=True)
        assert "symlinked package directory" in cli("pkg", "remove", "linked-library", ok=False)
        cli("pkg", "install", v1)
        version_dir = default_home / "remove-library/1.0.0"
        stored_version = root / "external installed version"
        version_dir.rename(stored_version)
        version_dir.symlink_to(stored_version, target_is_directory=True)
        assert "symlinked package version" in cli("pkg", "remove", "remove-library", ok=False)
        assert (stored_version / "modules.index").exists()
        version_dir.unlink()
        stored_version.rename(version_dir)
        cli("pkg", "uninstall", "remove-library")

        # Native artifacts, extracted sources and caches belong to the version.
        fixtures = Path(__file__).resolve().parent / "fixtures"
        cli("pkg", "install", fixtures / "native_class_demo", "--version", "1.0.0", env=custom_env)
        native_dir = custom_home / "nat.box/1.0.0"
        assert (native_dir / "native-library.meta").is_file()
        assert (native_dir / ".native").is_dir()
        cli("pkg", "remove", "nat.box", env=custom_env)
        assert not (custom_home / "nat.box").exists()
        assert (fixtures / "native_class_demo/native/box.c").is_file()

        # Removing an unknown package must not create the installation home.
        missing_home = root / "missing home"
        assert "package is not installed" in cli("pkg", "remove", "absent",
            env=dict(environment, SPUTNIK_PKG_HOME=str(missing_home)), ok=False)
        assert not missing_home.exists()
        assert checkout_bytes == {p: p.read_bytes() for p in checkout_bytes}

    print("package_remove_cli_test ok")


if __name__ == "__main__":
    main()
