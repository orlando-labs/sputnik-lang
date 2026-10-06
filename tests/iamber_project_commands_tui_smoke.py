#!/usr/bin/env python3
"""Real-PTY regressions for iamber's project command prompt."""

import fcntl
import json
import os
from pathlib import Path
import pty
import signal
import struct
import subprocess
import sys
import tempfile
import termios

from iamber_tabs_tui_smoke import CTRL_S, F6, F7, Screen, Terminal


# xterm-256color emits F3 in the SS3 form.  The existing tabs smoke test uses
# the same form for F4, and importing its Screen/Terminal keeps these checks
# valid across partial ANSI redraws.
F3 = b"\x1bOR"


class ScratchTerminal(Terminal):
    """The imported PTY harness with no --project argument."""

    def __init__(self, binary):
        self.pid, self.master = pty.fork()
        if self.pid == 0:
            os.execve(binary, [binary], dict(os.environ, TERM="xterm-256color"))
        self.reaped = False
        self.output = bytearray()
        self.screen = Screen(36, 180)
        self.awaiting_output = False
        self.before_command_screen = None
        fcntl.ioctl(self.master, termios.TIOCSWINSZ,
                    struct.pack("HHHH", 36, 180, 0, 0))
        os.kill(self.pid, signal.SIGWINCH)


def make_project(binary, directory):
    directory.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([binary, "--new-project", str(directory)], check=True,
                   capture_output=True)
    return directory / "project.json"


def read_document(document_file):
    return json.loads(document_file.read_text())


def open_command(terminal):
    terminal.send(F3)
    terminal.expect(b"new-sheet <id> [title]")


def run_command(terminal, command, status=None):
    open_command(terminal)
    terminal.send(command.encode("utf-8") + b"\n")
    if status is not None:
        terminal.expect(status)


def type_module_source(terminal):
    # Newline insertion supplies the two-space class indentation.  Remove the
    # final auto-indent before the top-level export.
    source = (b"eclass A:\n"
              b"def init(@a)\n"
              b"def value(): @a\n"
              b"\x08\x08export A")
    terminal.send(source)
    terminal.expect(b"EDIT")
    terminal.send(CTRL_S)
    terminal.expect(b"module source saved")


def project_commands_and_apply(binary, root):
    project = root / "commands.amberbook"
    document_file = make_project(binary, project)
    module_file = project / "modules" / "models.am"

    terminal = Terminal(binary, project)
    try:
        terminal.expect_current(b"sheet:main")

        # F3 is modal in edit mode and does not evaluate the cell.  Create the
        # module, then type/save a source body in the newly selected module.
        terminal.send(b"e# main-before-module\n")
        terminal.expect(b"# main-before-module")
        run_command(terminal, "new-module models",
                    b"module created; structure saved; not run")
        terminal.expect_current(b"module:models")
        document = read_document(document_file)
        assert [module["id"] for module in document["modules"]] == ["models"]
        assert document["auto_imports"] == []
        assert module_file.read_text() == "package models\n"
        assert b"=>" not in terminal.output

        type_module_source(terminal)
        expected_module = (
            "package models\n"
            "class A:\n"
            "  def init(@a)\n"
            "  def value(): @a\n"
            "export A"
        )
        assert module_file.read_text() == expected_module

        # The command changes only project structure.  It keeps this module
        # tab selected and leaves the saved source untouched.
        run_command(
            terminal, "auto-import models on",
            b"auto-import enabled; structure saved; F6 Apply to use changes",
        )
        terminal.expect_current(b"module:models")
        terminal.expect(b"auto-import:on")
        assert module_file.read_text() == expected_module
        assert read_document(document_file)["auto_imports"] == ["models"]
        assert b"=>" not in terminal.output

        # F7 switches directly from the module's edit mode.  Type a Watch
        # expression on the main sheet and explicitly Apply the new import.
        terminal.send(F7)
        terminal.expect_current(b"sheet:main", dirty=True)
        # Main was already in edit mode when F3 created the module, so its
        # preserved dirty buffer accepts the expression directly.
        terminal.send(b"a = A(7)\na.value()")
        terminal.expect(b"a.value()")
        terminal.send(F6)
        terminal.expect(b"environment applied")
        terminal.expect(b"=> 7")

        # Turning auto-import off persists the manifest setting but leaves the
        # current environment alone until another F6 Apply.
        run_command(
            terminal, "auto-import models off",
            b"auto-import disabled; structure saved; F6 Apply to use changes",
        )
        terminal.expect_current(b"sheet:main", dirty=True)
        assert read_document(document_file)["auto_imports"] == []
        assert module_file.read_text() == expected_module
        # The previous result remains visible before the explicit Apply.
        terminal.expect(b"=> 7")

        # F6 is intentionally the boundary at which the saved auto-import
        # setting is consumed.  The source remains a dirty in-memory buffer.
        terminal.send(F6)
        terminal.expect(b"environment Apply failed:")
        terminal.expect_current(b"sheet:main", dirty=True)
        # A failed candidate environment must not erase the successful result
        # produced by the previously applied environment.
        terminal.expect(b"=> 7")
        assert read_document(document_file)["auto_imports"] == []

        terminal.send(b"\x1b[21~")
        terminal.expect(b"unsaved changes")
        terminal.send(b"Q")
        terminal.exit()
    finally:
        terminal.close()


