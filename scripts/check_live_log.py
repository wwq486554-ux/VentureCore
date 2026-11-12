#!/usr/bin/env python3
"""Judge a live <Platform>.log against the expected startup shape.

Usage
-----
    python3 scripts/check_live_log.py [--dir GAME_DIR] [--log PATH]
                                     [--mode normal|probe]
                                     [--generated PATH] [--example PATH]
                                     [--probe-ini PATH] [--ini PATH]
    python3 scripts/check_live_log.py --self-check

The platform identity (platform name, log file name, INI file name) is derived
at run time from ``src/Core/PlatformConfig.h`` (``VC_PLATFORM_NAME`` /
``VC_PLATFORM_LOG_NAME`` / ``VC_PLATFORM_INI_NAME``, parsed with a regular
expression) - no platform name is hard coded, so a derived platform (see
docs/MakeYourOwnPlatform.md) gets the right file names and the right process
header/version-line patterns.  A header that defines neither macro is an
actionable error.

The platform log is append-only, so only the **last process block** (the part
after the last ``=== <Platform> ... process N started ... ===`` header) is
judged.  ``--dir`` defaults to the current directory, then to ``../ra2yrsample``
next to the repository; the log is ``<dir>/<VC_PLATFORM_LOG_NAME>``.

What is checked
---------------
normal mode (an ordinary session):
  * the version line and the RELEASE build marker are present;
  * the `hosts   : ...` line is the 2.0 **set** form (no old level suffix) and
    its `(N host(s))` count matches the listed names;
  * `ini keys: N registered, M invalid, K unknown` exists and M == K == 0;
  * `startup complete` is present and the [Features] lines are enabled;
  * no [ERROR] line in the block, no lineage conflict, and (if `<dir>/syringe.log`
    exists) the last `Done with exit code` is 0;
  * if a generated template exists, it is **fresh** (its mtime is later than the
    start timestamp of the judged process block - a leftover template from an
    earlier run must not count) and its key set matches the shipped
    `<VC_PLATFORM_INI_NAME>.example`;
  * save/load and scenario-clear evidence are reported as hints, not failures.

probe mode (the shipped probe fixture: unknown keys, invalid values,
DumpIniTemplate=yes, LogLevel=debug):
  * the structural checks above, plus
  * the shipped probe fixture exists in the repository and requests the template
    dump (`DumpIniTemplate=yes`, `LogLevel=debug`);
  * the game directory's `<VC_PLATFORM_INI_NAME>` **is** that fixture (same
    parsed entries), which is what justifies the expected invalid/unknown
    counters below;
  * invalid >= 1 and unknown >= 2, and at least one "did you mean Example?"
    suggestion;
  * the generated template is fresh and its key set matches the shipped example.

The generated template is written *after* the startup summary (see
src/Core/Platform.cpp, DumpIniTemplate), so a fresh template's mtime is always
later than the process-block start.  The comparison itself is delegated to
scripts/compare_ini_template.py (which now fails on duplicate keys).

Exit status is 0 when every hard check passes; WARN lines do not fail.  On a
failure the script prints the excerpt to send back.

``--self-check`` runs the built-in offline cases (synthetic logs and templates in
a temporary directory, no game run, no Wine) and exits non-zero if this tool
regresses; it is the entry point for the build verification wiring
(``python3 scripts/check_live_log.py --self-check``).
"""

from __future__ import annotations

import argparse
import datetime
import io
import os
import re
import shutil
import sys
import tempfile
from contextlib import redirect_stdout
from typing import Dict, List, Optional, Tuple

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(SCRIPT_DIR)
if SCRIPT_DIR not in sys.path:
    sys.path.insert(0, SCRIPT_DIR)

# The comparison tool is the single implementation of "do these two INI files
# agree"; both the CLI and this script (and their self-checks) use it.
import compare_ini_template as ini_tool


class Result:
    __slots__ = ("ok", "label", "detail")

    def __init__(self, ok: bool, label: str, detail: str = "") -> None:
        self.ok = ok
        self.label = label
        self.detail = detail


class Evaluation:
    """The verdict of one evaluation: an optional fatal error or the checks."""

    __slots__ = ("error", "results", "hints", "directory", "log_path")

    def __init__(self) -> None:
        self.error: Optional[str] = None
        self.results: List[Result] = []
        self.hints: List[str] = []
        self.directory = ""
        self.log_path = ""


