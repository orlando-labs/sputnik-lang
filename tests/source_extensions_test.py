#!/usr/bin/env python3
"""Each public source suffix supports the same run and native build commands."""

from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    binary = str(Path(sys.argv[1] if len(sys.argv) > 1 else "build/sputnik").resolve())

    def run(*args):
        result = subprocess.run(args, text=True, capture_output=True, timeout=180)
        assert result.returncode == 0, result.stdout + result.stderr
        return result.stdout.strip()

    with tempfile.TemporaryDirectory(prefix="sputnik-extensions-") as directory:
        for suffix in (".s", ".spu", ".sputnik"):
            source = Path(directory) / ("answer" + suffix)
            source.write_text("40 + 2\n")
            assert run(binary, str(source)) == "42", suffix
            assert run(binary, "run", str(source)) == "42", suffix
            executable = Path(directory) / ("answer-" + suffix[1:])
            run(binary, "build", str(source), "--require-full-native", "-o", str(executable))
            assert run(str(executable)) == "42", suffix
    print("source_extensions_test ok: .s, .spu, .sputnik")


if __name__ == "__main__":
    main()
