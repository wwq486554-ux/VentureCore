#!/usr/bin/env python3
"""Offline hook-window validation against the target engine executable.

The platform declares its engine hooks in the DLL's ``.syhks00`` section: one
16-byte ``hookdecl`` record per patch site
(``{u32 address, u32 size, u32 name_pointer, u32 pad}``).  SyringeEx overwrites
the first ``size`` bytes at ``address`` with a jump, so a wrong window silently
corrupts the game.  This script proves, without Windows and without running the
game, that every declared window is a valid instruction window of the pinned
engine executable.

It reads the records from the DLL and validates all of them against
``gamemd.exe`` using a linear disassembly of the engine bytes (GNU ``objdump``,
read-only - the engine executable is never executed):

  * the record address lies inside the engine's ``.text`` section; the absolute
    hook address is translated through the image's PE ``ImageBase`` (the
    supported case is the engine loaded at its preferred base, and the reported
    range adapts to whatever ``ImageBase`` the supplied executable header has);
  * ``size`` is at least 5 bytes (the size of a 32-bit near jump);
  * starting at ``address``, the decoded instructions tile the window exactly:
    instruction follows instruction, none is cut in half, and the last covered
    instruction ends precisely at ``address + size`` (so ``address + size`` is an
    instruction boundary as well);
  * ``address`` is decoded as an instruction start by the same pass;
  * ``address + size`` is the start of the next engine instruction;
  * the declared windows do not overlap each other;
  * optionally, the number of records matches the expected count (the hook
    catalogue the caller parsed).

Everything is read-only.  The engine path is an explicit input: the
``--gamemd`` command-line argument or the ``VCS_GAMEMD_EXE`` environment
variable.  No path outside this repository is hard-coded, and the verifier
itself never invokes the engine.  When no engine is supplied,
``scripts/verify_build.py`` reports an explicit SKIP (with a warning) for its
offline hook-window check - never a silent PASS.

Usage
-----
    python3 scripts/verify_hook_windows.py --gamemd /path/to/gamemd.exe
    VCS_GAMEMD_EXE=/path/to/gamemd.exe python3 scripts/verify_hook_windows.py

    python3 scripts/verify_build.py --gamemd /path/to/gamemd.exe

Exit status: 0 - every window validated; 1 - at least one record or window is
invalid; 2 - the check could not run (no engine supplied, engine/DLL unreadable,
or ``objdump`` missing).
"""

from __future__ import annotations

import argparse
import os
import re
import struct
import subprocess
import sys
from typing import Dict, List, NamedTuple, Optional, Sequence

# ---------------------------------------------------------------------------
# Facts about the SyringeEx hook declaration / PE image / x86 decoding
# ---------------------------------------------------------------------------

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_DLL = os.path.join(REPO_ROOT, "Release", "VentureCore.dll")

# The environment variable and the command-line argument that select the engine
# executable.  Both are explicit inputs; nothing is discovered automatically.
GAMEMD_ENV_VAR = "VCS_GAMEMD_EXE"

HOOK_SECTION_NAME = ".syhks00"
HOOK_ENTRY_SIZE = 16  # sizeof(hookdecl) with #pragma pack(push, 16)

# A SyringeEx hook overwrites the site with a 32-bit near jump, which needs five
# bytes; a smaller declared window cannot hold one and is rejected outright.
MIN_HOOK_SIZE = 5

# objdump is asked to decode a little past the window so that the instruction
# starting at address+size is fully visible and can be used as the "next
# instruction start" proof (an x86 instruction is at most 15 bytes long).
DECODE_PAD_BYTES = 64
OBJDUMP_TIMEOUT_SECONDS = 120

IMAGE_FILE_MACHINE_I386 = 0x014C
PE32_MAGIC = 0x10B
IMAGE_SCN_MEM_EXECUTE = 0x20000000

MAX_CSTRING = 512

# One disassembly line: "  7cd810:\t8b ec                \tmov    ebp,esp".
DISASSEMBLY_LINE_RE = re.compile(
    r"^\s*([0-9a-f]+):\s+((?:[0-9a-f]{2}\s+)+)(.*)$")


# ---------------------------------------------------------------------------
# Errors
# ---------------------------------------------------------------------------


