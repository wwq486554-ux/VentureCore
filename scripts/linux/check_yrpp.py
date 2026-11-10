#!/usr/bin/env python3
"""Verify that external/YRpp is the pinned YRpp revision.

The pin lives in ``src/Core/EnginePin.h`` (``VC_YRPP_PIN``).  This script is the
build-time gate: ``scripts/linux/build_linux.sh`` calls it before compiling and
aborts when the local YRpp checkout does not match the pin.  ``src/Engine/EngineCompat.h``
is the second (compile-time) gate; together they make sure a build that happens
to compile is also a build against the engine headers the platform was written
for.

The revision is taken from, in order:

1. the YRpp git checkout itself (``external/YRpp/.git`` exists, e.g. once it is
   a real submodule);
2. ``external/YRpp.commit`` (written by ``scripts/linux/fetch_yrpp.sh`` for
   local, non-git copies).

Usage:
    scripts/linux/check_yrpp.py [--quiet]
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
YRPP_DIR = REPO_ROOT / "external" / "YRpp"
PIN_HEADER = REPO_ROOT / "src" / "Core" / "EnginePin.h"
COMMIT_FILE = REPO_ROOT / "external" / "YRpp.commit"


def read_expected_pin() -> str:
    if not PIN_HEADER.is_file():
        raise SystemExit(f"check_yrpp: missing {PIN_HEADER}")
    text = PIN_HEADER.read_text(encoding="utf-8", errors="replace")
    match = re.search(r'#define\s+VC_YRPP_PIN\s+"([0-9a-fA-F]{7,40})"', text)
    if not match:
        raise SystemExit("check_yrpp: VC_YRPP_PIN is not defined in src/Core/EnginePin.h")
    return match.group(1).lower()


def git_head(path: Path) -> str | None:
    """Return the HEAD commit of a git work tree, or None when it is not one."""
    if not (path / ".git").exists():
        return None
    try:
        out = subprocess.run(
            ["git", "-C", str(path), "rev-parse", "HEAD"],
            capture_output=True,
            text=True,
            timeout=30,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    if out.returncode != 0:
        return None
    return out.stdout.strip().lower() or None


def read_commit_file() -> str | None:
    if not COMMIT_FILE.is_file():
        return None
    value = COMMIT_FILE.read_text(encoding="utf-8", errors="replace").strip().lower()
    return value or None


def main() -> int:
    parser = argparse.ArgumentParser(description="Check the pinned YRpp revision.")
    parser.add_argument("--quiet", action="store_true", help="only report problems")
    args = parser.parse_args()

    expected = read_expected_pin()

    if not (YRPP_DIR / "YRPP.h").is_file():
        print(
            "check_yrpp: FAIL - external/YRpp is missing or incomplete.\n"
            "            fetch it with: scripts/linux/fetch_yrpp.sh",
            file=sys.stderr,
        )
        return 1

    actual = git_head(YRPP_DIR) or read_commit_file()
    if actual is None:
        print(
            "check_yrpp: FAIL - cannot determine the YRpp revision (no git checkout and\n"
            "            no external/YRpp.commit). Run scripts/linux/fetch_yrpp.sh.",
            file=sys.stderr,
        )
        return 1

    if not expected.startswith(actual) and not actual.startswith(expected):
        print(
            "check_yrpp: FAIL - YRpp revision mismatch.\n"
            f"            expected: {expected}\n"
            f"            actual  : {actual}\n"
            "            fix with: scripts/linux/fetch_yrpp.sh   (or update the pin in\n"
            "                      src/Core/EnginePin.h and re-run the compatibility checks)",
            file=sys.stderr,
        )
        return 1

    if not args.quiet:
        print(f"check_yrpp: OK - YRpp {expected}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
