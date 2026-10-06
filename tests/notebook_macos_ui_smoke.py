#!/usr/bin/env python3
"""Exercise the real AppKit/SwiftUI host with an isolated on-disk fixture."""
import json
import pathlib
import subprocess
import sys
import tempfile


def main():
    executable = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="amber-notebook-ui-") as directory:
        root = pathlib.Path(directory)
        project = root / "First notebook.amberbook"
        project.mkdir()
        paragraphs = ["A live notebook\n", "1.\tFirst item\n", "2.\tSecond item\n"]
        records, offset = [], 0
        for index, paragraph in enumerate(paragraphs):
            records.append({"start": offset, "length": len(paragraph),
                            **({"style": "heading1"} if index == 0 else {"list": "numbered"})})
            offset += len(paragraph)
        table_start = offset
        table_cells = []
        for content in ["Name\n", "Value\n"]:
            table_cells.append({"start": offset, "length": len(content)})
            offset += len(content)
        manifest = {
            "format": "amber-notebook", "version": 1,
            "title": "First notebook", "active_sheet": "main",
            "sheets": [{"id": "main", "title": "Getting started", "cells": [
                {"id": "3", "kind": "text", "source": "".join(paragraphs) + "Name\nValue\n",
                 "formatting": {"version": 2, "runs": [], "paragraphs": records,
                                "tables": [{"start": table_start, "length": offset - table_start,
                                            "columns": 2, "cells": table_cells}]}},
                {"id": "1", "kind": "code", "source": "x = 21\nx\n", "mode": "watch"},
                {"id": "4", "kind": "text", "source": "Published variable: {{x}}\n"},
                {"id": "2", "kind": "code", "source": "x * 2\n", "mode": "watch"},
            ]}], "modules": [], "auto_imports": [],
        }
        # Test data generation is deliberately separate from user projects.
        source = json.dumps(manifest)
        (project / "project.json").write_text(source)
        png = root / "notebook.png"
        flags = ["--isolated-worker"] if "--isolated-worker" in sys.argv[2:] else []
        result = subprocess.run([str(executable), *flags, "--smoke-test", str(project),
                                 "--snapshot-png", str(png)],
                                capture_output=True, text=True, timeout=60)
        print(result.stdout, end="")
        if result.returncode != 0:
            print(result.stderr, file=sys.stderr)
            if result.returncode < 0:
                print(f"Native host terminated by signal {-result.returncode}. "
                      "Run this GUI test in a logged-in macOS desktop session; "
                      "a restricted process sandbox can deny AppKit registration.",
                      file=sys.stderr)
            raise SystemExit(result.returncode)
        assert "notebook macOS UI smoke ok" in result.stdout
        assert png.read_bytes().startswith(b"\x89PNG\r\n\x1a\n")
        assert png.stat().st_size > 10_000, "native view did not render"
        assert (project / "project.json").read_text() == source, "Run must not save"


if __name__ == "__main__":
    main()
