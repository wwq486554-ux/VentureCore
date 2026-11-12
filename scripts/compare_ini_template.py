#!/usr/bin/env python3
"""Compare the generated <Platform>.ini template with the shipped .ini.example.

Usage
-----
    python3 scripts/compare_ini_template.py [GENERATED] [EXAMPLE] [--values]
    python3 scripts/compare_ini_template.py --self-check

``GENERATED`` defaults to ``<VC_PLATFORM_INI_NAME>.generated`` (the file the
platform writes next to the log when ``[Platform] DumpIniTemplate=yes`` is set)
and ``EXAMPLE`` defaults to ``<VC_PLATFORM_INI_NAME>.example``.  Both file names
are derived at run time from ``src/Core/PlatformConfig.h``
(``VC_PLATFORM_INI_NAME``, falling back to ``VC_PLATFORM_NAME`` + ``.ini``), so a
derived platform - see docs/MakeYourOwnPlatform.md - needs no edit here; no
platform name is hard coded.  Relative paths resolve against the current
directory first and against the repository root second.

What is compared
----------------
The **key set** (section + key, case-insensitive) of the two files.  Active
entries only: comments (``;``/``#``) and blank lines are ignored, and a key that
is only mentioned in a comment does not count.

A **duplicate (section, key)** is a FAIL at parse time, not a warning: the Win32
profile read the platform is built on cannot see both definitions, so a file
with a repeated key has no single faithful key/value set to compare and the
result of reading it depends on the profile-API implementation.  Both files are
checked; the offending line numbers are printed.

``--values`` additionally compares the raw value text (case-insensitive,
surrounding whitespace trimmed) of the keys the two files share.  Semantics:
the generated template is rendered by the R1 dump from the key registry and
lists the *declared defaults* of every registered key (INI-04) - it is not a
snapshot of the developer machine's effective values, so it can be compared
directly with the shipped ``.example``.  A difference still deserves a look at
the annotations in the generated file, but the comparison no longer depends on
which keys the local run happened to read.  (See docs/Ini.md, "R1 template
dump".)

Exit status
-----------
0 when the two key sets match (and, with ``--values``, the shared values match),
1 otherwise (the differences are printed).  This is a development aid for the R1
template route: it catches a ``.ini.example`` that documents a key the code no
longer reads, or a registered key the example forgot.  It does not launch the
game and does not validate values against the declared types (the registry does
that at startup).

``--self-check`` runs the built-in offline cases (a temporary directory, no
game, no Wine) and exits non-zero if this tool regresses; it is the entry point
for the build verification wiring (``python3 scripts/compare_ini_template.py
--self-check``).
"""

from __future__ import annotations

import argparse
import io
import os
import re
import shutil
import sys
import tempfile
from contextlib import redirect_stdout
from typing import Dict, List, Tuple

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(SCRIPT_DIR)

_DEFINE_TEMPLATE = r'^[ \t]*#[ \t]*define[ \t]+%s[ \t]+"([^"]+)"'


def _read_define(text: str, macro: str) -> str:
    """Returns the last ``#define <macro> "value"`` in the header, or ``""``."""
    matches = re.findall(_DEFINE_TEMPLATE % re.escape(macro), text, re.MULTILINE)
    return matches[-1] if matches else ""


def load_platform_identity(repo_root: str = REPO_ROOT) -> Dict[str, str]:
    """Derives the platform name and file names from src/Core/PlatformConfig.h.

    The script must not hard code the platform it belongs to (a derived platform
    renames VC_PLATFORM_NAME / the INI / the log in that one header).  Raises
    SystemExit with an actionable message when the header cannot be read or does
    not define the identity at all.
    """
    config_path = os.path.join(repo_root, "src", "Core", "PlatformConfig.h")
    try:
        with open(config_path, "r", encoding="utf-8", errors="replace") as handle:
            text = handle.read()
    except OSError as exc:
        raise SystemExit(
            "cannot read %s to derive the platform identity: %s" % (config_path, exc)) from exc

    name = _read_define(text, "VC_PLATFORM_NAME")
    ini_name = _read_define(text, "VC_PLATFORM_INI_NAME")
    log_name = _read_define(text, "VC_PLATFORM_LOG_NAME")
    if not name and not ini_name:
        raise SystemExit(
            "%s does not define VC_PLATFORM_NAME or VC_PLATFORM_INI_NAME - cannot "
            "derive the platform INI/log file names" % config_path)
    if not name:
        name = os.path.splitext(ini_name)[0]
    if not ini_name:
        ini_name = name + ".ini"
    if not log_name:
        log_name = name + ".log"
    return {"name": name, "ini_name": ini_name, "log_name": log_name, "config": config_path}