# ---------------------------------------------------------------------------
# Platform identity and paths
# ---------------------------------------------------------------------------

def load_identity() -> Dict[str, str]:
    return ini_tool.load_platform_identity(REPO_ROOT)


def resolve_log(args: argparse.Namespace, identity: Dict[str, str]) -> Tuple[str, str]:
    log_name = identity["log_name"]
    if args.log:
        path = os.path.abspath(os.path.expanduser(args.log))
        return os.path.dirname(path), path
    directory = args.dir
    if not directory:
        if os.path.isfile(os.path.join(os.getcwd(), log_name)):
            directory = os.getcwd()
        else:
            directory = os.path.join(os.path.dirname(REPO_ROOT), "ra2yrsample")
    directory = os.path.abspath(os.path.expanduser(directory))
    return directory, os.path.join(directory, log_name)


def generated_path(args: argparse.Namespace, identity: Dict[str, str], directory: str) -> str:
    return args.generated or os.path.join(directory, identity["ini_name"] + ".generated")


def example_path(args: argparse.Namespace, identity: Dict[str, str]) -> str:
    return args.example or os.path.join(REPO_ROOT, identity["ini_name"] + ".example")


def probe_fixture_path(args: argparse.Namespace, identity: Dict[str, str]) -> str:
    if args.probe_ini:
        return os.path.abspath(os.path.expanduser(args.probe_ini))
    return os.path.join(REPO_ROOT, "scripts", "fixtures", "probe", identity["ini_name"])


def installed_ini_path(args: argparse.Namespace, identity: Dict[str, str],
                       directory: str) -> str:
    if args.ini:
        return os.path.abspath(os.path.expanduser(args.ini))
    return os.path.join(directory, identity["ini_name"])


# ---------------------------------------------------------------------------
# Log parsing
# ---------------------------------------------------------------------------

def process_header_pattern(identity: Dict[str, str]) -> re.Pattern:
    name = identity["name"]
    if not name:
        return re.compile(r"^=== \S+ .* process \d+ started ")
    return re.compile(r"^=== %s .* process \d+ started " % re.escape(name))


_PROCESS_START_RE = re.compile(
    r"started (\d{4})-(\d{2})-(\d{2}) (\d{2}):(\d{2}):(\d{2}) ===")


def last_process_block(lines: List[str],
                       identity: Dict[str, str]) -> Tuple[List[str], Optional[str]]:
    """Returns (lines from the last process header, that header line or None)."""
    start = 0
    header_line: Optional[str] = None
    header = process_header_pattern(identity)
    for index, line in enumerate(lines):
        if header.match(line):
            start = index
            header_line = line
    return lines[start:], header_line


def parse_process_start(header_line: Optional[str]) -> Optional[float]:
    """Epoch seconds of the `started YYYY-MM-DD HH:MM:SS` header field.

    The header is written with GetLocalTime and is judged on the machine that
    ran the game, so interpreting the naive timestamp in the local time zone is
    the right reading (same assumption as reading a log's file mtime).
    """
    if not header_line:
        return None
    match = _PROCESS_START_RE.search(header_line)
    if not match:
        return None
    try:
        stamp = datetime.datetime(*(int(value) for value in match.groups()))
    except ValueError:
        return None
    return stamp.timestamp()


def find_line(block: List[str], pattern: str) -> Optional[str]:
    regex = re.compile(pattern)
    for line in block:
        if regex.search(line):
            return line
    return None


def format_time(epoch: float) -> str:
    return datetime.datetime.fromtimestamp(epoch).strftime("%Y-%m-%d %H:%M:%S")


# ---------------------------------------------------------------------------
# Checks
# ---------------------------------------------------------------------------

def check_hosts(block: List[str], results: List[Result]) -> None:
    line = find_line(block, r"hosts   : ")
    if line is None:
        results.append(Result(False, "hosts line", "no `hosts   :` line in the block"))
        return
    if "tier" in line.lower():
        results.append(Result(False, "hosts line is the 2.0 set form", line.strip()))
        return
    text = line.split("hosts   : ", 1)[1].strip()
    if text == "<none>":
        results.append(Result(True, "hosts line is the 2.0 set form", "hosts: <none>"))
        return
    match = re.match(r"^(?P<names>.+) \((?P<count>\d+) host\(s\)\)$", text)
    if not match:
        results.append(Result(False, "hosts line is the 2.0 set form",
                              "unexpected hosts text: %r" % text))
        return
    names = [name.strip() for name in match.group("names").split(",")]
    count = int(match.group("count"))
    ok = len(names) == count
    results.append(Result(ok, "hosts line is the 2.0 set form",
                          "hosts: %s (%d listed, count %d)" % (", ".join(names), len(names), count)))


