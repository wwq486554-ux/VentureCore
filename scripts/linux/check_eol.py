#!/usr/bin/env python3
"""Check (and optionally fix) line endings of a repository's text files.

Why this exists
---------------
The project's `.gitattributes` declares `* text eol=crlf`. Git normalises on
commit, so a file with the wrong working-copy line endings still commits cleanly
and still compiles - which is exactly why the problem goes unnoticed. What it
does break is the working copy: the file ends up LF while every other file is
CRLF, `git status` starts emitting "LF will be replaced by CRLF" warnings, and a
later CRLF→LF flip shows up as a whole-file diff that hides the real change.

The usual way to create the problem is editing a file with a tool that does not
preserve line endings - in particular Python's `open(path, "w")` in text mode
translates the "\\n" you write into the platform default. The safe pattern is:

    read  with open(path, encoding="utf-8")                    # universal newlines
    write with open(path, "w", encoding="utf-8", newline="\\r\\n")

Usage
-----
    scripts/linux/check_eol.py                # report only; exit 1 if any file is wrong
    scripts/linux/check_eol.py --fix          # rewrite the offending files as CRLF
    scripts/linux/check_eol.py --quiet        # only print on problems
    scripts/linux/check_eol.py PATH [PATH...] # also accept files/dirs to check

Both directions are checked, and the `eol=lf` one is not cosmetic: `.gitattributes`
declares `*.sh text eol=lf`, and a shell script with CRLF endings makes bash fail
outright with `unexpected token $'\r'` (the `case "$1" in` line never matches).

`build_linux.sh` runs this in report mode before compiling and only warns, so it
can never turn a good build into a failed one.

Git's own files are never touched: a directory called `.git` is pruned, and so is
the `YRpp/.git` gitlink *file* (see `is_git_metadata`). Everything else that
`.gitattributes` declares `text` is fair game.
"""

from __future__ import annotations

import argparse
import fnmatch
import os
import sys
from typing import Dict, Iterable, List, Tuple

# Never descend into these - they either are not ours or are build output.
SKIP_DIRS = {
    ".git", ".vs", "IntDir", "Release", "Debug", "node_modules", "__pycache__",
    # external/ holds dependencies that are not ours (YRpp): never rewrite them.
    "external",
}

# Fallback when .gitattributes cannot be read: what the project actually uses.
FALLBACK_CRLF = ["*.cpp", "*.h", "*.hpp", "*.inl", "*.md", "*.txt", "*.ini", "*.vcxproj", "*.props", "*.sln"]
FALLBACK_LF = ["*.sh"]


def parse_gitattributes(repo_root: str) -> Tuple[List[str], List[str]]:
    """Return (crlf_patterns, lf_patterns) in file order, first match wins."""
    path = os.path.join(repo_root, ".gitattributes")
    crlf: List[str] = []
    lf: List[str] = []

    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            lines = fh.readlines()
    except OSError:
        return FALLBACK_CRLF, FALLBACK_LF

    for line in lines:
        line = line.strip()

        if not line or line.startswith("#"):
            continue

        parts = line.split()
        if len(parts) < 2:
            continue

        pattern, attrs = parts[0], parts[1:]

        # `-text` means binary: leave it alone entirely.
        if "-text" in attrs:
            continue

        if "eol=crlf" in attrs:
            crlf.append(pattern)
        elif "eol=lf" in attrs:
            lf.append(pattern)

    return (crlf or FALLBACK_CRLF), lf


def wanted_eol(rel_path: str, crlf_patterns: List[str], lf_patterns: List[str]) -> str | None:
    """'crlf', 'lf', or None when the file is not covered."""
    base = os.path.basename(rel_path)

    # First match wins, and the attributes file is ordered most-general first.
    for pattern in lf_patterns:
        if fnmatch.fnmatch(base, pattern) or fnmatch.fnmatch(rel_path, pattern):
            return "lf"

    for pattern in crlf_patterns:
        if fnmatch.fnmatch(base, pattern) or fnmatch.fnmatch(rel_path, pattern):
            return "crlf"

    return None


