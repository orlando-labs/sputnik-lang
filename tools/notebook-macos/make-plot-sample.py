#!/usr/bin/env python3
"""Create a notebook linked to a local amber-plot package (copy only with --bundle)."""
import argparse
import json
from pathlib import Path
import shutil
import os


def create_sample(output: Path, plot_source: Path, board=False, bundle=False):
    # Never overwrite a notebook the user may have edited.
    if output.exists():
        raise FileExistsError(f"Refusing to replace existing project: {output}")
    package = plot_source.resolve().parent.parent
    if not (package / "amber.build.yaml").is_file():
        raise ValueError(f"No amber.build.yaml in plot package root: {package}")
    output.mkdir(parents=True)
    try:
        if bundle:
            (output / "modules").mkdir()
            (output / "modules" / "plot.am").write_text(plot_source.read_text(encoding="utf-8"), encoding="utf-8")
        source = '''import plot
p = plot.figure(width: 800, height: 540).line([[0, 0], [1, 1], [2, 4], [3, 9]])
q = plot.figure3d(width: 800, height: 540).surface([[0, 1, 0], [1, 3, 1], [0, 1, 0]])
notebook.show(q, caption: "Surface · amber-plot 3D", order: 2)
notebook.show(p, caption: "Quadratic curve · amber-plot 2D", order: 1)
42
'''
        document = {
            "format": "amber-notebook", "version": 3,
            "title": "Amber Plot · Figures", "active_sheet": "main",
            "sheets": [{"id": "main", "title": "Figures", "cells": [
                {"id": "1", "kind": "code", "source": source, "mode": "watch"}
            ]}],
            "modules": [{"id": "plot", "path": "modules/plot.am"}] if bundle else [],
            "auto_imports": [], "inputs": [], "boards": [],
            "dependencies": [] if bundle else [{"path": os.path.relpath(package, output.resolve())}],
        }
        if board:
            document.update(title="Interactive Curve", inputs=[
                {"id": "gain", "title": "Gain", "type": "number", "default": 1.0, "minimum": 0, "maximum": 10},
                {"id": "enabled", "title": "Enabled", "type": "boolean", "default": True},
                {"id": "label", "title": "Caption", "type": "string", "default": "Quadratic curve"},
            ], boards=[{"id": "dashboard", "title": "Interactive Curve", "sheet": "main", "columns": 2, "components": [
                {"id": "gain", "kind": "input", "title": "Curve gain", "input": "gain"},
                {"id": "value", "kind": "text", "title": "Computed value", "cell": "1", "binding": "value"},
                {"id": "plot", "kind": "plot", "title": "Live figure", "cell": "1"},
                {"id": "run", "kind": "run", "title": "Refresh manual result", "cell": "2"},
                {"id": "caption", "kind": "input", "title": "Figure caption", "input": "label"},
                {"id": "manual", "kind": "text", "title": "Manual snapshot", "cell": "2"},
            ]}])
            document["sheets"][0].update(title="Controller", cells=[
                {"id": "1", "kind": "code", "mode": "watch", "source": '''import plot
gain = notebook.input("gain")
value = gain * 42
p = plot.figure(width: 800, height: 480).line([[0, 0], [1, gain], [2, 4 * gain], [3, 9 * gain]])
notebook.show(p, caption: notebook.input("label"))
value
'''},
                {"id": "2", "kind": "code", "mode": "manual", "source": 'notebook.input("gain") * 42\n'},
                {"id": "3", "kind": "code", "mode": "watch", "source": "42\n"},
            ])
        (output / "project.json").write_text(
            json.dumps(document, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    except Exception:
        shutil.rmtree(output)  # Only the new directory created by this call.
        raise


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--plot-source", type=Path, default=Path("../amber-plot/src/plot.am"))
    parser.add_argument("--board", action="store_true", help="Create an interactive dashboard sample")
    parser.add_argument("--bundle", action="store_true", help="Explicitly copy plot.am into modules instead of linking its directory")
    args = parser.parse_args()
    create_sample(args.output, args.plot_source, board=args.board, bundle=args.bundle)
    print(args.output.resolve())