class IniParseResult:
    """Parsed active entries of one INI file plus diagnostics."""

    __slots__ = ("entries", "warnings", "errors", "first_line")

    def __init__(self) -> None:
        self.entries: Dict[Tuple[str, str], str] = {}
        self.warnings: List[str] = []
        self.errors: List[str] = []
        self.first_line: Dict[Tuple[str, str], int] = {}


def strip_comment(line: str) -> str:
    """Removes an INI comment from a line (full-line or trailing)."""
    for marker in (";", "#"):
        index = line.find(marker)
        if index >= 0:
            line = line[:index]
    return line


def parse_ini(path: str) -> IniParseResult:
    """Parses one INI file; duplicate keys become errors, not warnings."""
    result = IniParseResult()
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
            result.warnings.append("%s:%d: skipping a line without '='" % (path, line_number))
            continue
        key, value = line.split("=", 1)
        key = key.strip().lower()
        if not section:
            result.warnings.append("%s:%d: key before any section" % (path, line_number))
        if not key:
            result.warnings.append("%s:%d: empty key" % (path, line_number))
            continue
        pair = (section, key)
        if pair in result.entries:
            result.errors.append(
                "%s:%d: duplicate key [%s] %s (first defined at line %d) - the "
                "profile API cannot read both definitions faithfully"
                % (path, line_number, section, key, result.first_line[pair]))
            continue
        result.entries[pair] = value.strip()
        result.first_line[pair] = line_number
    return result


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


def compare_paths(
    generated_path: str,
    example_path: str,
    compare_values: bool = False,
) -> Tuple[int, List[str]]:
    """Compares two INI files; returns (exit_code, report_lines).

    The report lines are what the CLI prints.  Kept separate from main() so the
    live-log checker (and the self-check below) can use the same verdict without
    scraping stdout.
    """
    lines: List[str] = []
    generated = parse_ini(generated_path)
    example = parse_ini(example_path)

    for warning in generated.warnings + example.warnings:
        lines.append("warning: %s" % warning)

    errors = list(generated.errors) + list(example.errors)
    if errors:
        lines.append("FAIL: duplicate key(s) in the INI text - a repeated "
                     "(section, key) makes the file ambiguous to the profile API:")
        for error in errors:
            lines.append("  %s" % error)
        return 1, lines

    only_generated = sorted(set(generated.entries) - set(example.entries))
    only_example = sorted(set(example.entries) - set(generated.entries))

    value_differences: List[Tuple[Tuple[str, str], str, str]] = []
    if compare_values:
        for pair in sorted(set(generated.entries) & set(example.entries)):
            generated_value = generated.entries[pair].strip().lower()
            example_value = example.entries[pair].strip().lower()
            if generated_value != example_value:
                value_differences.append((pair, generated.entries[pair], example.entries[pair]))

    if not only_generated and not only_example and not value_differences:
        lines.append("PASS: %d key(s) agree between the template and the example."
                     % len(generated.entries))
        return 0, lines

    if only_generated:
        lines.append("FAIL: %d key(s) are read by the code but missing from the example:"
                     % len(only_generated))
        for pair in only_generated:
            lines.append("  missing from example: %s" % format_pair(pair))
    if only_example:
        lines.append("FAIL: %d key(s) are in the example but no longer registered:"
                     % len(only_example))
        for pair in only_example:
            lines.append("  not registered      : %s" % format_pair(pair))
    if value_differences:
        lines.append("FAIL: %d value(s) differ:" % len(value_differences))
        for pair, generated_value, example_value in value_differences:
            lines.append("  %s: generated %r vs example %r"
                         % (format_pair(pair), generated_value, example_value))
    return 1, lines


# ---------------------------------------------------------------------------
# Offline self-check
# ---------------------------------------------------------------------------

def _write(path: str, text: str) -> None:
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)


