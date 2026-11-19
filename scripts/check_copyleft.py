#!/usr/bin/env python3
"""Licensing gate: no copyleft-marked third-party implementation may be compiled in.

Why this exists
---------------
The platform compiles against YRpp, which as a whole has no license declaration.
One file in the pinned revision does: ``QueueClass.h`` carries an explicit
``GPL-3.0-or-later`` notice (it comes from TS++) and defines every queue member
function **inline in the header**.  Calling one of those members therefore
compiles a GPL implementation into this DLL, and the distributed binary would be
a GPL-3.0 work - which the MIT grant in ``LICENSE.md`` does not promise.  See the
"YRpp" section of ``NOTICE.md``.

The platform avoids the problem by not instantiating anything from that header:
the engine event queue is appended to by its own code in
``src/Engine/Hooks.Events.cpp``, written against the layout measured in
``gamemd.exe``.

What is checked
---------------
1. **The copyleft file set of the pinned tree.**  Every file below
   ``external/YRpp`` is scanned for a copyleft notice (GPL / LGPL / AGPL, in the
   spellings below).  The set must be exactly ``EXPECTED_COPYLEFT_FILES`` - one
   audited file.  A new one means the pin brought a new obligation and needs a
   licensing review, not a routine bump; a missing one means the audit is stale
   and must be re-done.

2. **No copyleft type is named.**  A declared type of an audited file (currently
   ``QueueClass``) may not appear in this repository's own sources: naming it is
   one keystroke away from instantiating it, and the platform never needs the
   name - the engine object is reached through ``EventClass::OutList``.

3. **No member is called on an engine queue object.**  ``OutList``, ``DoList``
   and ``MegaMissionList`` are the ``QueueClass`` instances in ``EventClass.h`` at
   the pinned revision; ``OutList.Add(...)`` and friends are exactly the calls
   that instantiate the GPL template.  The deprecated ``AddEvent(...)`` helper is
   rejected as well because it calls ``OutList.Add`` itself.

The scan is **lexical**: C/C++ comments and string/character literals are
stripped before the patterns run, so the documentation comments that explain
this very rule do not trip it.  It is a source gate, not a proof about the
binary; the release artefact is additionally inspected by hand when the queue
code changes (the appendix of ``docs/EventChannel.md`` describes the shape).

Usage
-----
    scripts/check_copyleft.py               # run the audit (exit 1 on a violation)
    scripts/check_copyleft.py --quiet       # only report problems
    scripts/check_copyleft.py --self-check  # prove the scanner itself works

``scripts/verify_build.py`` runs the same ``audit()`` as one of its release-gate
checks, so a release cannot be built from sources that reintroduce the call.

Exit status: 0 when clean, 1 on a violation or a broken self-check.
"""

from __future__ import annotations

import argparse
import os
import re
import sys
import tempfile
from pathlib import Path
from typing import Dict, Iterator, List, Optional, Sequence, Tuple

REPO_ROOT = Path(__file__).resolve().parent.parent
YRPP_DIR = REPO_ROOT / "external" / "YRpp"

# This repository's own code.  ``external/`` is the dependency itself and
# ``scripts/`` holds this gate (a checker has to name what it rejects).
SOURCE_DIRS: Tuple[str, ...] = ("src", "include", "tests")
SOURCE_EXTENSIONS: Tuple[str, ...] = (
    ".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl")

# The audited set: what the pinned YRpp revision actually contains (one of 337
# source files).  Changing this tuple is a licensing decision - record it in
# NOTICE.md, not just here.
EXPECTED_COPYLEFT_FILES: Tuple[str, ...] = ("QueueClass.h",)

# Copyleft notice spellings.  The first two are the licence prose the pinned file
# uses; the last two catch the short forms other TS++-derived headers may use.
# Deliberately no bare "GPL" match: it appears inside ordinary identifiers such
# as ``OwningPlayer`` and ``MaximumBuildingPlacementFailures``.
COPYLEFT_MARKERS: Tuple[re.Pattern, ...] = (
    re.compile(r"GNU\s+(?:Lesser\s+|Affero\s+)?General\s+Public\s+License",
               re.IGNORECASE),
    re.compile(r"\bGNU\s+GPL\b", re.IGNORECASE),
    re.compile(r"\b(?:LGPL|AGPL|GPL)\s*[-v]?\s*[23](?:\.\d+)?\b"),
    re.compile(r"\bGPLv[23]\b"),
)

# Declared types of the audited files (see EXPECTED_COPYLEFT_FILES).
COPYLEFT_TYPE_NAMES: Tuple[str, ...] = ("QueueClass",)

# Engine objects whose type is a QueueClass in EventClass.h at the pinned
# revision.  Extend this list when a new pin adds another instance and the
# platform starts using it.
QUEUE_INSTANCE_NAMES: Tuple[str, ...] = ("OutList", "DoList", "MegaMissionList")

