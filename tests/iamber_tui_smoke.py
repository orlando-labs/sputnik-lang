#!/usr/bin/env python3
"""Isolated POSIX PTY smoke for the real curses adapter (no user terminal)."""

import errno
import fcntl
import os
import pty
import select
import signal
import struct
import sys
import termios
import time


def main():
    binary = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "build/iamber")
    pid, master = pty.fork()
    if pid == 0:
        environment = dict(os.environ, TERM="xterm-256color")
        os.execve(binary, [binary], environment)

    reaped = False
    output = bytearray()

    def resize(rows, cols):
        fcntl.ioctl(master, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
        os.kill(pid, signal.SIGWINCH)

    def collect(timeout):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            remaining = deadline - time.monotonic()
            ready, _, _ = select.select([master], [], [], max(remaining, 0))
            if not ready:
                return
            try:
                chunk = os.read(master, 65536)
            except OSError as error:
                if error.errno == errno.EIO:
                    return
                raise
            if not chunk:
                return
            output.extend(chunk)

    def expect_text(text, timeout=3.0):
        deadline = time.monotonic() + timeout
        while text not in output and time.monotonic() < deadline:
            collect(min(0.05, deadline - time.monotonic()))
        if text not in output:
            raise AssertionError(f"missing terminal output {text!r}: {bytes(output[-1600:])!r}")

    try:
        resize(26, 100)
        expect_text(b"iamber ready")
        output.clear()
        # Resize must wake the idle owner without keyboard or runtime traffic.
        resize(10, 40)
        expect_text(b"iamber needs at least 60x12")
        output.clear()
        resize(26, 100)
        expect_text(b"iamber ready")
        output.clear()

        # Keep ncurses' application-keypad and standalone Escape semantics.
        # Replace the final digit via Left + Delete, then leave edit mode.
        os.write(master, b"e6 * 8")
        collect(0.1)
        os.write(master, b"\x1bOD\x1b[3~7")
        collect(0.1)
        os.write(master, b"\x1b")
        expect_text(b"=> 42", timeout=5.0)
        output.clear()

        # Terminal output should stay quiet with no input/activity.
        collect(0.1)
        output.clear()
        collect(0.25)
        if output:
            raise AssertionError(f"unexpected idle terminal output: {bytes(output)!r}")

        os.write(master, b"q")
        deadline = time.monotonic() + 3.0
        while time.monotonic() < deadline:
            collect(0.05)
            ended, status = os.waitpid(pid, os.WNOHANG)
            if ended == pid:
                reaped = True
                if not os.WIFEXITED(status) or os.WEXITSTATUS(status) != 0:
                    raise AssertionError(f"iamber exited with status {status}")
                print("iamber_tui_smoke ok")
                return
        raise AssertionError("iamber did not exit after q")
    finally:
        if not reaped:
            # Only the child spawned by this test is ever signalled/reaped.
            try:
                os.kill(pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            os.waitpid(pid, 0)
        os.close(master)


if __name__ == "__main__":
    main()