class HookWindowError(Exception):
    """The offline hook-window validation cannot run or its input is unusable."""


# ---------------------------------------------------------------------------
# A tiny read-only PE32 parser - enough for the .syhks00 records and the engine
# .text range.  Deliberately independent from verify_build.py's parser so this
# script can also be executed on its own.
# ---------------------------------------------------------------------------


class PEImage:
    """A minimal PE32/i386 reader."""

    def __init__(self, path: str) -> None:
        self.path = path
        try:
            with open(path, "rb") as handle:
                self.data = handle.read()
        except OSError as exc:
            raise HookWindowError("cannot read %s: %s" % (path, exc)) from exc

        data = self.data
        self.file_size = len(data)
        if self.file_size < 0x40 or data[:2] != b"MZ":
            raise HookWindowError("%s is not a PE image (missing MZ signature)"
                                  % path)
        e_lfanew = self._u32(0x3C)
        if e_lfanew + 24 > self.file_size or data[e_lfanew:e_lfanew + 4] != b"PE\0\0":
            raise HookWindowError("%s is not a PE image (missing PE signature)"
                                  % path)
        coff_offset = e_lfanew + 4
        self.machine = self._u16(coff_offset)
        num_sections = self._u16(coff_offset + 2)
        size_optional = self._u16(coff_offset + 16)
        optional_offset = coff_offset + 20
        if self.machine != IMAGE_FILE_MACHINE_I386:
            raise HookWindowError(
                "%s: COFF machine is 0x%04X, expected 0x%04X (i386)"
                % (path, self.machine, IMAGE_FILE_MACHINE_I386))
        magic = self._u16(optional_offset)
        if magic != PE32_MAGIC:
            raise HookWindowError(
                "%s: OptionalHeader magic is 0x%03X, expected 0x%03X (PE32)"
                % (path, magic, PE32_MAGIC))
        self.image_base = self._u32(optional_offset + 28)

        if num_sections == 0 or num_sections > 96:
            raise HookWindowError("%s: implausible section count %d"
                                  % (path, num_sections))
        sections_offset = optional_offset + size_optional
        if sections_offset + num_sections * 40 > self.file_size:
            raise HookWindowError("%s: section table runs past the end of the "
                                  "file" % path)
        self.sections: Dict[str, Dict[str, int]] = {}
        for index in range(num_sections):
            base = sections_offset + index * 40
            name = data[base:base + 8].rstrip(b"\0").decode("latin-1")
            self.sections[name] = {
                "virtual_size": self._u32(base + 8),
                "virtual_address": self._u32(base + 12),
                "size_of_raw": self._u32(base + 16),
                "pointer_to_raw": self._u32(base + 20),
                "characteristics": self._u32(base + 36),
            }

    def _u16(self, offset: int) -> int:
        if offset + 2 > self.file_size:
            raise HookWindowError("%s: read of u16 at 0x%X is past the end of "
                                  "the file" % (self.path, offset))
        return struct.unpack_from("<H", self.data, offset)[0]

    def _u32(self, offset: int) -> int:
        if offset + 4 > self.file_size:
            raise HookWindowError("%s: read of u32 at 0x%X is past the end of "
                                  "the file" % (self.path, offset))
        return struct.unpack_from("<I", self.data, offset)[0]

    def find_section(self, name: str) -> Optional[Dict[str, int]]:
        return self.sections.get(name)

    def rva_to_offset(self, rva: int) -> int:
        for name, section in self.sections.items():
            start = section["virtual_address"]
            span = max(section["virtual_size"], section["size_of_raw"])
            if start <= rva < start + span:
                delta = rva - start
                if delta >= section["size_of_raw"]:
                    raise HookWindowError(
                        "%s: RVA 0x%08X is inside section %s but beyond its raw "
                        "data" % (self.path, rva, name))
                offset = section["pointer_to_raw"] + delta
                if offset >= self.file_size:
                    raise HookWindowError(
                        "%s: RVA 0x%08X maps to file offset 0x%X past the end of "
                        "the file" % (self.path, rva, offset))
                return offset
        raise HookWindowError("%s: RVA 0x%08X does not map to any section"
                              % (self.path, rva))

    def read_cstring(self, rva: int, max_length: int = MAX_CSTRING) -> str:
        if rva == 0:
            raise HookWindowError("%s: name pointer is NULL" % self.path)
        offset = self.rva_to_offset(rva)
        end = min(self.file_size, offset + max_length)
        terminator = self.data.find(b"\0", offset, end)
        if terminator < 0:
            raise HookWindowError(
                "%s: C string at RVA 0x%08X is not NUL-terminated within %d bytes"
                % (self.path, rva, max_length))
        raw = self.data[offset:terminator]
        if any(not 0x20 <= byte <= 0x7E for byte in raw):
            raise HookWindowError(
                "%s: C string at RVA 0x%08X contains a non-printable byte"
                % (self.path, rva))
        return raw.decode("ascii")