def check_ini_summary(block: List[str], mode: str, results: List[Result]) -> None:
    line = find_line(block, r"ini keys: \d+ registered")
    if line is None:
        results.append(Result(False, "ini keys summary line",
                              "no `ini keys: N registered, M invalid, K unknown` line"))
        return
    match = re.search(r"ini keys: (\d+) registered, (\d+) invalid, (\d+) unknown", line)
    if not match:
        results.append(Result(False, "ini keys summary line", line.strip()))
        return
    registered, invalid, unknown = (int(value) for value in match.groups())
    results.append(Result(True, "ini keys summary line",
                          "%d registered, %d invalid, %d unknown"
                          % (registered, invalid, unknown)))
    if mode == "probe":
        results.append(Result(invalid >= 1,
                              "probe: at least one invalid value was reported",
                              "invalid=%d" % invalid))
        results.append(Result(unknown >= 2,
                              "probe: at least two unknown keys were reported",
                              "unknown=%d" % unknown))
        warning = find_line(block, r"did you mean Example\?")
        results.append(Result(warning is not None,
                              "probe: misspelled [Features] switch suggests Example",
                              (warning or "").strip()[:160]))
    else:
        results.append(Result(invalid == 0 and unknown == 0,
                              "normal: no invalid/unknown INI entries",
                              "invalid=%d, unknown=%d" % (invalid, unknown)))


def _report_summary(lines: List[str]) -> str:
    for line in lines:
        stripped = line.strip()
        if stripped.startswith(("PASS:", "FAIL:")):
            return stripped[:220]
    if lines:
        return lines[-1].strip()[:220]
    return "compare_ini_template.py produced no report"


def _describe_ini_difference(expected, actual) -> str:
    only_expected = sorted(set(expected) - set(actual))
    only_actual = sorted(set(actual) - set(expected))
    differing = sorted(pair for pair in set(expected) & set(actual)
                       if expected[pair] != actual[pair])
    parts: List[str] = []
    if only_expected:
        parts.append("missing from the installed INI: %s" % ", ".join(
            ini_tool.format_pair(pair) for pair in only_expected[:3]))
    if only_actual:
        parts.append("not part of the fixture: %s" % ", ".join(
            ini_tool.format_pair(pair) for pair in only_actual[:3]))
    if differing:
        parts.append("value differs: %s" % ", ".join(
            ini_tool.format_pair(pair) for pair in differing[:3]))
    return "; ".join(parts) if parts else "parsed entries differ"


def check_probe_fixture(args: argparse.Namespace, identity: Dict[str, str],
                        directory: str, results: List[Result]) -> None:
    """probe mode: the shipped fixture is installed and justifies the counters."""
    fixture = probe_fixture_path(args, identity)
    if not os.path.isfile(fixture):
        results.append(Result(False, "probe: shipped probe fixture exists",
                              "no probe fixture at %s (it is committed in the "
                              "repository; see the module docstring)" % fixture))
        return
    results.append(Result(True, "probe: shipped probe fixture exists",
                          os.path.relpath(fixture, REPO_ROOT)))
    try:
        fixture_ini = ini_tool.parse_ini(fixture)
    except SystemExit as exc:
        results.append(Result(False, "probe: shipped probe fixture is readable", str(exc)))
        return
    if fixture_ini.errors:
        results.append(Result(False, "probe: shipped probe fixture has no duplicate keys",
                              fixture_ini.errors[0]))
        return

    dump_value = fixture_ini.entries.get(("platform", "dumpinitemplate"), "")
    results.append(Result(dump_value.strip().lower() == "yes",
                          "probe: fixture requests DumpIniTemplate=yes",
                          "DumpIniTemplate=%s" % (dump_value or "<missing>")))
    loglevel_value = fixture_ini.entries.get(("platform", "loglevel"), "")
    results.append(Result(loglevel_value.strip().lower() == "debug",
                          "probe: fixture requests LogLevel=debug",
                          "LogLevel=%s" % (loglevel_value or "<missing>")))

    installed = installed_ini_path(args, identity, directory)
    label = "probe: game directory INI is the shipped probe fixture"
    if not os.path.isfile(installed):
        results.append(Result(False, label,
                              "no %s - copy the shipped probe fixture there before "
                              "the probe run" % installed))
        return
    try:
        installed_ini = ini_tool.parse_ini(installed)
    except SystemExit as exc:
        results.append(Result(False, label, str(exc)))
        return
    if installed_ini.errors:
        results.append(Result(False, label, installed_ini.errors[0]))
        return
    same = installed_ini.entries == fixture_ini.entries
    if same:
        detail = "%d parsed INI entr(ies) match %s" % (
            len(fixture_ini.entries), os.path.basename(fixture))
    else:
        detail = _describe_ini_difference(fixture_ini.entries, installed_ini.entries)
    results.append(Result(same, label, detail))


