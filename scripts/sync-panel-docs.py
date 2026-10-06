#!/usr/bin/env python3
"""Copy the repository documentation into public/docs/ for the panel's Help view.

    python3 scripts/sync-panel-docs.py           # refresh public/docs/
    python3 scripts/sync-panel-docs.py --check   # CI: fail if they differ

The Markdown files at the repository root are the source of truth. The panel
renders the copies under public/docs/ (public/ is what the server serves and
what a binary without a local public/ downloads), so they must be kept in step.
"""
import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DOCS = ["README.md", "API.md", "EVENTS.md", "SCRIPTING.md"]
TARGET = ROOT / "public" / "docs"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true", help="report drift instead of fixing it")
    args = parser.parse_args()
    stale = []
    for name in DOCS:
        source = (ROOT / name).read_bytes()
        target = TARGET / name
        if target.exists() and target.read_bytes() == source:
            continue
        stale.append(name)
        if not args.check:
            TARGET.mkdir(parents=True, exist_ok=True)
            target.write_bytes(source)
    if args.check and stale:
        print("public/docs/ is out of date for: " + ", ".join(stale))
        print("run: python3 scripts/sync-panel-docs.py")
        return 1
    print("public/docs/: " + ("updated " + ", ".join(stale) if stale else "up to date"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
