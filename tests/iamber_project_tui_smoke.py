#!/usr/bin/env python3
"""Project open/save/quit regressions against an isolated real curses child."""

import errno
import fcntl
import json
import os
from pathlib import Path
import pty
import select
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import time


class Terminal:
    def __init__(self, binary, project):
        self.pid, self.master = pty.fork()
        if self.pid == 0:
            os.execve(binary, [binary, "--project", str(project)],
                      dict(os.environ, TERM="xterm-256color"))
        self.reaped = False
        self.output = bytearray()
        fcntl.ioctl(self.master, termios.TIOCSWINSZ,
                    struct.pack("HHHH", 30, 160, 0, 0))
        os.kill(self.pid, signal.SIGWINCH)

    def collect(self, duration=0.05):
        deadline = time.monotonic() + duration
        while time.monotonic() < deadline:
            ready, _, _ = select.select([self.master], [], [],
                                        max(0, deadline - time.monotonic()))
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

    def send(self, keys):
        self.output.clear()
        os.write(self.master, keys)

    def expect(self, text, timeout=5):
        deadline = time.monotonic() + timeout
        while text not in self.output and time.monotonic() < deadline:
            self.collect()
        if text not in self.output:
            raise AssertionError(f"missing {text!r}: {bytes(self.output[-2400:])!r}")

    def exit(self):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            self.collect()
            ended, status = os.waitpid(self.pid, os.WNOHANG)
            if ended:
                self.reaped = True
                assert os.WIFEXITED(status) and os.WEXITSTATUS(status) == 0, status
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