def _case_platform_identity() -> List[str]:
    """Checks the header-derived file names (no hard-coded platform name)."""
    problems: List[str] = []
    identity = load_platform_identity(REPO_ROOT)
    if not identity["name"]:
        problems.append("the repository header yields an empty platform name")
    if not identity["ini_name"].endswith(".ini"):
        problems.append("VC_PLATFORM_INI_NAME does not end in .ini: %r" % identity["ini_name"])
    if not identity["log_name"].endswith(".log"):
        problems.append("VC_PLATFORM_LOG_NAME does not end in .log: %r" % identity["log_name"])

    temporary = tempfile.mkdtemp(prefix="vc-compare-identity-")
    try:
        config_dir = os.path.join(temporary, "src", "Core")
        os.makedirs(config_dir)
        # Only VC_PLATFORM_NAME: the INI/log names must be derived from it.
        _write(os.path.join(config_dir, "PlatformConfig.h"),
               '#define VC_PLATFORM_NAME "TestPlatform"\n')
        derived = load_platform_identity(temporary)
        if derived["ini_name"] != "TestPlatform.ini":
            problems.append("the INI name does not fall back to <name>.ini: %r" % derived["ini_name"])
        if derived["log_name"] != "TestPlatform.log":
            problems.append("the log name does not fall back to <name>.log: %r" % derived["log_name"])
        # Explicit overrides win over the fallback.
        _write(os.path.join(config_dir, "PlatformConfig.h"),
               '#define VC_PLATFORM_NAME "TestPlatform"\n'
               '#define VC_PLATFORM_INI_NAME "Other.ini"\n'
               '#define VC_PLATFORM_LOG_NAME "Other.log"\n')
        overridden = load_platform_identity(temporary)
        if overridden["ini_name"] != "Other.ini" or overridden["log_name"] != "Other.log":
            problems.append("explicit VC_PLATFORM_INI_NAME/_LOG_NAME are not used: %r"
                            % (overridden,))
        # A header without the identity macros is an actionable SystemExit.
        _write(os.path.join(config_dir, "PlatformConfig.h"), "#define SOMETHING_ELSE 1\n")
        try:
            load_platform_identity(temporary)
            problems.append("a header without the platform macros did not fail")
        except SystemExit:
            pass
    finally:
        shutil.rmtree(temporary, ignore_errors=True)
    return problems


def _compare_case(directory: str, name: str, generated: str, example: str,
                  compare_values: bool, expected_rc: int,
                  expected_phrases: Tuple[str, ...]) -> List[str]:
    problems: List[str] = []
    case_dir = os.path.join(directory, name.replace(" ", "-"))
    os.makedirs(case_dir, exist_ok=True)
    generated_path = os.path.join(case_dir, "generated.ini")
    example_path = os.path.join(case_dir, "example.ini")
    _write(generated_path, generated)
    _write(example_path, example)
    output = io.StringIO()
    with redirect_stdout(output):
        rc, lines = compare_paths(generated_path, example_path, compare_values)
    report = "\n".join(lines)
    if rc != expected_rc:
        problems.append("exit code %d, expected %d" % (rc, expected_rc))
    for phrase in expected_phrases:
        if phrase not in report:
            problems.append("report does not contain %r" % phrase)
    return problems


