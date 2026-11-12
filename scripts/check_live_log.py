#!/usr/bin/env python3
"""Judge a live VentureCore.log against the expected startup shape.

Usage
-----
    python3 scripts/check_live_log.py [--dir GAME_DIR] [--log PATH]
                                     [--mode normal|probe]
                                     [--generated PATH] [--example PATH]

The platform log is append-only, so only the **last process block** (the part
after the last `=== VentureCore v... process N started ... ===` header) is
judged.  `--dir` defaults to the current directory, then to `../ra2yrsample`
next to the repository; the log is `<dir>/VentureCore.log`.

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
  * save/load and scenario-clear evidence are reported as hints, not failures.

probe mode (the shipped probe .ini: unknown keys, invalid values,
DumpIniTemplate=yes, LogLevel=debug):
  * the structural checks above, plus
  * invalid >= 1 and unknown >= 2, and at least one "did you mean Example?"
    suggestion;
  * `<dir>/VentureCore.ini.generated` exists and its key set matches
    `VentureCore.ini.example` (via scripts/compare_ini_template.py).

Exit status is 0 when every hard check passes; WARN lines do not fail.  On a
failure the script prints the excerpt to send back.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from typing import List, Optional, Tuple

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(SCRIPT_DIR)


class Result:
    __slots__ = ("ok", "label", "detail")

    def __init__(self, ok: bool, label: str, detail: str = "") -> None:
        self.ok = ok
        self.label = label
        self.detail = detail


def resolve_log(args: argparse.Namespace) -> Tuple[str, str]:
    if args.log:
        path = os.path.abspath(os.path.expanduser(args.log))
        return os.path.dirname(path), path
    directory = args.dir
    if not directory:
        if os.path.isfile(os.path.join(os.getcwd(), "VentureCore.log")):
            directory = os.getcwd()
        else:
            directory = os.path.join(os.path.dirname(REPO_ROOT), "ra2yrsample")
    directory = os.path.abspath(os.path.expanduser(directory))
    return directory, os.path.join(directory, "VentureCore.log")


def last_process_block(lines: List[str]) -> List[str]:
    start = 0
    header = re.compile(r"^=== VentureCore .* process .* started ")
    for index, line in enumerate(lines):
        if header.match(line):
            start = index
    return lines[start:]


def find_line(block: List[str], pattern: str) -> Optional[str]:
    regex = re.compile(pattern)
    for line in block:
        if regex.search(line):
            return line
    return None


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


def check_generated(args: argparse.Namespace, directory: str, mode: str,
                    results: List[Result]) -> None:
    generated = args.generated or os.path.join(directory, "VentureCore.ini.generated")
    if not os.path.isfile(generated):
        message = "no generated template at %s" % generated
        results.append(Result(mode != "probe",
                              "generated template key set matches the example", message))
        return
    example = args.example or os.path.join(REPO_ROOT, "VentureCore.ini.example")
    compare = os.path.join(SCRIPT_DIR, "compare_ini_template.py")
    proc = subprocess.run([sys.executable, compare, generated, example],
                          capture_output=True, text=True)
    detail = (proc.stdout or proc.stderr).strip().splitlines()
    summary = detail[-1] if detail else "compare_ini_template.py produced no output"
    results.append(Result(proc.returncode == 0,
                          "generated template key set matches the example", summary))


def main(argv: List[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Judge the last process block of a live VentureCore.log.")
    parser.add_argument("--dir", default=None, help="game directory (log + generated ini)")
    parser.add_argument("--log", default=None, help="explicit path of VentureCore.log")
    parser.add_argument("--mode", choices=("normal", "probe"), default="normal")
    parser.add_argument("--generated", default=None,
                        help="explicit path of VentureCore.ini.generated")
    parser.add_argument("--example", default=None,
                        help="example ini to compare with (default: repository example)")
    args = parser.parse_args(argv)

    directory, log_path = resolve_log(args)
    print("VentureCore live log check")
    print("  mode: %s" % args.mode)
    print("  log : %s" % log_path)
    print()

    if not os.path.isfile(log_path):
        print("FAIL: no log file at %s" % log_path)
        print("(run the game once with VentureCore.dll installed, then retry)")
        return 1

    with open(log_path, "r", encoding="utf-8", errors="replace") as handle:
        lines = handle.read().splitlines()
    block = last_process_block(lines)
    if not block:
        print("FAIL: the log has no process block")
        return 1

    results: List[Result] = []

    # -- startup block -----------------------------------------------------
    version_line = find_line(
        block, r"\[INFO \] VentureCore v[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+ \((?:RELEASE|NIGHTLY|LOCAL) build\)")
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

    # -- mode-specific INI template ---------------------------------------
    if args.mode == "probe":
        check_generated(args, directory, args.mode, results)
    elif os.path.isfile(args.generated or os.path.join(directory, "VentureCore.ini.generated")):
        check_generated(args, directory, args.mode, results)

    # -- hints (never fatal) ----------------------------------------------
    hints: List[str] = []
    if find_line(block, r"scenario clear: extension state reset") is None:
        hints.append("no `scenario clear` line (did you enter and leave a scenario?)")
    if find_line(block, r"savegame trailer:") is None:
        hints.append("no `savegame trailer` line (save once to exercise the save layer)")
    if find_line(block, r"savegame: extension state restored") is None:
        hints.append("no `savegame: extension state restored` line (load that save)")

    # -- report ------------------------------------------------------------
    failures = [result for result in results if not result.ok]
    for result in results:
        print("[%s] %s%s" % ("PASS" if result.ok else "FAIL", result.label,
                             ("  (%s)" % result.detail) if result.detail else ""))
    if hints:
        print()
        for hint in hints:
            print("[WARN] %s" % hint)
    print()
    if failures:
        print("RESULT: FAIL (%d of %d checks failed)" % (len(failures), len(results)))
        print("Send me this output, the last process block of %s" % log_path)
        return 1
    print("RESULT: PASS (%d checks)" % len(results))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
