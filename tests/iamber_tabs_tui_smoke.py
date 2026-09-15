#!/usr/bin/env python3
"""Real-PTY regressions for iamber's persistent project tabs."""

import errno
import fcntl
import json
import codecs
import os
from pathlib import Path
import pty
import re
import select
import signal
import struct
import sys
import tempfile
import termios
import time


F6 = b"\x1b[17~"
F7 = b"\x1b[18~"
F8 = b"\x1b[19~"
# xterm-256color emits F4 in the SS3 form; CSI 13~ is not recognized by
# ncurses here and is interpreted as a standalone Escape followed by text.
F4 = b"\x1bOS"
F10 = b"\x1b[21~"
CTRL_S = b"\x13"


class Screen:
    """Minimal xterm screen model for assertions across curses redraws.

    Curses commonly repaints an edited line one character at a time using
    cursor-addressing escapes.  The PTY byte stream therefore does not
    contain the final source as a contiguous string.  Keep enough terminal
    state to assert on the rendered screen while retaining the raw stream for
    event/status assertions.
    """

    def __init__(self, rows, cols):
        self.rows = rows
        self.cols = cols
        self.cells = []
        self.row = 0
        self.col = 0
        self.saved = (0, 0)
        self.state = "normal"
        self.csi = ""
        self.decoder = codecs.getincrementaldecoder("utf-8")("replace")
        self._clear()

    def _clear(self):
        self.cells = [[" "] * self.cols for _ in range(self.rows)]

    def resize(self, rows, cols):
        old = self.cells
        self.rows, self.cols = rows, cols
        self.cells = [[" "] * cols for _ in range(rows)]
        for row in range(min(rows, len(old))):
            for col in range(min(cols, len(old[row]))):
                self.cells[row][col] = old[row][col]
        self.row = min(self.row, rows - 1)
        self.col = min(self.col, cols - 1)

    def _erase_line(self, mode):
        if mode == 1:
            start, end = 0, self.col
        elif mode == 2:
            start, end = 0, self.cols
        else:
            start, end = self.col, self.cols
        for col in range(max(0, start), min(self.cols, end)):
            self.cells[self.row][col] = " "

    def _erase_screen(self, mode):
        if mode == 2:
            self._clear()
        elif mode == 1:
            for row in range(self.row + 1):
                end = self.cols if row < self.row else self.col + 1
                for col in range(end):
                    self.cells[row][col] = " "
        else:
            for row in range(self.row, self.rows):
                start = self.col if row == self.row else 0
                for col in range(start, self.cols):
                    self.cells[row][col] = " "

    def _param(self, params, index, default=1):
        fields = params.split(";") if params else []
        if index >= len(fields) or fields[index] in ("", "?"):
            return default
        try:
            return int(fields[index].lstrip("?"))
        except ValueError:
            return default

    def _csi_command(self, params, command):
        # SGR, private modes, and scroll-region changes do not affect the
        # plain text assertions made by this test.
        if command == "A":
            self.row = max(0, self.row - self._param(params, 0))
        elif command in ("B", "e"):
            self.row = min(self.rows - 1, self.row + self._param(params, 0))
        elif command == "C":
            self.col = min(self.cols - 1, self.col + self._param(params, 0))
        elif command == "D":
            self.col = max(0, self.col - self._param(params, 0))
        elif command in ("E",):
            self.row = min(self.rows - 1, self.row + self._param(params, 0))
            self.col = 0
        elif command in ("F",):
            self.row = max(0, self.row - self._param(params, 0))
            self.col = 0
        elif command == "G":
            self.col = min(self.cols - 1, self._param(params, 0) - 1)
        elif command in ("d",):
            self.row = min(self.rows - 1, self._param(params, 0) - 1)
        elif command in ("H", "f"):
            self.row = min(self.rows - 1, self._param(params, 0) - 1)
            self.col = min(self.cols - 1, self._param(params, 1) - 1)
        elif command == "J":
            self._erase_screen(self._param(params, 0, 0))
        elif command == "K":
            self._erase_line(self._param(params, 0, 0))
        elif command == "P":
            count = min(self.cols - self.col, self._param(params, 0))
            line = self.cells[self.row]
            line[self.col:self.cols - count] = line[self.col + count:]
            line[self.cols - count:] = [" "] * count
        elif command == "@":
            count = min(self.cols - self.col, self._param(params, 0))
            line = self.cells[self.row]
            line[self.col + count:] = line[self.col:self.cols - count]
            line[self.col:self.col + count] = [" "] * count
        elif command == "X":
            count = min(self.cols - self.col, self._param(params, 0))
            for col in range(self.col, self.col + count):
                self.cells[self.row][col] = " "
        elif command == "s":
            self.saved = (self.row, self.col)
        elif command == "u":
            self.row, self.col = self.saved

    def _put(self, char):
        if self.col >= self.cols:
            self.col = 0
            self.row = min(self.rows - 1, self.row + 1)
        self.cells[self.row][self.col] = char
        self.col += 1

    def feed(self, data):
        for char in self.decoder.decode(data):
            if self.state == "normal":
                if char == "\x1b":
                    self.state = "escape"
                elif char == "\r":
                    self.col = 0
                elif char == "\n":
                    self.row = min(self.rows - 1, self.row + 1)
                elif char == "\b":
                    self.col = max(0, self.col - 1)
                elif char == "\t":
                    self.col = min(self.cols - 1, (self.col // 8 + 1) * 8)
                elif ord(char) >= 0x20 and char != "\x7f":
                    self._put(char)
            elif self.state == "escape":
                if char == "[":
                    self.state = "csi"
                    self.csi = ""
                elif char == "]":
                    self.state = "osc"
                elif char in "()*+-.\/":
                    self.state = "escape_intermediate"
                elif char in "78":
                    if char == "7":
                        self.saved = (self.row, self.col)
                    else:
                        self.row, self.col = self.saved
                    self.state = "normal"
                elif char == "M":
                    self.row = max(0, self.row - 1)
                    self.state = "normal"
                else:
                    # Charset selectors (ESC ( B), keypad modes, and other
                    # one-byte controls have no visible text effect here.
                    self.state = "normal"
            elif self.state == "escape_intermediate":
                self.state = "normal"
            elif self.state == "csi":
                if "@" <= char <= "~":
                    self._csi_command(self.csi, char)
                    self.state = "normal"
                else:
                    self.csi += char
            else:  # OSC: consume through BEL or ST.
                if char == "\x07":
                    self.state = "normal"
                elif char == "\x1b":
                    self.state = "osc_st"
            if self.state == "osc_st" and char == "\\":
                self.state = "normal"

    def text(self):
        return "\n".join("".join(row).rstrip() for row in self.cells)


class Terminal:
    """Small PTY wrapper matching the existing iamber project smoke test."""

    def __init__(self, binary, project, *options):
        self.pid, self.master = pty.fork()
        if self.pid == 0:
            argv = [binary, "--project", str(project), *options]
            os.execve(binary, argv, dict(os.environ, TERM="xterm-256color"))
        self.reaped = False
        self.output = bytearray()
        self.screen = Screen(36, 180)
        self.awaiting_output = False
        self.before_command_screen = None
        fcntl.ioctl(self.master, termios.TIOCSWINSZ,
                    struct.pack("HHHH", 36, 180, 0, 0))
        os.kill(self.pid, signal.SIGWINCH)

    def collect(self, duration=0.05):
        deadline = time.monotonic() + duration
        while time.monotonic() < deadline:
            ready, _, _ = select.select(
                [self.master], [], [], max(0, deadline - time.monotonic()))
            if not ready:
                return
            try:
                chunk = os.read(self.master, 65536)
            except OSError as error:
                if error.errno == errno.EIO:
                    return
                raise
            if not chunk:
                return
            self.output.extend(chunk)
            self.screen.feed(chunk)

    def send(self, keys):
        self.before_command_screen = self.screen.text()
        self.awaiting_output = True
        self.output.clear()
        os.write(self.master, keys)

    def resize(self, rows, cols):
        self.before_command_screen = self.screen.text()
        self.awaiting_output = True
        self.output.clear()
        self.screen.resize(rows, cols)
        fcntl.ioctl(self.master, termios.TIOCSWINSZ,
                    struct.pack("HHHH", rows, cols, 0, 0))
        os.kill(self.pid, signal.SIGWINCH)

    def expect(self, text, timeout=5):
        deadline = time.monotonic() + timeout
        rendered = text.decode("utf-8", "replace")
        while time.monotonic() < deadline:
            if self.awaiting_output:
                self.collect()
                if self.output:
                    self.awaiting_output = False
                    # Drain the rest of this redraw before consulting the
                    # screen, so a stale header cannot satisfy the assertion.
                    self.collect(0.02)
                continue
            if (text in self.output
                    or (rendered in self.screen.text()
                        and self.screen.text() != self.before_command_screen)):
                return
            self.collect()
        if text not in self.output and rendered not in self.screen.text():
            raise AssertionError(
                f"missing {text!r}: raw={bytes(self.output[-1600:])!r} "
                f"screen={self.screen.text()[-1600:]!r}")
        raise AssertionError(
            f"stale screen matched {text!r}: raw={bytes(self.output[-1600:])!r} "
            f"screen={self.screen.text()[-1600:]!r}")

    def expect_pattern(self, pattern, timeout=5):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.awaiting_output:
                self.collect()
                if self.output:
                    self.awaiting_output = False
                    self.collect(0.02)
                continue
            if (re.search(pattern, self.output) is not None
                    or (re.search(pattern, self.screen.text().encode()) is not None
                        and self.screen.text() != self.before_command_screen)):
                return
            self.collect()
        if (re.search(pattern, self.output) is None
                and re.search(pattern, self.screen.text().encode()) is None):
            raise AssertionError(
                f"missing pattern {pattern!r}: raw={bytes(self.output[-1600:])!r} "
                f"screen={self.screen.text()[-1600:]!r}")
        raise AssertionError(
            f"stale screen matched {pattern!r}: raw={bytes(self.output[-1600:])!r} "
            f"screen={self.screen.text()[-1600:]!r}")

    def expect_current(self, tab, dirty=False, stale=False, timeout=5):
        marker = (rb"\*" if dirty else rb"") + (rb"!env" if stale else rb"")
        self.expect_pattern(rb"\[" + re.escape(tab) + marker + rb"\]", timeout)

    def expect_dirty(self, tab, stale=False, timeout=5):
        self.expect_current(tab, dirty=True, stale=stale, timeout=timeout)

    def leave_edit(self):
        """Synchronize a standalone Escape before sending a function key."""
        self.send(b"\x1b")
        self.expect(b"NAV")

    def exit(self):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            self.collect()
            ended, status = os.waitpid(self.pid, os.WNOHANG)
            if ended:
                self.reaped = True
                if not os.WIFEXITED(status) or os.WEXITSTATUS(status) != 0:
                    raise AssertionError(f"project terminal exited {status}")
                return
        raise AssertionError("project terminal did not exit")

    def close(self):
        if not self.reaped:
            try:
                os.kill(self.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            os.waitpid(self.pid, 0)
        os.close(self.master)


def write_fixture(root):
    project = root / "tabs.amberbook"
    modules = project / "modules"
    modules.mkdir(parents=True)
    models = (
        "class A:\n"
        "  def init(@a)\n"
        "  def value(): @a\n"
        "export A\n"
    )
    (modules / "models.am").write_text(models)
    (modules / "unused.am").write_text("class Unused:\n  def init()\nexport Unused\n")
    document = {
        "format": "amber-notebook",
        "version": 1,
        "title": "tabs",
        "active_sheet": "main",
        "sheets": [
            {
                "id": "main",
                "title": "Main",
                "cells": [
                    {"id": "1", "kind": "code", "source": "a = A(7)\n",
                     "mode": "watch"},
                    {"id": "2", "kind": "code", "source": "a.value()\n",
                     "mode": "watch"},
                ],
            },
            {
                "id": "other",
                "title": "Other",
                "cells": [
                    {"id": "3", "kind": "code", "source": "2\n",
                     "mode": "watch"},
                ],
            },
        ],
        "modules": [
            {"id": "models", "path": "modules/models.am"},
            {"id": "unused", "path": "modules/unused.am"},
        ],
        "auto_imports": ["models"],
    }
    document_file = project / "project.json"
    document_file.write_text(json.dumps(document, indent=2) + "\n")
    return project, document_file, modules / "models.am", models


def prefix_source(terminal, marker):
    """Prefix the current code buffer without evaluating it."""
    terminal.send(b"e" + marker + b"\n")
    terminal.expect(marker)


def tab_navigation_and_inactive_dirty(binary, root):
    project, document_file, module_file, original_module = write_fixture(root)

    # --sheet chooses the initial sheet tab, even though the manifest's
    # active_sheet is main.
    terminal = Terminal(binary, project, "--sheet", "other")
    try:
        terminal.expect_current(b"sheet:other")
        terminal.send(b"q")
        terminal.exit()
    finally:
        terminal.close()

    original_document = json.loads(document_file.read_text())
    original_other = original_document["sheets"][1]["cells"][0]["source"]

    terminal = Terminal(binary, project, "--sheet", "main")
    try:
        terminal.expect_current(b"sheet:main")
        terminal.expect(b"sheet:other")
        terminal.expect(b"module:models")
        terminal.expect(b"module:unused")

        # F7/F8 are previous/next with wrapping. The main edit must remain an
        # edit when it is inactive; switching must not run or save it.
        terminal.send(F7)
        terminal.expect_current(b"module:unused")
        terminal.send(F8)
        terminal.expect_current(b"sheet:main")
        prefix_source(terminal, b"# main-edit")
        terminal.expect(b"EDIT")

        terminal.send(F8)
        terminal.expect_current(b"sheet:other")
        terminal.send(F7)
        terminal.expect_current(b"sheet:main", dirty=True)
        terminal.expect(b"# main-edit")
        terminal.expect(b"EDIT")

        # Save is scoped to the active sheet tab. It must not write any other
        # tab's dirty state or module source.
        terminal.send(CTRL_S)
        terminal.expect(b"project saved")
        saved_document = json.loads(document_file.read_text())
        assert "# main-edit" in saved_document["sheets"][0]["cells"][0]["source"]
        assert saved_document["sheets"][1]["cells"][0]["source"] == original_other
        assert module_file.read_text() == original_module

        terminal.leave_edit()
        terminal.send(F8)
        terminal.expect_current(b"sheet:other")
        prefix_source(terminal, b"# other-dirty")
        terminal.leave_edit()

        terminal.send(F8)
        terminal.expect_current(b"module:models")
        prefix_source(terminal, b"# module-dirty")
        terminal.leave_edit()

        # Return through the inactive dirty tabs and check their buffers are
        # still present. This also exercises nav-mode switching.
        terminal.send(F7)
        terminal.expect_current(b"sheet:other", dirty=True)
        terminal.expect(b"# other-dirty")
        terminal.expect_dirty(b"sheet:other")
        terminal.send(F8)
        terminal.expect_current(b"module:models", dirty=True)
        terminal.expect(b"# module-dirty")
        terminal.expect_dirty(b"module:models")

        # A sheet Apply consumes only saved module bytes. Any dirty module tab
        # blocks it without clearing the current sheet's prior state.
        terminal.send(F7)
        terminal.expect_current(b"sheet:other", dirty=True)
        terminal.send(F7)
        terminal.expect_current(b"sheet:main")
        terminal.send(F6)
        terminal.expect(b"save modified module tabs before Apply")

        # Exercise wrapping from the last declared tab back to the first.
        terminal.send(F8)
        terminal.expect_current(b"sheet:other", dirty=True)
        terminal.send(F8)
        terminal.expect_current(b"module:models", dirty=True)
        terminal.send(F8)
        terminal.expect_current(b"module:unused")
        terminal.send(F8)
        terminal.expect_current(b"sheet:main")

        # Quit must see dirty inactive tabs, and Q is the explicit discard for
        # all of them. The on-disk inactive buffers must remain untouched.
        terminal.send(F10)
        terminal.expect(b"unsaved changes")
        terminal.send(b"Q")
        terminal.exit()
    finally:
        terminal.close()

    after_quit = json.loads(document_file.read_text())
    assert "# main-edit" in after_quit["sheets"][0]["cells"][0]["source"]
    assert after_quit["sheets"][1]["cells"][0]["source"] == original_other
    assert module_file.read_text() == original_module


def module_save_apply_and_failed_apply(binary, root):
    project, document_file, module_file, original_module = write_fixture(root)

    # --module selects the module tab initially; opening it does not run or
    # install the source in sheets.
    terminal = Terminal(binary, project, "--module", "models")
    try:
        terminal.expect_current(b"module:models")
        terminal.expect(b"module opened; not run or installed in sheets")
        assert b"=> <instance A>" not in terminal.output

        # Save the module, then navigate to its sheet and explicitly apply the
        # saved module exports. The sheet result proves A was installed.
        prefix_source(terminal, b"# module-saved")
        terminal.send(F4)
        terminal.expect(b"module source saved")
        assert module_file.read_text().startswith("# module-saved\n")
        assert json.loads(document_file.read_text())["sheets"][0]["cells"][0]["source"] == "a = A(7)\n"

        terminal.leave_edit()
        terminal.send(F7)
        terminal.expect_current(b"sheet:other")
        terminal.send(F7)
        terminal.expect_current(b"sheet:main")
        terminal.send(F6)
        terminal.expect(b"environment applied")
        terminal.expect(b"=> 7")

        # A successful environment change marks the other sheet stale. Running
        # it directly is blocked; explicit F6 catches that sheet up.
        terminal.send(F8)
        terminal.expect_current(b"sheet:other", stale=True)
        terminal.expect(b"environment changed; F6 Apply this sheet")
        terminal.send(b"\n")
        terminal.expect(b"environment changed; F6 Apply this sheet")
        assert b"=> 2" not in terminal.output
        # Make this Apply a real environment transition so the previously
        # applied main sheet becomes stale after the other sheet catches up.
        module_file.write_text("# environment-change\n" + module_file.read_text())
        terminal.send(F6)
        terminal.expect_current(b"sheet:other")
        terminal.expect(b"environment applied")
        terminal.expect(b"=> 2")

        # Applying the other sheet makes main stale but preserves its result;
        # Apply main explicitly before testing a failed module initialization.
        terminal.send(F7)
        terminal.expect_current(b"sheet:main", stale=True)
        terminal.expect(b"=> 7")
        terminal.expect(b"environment changed; F6 Apply this sheet")
        terminal.send(F6)
        terminal.expect_current(b"sheet:main")
        terminal.expect(b"environment applied")
        terminal.expect(b"=> 7")

        # A tab switch must preserve the evaluated output and must not reopen
        # or initialize the module session.
        terminal.send(F8)
        terminal.expect_current(b"sheet:other")
        terminal.send(F7)
        terminal.expect_current(b"sheet:main")
        terminal.expect(b"=> 7")
        assert b"module opened; not run or installed in sheets" not in terminal.output

        # A real resize redraws the selected tab without executing anything;
        # once redrawn, a genuinely idle PTY remains quiet.
        terminal.resize(34, 180)
        terminal.expect_current(b"sheet:main")
        terminal.expect(b"=> 7")
        terminal.output.clear()
        terminal.collect(0.25)
        if terminal.output:
            raise AssertionError(
                f"unexpected idle output after tab switch: {bytes(terminal.output)!r}")

        # A failed environment Apply is transactional: the old result remains
        # visible instead of being cleared by the failed module init.
        module_file.write_text('raise "apply rollback"\n')
        terminal.send(F6)
        terminal.expect(b"environment Apply failed")
        terminal.expect(b"=> 7")

        terminal.send(b"q")
        terminal.exit()
    finally:
        terminal.close()

    # The failed Apply does not rewrite the project manifest or module source.
    assert json.loads(document_file.read_text())["sheets"][0]["cells"][0]["source"] == "a = A(7)\n"
    assert module_file.read_text() == 'raise "apply rollback"\n'


def main():
    binary = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "build/iamber")
    with tempfile.TemporaryDirectory(prefix="iamber-tabs-tui-") as temporary:
        tab_navigation_and_inactive_dirty(binary, Path(temporary) / "tabs-a")
        module_save_apply_and_failed_apply(binary, Path(temporary) / "tabs-b")
    print("iamber_tabs_tui_smoke ok")


if __name__ == "__main__":
    main()
