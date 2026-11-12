#!/usr/bin/env python3
"""Compare the generated <Platform>.ini template with the shipped .ini.example.

Usage
-----
    python3 scripts/compare_ini_template.py [GENERATED] [EXAMPLE] [--values]

``GENERATED`` defaults to ``VentureCore.ini.generated`` (the file the platform
writes next to the log when ``[Platform] DumpIniTemplate=yes`` is set) and
``EXAMPLE`` defaults to ``VentureCore.ini.example``.  Relative paths resolve
against the current directory first and against the repository root second.

What is compared
----------------
The **key set** (section + key, case-insensitive) of the two files.  Active
entries only: comments (``;``/``#``) and blank lines are ignored, and a key that
is only mentioned in a comment does not count.  ``--values`` additionally
compares the raw value text (case-insensitive, surrounding whitespace trimmed).

Exit status
-----------
0 when the two key sets match, 1 otherwise (the differences are printed).  This
is a development aid for the R1 template route: it catches a ``.ini.example``
that documents a key the code no longer reads, or a registered key the example
forgot.  It does not launch the game and does not validate values against the
declared types (the registry does that at startup).
"""

from __future__ import annotations

import argparse
import os
import sys
from typing import Dict, List, Tuple

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_GENERATED = "VentureCore.ini.generated"
DEFAULT_EXAMPLE = "VentureCore.ini.example"


def strip_comment(line: str) -> str:
    """Removes an INI comment from a line (full-line or trailing)."""
    for marker in (";", "#"):
        index = line.find(marker)
        if index >= 0:
            line = line[:index]
    return line


def parse_ini(path: str) -> Tuple[Dict[Tuple[str, str], str], List[str]]:
    """Returns {(section_lower, key_lower): value} plus warnings."""
    entries: Dict[Tuple[str, str], str] = {}
    warnings: List[str] = []
    section = ""
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            lines = handle.read().splitlines()
    except OSError as exc:
        raise SystemExit("cannot read %s: %s" % (path, exc)) from exc

    for line_number, raw_line in enumerate(lines, 1):
        line = strip_comment(raw_line).strip()
        if not line:
            continue
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1].strip().lower()
            continue
        if "=" not in line:
            warnings.append("%s:%d: skipping a line without '='" % (path, line_number))
            continue
        key, value = line.split("=", 1)
        key = key.strip().lower()
        if not section:
            warnings.append("%s:%d: key before any section" % (path, line_number))
        if not key:
            warnings.append("%s:%d: empty key" % (path, line_number))
            continue
        pair = (section, key)
        if pair in entries:
            warnings.append(
                "%s:%d: duplicate key [%s] %s (keeping the first)"
                % (path, line_number, section, key))
            continue
        entries[pair] = value.strip()
    return entries, warnings


def resolve_path(argument: str) -> str:
    expanded = os.path.expanduser(argument)
    if os.path.isabs(expanded):
        return expanded
    relative_to_cwd = os.path.abspath(expanded)
    if os.path.exists(relative_to_cwd):
        return relative_to_cwd
    return os.path.join(REPO_ROOT, expanded)


def format_pair(pair: Tuple[str, str]) -> str:
    section, key = pair
    return "[%s] %s" % (section, key)


def main(argv: List[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Compare a generated <Platform>.ini template with the "
                    "shipped .ini.example (key sets by default).")
    parser.add_argument("generated", nargs="?", default=DEFAULT_GENERATED,
                        help="generated template (default: %s)" % DEFAULT_GENERATED)
    parser.add_argument("example", nargs="?", default=DEFAULT_EXAMPLE,
                        help="shipped example (default: %s)" % DEFAULT_EXAMPLE)
    parser.add_argument("--values", action="store_true",
                        help="also compare the raw value text")
    args = parser.parse_args(argv)

    generated_path = resolve_path(args.generated)
    example_path = resolve_path(args.example)

    print("INI template comparison")
    print("  generated: %s" % generated_path)
    print("  example  : %s" % example_path)
    print()

    generated, generated_warnings = parse_ini(generated_path)
    example, example_warnings = parse_ini(example_path)
    for warning in generated_warnings + example_warnings:
        print("warning: %s" % warning)

    only_generated = sorted(set(generated) - set(example))
    only_example = sorted(set(example) - set(generated))

    value_differences: List[Tuple[Tuple[str, str], str, str]] = []
    if args.values:
        for pair in sorted(set(generated) & set(example)):
            generated_value = generated[pair].strip().lower()
            example_value = example[pair].strip().lower()
            if generated_value != example_value:
                value_differences.append((pair, generated[pair], example[pair]))

    if not only_generated and not only_example and not value_differences:
        print("PASS: %d key(s) agree between the template and the example."
              % len(generated))
        return 0

    if only_generated:
        print("FAIL: %d key(s) are read by the code but missing from the example:"
              % len(only_generated))
        for pair in only_generated:
            print("  missing from example: %s" % format_pair(pair))
    if only_example:
        print("FAIL: %d key(s) are in the example but no longer registered:"
              % len(only_example))
        for pair in only_example:
            print("  not registered      : %s" % format_pair(pair))
    if value_differences:
        print("FAIL: %d value(s) differ:" % len(value_differences))
        for pair, generated_value, example_value in value_differences:
            print("  %s: generated %r vs example %r"
                  % (format_pair(pair), generated_value, example_value))

    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
