"""Shared provenance and statistics helpers for polyglot benchmarks."""

from __future__ import annotations

import hashlib
import math
import os
import platform
import random
import statistics
import subprocess
import sys
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Sequence


_T_CRITICAL_95 = {
    1: 12.706,
    2: 4.303,
    3: 3.182,
    4: 2.776,
    5: 2.571,
    6: 2.447,
    7: 2.365,
    8: 2.306,
    9: 2.262,
    10: 2.228,
    11: 2.201,
    12: 2.179,
    13: 2.160,
    14: 2.145,
    15: 2.131,
    16: 2.120,
    17: 2.110,
    18: 2.101,
    19: 2.093,
    20: 2.086,
    21: 2.080,
    22: 2.074,
    23: 2.069,
    24: 2.064,
    25: 2.060,
    26: 2.056,
    27: 2.052,
    28: 2.048,
    29: 2.045,
    30: 2.042,
}


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def file_provenance(path: Path) -> Dict[str, Any]:
    resolved = path.resolve()
    stat = resolved.stat()
    return {
        "path": str(resolved),
        "sha256": sha256_file(resolved),
        "size_bytes": stat.st_size,
        "mtime_ns": stat.st_mtime_ns,
    }


def tree_provenance(
    paths: Iterable[Path], *, relative_to: Optional[Path] = None
) -> Dict[str, Any]:
    root = relative_to.resolve() if relative_to is not None else None
    files: List[Path] = []
    ignored_parts = {".git", "__pycache__"}
    for candidate in paths:
        resolved = candidate.resolve()
        if resolved.is_file():
            files.append(resolved)
        elif resolved.is_dir():
            files.extend(
                path
                for path in resolved.rglob("*")
                if path.is_file()
                and not any(part in ignored_parts for part in path.parts)
            )
    unique = sorted(set(files), key=lambda path: str(path))
    digest = hashlib.sha256()
    records = []
    for path in unique:
        try:
            label = str(path.relative_to(root)) if root is not None else str(path)
        except ValueError:
            label = str(path)
        file_hash = sha256_file(path)
        digest.update(label.encode("utf-8"))
        digest.update(b"\0")
        digest.update(file_hash.encode("ascii"))
        digest.update(b"\n")
        records.append({"path": label, "sha256": file_hash})
    return {
        "sha256": digest.hexdigest(),
        "file_count": len(records),
        "files": records,
    }


def _captured(args: Sequence[str], cwd: Optional[Path] = None) -> Optional[str]:
    try:
        completed = subprocess.run(
            list(args),
            cwd=str(cwd) if cwd is not None else None,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            timeout=10.0,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    if completed.returncode != 0:
        return None
    return completed.stdout.strip()


def git_provenance(path: Path) -> Dict[str, Any]:
    resolved = path.resolve()
    top = _captured(["git", "rev-parse", "--show-toplevel"], resolved)
    if not top:
        return {"path": str(resolved), "available": False}
    root = Path(top).resolve()
    commit = _captured(["git", "rev-parse", "HEAD"], root)
    branch = _captured(["git", "branch", "--show-current"], root)
    describe = _captured(
        ["git", "describe", "--always", "--dirty", "--tags"], root
    )
    status_text = _captured(
        ["git", "status", "--porcelain=v1", "--untracked-files=no"], root
    )
    status = status_text.splitlines() if status_text else []
    return {
        "path": str(root),
        "available": True,
        "commit": commit,
        "branch": branch,
        "describe": describe,
        "tracked_dirty": bool(status),
        "tracked_status": status,
    }


def host_provenance() -> Dict[str, Any]:
    cpu_model = _captured(["sysctl", "-n", "machdep.cpu.brand_string"])
    if not cpu_model:
        cpu_model = _captured(["sysctl", "-n", "hw.model"])
    return {
        "platform": platform.platform(),
        "system": platform.system(),
        "release": platform.release(),
        "machine": platform.machine(),
        "processor": platform.processor(),
        "cpu_model": cpu_model,
        "logical_cpu_count": os.cpu_count(),
        "python": sys.version.splitlines()[0],
        "timezone": os.environ.get("TZ"),
    }


def summary_stats(values: Sequence[float]) -> Dict[str, Any]:
    numeric = [float(value) for value in values]
    if not numeric:
        return {
            "count": 0,
            "values": [],
            "mean": None,
            "median": None,
            "stdev": None,
            "cv_percent": None,
            "min": None,
            "max": None,
            "ci95_mean_low": None,
            "ci95_mean_high": None,
        }
    mean = statistics.mean(numeric)
    median = statistics.median(numeric)
    stdev = statistics.stdev(numeric) if len(numeric) > 1 else 0.0
    cv = stdev / mean * 100.0 if mean != 0.0 else None
    if len(numeric) > 1:
        df = len(numeric) - 1
        critical = _T_CRITICAL_95.get(df, 1.96)
        margin = critical * stdev / math.sqrt(len(numeric))
    else:
        margin = 0.0
    return {
        "count": len(numeric),
        "values": numeric,
        "mean": mean,
        "median": median,
        "stdev": stdev,
        "cv_percent": cv,
        "min": min(numeric),
        "max": max(numeric),
        "ci95_mean_low": mean - margin,
        "ci95_mean_high": mean + margin,
    }


def balanced_orders(
    names: Sequence[str], repeats: int, seed: int
) -> List[List[str]]:
    if repeats < 1:
        raise ValueError("repeats must be positive")
    if not names:
        return []
    base = list(names)
    random.Random(seed).shuffle(base)
    return [
        base[index % len(base) :] + base[: index % len(base)]
        for index in range(repeats)
    ]


def relevant_environment(names: Sequence[str]) -> Dict[str, str]:
    return {name: os.environ[name] for name in names if name in os.environ}