QUEUE_MEMBER_CALL_RE = re.compile(
    r"\b(?:%s)\s*(?:\.|->)\s*[A-Za-z_]\w*\s*\(" %
    "|".join(re.escape(name) for name in QUEUE_INSTANCE_NAMES))
# ``EventClass::AddEvent`` is deprecated in YRpp but still calls OutList.Add.
QUEUE_HELPER_CALL_RE = re.compile(r"\bAddEvent\s*\(")
IDENTIFIER_RES: Tuple[Tuple[str, re.Pattern], ...] = tuple(
    (name, re.compile(r"\b%s\b" % re.escape(name)))
    for name in COPYLEFT_TYPE_NAMES)

MAX_REPORTED_HITS = 10


def strip_cpp_comments(text: str) -> str:
    """Blank out comments while keeping every line break (and line numbers).

    String and character literals are kept as they are, so a ``//`` inside a
    literal is not treated as a comment.  The result has the same number of
    lines as the input; comment characters become spaces.
    """
    out: List[str] = []
    index = 0
    length = len(text)

    while index < length:
        char = text[index]

        if char == "/" and index + 1 < length and text[index + 1] == "/":
            while index < length and text[index] != "\n":
                out.append(" ")
                index += 1
            continue

        if char == "/" and index + 1 < length and text[index + 1] == "*":
            out.append("  ")
            index += 2
            while index < length and not (
                    text[index] == "*" and index + 1 < length
                    and text[index + 1] == "/"):
                out.append("\n" if text[index] == "\n" else " ")
                index += 1
            if index < length:
                out.append("  ")
                index += 2
            continue

        if char in ("'", '"'):
            quote = char
            out.append(char)
            index += 1
            while index < length:
                current = text[index]
                out.append(current)
                if current == "\\" and index + 1 < length:
                    out.append(text[index + 1])
                    index += 2
                    continue
                index += 1
                if current == quote:
                    break
            continue

        out.append(char)
        index += 1

    return "".join(out)


def iter_source_files(tree: Path) -> Iterator[Path]:
    """Yield the source files below ``tree`` (``.git`` and build output skipped)."""
    for current, directories, files in os.walk(tree):
        directories[:] = sorted(
            name for name in directories
            if name not in (".git", "Release", "Debug", "IntDir"))
        for name in sorted(files):
            if name.endswith(SOURCE_EXTENSIONS):
                yield Path(current) / name


def find_copyleft_files(tree: Path) -> Dict[str, Tuple[int, str]]:
    """Return ``{relative path: (line, line text)}`` for files with a copyleft notice."""
    found: Dict[str, Tuple[int, str]] = {}
    root = tree.resolve()

    for path in iter_source_files(tree):
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for number, line in enumerate(text.splitlines(), 1):
            if any(marker.search(line) for marker in COPYLEFT_MARKERS):
                found[path.relative_to(root).as_posix()] = (number, line.strip())
                break

    return found


def find_queue_violations(repo_root: Path) -> List[str]:
    """Return ``file:line: text`` for every forbidden use in this repository."""
    hits: List[str] = []
    patterns = [pattern for _, pattern in IDENTIFIER_RES]
    patterns.append(QUEUE_MEMBER_CALL_RE)
    patterns.append(QUEUE_HELPER_CALL_RE)

    for directory in SOURCE_DIRS:
        tree = repo_root / directory
        if not tree.is_dir():
            continue
        for path in iter_source_files(tree):
            try:
                text = path.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            code = strip_cpp_comments(text)
            relative = path.relative_to(repo_root).as_posix()
            for number, line in enumerate(code.splitlines(), 1):
                if any(pattern.search(line) for pattern in patterns):
                    hits.append("%s:%d: %s" % (relative, number, line.strip()[:140]))

    return hits