def check_generated(args: argparse.Namespace, identity: Dict[str, str],
                    directory: str, mode: str,
                    process_start: Optional[float], results: List[Result]) -> None:
    generated = generated_path(args, identity, directory)
    if not os.path.isfile(generated):
        results.append(Result(mode != "probe",
                              "generated template key set matches the example",
                              "no generated template at %s" % generated))
        return

    if process_start is None:
        results.append(Result(False, "generated template is fresh (mtime after the process start)",
                              "cannot parse the `started YYYY-MM-DD HH:MM:SS` field of "
                              "the process header"))
    else:
        mtime = os.path.getmtime(generated)
        results.append(Result(mtime > process_start,
                              "generated template is fresh (mtime after the process start)",
                              "generated mtime %s, process started %s"
                              % (format_time(mtime), format_time(process_start))))

    example = example_path(args, identity)
    rc, lines = ini_tool.compare_paths(generated, example, compare_values=False)
    results.append(Result(rc == 0, "generated template key set matches the example",
                          _report_summary(lines)))


def evaluate(args: argparse.Namespace, identity: Dict[str, str]) -> Evaluation:
    evaluation = Evaluation()
    directory, log_path = resolve_log(args, identity)
    evaluation.directory = directory
    evaluation.log_path = log_path

    if not os.path.isfile(log_path):
        evaluation.error = ("no log file at %s\n(run the game once with the platform "
                            "DLL installed, then retry)" % log_path)
        return evaluation

    with open(log_path, "r", encoding="utf-8", errors="replace") as handle:
        lines = handle.read().splitlines()
    block, header_line = last_process_block(lines, identity)
    if not block:
        evaluation.error = "the log has no process block"
        return evaluation
    process_start = parse_process_start(header_line)
    results = evaluation.results

    # -- startup block -----------------------------------------------------
    platform = re.escape(identity["name"]) if identity["name"] else r"\S+"
    version_pattern = (
        r"\[INFO \] %s v[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+ "
        r"\((?:RELEASE|NIGHTLY|LOCAL) build\)" % platform)
    version_line = find_line(block, version_pattern)
    results.append(Result(version_line is not None, "version line",
                          (version_line or "").strip()))
    results.append(Result(version_line is not None and "RELEASE build" in version_line,
                          "RELEASE build", ""))

    check_hosts(block, results)

    lineage = find_line(block, r"lineage : ")
    conflict = find_line(block, r"lineage : conflicting")
    results.append(Result(conflict is None, "no lineage conflict in this process",
                          (lineage or "").strip()))

    check_ini_summary(block, args.mode, results)

    startup = find_line(block, r"startup complete - \d+ feature\(s\) registered")
    results.append(Result(startup is not None, "startup complete", (startup or "").strip()))

    feature_lines = [line for line in block if re.search(r"\[Features\] .*: enabled", line)]
    disabled_host_lines = [line for line in block if "disabled - requires" in line]
    results.append(Result(len(feature_lines) >= 2,
                          "both Example features enabled",
                          "%d enabled feature line(s)" % len(feature_lines)))
    results.append(Result(not disabled_host_lines,
                          "no unexpected host-gated feature",
                          (disabled_host_lines[0].strip() if disabled_host_lines else "")))

    error_lines = [line for line in block if "[ERROR]" in line]
    results.append(Result(not error_lines, "no [ERROR] line in the process block",
                          error_lines[0].strip()[:200] if error_lines else ""))

    # -- external crash evidence ------------------------------------------
    syringe_log = os.path.join(directory, "syringe.log")
    if os.path.isfile(syringe_log):
        with open(syringe_log, "r", encoding="utf-8", errors="replace") as handle:
            syringe_tail = handle.read().splitlines()[-200:]
        exit_lines = [line for line in syringe_tail if "Done with exit code" in line]
        if exit_lines:
            exit_line = exit_lines[-1]
            results.append(Result("Done with exit code 0" in exit_line,
                                  "syringe exit code 0", exit_line.strip()))
        else:
            results.append(Result(True, "syringe exit code 0",
                                  "no exit line yet (game may still be running)"))

    # -- mode-specific INI template and probe fixture ---------------------
    if args.mode == "probe":
        check_probe_fixture(args, identity, directory, results)
        check_generated(args, identity, directory, args.mode, process_start, results)
    elif os.path.isfile(generated_path(args, identity, directory)):
        check_generated(args, identity, directory, args.mode, process_start, results)

    # -- hints (never fatal) ----------------------------------------------
    hints = evaluation.hints
    if find_line(block, r"scenario clear: extension state reset") is None:
        hints.append("no `scenario clear` line (did you enter and leave a scenario?)")
    if find_line(block, r"savegame trailer:") is None:
        hints.append("no `savegame trailer` line (save once to exercise the save layer)")
    if find_line(block, r"savegame: extension state restored") is None:
        hints.append("no `savegame: extension state restored` line (load that save)")
    return evaluation


