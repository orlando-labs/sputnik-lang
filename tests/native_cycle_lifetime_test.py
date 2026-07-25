#!/usr/bin/env python3
"""Bound full-native cycle memory across calls from a long-running entry."""

from __future__ import annotations

import json
import os
import subprocess
import sys
import time
from pathlib import Path


MAX_RSS_BYTES = 128 * 1024 * 1024


def fail(message: str) -> None:
    raise SystemExit(f"native cycle lifetime test failed: {message}")


def process_rss(pid: int) -> int | None:
    statm = Path(f"/proc/{pid}/statm")
    if statm.is_file():
        fields = statm.read_text(encoding="utf-8").split()
        if len(fields) >= 2:
            return int(fields[1]) * os.sysconf("SC_PAGE_SIZE")
    result = subprocess.run(
        ["ps", "-o", "rss=", "-p", str(pid)],
        capture_output=True,
        text=True,
    )
    text = result.stdout.strip()
    return int(text) * 1024 if result.returncode == 0 and text.isdigit() else None


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    amberc = Path(sys.argv[1]) if len(sys.argv) > 1 else root / "build/amberc"
    work = (
        Path(sys.argv[2])
        if len(sys.argv) > 2
        else root / "build/native-cycle-lifetime"
    )
    if not amberc.is_absolute():
        amberc = (root / amberc).resolve()
    if not work.is_absolute():
        work = (root / work).resolve()
    work.mkdir(parents=True, exist_ok=True)

    source = root / "tests/fixtures/native_cycle_lifetime_core/main.am"
    executable = work / "native-cycle-lifetime"
    build = subprocess.run(
        [
            str(amberc),
            "build",
            str(source),
            "--target",
            "native",
            "--entry",
            "main",
            "--require-full-native",
            "-o",
            str(executable),
            "--out-dir",
            str(work),
        ],
        cwd=root,
        capture_output=True,
        text=True,
        timeout=240,
    )
    if build.returncode != 0:
        fail(build.stderr or build.stdout)
    result = json.loads(build.stdout)
    if result.get("native_full_coverage") is not True:
        fail(f"build did not report full native coverage: {result}")
    if result.get("bytecode_fallback") is not False:
        fail(f"build retained bytecode fallback: {result}")

    process = subprocess.Popen(
        [str(executable)],
        cwd=root,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    peak_rss = 0
    deadline = time.monotonic() + 30
    while process.poll() is None:
        if time.monotonic() >= deadline:
            process.kill()
            process.wait()
            fail("native executable exceeded 30 seconds")
        rss = process_rss(process.pid)
        if rss is not None:
            peak_rss = max(peak_rss, rss)
        time.sleep(0.02)
    stdout, stderr = process.communicate()
    if process.returncode != 0:
        fail(f"native executable exited {process.returncode}: {stderr}")
    if stdout != "300000\n":
        fail(f"unexpected stdout: {stdout!r}")
    if peak_rss == 0:
        fail("RSS could not be observed")
    if peak_rss > MAX_RSS_BYTES:
        fail(
            f"peak RSS {peak_rss} exceeded bounded limit {MAX_RSS_BYTES}"
        )
    print(f"native cycle lifetime test: ok (peak_rss={peak_rss})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