def sheet_creation_and_failures(binary, root):
    project = root / "structure.amberbook"
    document_file = make_project(binary, project)
    outside_module = project.parent / "x.am"
    outside_module.write_text("sentinel\n")

    terminal = Terminal(binary, project)
    try:
        terminal.expect_current(b"sheet:main")
        original_document_bytes = document_file.read_bytes()

        # Keep an unsaved source edit in main while the structural command
        # saves only the manifest and selects the inserted sheet.
        terminal.send(b"e# dirty inactive source\n")
        terminal.expect(b"# dirty inactive source")
        run_command(terminal, "new-sheet analysis My analysis",
                    b"sheet created; structure saved; not evaluated")
        terminal.expect_current(b"sheet:analysis")
        terminal.expect(b"sheet:main*")
        document = read_document(document_file)
        assert [sheet["id"] for sheet in document["sheets"]] == [
            "main", "analysis"
        ]
        analysis = document["sheets"][1]
        assert analysis["title"] == "My analysis"
        assert len(analysis["cells"]) == 1
        assert analysis["cells"][0]["source"] == ""
        assert analysis["cells"][0]["mode"] == "watch"
        assert document["modules"] == []
        # The inactive dirty main buffer was not folded into the structural
        # manifest save.
        assert document["sheets"][0]["cells"][0]["source"] == ""

        # Duplicate IDs and traversal-like IDs fail transactionally with the
        # required project: prefix and leave both old buffers and disk intact.
        terminal.send(F7)
        terminal.expect_current(b"sheet:main", dirty=True)
        before_duplicate = document_file.read_bytes()
        run_command(terminal, "new-sheet analysis duplicate")
        terminal.expect(b"project:")
        assert document_file.read_bytes() == before_duplicate
        terminal.expect_current(b"sheet:main", dirty=True)

        run_command(terminal, "new-module ../x")
        terminal.expect(b"project:")
        assert document_file.read_bytes() == before_duplicate
        assert outside_module.read_text() == "sentinel\n"
        assert not (project / "modules" / "x.am").exists()
        terminal.expect_current(b"sheet:main", dirty=True)

        # Escape closes the prompt without creating a tab or touching disk.
        before_cancel = document_file.read_bytes()
        open_command(terminal)
        terminal.send(b"new-sheet cancelled\x1b")
        terminal.expect(b"project command cancelled")
        terminal.expect_current(b"sheet:main", dirty=True)
        assert document_file.read_bytes() == before_cancel
        assert b"sheet:cancelled" not in terminal.screen.text().encode()
        assert document_file.read_bytes() != b"" and original_document_bytes != b""

        terminal.send(b"\x1b[21~")
        terminal.expect(b"unsaved changes")
        terminal.send(b"Q")
        terminal.exit()
    finally:
        terminal.close()


def no_project_f3(binary):
    terminal = ScratchTerminal(binary)
    try:
        terminal.expect(b"NAV")
        terminal.send(F3)
        terminal.expect(b"project commands require --project")
        assert b"new-sheet <id> [title]" not in terminal.output
        terminal.send(b"q")
        terminal.exit()
    finally:
        terminal.close()


def directory_dependency_commands(binary, root):
    project = root / "linked.amberbook"
    document_file = make_project(binary, project)
    package = root / "Local package"
    package.mkdir()
    (package / "amber.toml").write_text(
        '[package]\nname = "sample"\nversion = "1.0.0"\nroot = "sample"\n'
        '[[modules]]\nname = "sample"\npath = "sample.am"\n')
    source = "package sample\nexport answer\ndef answer(): 42\n"
    (package / "sample.am").write_text(source)
    document = read_document(document_file)
    document["sheets"][0]["cells"][0]["source"] = "import sample\nsample.answer()\n"
    document_file.write_text(json.dumps(document))
    terminal = Terminal(binary, project)
    try:
        terminal.expect_current(b"sheet:main")
        run_command(terminal, "add-dependency ../Local package", b"dependency linked;")
        document = read_document(document_file)
        assert document["version"] == 3 and document["modules"] == []
        assert document["dependencies"] == [{"path": "../Local package", "auto_import": False}]
        assert not (project / "modules").exists()
        terminal.send(F6)
        terminal.expect(b"=> 42")
        info = subprocess.run([binary, "--project-info", str(project)], check=True, capture_output=True, text=True)
        assert "../Local package" in info.stdout
        run = subprocess.run([binary, "--project", str(project), "--run-sheet"], check=True, capture_output=True, text=True)
        assert "42" in run.stdout
        run_command(terminal, "remove-dependency ../Local package", b"dependency unlinked;")
        assert read_document(document_file)["dependencies"] == []
        assert (package / "sample.am").read_text() == source
        terminal.send(b"q")
        terminal.exit()
    finally:
        terminal.close()


def main():
    binary = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "build/iamber")
    with tempfile.TemporaryDirectory(prefix="iamber-project-commands-tui-") as temporary:
        root = Path(temporary)
        no_project_f3(binary)
        project_commands_and_apply(binary, root / "apply")
        sheet_creation_and_failures(binary, root / "structure")
        directory_dependency_commands(binary, root / "dependencies")
    print("iamber_project_commands_tui_smoke ok")


if __name__ == "__main__":
    main()