# ---------------------------------------------------------------------------
# Offline self-check
# ---------------------------------------------------------------------------

_SYNTHETIC_TEMPLATE = ("[Platform]\nLogLevel=info\n[Features]\n"
                       "Example=yes\nExampleIniKey=yes\n")


def _write(path: str, text: str, crlf: bool = False) -> None:
    with open(path, "w", encoding="utf-8", newline="\r\n" if crlf else "\n") as handle:
        handle.write(text)


def _synthetic_log(identity: Dict[str, str], header_time: datetime.datetime,
                   probe: bool, error: bool = False,
                   summary: Optional[str] = None) -> str:
    name = identity["name"]
    lines = [
        "=== %s v9.9.9.9 (RELEASE) process 4242 started %s ==="
        % (name, header_time.strftime("%Y-%m-%d %H:%M:%S")),
        "[INFO ] %s v9.9.9.9 (RELEASE build)" % name,
        "[INFO ] ini     : %s (found)" % identity["ini_name"],
        "[INFO ] hosts   : <none>",
        "[INFO ] lineage : <none>",
    ]
    if summary is None:
        summary = ("9 registered, 1 invalid, 2 unknown" if probe
                   else "9 registered, 0 invalid, 0 unknown")
    lines.append("[INFO ] ini keys: %s" % summary)
    if probe:
        lines.append("[WARN ] unknown feature switch [Features] Exampel - ignored "
                     "(did you mean Example?)")
    lines += [
        "[INFO ] startup complete - 2 feature(s) registered",
        "[INFO ] [Features] Example: enabled",
        "[INFO ] [Features] ExampleIniKey: enabled",
    ]
    if error:
        lines.append("[ERROR] synthetic failure")
    return "\r\n".join(lines) + "\r\n"


def _make_args(directory: str, mode: str = "normal",
               generated: Optional[str] = None,
               example: Optional[str] = None,
               probe_ini: Optional[str] = None,
               ini: Optional[str] = None,
               log: Optional[str] = None) -> argparse.Namespace:
    return argparse.Namespace(dir=directory, log=log, mode=mode,
                              generated=generated, example=example,
                              probe_ini=probe_ini, ini=ini, self_check=False)


def _prepare_template_files(directory: str, identity: Dict[str, str],
                            header_time: datetime.datetime, fresh: bool) -> Tuple[str, str]:
    """Writes a generated template + example; returns (generated, example)."""
    example = os.path.join(directory, "example.ini")
    generated = os.path.join(directory, identity["ini_name"] + ".generated")
    _write(example, _SYNTHETIC_TEMPLATE)
    _write(generated, _SYNTHETIC_TEMPLATE)
    mtime = header_time.timestamp() + (30.0 if fresh else -60.0)
    os.utime(generated, (mtime, mtime))
    return generated, example


