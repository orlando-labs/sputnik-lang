#!/usr/bin/env python3
"""Publish both cheat sheet translations to the static site; --check detects drift."""

import argparse
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FILES = ("cheat-sheet.md", "cheat-sheet.ru.md")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="fail if either published translation is stale")
    args = parser.parse_args()
    for name in FILES:
        source = (ROOT / "docs" / name).read_bytes()
        output = ROOT / "site" / name
        if args.check:
            if not output.exists() or output.read_bytes() != source:
                parser.exit(1, f"{name} is stale; run python3 tools/sync_cheat_sheet.py\n")
        else:
            output.write_bytes(source)
    print("Website cheat sheets are current" if args.check else "Published English and Russian cheat sheets")


if __name__ == "__main__":
    main()