def run_self_check() -> int:
    print("compare_ini_template self-check")
    print("  repository: %s" % REPO_ROOT)
    print()

    temporary = tempfile.mkdtemp(prefix="vc-compare-selfcheck-")
    cases: List[Tuple[str, List[str]]] = []
    try:
        base = "[Platform]\nLogLevel=info\n[Features]\nExample=yes\n"
        cases.append(("identical key sets", _compare_case(
            temporary, "identical", base, base, False, 0, ("PASS:", "2 key(s)"))))
        cases.append(("a registered key missing from the example", _compare_case(
            temporary, "missing",
            base + "[MyFeature]\nKey=1\n", base, False, 1,
            ("FAIL:", "missing from example", "[myfeature] key"))))
        cases.append(("an example key that is no longer registered", _compare_case(
            temporary, "stale", base, base + "[MyFeature]\nKey=1\n", False, 1,
            ("FAIL:", "no longer registered", "[myfeature] key"))))
        cases.append(("comments, blank lines and case are ignored", _compare_case(
            temporary, "comments",
            "; header\n\n[platform]\nloglevel=info ; trailing\n[FEATURES]\nExample=YES\n",
            "# other comment\n[Platform]\nLogLevel=debug\n[Features]\nExample=no\n",
            False, 0, ("PASS:",))))
        cases.append(("duplicate key in the generated template", _compare_case(
            temporary, "duplicate-generated",
            base + "[Platform]\nLogLevel=debug\n", base, False, 1,
            ("FAIL:", "duplicate key", "[platform] loglevel"))))
        cases.append(("duplicate key in the example", _compare_case(
            temporary, "duplicate-example",
            base, base + "[Features]\nExample=no\n", False, 1,
            ("FAIL:", "duplicate key"))))
        cases.append(("case-insensitive duplicate keys are duplicates too", _compare_case(
            temporary, "duplicate-case",
            "; comment\n[Platform]\n[platform]\nLogLevel=info\nLogLevel=debug\n",
            base, False, 1, ("FAIL:", "duplicate key"))))
        cases.append(("--values with equal values", _compare_case(
            temporary, "values-equal", base, base, True, 0, ("PASS:",))))
        cases.append(("--values with whitespace/case-only differences", _compare_case(
            temporary, "values-normalised",
            "[Platform]\nLogLevel=  INFO  \n", "[Platform]\nLogLevel=info\n", True, 0,
            ("PASS:",))))
        cases.append(("--values with a real difference", _compare_case(
            temporary, "values-differ",
            "[Platform]\nLogLevel=debug\n", "[Platform]\nLogLevel=info\n", True, 1,
            ("FAIL:", "value(s) differ"))))
        cases.append(("a line without '=' is only a warning", _compare_case(
            temporary, "no-equals",
            base + "Garbage line without separator\n", base, False, 0,
            ("warning:", "skipping a line without '='", "PASS:"))))

        # A missing file is an actionable SystemExit, not a traceback.
        missing_problems: List[str] = []
        try:
            compare_paths(os.path.join(temporary, "does-not-exist.ini"),
                          os.path.join(temporary, "also-missing.ini"))
            missing_problems.append("a missing file did not fail")
        except SystemExit as exc:
            if "cannot read" not in str(exc):
                missing_problems.append("the missing-file message is not actionable: %r" % str(exc))
        cases.append(("a missing file fails with an actionable message", missing_problems))
        cases.append(("the platform file names are derived from PlatformConfig.h",
                      _case_platform_identity()))
    finally:
        shutil.rmtree(temporary, ignore_errors=True)

    failed = 0
    for name, problems in cases:
        if problems:
            failed += 1
            print("[FAIL] %s" % name)
            for problem in problems:
                print("       %s" % problem)
        else:
            print("[PASS] %s" % name)
    print()
    if failed:
        print("SELF-CHECK: FAILED (%d of %d case(s) failed)" % (failed, len(cases)))
        return 1
    print("SELF-CHECK: PASS (%d cases)" % len(cases))
    return 0


def main(argv: List[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Compare a generated <Platform>.ini template with the shipped "
                    ".ini.example (key sets by default; duplicate keys are a failure).")
    parser.add_argument("generated", nargs="?", default=None,
                        help="generated template (default: <VC_PLATFORM_INI_NAME>.generated, "
                             "derived from src/Core/PlatformConfig.h)")
    parser.add_argument("example", nargs="?", default=None,
                        help="shipped example (default: <VC_PLATFORM_INI_NAME>.example, "
                             "derived from src/Core/PlatformConfig.h)")
    parser.add_argument("--values", action="store_true",
                        help="also compare the raw value text of shared keys (see the "
                             "module docstring: the generated template holds effective "
                             "values, the example documents defaults)")
    parser.add_argument("--self-check", action="store_true",
                        help="run the built-in offline cases and exit non-zero on a regression")
    args = parser.parse_args(argv)

    if args.self_check:
        return run_self_check()

    identity = load_platform_identity()
    generated_argument = args.generated or (identity["ini_name"] + ".generated")
    example_argument = args.example or (identity["ini_name"] + ".example")
    generated_path = resolve_path(generated_argument)
    example_path = resolve_path(example_argument)

    print("INI template comparison")
    print("  platform : %s (%s)" % (identity["name"] or "?", identity["config"]))
    print("  generated: %s" % generated_path)
    print("  example  : %s" % example_path)
    print()

    rc, lines = compare_paths(generated_path, example_path, args.values)
    for line in lines:
        print(line)
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