def _case_shipped_probe_fixture(identity: Dict[str, str]) -> List[str]:
    """The repository ships the probe fixture the probe mode refers to."""
    problems: List[str] = []
    path = probe_fixture_path(_make_args(""), identity)
    if not os.path.isfile(path):
        return ["the shipped probe fixture is missing: %s" % path]
    try:
        parsed = ini_tool.parse_ini(path)
    except SystemExit as exc:
        return [str(exc)]
    if parsed.errors:
        problems.append("the shipped probe fixture has duplicate keys: %s" % parsed.errors[0])
    for section, key, expected in (("platform", "dumpinitemplate", "yes"),
                                   ("platform", "loglevel", "debug")):
        value = parsed.entries.get((section, key))
        if value is None:
            problems.append("the shipped probe fixture does not set [%s] %s" % (section, key))
        elif value.strip().lower() != expected:
            problems.append("the shipped probe fixture sets [%s] %s=%r, expected %r"
                            % (section, key, value, expected))
    return problems


def run_self_check(identity: Dict[str, str]) -> int:
    print("check_live_log self-check")
    print("  platform: %s" % identity["name"])
    print("  log/ini : %s / %s" % (identity["log_name"], identity["ini_name"]))
    print()

    temporary = tempfile.mkdtemp(prefix="vc-live-log-selfcheck-")
    cases: List[Tuple[str, List[str]]] = []
    counter = {"value": 0}

    def run_case(name: str, expect_clean: bool,
                 expected_failure_prefixes: Tuple[str, ...],
                 prepare) -> None:
        counter["value"] += 1
        case_dir = os.path.join(temporary, "case-%02d" % counter["value"])
        os.makedirs(case_dir)
        problems: List[str] = []
        try:
            args = prepare(case_dir)
            output = io.StringIO()
            with redirect_stdout(output):
                evaluation = evaluate(args, identity)
            if evaluation.error:
                problems.append("unexpected fatal error: %s"
                                % evaluation.error.splitlines()[0])
            else:
                failures = [result.label for result in evaluation.results if not result.ok]
                if expect_clean and failures:
                    problems.append("expected a clean verdict, got failure(s): %s"
                                    % ", ".join(failures))
                if not expect_clean and len(failures) != len(expected_failure_prefixes):
                    problems.append("expected %d failure(s), got %d: %s"
                                    % (len(expected_failure_prefixes), len(failures),
                                       ", ".join(failures) or "<none>"))
                for prefix in expected_failure_prefixes:
                    if not any(label.startswith(prefix) for label in failures):
                        problems.append("missing expected failure %r (got %s)"
                                        % (prefix, ", ".join(failures) or "<none>"))
        except Exception as exc:  # a self-check reports, it cannot crash the check
            problems.append("raised %s: %s" % (type(exc).__name__, exc))
        cases.append((name, problems))

    def normal_clean(case_dir: str) -> argparse.Namespace:
        _write(os.path.join(case_dir, identity["log_name"]),
               _synthetic_log(identity, datetime.datetime.now() - datetime.timedelta(seconds=120),
                              probe=False), crlf=True)
        return _make_args(case_dir)

    def normal_fresh_template(case_dir: str) -> argparse.Namespace:
        header_time = datetime.datetime.now() - datetime.timedelta(seconds=120)
        _write(os.path.join(case_dir, identity["log_name"]),
               _synthetic_log(identity, header_time, probe=False), crlf=True)
        generated, example = _prepare_template_files(case_dir, identity, header_time,
                                                     fresh=True)
        return _make_args(case_dir, generated=generated, example=example)

    def normal_stale_template(case_dir: str) -> argparse.Namespace:
        header_time = datetime.datetime.now() - datetime.timedelta(seconds=120)
        _write(os.path.join(case_dir, identity["log_name"]),
               _synthetic_log(identity, header_time, probe=False), crlf=True)
        generated, example = _prepare_template_files(case_dir, identity, header_time,
                                                     fresh=False)
        return _make_args(case_dir, generated=generated, example=example)

    def normal_error_line(case_dir: str) -> argparse.Namespace:
        _write(os.path.join(case_dir, identity["log_name"]),
               _synthetic_log(identity, datetime.datetime.now() - datetime.timedelta(seconds=120),
                              probe=False, error=True), crlf=True)
        return _make_args(case_dir)

    def normal_invalid_summary(case_dir: str) -> argparse.Namespace:
        _write(os.path.join(case_dir, identity["log_name"]),
               _synthetic_log(identity, datetime.datetime.now() - datetime.timedelta(seconds=120),
                              probe=False, summary="9 registered, 1 invalid, 2 unknown"), crlf=True)
        return _make_args(case_dir)

    def probe_clean(case_dir: str) -> argparse.Namespace:
        header_time = datetime.datetime.now() - datetime.timedelta(seconds=120)
        _write(os.path.join(case_dir, identity["log_name"]),
               _synthetic_log(identity, header_time, probe=True), crlf=True)
        fixture = probe_fixture_path(_make_args(case_dir), identity)
        with open(fixture, "r", encoding="utf-8", errors="replace") as handle:
            fixture_text = handle.read()
        _write(os.path.join(case_dir, identity["ini_name"]), fixture_text)
        generated, example = _prepare_template_files(case_dir, identity, header_time,
                                                     fresh=True)
        return _make_args(case_dir, mode="probe", generated=generated, example=example)

    def probe_missing_ini(case_dir: str) -> argparse.Namespace:
        header_time = datetime.datetime.now() - datetime.timedelta(seconds=120)
        _write(os.path.join(case_dir, identity["log_name"]),
               _synthetic_log(identity, header_time, probe=True), crlf=True)
        generated, example = _prepare_template_files(case_dir, identity, header_time,
                                                     fresh=True)
        return _make_args(case_dir, mode="probe", generated=generated, example=example)

    run_case("normal: a clean session log passes", True, (), normal_clean)
    run_case("normal: a fresh generated template passes", True, (),
             normal_fresh_template)
    run_case("normal: a stale generated template fails", False,
             ("generated template is fresh",), normal_stale_template)
    run_case("normal: an [ERROR] line fails", False,
             ("no [ERROR] line in the process block",), normal_error_line)
    run_case("normal: invalid/unknown counters fail", False,
             ("normal: no invalid/unknown INI entries",), normal_invalid_summary)
    run_case("probe: installed fixture + fresh template passes", True, (), probe_clean)
    run_case("probe: a missing probe INI fails", False,
             ("probe: game directory INI is the shipped probe fixture",),
             probe_missing_ini)
    cases.append(("the shipped probe fixture is committed and requests the dump",
                  _case_shipped_probe_fixture(identity)))
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
        description="Judge the last process block of a live <Platform>.log.")
    parser.add_argument("--dir", default=None, help="game directory (log + generated ini)")
    parser.add_argument("--log", default=None, help="explicit path of the platform log")
    parser.add_argument("--mode", choices=("normal", "probe"), default="normal")
    parser.add_argument("--generated", default=None,
                        help="explicit path of the generated template")
    parser.add_argument("--example", default=None,
                        help="example ini to compare with (default: repository example)")
    parser.add_argument("--probe-ini", default=None,
                        help="probe mode: explicit path of the shipped probe fixture "
                             "(default: scripts/fixtures/probe/<VC_PLATFORM_INI_NAME>)")
    parser.add_argument("--ini", default=None,
                        help="probe mode: explicit path of the installed <Platform>.ini "
                             "(default: <dir>/<VC_PLATFORM_INI_NAME>)")
    parser.add_argument("--self-check", action="store_true",
                        help="run the built-in offline cases and exit non-zero on a regression")
    args = parser.parse_args(argv)

    identity = load_identity()
    if args.self_check:
        return run_self_check(identity)

    print("%s live log check" % (identity["name"] or "platform"))
    print("  mode: %s" % args.mode)
    print("  log : %s" % resolve_log(args, identity)[1])
    print()

    evaluation = evaluate(args, identity)
    if evaluation.error:
        print("FAIL: %s" % evaluation.error)
        return 1

    failures = [result for result in evaluation.results if not result.ok]
    for result in evaluation.results:
        print("[%s] %s%s" % ("PASS" if result.ok else "FAIL", result.label,
                             ("  (%s)" % result.detail) if result.detail else ""))
    if evaluation.hints:
        print()
        for hint in evaluation.hints:
            print("[WARN] %s" % hint)
    print()
    if failures:
        print("RESULT: FAIL (%d of %d checks failed)"
              % (len(failures), len(evaluation.results)))
        print("Send me this output, the last process block of %s" % evaluation.log_path)
        return 1
    print("RESULT: PASS (%d checks)" % len(evaluation.results))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