# ---------------------------------------------------------------------------
# The engine executable and its .text range
# ---------------------------------------------------------------------------


class EngineImage:
    """gamemd.exe plus the validated VA range of its executable code."""

    __slots__ = ("path", "pe", "text_start", "text_end", "section_name")

    def __init__(self, path: str) -> None:
        pe = PEImage(path)
        section_name = ".text"
        section = pe.find_section(section_name)
        if section is None:
            # A renamed code section is still usable when the header marks it
            # executable; the report names whichever section was used.
            section_name = None
            for name, value in pe.sections.items():
                if value["characteristics"] & IMAGE_SCN_MEM_EXECUTE:
                    section_name, section = name, value
                    break
        if section is None or section_name is None:
            names = ", ".join(pe.sections) or "(none)"
            raise HookWindowError(
                "%s: no .text (or other executable) section found; sections "
                "present: %s" % (path, names))
        virtual_size = section["virtual_size"]
        size = virtual_size if virtual_size else section["size_of_raw"]
        start = pe.image_base + section["virtual_address"]
        if size == 0:
            raise HookWindowError("%s: the executable section %s is empty"
                                  % (path, section_name))
        self.path = path
        self.pe = pe
        self.text_start = start
        self.text_end = start + size
        self.section_name = section_name

    def describe(self) -> str:
        return ("engine %s: %s VA 0x%08X-0x%08X (ImageBase=0x%08X)"
                % (self.path, self.section_name, self.text_start,
                   self.text_end, self.pe.image_base))


# ---------------------------------------------------------------------------
# Reading the hook records from the DLL
# ---------------------------------------------------------------------------


class HookRecord(NamedTuple):
    index: int
    address: int
    size: int
    name: str


