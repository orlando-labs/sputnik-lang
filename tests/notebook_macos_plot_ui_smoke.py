#!/usr/bin/env python3
"""Render real 2D/3D plots in a native notebook window, using an isolated fixture."""
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    executable, plot_source = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    isolated = "--isolated-worker" in sys.argv[3:]
    flags = ["--isolated-worker"] if isolated else []
    script = Path(__file__).resolve().parents[1] / "tools/notebook-macos/make-plot-sample.py"
    spec = importlib.util.spec_from_file_location("plot_sample", script)
    sample = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(sample)
    with tempfile.TemporaryDirectory(prefix="amber-notebook-plot-ui-") as directory:
        project = Path(directory) / "Plots.amberbook"
        sample.create_sample(project, plot_source)
        assert not (project / "modules").exists(), "Default sample must link, not copy the package"
        original = (project / "project.json").read_bytes()
        png = Path(directory) / "plots.png"
        result = subprocess.run([str(executable), *flags, "--smoke-test", str(project),
                                 "--expect-figure-panels", "2", "--expect-interactive-plots", "1", "--snapshot-png", str(png)],
                                capture_output=True, text=True, timeout=90)
        print(result.stdout, end="")
        if result.returncode != 0:
            print(result.stderr, file=sys.stderr)
            raise SystemExit(result.returncode)
        assert png.read_bytes().startswith(b"\x89PNG\r\n\x1a\n")
        assert png.stat().st_size > 10000
        assert (project / "project.json").read_bytes() == original, "Rendering must not save outputs into source"
        if isolated:
            # Preview deliberately rejects project inputs instead of falling
            # back to a local VM. The board's input bridge is a later stage.
            return
        board = Path(directory) / "Board.amberbook"
        sample.create_sample(board, plot_source, board=True)
        assert not (board / "modules").exists(), "Board uses the same external dependency"
        original = (board / "project.json").read_bytes()
        result = subprocess.run([str(executable), "--smoke-test", str(board), "--smoke-board", "dashboard",
                                 "--expect-figure-panels", "1", "--expect-interactive-plots", "1", "--snapshot-png", str(png)],
                                capture_output=True, text=True, timeout=90)
        print(result.stdout, end="")
        if result.returncode != 0:
            print(result.stderr, file=sys.stderr)
            raise SystemExit(result.returncode)
        assert png.stat().st_size > 10000
        assert (board / "project.json").read_bytes() == original, "Runtime input edits must not save defaults"


if __name__ == "__main__":
    main()