def is_git_metadata(path: str) -> bool:
    """True for git's own files, which must never be rewritten.

    `YRpp/.git` is a *file* - a gitlink holding `gitdir: ../.git/modules/YRpp` -
    not a directory, so pruning directories alone does not protect it. It is
    matched by the `* text eol=crlf` rule, and giving it a CRLF ending writes a
    stray \\r into the path git parses, which is a real way to break the
    submodule. `.gitignore`, `.gitattributes` and `.gitmodules` are ordinary text
    and are deliberately still checked.
    """
    parts = os.path.normpath(path).split(os.sep)

    return ".git" in parts or os.path.basename(path) == ".git"


def walk_files(roots: Iterable[str]) -> Iterable[str]:
    for root in roots:
        if os.path.isfile(root):
            if not is_git_metadata(root):
                yield root
            continue

        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]

            for name in filenames:
                path = os.path.join(dirpath, name)

                if not is_git_metadata(path):
                    yield path


def classify(path: str) -> str:
    """'crlf', 'lf', 'mixed', 'empty' or 'binary'."""
    try:
        with open(path, "rb") as fh:
            data = fh.read()
    except OSError:
        return "binary"

    if not data:
        return "empty"

    # A NUL byte in the first block is the usual binary heuristic.
    if b"\0" in data[:4096]:
        return "binary"

    crlf = data.count(b"\r\n")
    lf = data.count(b"\n")

    if crlf == 0 and lf == 0:
        return "empty"
    if crlf == lf:
        return "crlf"
    if crlf == 0:
        return "lf"
    return "mixed"


def normalise(path: str, want: str) -> None:
    """Rewrite the file so every line ends with `want` ('crlf' or 'lf')."""
    with open(path, "rb") as fh:
        data = fh.read()

    # Normalise to LF first, so a mixed file never ends up with a doubled CR.
    data = data.replace(b"\r\n", b"\n")

    if want == "crlf":
        data = data.replace(b"\n", b"\r\n")

    with open(path, "wb") as fh:
        fh.write(data)


def main(argv: List[str]) -> int:
    here = os.path.dirname(os.path.abspath(__file__))
    repo_root = os.path.abspath(os.path.join(here, "..", ".."))

    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("paths", nargs="*", default=None, help="files or dirs (default: the repo root)")
    ap.add_argument("--fix", action="store_true", help="rewrite offending files as CRLF")
    ap.add_argument("--quiet", action="store_true", help="print nothing when clean")
    args = ap.parse_args(argv)

    crlf_patterns, lf_patterns = parse_gitattributes(repo_root)
    roots = args.paths or [repo_root]

    bad: List[Tuple[str, str, str]] = []  # (path, actual, want)

    for path in walk_files(roots):
        rel = os.path.relpath(path, repo_root)
        want = wanted_eol(rel, crlf_patterns, lf_patterns)

        if want is None:
            continue

        actual = classify(path)

        if actual in ("empty", "binary"):
            continue

        if actual != want:
            # Both directions matter and neither is cosmetic in the LF case: a
            # shell script with CRLF endings makes bash fail with
            # "unexpected token $'\r'", because `case "$1" in\r` never matches.
            bad.append((rel, actual, want))

    if not bad:
        if not args.quiet:
            print(f"[eol] {len(crlf_patterns) + len(lf_patterns)} rule(s) checked, all files use the declared line endings")
        return 0

    if args.fix:
        for rel, _actual, want in bad:
            normalise(os.path.join(repo_root, rel), want)

        print(f"[eol] normalised {len(bad)} file(s):")
        for rel, actual, want in bad:
            print(f"        {actual:5} -> {want}  {rel}")

        return 0

    print(f"[eol] {len(bad)} file(s) do not use the line endings .gitattributes declares:")
    for rel, actual, want in bad[:40]:
        print(f"        expected {want}, found {actual:5}  {rel}")

    if len(bad) > 40:
        print(f"        ... and {len(bad) - 40} more")

    print()
    print("[eol] This does not break the build, but it makes the working copy differ from")
    print("[eol] every other file and turns a later normalisation into a whole-file diff.")
    print("[eol] Fix with:  scripts/linux/check_eol.py --fix")
    print("[eol] Cause is usually an editor/script that rewrites text without preserving")
    print("[eol] line endings; in Python write with newline='\\r\\n'.")
    return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
