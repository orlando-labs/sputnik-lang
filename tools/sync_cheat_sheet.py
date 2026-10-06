#!/usr/bin/env python3
"""Publish docs/cheat-sheet.md to the static site; --check detects stale copies."""

import argparse
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "docs" / "cheat-sheet.md"
OUTPUT = ROOT / "site" / "cheat-sheet.md"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="fail if the published copy is stale")
    args = parser.parse_args()
    source = SOURCE.read_bytes()
    if args.check:
        if not OUTPUT.exists() or OUTPUT.read_bytes() != source:
            parser.exit(1, "Cheat sheet is stale; run python3 tools/sync_cheat_sheet.py\n")
        print("Website cheat sheet is current")
    else:
        OUTPUT.write_bytes(source)
        print("Published docs/cheat-sheet.md to site/cheat-sheet.md")


if __name__ == "__main__":
    main()