def audit(yrpp_dir: Path = YRPP_DIR,
          repo_root: Path = REPO_ROOT) -> Tuple[str, str, List[str]]:
    """Run the audit; return ``(status, detail, extra)`` with status pass/skip/fail."""
    hits = find_queue_violations(repo_root)

    if not yrpp_dir.is_dir():
        if hits:
            return ("fail",
                    "the platform sources use a copyleft-marked engine queue",
                    hits)
        return ("skip",
                "%s is not installed - the copyleft file set of the pinned tree "
                "could not be audited; the platform sources are clean"
                % _relative(yrpp_dir, repo_root),
                [])

    found = find_copyleft_files(yrpp_dir)
    actual = tuple(sorted(found))
    expected = tuple(sorted(EXPECTED_COPYLEFT_FILES))
    extra: List[str] = []

    if actual != expected:
        for rel in sorted(set(actual) - set(expected)):
            line, text = found[rel]
            extra.append("new copyleft notice: %s:%d: %s" % (rel, line, text))
        for rel in sorted(set(expected) - set(actual)):
            extra.append("audited file no longer marked: %s" % rel)
        extra.append("this is a licensing review, not a routine pin update: "
                     "decide how the file is used, then update "
                     "EXPECTED_COPYLEFT_FILES and NOTICE.md together")
        return ("fail",
                "the set of copyleft-marked files in the pinned YRpp tree changed "
                "(%d found, %d audited)" % (len(actual), len(expected)),
                extra)

    if hits:
        extra = hits[:MAX_REPORTED_HITS]
        if len(hits) > MAX_REPORTED_HITS:
            extra.append("... and %d more" % (len(hits) - MAX_REPORTED_HITS))
        extra.append("%s: a member defined inline in this header is compiled "
                     "into the DLL when it is called - write the operation "
                     "against the engine layout instead (see NOTICE.md)."
                     % ", ".join(EXPECTED_COPYLEFT_FILES))
        return ("fail",
                "%d use(s) of a copyleft-marked YRpp type or engine queue"
                % len(hits),
                extra)

    return ("pass",
            "the pinned tree has exactly %d audited copyleft file(s) (%s); no "
            "source in %s names or calls one"
            % (len(expected), ", ".join(expected), ", ".join(SOURCE_DIRS)),
            [])


def _relative(path: Path, root: Path) -> str:
    try:
        return path.resolve().relative_to(root.resolve()).as_posix()
    except ValueError:
        return str(path)


def self_check() -> int:
    """Prove the scanner detects a violation and ignores the documented shapes."""
    failures: List[str] = []

    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        yrpp = root / "external" / "YRpp"
        yrpp.mkdir(parents=True)
        (root / "src").mkdir()

        # (a) a copyleft header is detected
        (yrpp / "QueueClass.h").write_text(
            "/* TS++ - GNU General Public License, version 3 or later */\n"
            "template<class T, int size> class QueueClass {};\n",
            encoding="utf-8")
        # (b) ordinary identifiers that merely contain "gpl" are not
        (yrpp / "TechnoClass.h").write_text(
            "class TechnoClass { bool CountedAsOwned; int MaximumBuildingPlacementFailures; };\n",
            encoding="utf-8")
        found = find_copyleft_files(yrpp)
        if tuple(sorted(found)) != ("QueueClass.h",):
            failures.append("(a/b) detection returned %r" % (sorted(found),))

        # (c) a real call is flagged, (d) the same call inside a comment is not
        (root / "src" / "good.cpp").write_text(
            "// never write EventClass::OutList.Add(event) here\n"
            "auto& queue = *reinterpret_cast<void*>(&EventClass::OutList);\n",
            encoding="utf-8")
        (root / "src" / "bad.cpp").write_text(
            "bool send(const EventClass& e) { return EventClass::OutList.Add(e); }\n",
            encoding="utf-8")
        hits = find_queue_violations(root)
        if not any("bad.cpp" in hit for hit in hits):
            failures.append("(c) the OutList.Add call was not detected")
        if any("good.cpp" in hit for hit in hits):
            failures.append("(d) a call inside a comment was reported")

        # (e) the declaration-only uses the base really has must pass
        (root / "src" / "bad.cpp").write_text(
            "static_assert(sizeof(EventOutQueue) == sizeof(EventClass::OutList), \"\");\n"
            "auto* queue = &EventClass::OutList;\n",
            encoding="utf-8")
        if find_queue_violations(root):
            failures.append("(e) a declaration-only use was reported")

        # (f) the audit() wrapper reports pass/fail/skip consistently
        status, _, _ = audit(yrpp, root)
        if status != "pass":
            failures.append("(f) audit() reported %r for a clean tree" % (status,))
        status, _, _ = audit(root / "missing", root)
        if status != "skip":
            failures.append("(f) audit() reported %r without an installed YRpp" % (status,))

    if failures:
        for failure in failures:
            print("SELF-CHECK: FAIL - %s" % failure, file=sys.stderr)
        return 1

    print("SELF-CHECK: PASS")
    return 0


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="Fail when a copyleft-marked YRpp implementation is "
                    "compiled into the platform, or when the audited copyleft "
                    "file set of the pinned YRpp tree changes.")
    parser.add_argument("--quiet", action="store_true", help="only report problems")
    parser.add_argument("--self-check", action="store_true",
                        help="verify the scanner itself with temporary fixtures")
    args = parser.parse_args(argv)

    if args.self_check:
        return self_check()

    status, detail, extra = audit()

    if status == "pass":
        if not args.quiet:
            print("check_copyleft: OK - %s" % detail)
        return 0

    print("check_copyleft: %s - %s"
          % ("SKIP" if status == "skip" else "FAIL", detail), file=sys.stderr)
    for line in extra:
        print("               %s" % line, file=sys.stderr)
    return 0 if status == "skip" else 1


if __name__ == "__main__":
    sys.exit(main())
