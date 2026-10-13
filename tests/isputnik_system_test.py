#!/usr/bin/env python3
"""Exercise native imports and explicit process grants through notebook CLI."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    binary = Path(sys.argv[1] if len(sys.argv) > 1 else "build/isputnik").resolve()
    with tempfile.TemporaryDirectory(prefix="isputnik-system-") as temporary:
        project = Path(temporary) / "test.sputnikbook"
        subprocess.run([str(binary), "--new-project", str(project)],
                       check=True, capture_output=True, timeout=10)
        path = project / "project.json"
        document = json.loads(path.read_text())
        document["sheets"][0]["cells"] = [
            {"id": "1", "kind": "code", "mode": "watch", "source":
             'import system\nsystem.cmd"/usr/bin/printf notebook".output'},
            {"id": "2", "kind": "code", "mode": "watch", "source":
             'from system import cmd\ncmd\'printf "%s" #{"tag"}\'.output'},
            {"id": "3", "kind": "code", "mode": "watch", "source":
             'import system\ncommand = system.cmd"/usr/bin/printf output"\n'
             '24.times.threaded(4).map: command.output .group: $it .transform_values: $it.size\n'
             'print $_'},
        ]
        path.write_text(json.dumps(document))
        saved = path.read_bytes()
        for grant, permitted in ((None, False),
                                 ("process.spawn=/bin/cat", False),
                                 ("process.spawn=/usr/bin/printf", True),
                                 ("process.spawn", True)):
            command = [str(binary)]
            if grant:
                command += ["--grant", grant]
            command += ["--project", str(project), "--run-sheet"]
            result = subprocess.run(command, capture_output=True, text=True, timeout=15)
            if permitted:
                assert result.returncode == 0, result.stdout + result.stderr
                assert 'cell 1 => "notebook"' in result.stdout, result.stdout
                assert 'cell 2 => "tag"' in result.stdout, result.stdout
                assert '{output: 24}' in result.stdout, result.stdout
            else:
                assert result.returncode != 0 and "CapabilityError" in result.stderr, result
                assert "NB1003" not in result.stderr and "NB1002" not in result.stderr
            assert path.read_bytes() == saved, "running a notebook must not persist host grants"
        # The terminal project runner accepts ordinary definitions across cells.
        document["sheets"][0]["cells"] = [
            {"id": "1", "kind": "code", "mode": "watch", "source":
             'class Box:\n  def init(@value)\n  def answer(): @value + 1\n'},
            {"id": "2", "kind": "code", "mode": "watch", "source":
             'box = Box(41)\nbox.answer()\n'},
            {"id": "3", "kind": "code", "mode": "watch", "source":
             'def twice(value): value * 2\ntwice(box.answer())\n'},
        ]
        path.write_text(json.dumps(document))
        saved = path.read_bytes()
        for _ in range(2):
            result = subprocess.run([str(binary), "--project", str(project), "--run-sheet"],
                                    capture_output=True, text=True, timeout=15)
            assert result.returncode == 0, result.stdout + result.stderr
            assert "cell 2 => 42" in result.stdout and "cell 3 => 84" in result.stdout, result.stdout
            assert path.read_bytes() == saved, "definition execution must not save the project"
        document["sheets"][0]["cells"][0]["source"] = 'class Box:\n  def init(@value)\n  def answer(): 1 / 0\n'
        path.write_text(json.dumps(document))
        result = subprocess.run([str(binary), "--project", str(project), "--run-sheet"],
                                capture_output=True, text=True, timeout=15)
        assert result.returncode != 0 and "ZeroDivisionError" in result.stderr, result.stderr
    print("isputnik system CLI tests passed")


if __name__ == "__main__":
    main()