def main():
    binary = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "build/iamber")
    with tempfile.TemporaryDirectory(prefix="iamber-project-tui-") as temporary:
        project = Path(temporary) / "scratch.amberbook"
        subprocess.run([binary, "--new-project", str(project)], check=True,
                       capture_output=True)
        assert subprocess.run([binary, "--new-project", str(project)],
                              capture_output=True).returncode != 0
        document_file = project / "project.json"
        document = json.loads(document_file.read_text())
        original_id = document["sheets"][0]["cells"][0]["id"]
        document["ui"] = {"theme": "native", "zoom": 1.25}
        document["sheets"][0]["cells"][0]["folded"] = True
        document["sheets"].append({"id": "other", "title": "Other", "cells": [
            {"id": "8000", "kind": "code", "source": "11", "mode": "manual"}]})
        document_file.write_text(json.dumps(document))
        untouched_sheet = document["sheets"][1]

        terminal = Terminal(binary, project)
        try:
            terminal.expect(b"project opened; not evaluated")
            terminal.send(b"e6 * 7\x13")  # Save inside edit mode, no evaluation.
            terminal.expect(b"project saved")
            saved = json.loads(document_file.read_text())
            assert saved["sheets"][0]["cells"][0]["source"] == "6 * 7"
            assert saved["sheets"][0]["cells"][0]["id"] == original_id
            assert saved["sheets"][0]["cells"][0]["folded"] is True
            assert saved["sheets"][1] == untouched_sheet
            assert saved["ui"] == document["ui"]
            assert b"=> 42" not in terminal.output
            terminal.send(b"\x1b[21~")  # F10 exits edit mode directly.
            terminal.exit()
        finally:
            terminal.close()

        saved_bytes = document_file.read_bytes()
        terminal = Terminal(binary, project)
        try:
            terminal.expect(b"project opened; not evaluated")
            assert b"=> 42" not in terminal.output
            terminal.send(b"e\x05 + 1\x1b[21~")
            terminal.expect(b"unsaved changes")
            assert document_file.read_bytes() == saved_bytes
            terminal.send(b"Q")
            terminal.exit()
        finally:
            terminal.close()
        assert document_file.read_bytes() == saved_bytes

        terminal = Terminal(binary, project)
        try:
            terminal.expect(b"project opened; not evaluated")
            terminal.send(b"e\xc3\x13")  # An incomplete UTF-8 edit cannot save.
            terminal.expect(b"invalid UTF-8")
            assert document_file.read_bytes() == saved_bytes
            terminal.send(b"\x1b[21~")
            terminal.expect(b"unsaved changes")
            terminal.send(b"Q")
            terminal.exit()
        finally:
            terminal.close()

        terminal = Terminal(binary, project)
        try:
            terminal.expect(b"project opened; not evaluated")
            external = json.loads(document_file.read_text())
            external["title"] = "External edit"
            document_file.write_text(json.dumps(external))
            external_bytes = document_file.read_bytes()
            terminal.send(b"e\x05 + 2\x13")
            terminal.expect(b"changed externally")
            assert document_file.read_bytes() == external_bytes
            terminal.send(b"\x1b[21~")
            terminal.expect(b"unsaved changes")
            terminal.send(b"Q")
            terminal.exit()
        finally:
            terminal.close()

        info = subprocess.run([binary, "--project-info", str(project)],
                              check=True, capture_output=True, text=True)
        assert "sheet other" in info.stdout and "not evaluated" in info.stdout

        # A bundled module is a full Amber source file. Check is compile-only;
        # Run stays explicit and isolated; a sheet applies saved modules itself.
        modules = project / "modules"
        modules.mkdir()
        module_file = modules / "models.am"
        module_file.write_text('raise "check must not execute"\n')
        document = json.loads(document_file.read_text())
        document["modules"].append({"id": "models", "path": "modules/models.am"})
        document_file.write_text(json.dumps(document))
        checked = subprocess.run(
            [binary, "--project", str(project), "--check-module", "models"],
            check=True, capture_output=True, text=True)
        assert "source was not executed" in checked.stdout

        module_file.write_text(
            "class A:\n"
            "  def init(@a)\n"
            "export A\n"
            "A(7)\n")
        run = subprocess.run(
            [binary, "--project", str(project), "--run-module", "models"],
            check=True, capture_output=True, text=True)
        assert "=> <instance A>" in run.stdout
        assert "exports were not installed in sheets" in run.stdout

        # Open the module editor without running its source.
        pid, master = pty.fork()
        if pid == 0:
            os.execve(binary,
                      [binary, "--project", str(project), "--module", "models"],
                      dict(os.environ, TERM="xterm-256color"))
        try:
            fcntl.ioctl(master, termios.TIOCSWINSZ,
                        struct.pack("HHHH", 30, 160, 0, 0))
            os.kill(pid, signal.SIGWINCH)
            output = bytearray()
            deadline = time.monotonic() + 5
            while b"module opened; not run or installed in sheets" not in output:
                if time.monotonic() >= deadline:
                    raise AssertionError(f"module tab did not open: {bytes(output)!r}")
                ready, _, _ = select.select([master], [], [], 0.1)
                if ready:
                    output.extend(os.read(master, 65536))
            assert b"=> <instance A>" not in output
            os.write(master, b"q")
            deadline = time.monotonic() + 5
            ended = 0
            status = 0
            while ended == 0 and time.monotonic() < deadline:
                ready, _, _ = select.select([master], [], [], 0.05)
                if ready:
                    try:
                        output.extend(os.read(master, 65536))
                    except OSError as error:
                        if error.errno != errno.EIO:
                            raise
                ended, status = os.waitpid(pid, os.WNOHANG)
            assert ended == pid, "module tab did not exit"
            assert os.WIFEXITED(status) and os.WEXITSTATUS(status) == 0
        finally:
            try:
                os.kill(pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            try:
                os.waitpid(pid, 0)
            except ChildProcessError:
                pass
            os.close(master)

        # Applying saved bundled exports is explicit, including when leaving
        # an edited Watch cell. The headless command uses the same environment.
        module_file.write_text(
            "class A:\n"
            "  def init(@a)\n"
            "  def value(): @a\n"
            "export A\n")
        document = json.loads(document_file.read_text())
        document["auto_imports"] = ["models"]
        document["sheets"][0]["cells"] = [
            {"id": original_id, "kind": "code", "source": "a = A(7)",
             "mode": "watch"},
            {"id": "8001", "kind": "code", "source": "a.value()",
             "mode": "watch"}]
        document_file.write_text(json.dumps(document))
        persisted = document_file.read_bytes()
        ran = subprocess.run([binary, "--project", str(project), "--run-sheet"],
                             check=True, capture_output=True, text=True)
        assert "cell 8001 => 7" in ran.stdout, ran.stdout + ran.stderr
        assert document_file.read_bytes() == persisted
        other = subprocess.run(
            [binary, "--project", str(project), "--run-sheet", "other"],
            check=True, capture_output=True, text=True)
        assert "cell 8000 => 11" in other.stdout  # Explicitly runs Manual too.
        terminal = Terminal(binary, project)
        try:
            terminal.expect(b"project opened; not evaluated")
            assert b"=> 7" not in terminal.output
            terminal.send(b"e\x05 ")
            terminal.collect()
            terminal.send(b"\x1b")
            terminal.collect(0.3)
            assert b"=> 7" not in terminal.output  # Escape isn't Apply.
            terminal.send(b"\x1b[17~")  # F6: compile/init + Watch.
            terminal.expect(b"environment applied")
            terminal.expect(b"=> 7")
            module_file.write_text(
                "class A:\n"
                "  def init(@a)\n"
                "  def value(): @a + 5\n"
                "export A\n")
            terminal.send(b"\x1b[17~")
            terminal.expect(b"=> 12")
            module_file.write_text('raise "apply rollback"\n')
            terminal.send(b"\x1b[17~")
            terminal.expect(b"Apply failed")
            terminal.send(b"\x1b[21~")
            terminal.expect(b"unsaved changes")
            terminal.send(b"Q")
            terminal.exit()
        finally:
            terminal.close()
        assert document_file.read_bytes() == persisted
        failed = subprocess.run(
            [binary, "--project", str(project), "--run-sheet"],
            capture_output=True, text=True)
        assert failed.returncode != 0 and "NB1003" in failed.stderr
    print("iamber_project_tui_smoke ok")


if __name__ == "__main__":
    main()