def read_hook_records(dll_path: str) -> List[HookRecord]:
    """Decode the DLL's .syhks00 section into hook records."""
    pe = PEImage(dll_path)
    section = pe.find_section(HOOK_SECTION_NAME)
    if section is None:
        names = ", ".join(pe.sections) or "(none)"
        raise HookWindowError(
            "%s: section %s not found; sections present: %s"
            % (dll_path, HOOK_SECTION_NAME, names))

    virtual_size = section["virtual_size"]
    raw_size = section["size_of_raw"]
    usable = min(virtual_size, raw_size)
    if usable == 0:
        raise HookWindowError("%s: section %s is empty"
                              % (dll_path, HOOK_SECTION_NAME))
    if usable % HOOK_ENTRY_SIZE != 0:
        raise HookWindowError(
            "%s: section %s usable size 0x%X (VirtualSize=0x%X, "
            "SizeOfRawData=0x%X) is not a multiple of the 16-byte hook record"
            % (dll_path, HOOK_SECTION_NAME, usable, virtual_size, raw_size))
    raw_start = section["pointer_to_raw"]
    if raw_start + raw_size > pe.file_size:
        raise HookWindowError(
            "%s: section %s raw data (offset 0x%X, size 0x%X) runs past the "
            "end of the file"
            % (dll_path, HOOK_SECTION_NAME, raw_start, raw_size))

    records: List[HookRecord] = []
    for index in range(usable // HOOK_ENTRY_SIZE):
        address, size, name_pointer, _pad = struct.unpack_from(
            "<IIII", pe.data, raw_start + index * HOOK_ENTRY_SIZE)
        if name_pointer == 0:
            # Not a hook: alignment padding, skipped the same way SyringeEx
            # skips it.
            continue
        try:
            name = pe.read_cstring(name_pointer - pe.image_base)
        except HookWindowError:
            name = "<unreadable>"
        records.append(HookRecord(index, address, size, name))

    if not records:
        raise HookWindowError("%s: section %s contains no record with a name "
                              "pointer" % (dll_path, HOOK_SECTION_NAME))
    return records


# ---------------------------------------------------------------------------
# Linear disassembly through objdump
# ---------------------------------------------------------------------------


class Instruction(NamedTuple):
    address: int
    length: int
    text: str
    decoded: bool  # False for objdump's .byte / (bad) fallbacks


def _parse_objdump_disassembly(output: str) -> Dict[int, Instruction]:
    instructions: Dict[int, Instruction] = {}
    for line in output.splitlines():
        match = DISASSEMBLY_LINE_RE.match(line)
        if match is None:
            continue
        address = int(match.group(1), 16)
        raw = match.group(2).split()
        text = match.group(3).strip()
        decoded = not (text.startswith(".byte") or "(bad)" in text)
        # The first occurrence per address wins (there is only one objdump
        # invocation per window, so this is belt-and-braces only).
        instructions.setdefault(
            address, Instruction(address, len(raw), text, decoded))
    return instructions


def decode_range(path: str, start: int, stop: int) -> Dict[int, Instruction]:
    """Disassemble [start, stop) and return {instruction_start: Instruction}."""
    if stop <= start:
        raise HookWindowError("empty disassembly range 0x%08X-0x%08X"
                              % (start, stop))
    command = [
        "objdump", "-d", "-M", "intel",
        "--start-address=0x%X" % start,
        "--stop-address=0x%X" % stop,
        path,
    ]
    try:
        result = subprocess.run(
            command, capture_output=True, text=True,
            timeout=OBJDUMP_TIMEOUT_SECONDS)
    except FileNotFoundError as exc:
        raise HookWindowError(
            "objdump not found in PATH - the offline hook-window check needs "
            "GNU binutils (install binutils)") from exc
    except subprocess.TimeoutExpired as exc:
        raise HookWindowError(
            "objdump timed out after %d s (range 0x%08X-0x%08X)"
            % (OBJDUMP_TIMEOUT_SECONDS, start, stop)) from exc
    except OSError as exc:
        raise HookWindowError("cannot run objdump: %s" % (exc,)) from exc
    if result.returncode != 0:
        detail = (result.stderr or result.stdout or "").strip()
        raise HookWindowError(
            "objdump failed (exit %d) for %s: %s"
            % (result.returncode, path, detail or "(no output)"))
    instructions = _parse_objdump_disassembly(result.stdout)
    if not instructions:
        raise HookWindowError(
            "objdump produced no instructions for 0x%08X-0x%08X in %s - is "
            "the engine file a PE image objdump understands?"
            % (start, stop, path))
    return instructions


# ---------------------------------------------------------------------------
# Per-window validation
# ---------------------------------------------------------------------------


class WindowResult(NamedTuple):
    record: HookRecord
    problems: List[str]
    covered: List[Instruction]

    @property
    def ok(self) -> bool:
        return not self.problems


def _format_instruction(instruction: Instruction) -> str:
    return "0x%08X +%-2d %s" % (
        instruction.address, instruction.length, instruction.text)


def validate_window(engine: EngineImage, record: HookRecord) -> WindowResult:
    """Validate one hook window against the engine's decoded instructions."""
    problems: List[str] = []
    covered: List[Instruction] = []
    address = record.address
    size = record.size

    if size < MIN_HOOK_SIZE:
        problems.append(
            "size 0x%X (%d) is smaller than the %d-byte near jump a hook needs"
            % (size, size, MIN_HOOK_SIZE))
    if address == 0:
        problems.append("hook address is 0")
        return WindowResult(record, problems, covered)
    text_lo, text_hi = engine.text_start, engine.text_end
    if not text_lo <= address < text_hi:
        problems.append(
            "address 0x%08X is outside the engine's %s section "
            "0x%08X-0x%08X (ImageBase=0x%08X)"
            % (address, engine.section_name, text_lo, text_hi,
               engine.pe.image_base))
        return WindowResult(record, problems, covered)
    window_end = address + size
    if window_end > text_hi:
        problems.append(
            "window end 0x%08X runs past the end of the engine's %s section "
            "(0x%08X)" % (window_end, engine.section_name, text_hi))
        return WindowResult(record, problems, covered)

    # Decode a little past the window so the instruction after it is visible.
    decode_stop = min(window_end + DECODE_PAD_BYTES, text_hi)
    instructions = decode_range(engine.path, address, decode_stop)

    position = address
    while position < window_end:
        instruction = instructions.get(position)
        if instruction is None:
            problems.append(
                "no instruction starts at 0x%08X inside the window - the "
                "window does not begin on an instruction boundary or objdump "
                "lost sync" % position)
            break
        if not instruction.decoded:
            problems.append(
                "objdump could not decode the instruction at 0x%08X (%s) - the "
                "bytes are not valid x86 code" % (position, instruction.text))
            break
        if position + instruction.length > window_end:
            covered.append(instruction)
            problems.append(
                "instruction at 0x%08X (%d bytes, %s) crosses the window end "
                "0x%08X - the window cuts an instruction in half"
                % (position, instruction.length, instruction.text, window_end))
            break
        covered.append(instruction)
        position += instruction.length
    if position != window_end and not problems:
        problems.append(
            "instructions from 0x%08X cover only up to 0x%08X, not the full "
            "window end 0x%08X" % (address, position, window_end))

    if not problems:
        following = instructions.get(window_end)
        if following is None:
            problems.append(
                "0x%08X (address+size) is not the start of a decoded engine "
                "instruction" % window_end)
        elif not following.decoded:
            problems.append(
                "objdump could not decode the engine instruction starting at "
                "0x%08X (%s)" % (window_end, following.text))

    return WindowResult(record, problems, covered)


def find_overlaps(records: Sequence[HookRecord]) -> List[str]:
    """Report hook windows that overlap each other (including duplicates)."""
    problems: List[str] = []
    ordered = sorted(records, key=lambda record: (record.address, record.index))
    for previous, current in zip(ordered, ordered[1:]):
        previous_end = previous.address + previous.size
        if current.address < previous_end:
            problems.append(
                "overlapping windows: 0x%08X size 0x%X (%s) and 0x%08X size "
                "0x%X (%s)"
                % (previous.address, previous.size, previous.name,
                   current.address, current.size, current.name))
    return problems


# ---------------------------------------------------------------------------
# Whole-run validation report
# ---------------------------------------------------------------------------


class ValidationReport:
    """The result of validating every hook record against the engine."""

    __slots__ = ("dll_path", "engine", "records", "windows",
                 "overlap_problems", "count_problem")

    def __init__(self, dll_path: str, engine: EngineImage,
                 records: Sequence[HookRecord], windows: Sequence[WindowResult],
                 overlap_problems: Sequence[str],
                 count_problem: Optional[str]) -> None:
        self.dll_path = dll_path
        self.engine = engine
        self.records = list(records)
        self.windows = list(windows)
        self.overlap_problems = list(overlap_problems)
        self.count_problem = count_problem

    @property
    def failures(self) -> List[str]:
        failures: List[str] = []
        if self.count_problem:
            failures.append(self.count_problem)
        for window in self.windows:
            if window.ok:
                continue
            record = window.record
            header = ("window 0x%08X size 0x%X (%s)"
                      % (record.address, record.size, record.name))
            for problem in window.problems:
                failures.append("%s: %s" % (header, problem))
            for instruction in window.covered:
                failures.append("        covered: %s"
                                % _format_instruction(instruction))
        failures.extend(self.overlap_problems)
        return failures

    @property
    def ok(self) -> bool:
        return not self.failures

    @property
    def valid_window_count(self) -> int:
        return sum(1 for window in self.windows if window.ok)

    def summary(self) -> str:
        parts = [
            "%d/%d hook window%s cover complete engine instructions and end on "
            "an instruction boundary"
            % (self.valid_window_count, len(self.windows),
               "" if len(self.windows) == 1 else "s"),
            "no overlapping windows" if not self.overlap_problems
            else "%d overlapping window pair(s)" % len(self.overlap_problems),
            "%d record%s" % (len(self.records),
                             "" if len(self.records) == 1 else "s"),
            "%s VA 0x%08X-0x%08X (ImageBase=0x%08X)"
            % (self.engine.section_name, self.engine.text_start,
               self.engine.text_end, self.engine.pe.image_base),
        ]
        return "; ".join(parts)


def validate(dll_path: str, gamemd_path: str,
             expected_count: Optional[int] = None) -> ValidationReport:
    """Read the DLL hook records and validate them against the engine."""
    if not os.path.isfile(gamemd_path):
        raise HookWindowError(
            "engine executable not found: %s (set VCS_GAMEMD_EXE or pass "
            "--gamemd PATH)" % gamemd_path)
    engine = EngineImage(gamemd_path)
    records = read_hook_records(dll_path)
    count_problem: Optional[str] = None
    if expected_count is not None and len(records) != expected_count:
        count_problem = (
            "the DLL declares %d hook record(s) but the expected catalogue has "
            "%d" % (len(records), expected_count))
    windows = [validate_window(engine, record) for record in records]
    overlap_problems = find_overlaps(records)
    return ValidationReport(dll_path, engine, records, windows,
                            overlap_problems, count_problem)


# ---------------------------------------------------------------------------
# Standalone entry point
# ---------------------------------------------------------------------------


def resolve_gamemd_path(argument: Optional[str]) -> Optional[str]:
    """Select the engine path from the argument, then VCS_GAMEMD_EXE."""
    candidate = argument or os.environ.get(GAMEMD_ENV_VAR)
    if not candidate:
        return None
    expanded = os.path.expanduser(candidate)
    if not os.path.isabs(expanded):
        expanded = os.path.abspath(expanded)
    return expanded


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="Validate the DLL's .syhks00 hook windows against an "
                    "engine executable (read-only, offline).")
    parser.add_argument(
        "dll", nargs="?", default=DEFAULT_DLL,
        help="DLL to read the hook records from "
             "(default: Release/VentureCore.dll)")
    parser.add_argument(
        "--gamemd", default=None,
        help="gamemd.exe to validate the hook windows against "
             "(default: $%s)" % GAMEMD_ENV_VAR)
    parser.add_argument(
        "--expected-count", type=int, default=None,
        help="expected number of hook records (e.g. the hook catalogue row "
             "count); omit not to assert the count")
    args = parser.parse_args(argv)

    gamemd_path = resolve_gamemd_path(args.gamemd)
    if gamemd_path is None:
        print("[SKIP] no engine executable supplied: pass --gamemd PATH or set "
              "%s - the hook windows were NOT validated" % GAMEMD_ENV_VAR,
              file=sys.stderr)
        return 2

    print("VentureCore offline hook-window validation")
    print("  DLL    : %s" % args.dll)
    print("  engine : %s" % gamemd_path)
    print()

    try:
        report = validate(args.dll, gamemd_path, args.expected_count)
    except HookWindowError as exc:
        print("[FAIL] cannot run the offline hook-window validation: %s" % exc)
        return 2

    print("  %s" % report.engine.describe())
    print()
    for result in report.windows:
        record = result.record
        status = "PASS" if result.ok else "FAIL"
        print("[%02d] %s 0x%08X size=%-2d %s"
              % (record.index, status, record.address, record.size,
                 record.name))
        for problem in result.problems:
            print("      %s" % problem)
        if result.problems:
            for instruction in result.covered:
                print("      covered: %s" % _format_instruction(instruction))
    for problem in report.overlap_problems:
        print("[FAIL] %s" % problem)
    if report.count_problem:
        print("[FAIL] %s" % report.count_problem)

    print()
    if report.ok:
        print("SUMMARY: %s - OK" % report.summary())
        return 0
    print("SUMMARY: FAILED - %s" % report.summary())
    for failure in report.failures[:20]:
        print("  %s" % failure)
    if len(report.failures) > 20:
        print("  ... and %d more" % (len(report.failures) - 20))
    return 1


if __name__ == "__main__":
    sys.exit(main())
