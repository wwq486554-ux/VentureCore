#!/usr/bin/env python3
"""Static verification of a built VentureCore.dll.

Usage
-----
    python3 scripts/verify_build.py [DLL]

The positional argument is the DLL to inspect and defaults to
``Release/VentureCore.dll`` under the repository root (the parent directory of
``scripts/``).  A relative path is resolved against the current working
directory first and against the repository root second.  A non-default hook
catalogue can be selected with ``--catalogue PATH``.

Only the Python standard library is used, so the script runs on any stock
Python 3 installation.

What is checked
---------------
1. PE image format
       ``MZ`` / ``PE\\0\\0`` signatures, OptionalHeader magic 0x10B (PE32),
       COFF machine 0x014C (i386).

2. Hook section
       The ``.syhks00`` section that SyringeEx scans.  The linker pads
       ``SizeOfRawData`` up to the file alignment while ``Misc.VirtualSize``
       stays exact, so the live range is ``min(VirtualSize, SizeOfRawData)``
       (this is what SyringeEx reads as well).  It must be a multiple of the
       16-byte ``hookdecl`` record
       ``{u32 hook_address, u32 size, u32 name_pointer, u32 pad}``, where
       ``name_pointer`` is a VA in this DLL's image.  Records whose name pointer
       is 0 are alignment padding and are skipped, like SyringeEx does.  Each
       remaining name pointer is translated to a file offset through the
       section table and the NUL-terminated C string is read.

3. Hook catalogue
       The decoded ``(address, size, name)`` triples must equal, entry for
       entry, the table in ``docs/HookCatalogue.md``.  The catalogue is parsed
       from the markdown at run time - there is deliberately no second copy of
       the list in this script.  Keep the table format: five columns with
       ``0x...`` hexadecimal values in the first two cells (see
       docs/HookCatalogue.md).

4. Hook handler exports
       Every distinct hook handler name from the catalogue must appear in the
       DLL export table.  ``DEFINE_HOOK`` exports the handler with
       ``extern "C" __declspec(dllexport)``, so a substring match on the export
       names is enough (it also accepts a C++-decorated spelling).

5. Version resource strings
       The platform name, the version and the pinned YRpp commit are expected in
       the image.  The version must be present **twice**: as UTF-16LE (the
       VS_VERSIONINFO resource strings written by ``src/version.rc``) and as
       ASCII (the compiled-in ``VC_VERSION_DISPLAY`` literal used by the startup
       log and the lineage marker).  Requiring both means a build with the
       version resource stripped, or one that only embeds the version in a C++
       literal, fails this check.  A numeric ``VS_FIXEDFILEINFO`` block with the
       expected ``dwFileVersionMS/LS`` is accepted as a fallback when no literal
       version string is present at all.

6. Feature registration
       The ``example_unit_counter`` feature id is expected as an ASCII string
       literal, proving the Example module was compiled in and registered.

7. Log policy (source)
       The release-source assertion behind the logger's cost policy:
       ``OutputDebugStringA`` may only appear inside a ``#ifdef DEBUG`` /
       ``#if defined(DEBUG)`` region **and only in** ``src/Core/Log.cpp`` - the
       base has exactly one gated debugger helper, so a second copy is rejected
       even when it sits inside a DEBUG region.
       The check is line-based: it walks every ``src/**/*.cpp`` / ``*.h`` file,
       tracks the preprocessor conditional nesting (``#if``/``#ifdef``/
       ``#ifndef`` push, ``#elif`` recomputes its branch from its own condition,
       ``#else`` keeps the gate of the region it continues, ``#endif`` pops) and
       reports ``file:line`` for a call outside such a region or outside the
       helper file.  A RELEASE build therefore never reaches debugger output
       except through the one helper in ``src/Core/Log.cpp``, where the ``#else``
       branch is itself gated on the ``[Platform] DebugOutput=yes`` run-time
       switch.  Because it inspects the *sources* this check keeps working when
       the DLL was built from an older state, and its failure tells you exactly
       which line to move.  It is a lexical hygiene check, not a proof that the
       release branch is unreachable (see docs/Logging.md).

Exit status is 0 when every check passes and 1 otherwise.  The expected strings
and ids live in the constants below; when the platform is renamed (see
docs/Renaming.md) update them together with the rest of the rename.
"""

from __future__ import annotations

import argparse
import os
import re
import struct
import sys
from typing import Dict, List, Optional, Sequence, Tuple

# ---------------------------------------------------------------------------
# Expected values / fixed facts
# ---------------------------------------------------------------------------

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_DLL = os.path.join(REPO_ROOT, "Release", "VentureCore.dll")
DEFAULT_CATALOGUE = os.path.join(REPO_ROOT, "docs", "HookCatalogue.md")

# Platform identity (src/Core/PlatformConfig.h, src/version.rc).
PLATFORM_NAME = "VentureCore"

# The version strings expected in the image (src/Core/PlatformVersion.h).  The
# version resource writes VC_VERSION_DISPLAY ("v0.4.0.0-experimental"), which
# contains the long version "0.4.0.0" as a substring, so the first entry also
# covers the resource text.  The second entry is kept for a configurable
# prerelease spelling; see check_version_strings() for why both encodings of
# the version are required.
VERSION_STRINGS: Tuple[str, ...] = ("0.4.0.0", "0.4.0-experimental")

# The three-gate YRpp pin (src/Core/EnginePin.h).
YRPP_PIN = "c6c522f77b58508588d9a8652c53356e4aef7a96"

# Feature registry id of the sample module (src/Features/Example).
FEATURE_ID = "example_unit_counter"

# PE / SyringeEx layout facts.
HOOK_SECTION_NAME = ".syhks00"
HOOK_ENTRY_SIZE = 16  # sizeof(hookdecl) with #pragma pack(push, 16)
IMAGE_FILE_MACHINE_I386 = 0x014C
PE32_MAGIC = 0x10B

MAX_CSTRING = 512
MARKDOWN_HEX_RE = re.compile(r"^0[xX][0-9a-fA-F]+$")
IDENT_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")

VS_FIXEDFILEINFO_SIGNATURE = b"\xbd\x04\xef\xfe"  # 0xFEEF04BD, little endian


# ---------------------------------------------------------------------------
# Result reporting
# ---------------------------------------------------------------------------


class Check:
    """One PASS/FAIL line plus optional indented detail lines."""

    __slots__ = ("ok", "title", "detail", "extra")

    def __init__(self, ok: bool, title: str, detail: str = "",
                 extra: Optional[Sequence[str]] = None) -> None:
        self.ok = ok
        self.title = title
        self.detail = detail
        self.extra: List[str] = list(extra or ())


def print_report(checks: Sequence[Check]) -> None:
    for check in checks:
        print("[%s] %s" % ("PASS" if check.ok else "FAIL", check.title))
        if check.detail:
            print("       %s" % check.detail)
        for line in check.extra:
            print("       %s" % line)
    passed = sum(1 for c in checks if c.ok)
    total = len(checks)
    if passed == total:
        print("\nResult: %d/%d checks passed." % (passed, total))
    else:
        print("\nResult: FAILED - %d/%d checks passed." % (passed, total))


# ---------------------------------------------------------------------------
# PE parsing
# ---------------------------------------------------------------------------


class PEFormatError(Exception):
    """The file is not a PE32/i386 image VentureCore could have produced."""


class PEImage:
    """A tiny read-only PE32 parser - just enough for the checks above."""

    def __init__(self, data: bytes) -> None:
        self.data = data
        self.file_size = len(data)
        if self.file_size < 0x40:
            raise PEFormatError("file is only %d bytes - not a PE image"
                                % self.file_size)
        if data[:2] != b"MZ":
            raise PEFormatError("missing MZ signature at offset 0 (found %r)"
                                % data[:2])
        e_lfanew = self._u32(0x3C)
        if e_lfanew + 24 > self.file_size:
            raise PEFormatError("e_lfanew 0x%X points past the end of the file"
                                % e_lfanew)
        if data[e_lfanew:e_lfanew + 4] != b"PE\0\0":
            raise PEFormatError("missing PE signature at offset 0x%X" % e_lfanew)

        self.coff_offset = e_lfanew + 4
        self.machine = self._u16(self.coff_offset)
        self.num_sections = self._u16(self.coff_offset + 2)
        self.size_optional = self._u16(self.coff_offset + 16)
        self.optional_offset = self.coff_offset + 20
        if self.optional_offset + self.size_optional > self.file_size:
            raise PEFormatError("OptionalHeader runs past the end of the file")

        if self.machine != IMAGE_FILE_MACHINE_I386:
            raise PEFormatError(
                "COFF machine is 0x%04X, expected 0x%04X (i386) - the game "
                "only loads 32-bit images"
                % (self.machine, IMAGE_FILE_MACHINE_I386))

        self.magic = self._u16(self.optional_offset)
        if self.magic == PE32_MAGIC:
            self.image_base = self._u32(self.optional_offset + 28)
            self.data_dir_offset = self.optional_offset + 96
        elif self.magic == 0x20B:
            raise PEFormatError(
                "OptionalHeader magic is 0x20B (PE32+); expected 0x10B (PE32)")
        else:
            raise PEFormatError("unknown OptionalHeader magic 0x%04X"
                                % self.magic)
        self.num_data_dirs = self._u32(self.optional_offset + 92)

        if self.num_sections == 0 or self.num_sections > 96:
            raise PEFormatError("implausible section count %d"
                                % self.num_sections)
        sections_offset = self.optional_offset + self.size_optional
        if sections_offset + self.num_sections * 40 > self.file_size:
            raise PEFormatError("section table runs past the end of the file")

        self.sections: List[Dict[str, int]] = []
        for index in range(self.num_sections):
            base = sections_offset + index * 40
            self.sections.append({
                "index": index,
                "name": data[base:base + 8].rstrip(b"\0").decode("latin-1"),
                "virtual_size": self._u32(base + 8),
                "virtual_address": self._u32(base + 12),
                "size_of_raw": self._u32(base + 16),
                "pointer_to_raw": self._u32(base + 20),
            })

    # -- primitive readers -------------------------------------------------

    def _u16(self, offset: int) -> int:
        if offset + 2 > self.file_size:
            raise PEFormatError("read of u16 at 0x%X is past the end of the file"
                                % offset)
        return struct.unpack_from("<H", self.data, offset)[0]

    def _u32(self, offset: int) -> int:
        if offset + 4 > self.file_size:
            raise PEFormatError("read of u32 at 0x%X is past the end of the file"
                                % offset)
        return struct.unpack_from("<I", self.data, offset)[0]

    # -- helpers -----------------------------------------------------------

    def find_section(self, name: str) -> Optional[Dict[str, int]]:
        for section in self.sections:
            if section["name"] == name:
                return section
        return None

    def data_directory(self, index: int) -> Tuple[int, int]:
        if index >= self.num_data_dirs:
            return (0, 0)
        offset = self.data_dir_offset + index * 8
        return (self._u32(offset), self._u32(offset + 4))

    def rva_to_offset(self, rva: int) -> int:
        for section in self.sections:
            start = section["virtual_address"]
            span = max(section["virtual_size"], section["size_of_raw"])
            if start <= rva < start + span:
                delta = rva - start
                if delta >= section["size_of_raw"]:
                    raise PEFormatError(
                        "RVA 0x%08X is inside section %s but beyond its raw data"
                        % (rva, section["name"]))
                offset = section["pointer_to_raw"] + delta
                if offset >= self.file_size:
                    raise PEFormatError(
                        "RVA 0x%08X maps to file offset 0x%X past the end of "
                        "the file" % (rva, offset))
                return offset
        raise PEFormatError("RVA 0x%08X does not map to any section" % rva)

    def read_cstring(self, rva: int, max_length: int = MAX_CSTRING) -> str:
        if rva == 0:
            raise PEFormatError("name pointer is NULL")
        offset = self.rva_to_offset(rva)
        end = min(self.file_size, offset + max_length)
        terminator = self.data.find(b"\0", offset, end)
        if terminator < 0:
            raise PEFormatError("C string at RVA 0x%08X is not "
                                "NUL-terminated within %d bytes"
                                % (rva, max_length))
        raw = self.data[offset:terminator]
        for byte in raw:
            if not 0x20 <= byte <= 0x7E:
                raise PEFormatError("C string at RVA 0x%08X contains a "
                                    "non-printable byte 0x%02X" % (rva, byte))
        return raw.decode("ascii")


def open_pe(path: str) -> Tuple[PEImage, bytes]:
    try:
        with open(path, "rb") as handle:
            data = handle.read()
    except OSError as exc:
        raise PEFormatError("cannot read %s: %s" % (path, exc)) from exc
    return PEImage(data), data


# ---------------------------------------------------------------------------
# Check 2: the .syhks00 hook section
# ---------------------------------------------------------------------------


def read_hook_entries(pe: PEImage) -> Tuple[List[Tuple[int, int, str]], str]:
    """Decode the hook section into [(address, size, name), ...]."""
    section = pe.find_section(HOOK_SECTION_NAME)
    if section is None:
        names = ", ".join(s["name"] for s in pe.sections) or "(none)"
        raise PEFormatError("section %s not found; sections present: %s"
                            % (HOOK_SECTION_NAME, names))

    virtual_size = section["virtual_size"]
    raw_size = section["size_of_raw"]
    if virtual_size == 0:
        raise PEFormatError("section %s has virtual size 0 (no hook records)"
                            % HOOK_SECTION_NAME)
    # The linker pads SizeOfRawData up to the file alignment (0x200 in this
    # project) while Misc.VirtualSize stays exact, so the live range is the
    # smaller of the two.  SyringeEx reads the same range and ignores records
    # whose name pointer is 0 (alignment padding inside the range).
    usable = min(virtual_size, raw_size)
    if usable % HOOK_ENTRY_SIZE != 0:
        raise PEFormatError(
            "section %s usable size 0x%X (VirtualSize=0x%X, SizeOfRawData=0x%X) "
            "is not a multiple of the 16-byte hook record"
            % (HOOK_SECTION_NAME, usable, virtual_size, raw_size))
    raw_start = section["pointer_to_raw"]
    if raw_start + raw_size > pe.file_size:
        raise PEFormatError(
            "section %s raw data (offset 0x%X, size 0x%X) runs past the end of "
            "the file" % (HOOK_SECTION_NAME, raw_start, raw_size))

    count = usable // HOOK_ENTRY_SIZE
    entries: List[Tuple[int, int, str]] = []
    problems: List[str] = []
    non_zero_pads = 0
    skipped_padding = 0
    for index in range(count):
        offset = raw_start + index * HOOK_ENTRY_SIZE
        address, size, name_pointer, pad = struct.unpack_from(
            "<IIII", pe.data, offset)
        if name_pointer == 0:
            # Not a hook: padding to the file alignment (or an alignment gap
            # between records).  SyringeEx skips these as well.
            skipped_padding += 1
            continue
        if pad:
            non_zero_pads += 1
        if address == 0:
            problems.append("record %d has hook address 0" % index)
        # A size of 0 is legal in the wider ecosystem (Phobos declares some
        # hooks that only restore engine bytes through a same-address sibling),
        # so it is not treated as a parse error here; the catalogue comparison
        # decides whether the record is expected.
        try:
            if name_pointer < pe.image_base:
                raise PEFormatError(
                    "name pointer 0x%08X is below the image base 0x%08X"
                    % (name_pointer, pe.image_base))
            name = pe.read_cstring(name_pointer - pe.image_base)
        except PEFormatError as exc:
            problems.append("record %d (hook 0x%08X): %s"
                            % (index, address, exc))
            name = "<unreadable>"
        entries.append((address, size, name))
    if problems:
        raise PEFormatError("; ".join(problems[:6])
                            + (" (and %d more)" % (len(problems) - 6)
                               if len(problems) > 6 else ""))
    if not entries:
        raise PEFormatError("section %s contains no record with a name pointer"
                            % HOOK_SECTION_NAME)

    padding = raw_size - virtual_size
    detail = "%d hook record%s (VirtualSize=0x%X, SizeOfRawData=0x%X" % (
        len(entries), "" if len(entries) == 1 else "s", virtual_size, raw_size)
    if padding > 0:
        detail += "; 0x%X bytes of file-alignment padding ignored" % padding
    detail += ")"
    if skipped_padding:
        detail += "; %d record(s) without a name pointer skipped" % skipped_padding
    if non_zero_pads:
        detail += "; %d record(s) carry a non-zero pad field" % non_zero_pads
    return entries, detail


# ---------------------------------------------------------------------------
# Check 3: hook catalogue in docs/HookCatalogue.md
# ---------------------------------------------------------------------------


class CatalogueError(Exception):
    """docs/HookCatalogue.md is missing or its hook table cannot be parsed."""


def parse_hook_catalogue(path: str) -> List[Dict[str, object]]:
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            lines = handle.read().splitlines()
    except OSError as exc:
        raise CatalogueError("cannot read %s: %s" % (path, exc)) from exc

    rows: List[Dict[str, object]] = []
    for line_number, line in enumerate(lines, 1):
        stripped = line.strip()
        if not stripped.startswith("|"):
            continue
        cells = [cell.strip() for cell in stripped.strip("|").split("|")]
        if len(cells) < 5:
            continue
        if not (MARKDOWN_HEX_RE.match(cells[0])
                and MARKDOWN_HEX_RE.match(cells[1])):
            continue  # header, separator or an unrelated table
        match = IDENT_RE.search(cells[2])
        if match is None:
            raise CatalogueError(
                "line %d: the name cell %r does not contain a C identifier"
                % (line_number, cells[2]))
        rows.append({
            "address": int(cells[0], 16),
            "size": int(cells[1], 16),
            "name": match.group(0),
            "line": line_number,
        })

    if not rows:
        raise CatalogueError(
            "no hook rows found - expected a markdown table of the form "
            "'| 0x7CD810 | 0x9 | Venture_ExeRun | purpose | notes |'")

    seen: Dict[int, int] = {}
    for row in rows:
        address = int(row["address"])
        if address in seen:
            raise CatalogueError(
                "duplicate hook address 0x%08X on lines %d and %d"
                % (address, seen[address], int(row["line"])))
        seen[address] = int(row["line"])
    return rows


def check_hook_table(entries: Sequence[Tuple[int, int, str]],
                     rows: Sequence[Dict[str, object]]) -> Check:
    title = "Hook table matches docs/HookCatalogue.md"
    expected = sorted((int(r["address"]), int(r["size"]), str(r["name"]))
                      for r in rows)
    actual = sorted(entries)
    if actual == expected:
        return Check(True, title,
                     "%d/%d (address, size, name) entries match, in any order"
                     % (len(expected), len(expected)))

    expected_set = set(expected)
    actual_set = set(actual)
    missing = [item for item in expected if item not in actual_set]
    unexpected = [item for item in actual if item not in expected_set]
    extra: List[str] = []
    for address, size, name in missing[:8]:
        extra.append("missing from DLL : 0x%08X  0x%X  %s"
                     % (address, size, name))
    for address, size, name in unexpected[:8]:
        extra.append("not in catalogue: 0x%08X  0x%X  %s"
                     % (address, size, name))
    if len(missing) > 8 or len(unexpected) > 8:
        extra.append("... %d missing, %d unexpected in total"
                     % (len(missing), len(unexpected)))
    return Check(False, title,
                 "%d catalogue entries vs %d DLL entries"
                 % (len(expected), len(actual)), extra)


# ---------------------------------------------------------------------------
# Check 4: hook handler exports
# ---------------------------------------------------------------------------


def read_export_names(pe: PEImage) -> Tuple[List[str], int]:
    export_rva, _export_size = pe.data_directory(0)
    if export_rva == 0:
        raise PEFormatError("the image has no export directory "
                            "(DEFINE_HOOK handlers must be exported)")
    export_offset = pe.rva_to_offset(export_rva)
    number_of_functions = pe._u32(export_offset + 20)
    number_of_names = pe._u32(export_offset + 24)
    address_of_names = pe._u32(export_offset + 32)
    if number_of_names == 0 or number_of_names > 10000:
        raise PEFormatError("implausible export name count %d"
                            % number_of_names)
    names_table = pe.rva_to_offset(address_of_names)
    if names_table + number_of_names * 4 > pe.file_size:
        raise PEFormatError("export name table runs past the end of the file")
    names: List[str] = []
    for index in range(number_of_names):
        name_rva = pe._u32(names_table + index * 4)
        names.append(pe.read_cstring(name_rva, max_length=256))
    return names, number_of_functions


def check_hook_exports(pe: PEImage,
                       rows: Sequence[Dict[str, object]]) -> Check:
    title = "Hook handlers exported"
    try:
        export_names, function_count = read_export_names(pe)
    except PEFormatError as exc:
        return Check(False, title, str(exc))

    unique_handlers: List[str] = []
    for row in rows:
        name = str(row["name"])
        if name not in unique_handlers:
            unique_handlers.append(name)
    if not unique_handlers:
        return Check(False, title,
                     "the hook catalogue contains no handler names")

    missing = [handler for handler in unique_handlers
               if not any(handler in exported for exported in export_names)]
    detail = "%d/%d distinct hook handler names found in the export table " \
             "(%d named export%s, %d functions)" % (
                 len(unique_handlers) - len(missing), len(unique_handlers),
                 len(export_names), "" if len(export_names) == 1 else "s",
                 function_count)
    if missing:
        return Check(False, title, detail + "; missing: " + ", ".join(missing))
    return Check(True, title, detail)


# ---------------------------------------------------------------------------
# Check 5: version resource strings
# ---------------------------------------------------------------------------


def find_string_encodings(data: bytes, text: str) -> List[str]:
    """Return the encodings in which `text` occurs in the file."""
    encodings: List[str] = []
    try:
        ascii_bytes = text.encode("ascii")
    except UnicodeEncodeError:
        ascii_bytes = None
    if ascii_bytes and ascii_bytes in data:
        encodings.append("ASCII")
    if text.encode("utf-16-le") in data:
        encodings.append("UTF-16LE")
    return encodings


def expected_fixed_file_version() -> Tuple[int, int]:
    """The VS_FIXEDFILEINFO (dwFileVersionMS, dwFileVersionLS) of VERSION_STRINGS[0]."""
    parts = [int(part) for part in VERSION_STRINGS[0].split(".")]
    if len(parts) != 4 or any(part < 0 or part > 0xFFFF for part in parts):
        raise ValueError(
            "VERSION_STRINGS[0] must be four 16-bit numbers, got %r"
            % VERSION_STRINGS[0])
    return ((parts[0] << 16) | parts[1], (parts[2] << 16) | parts[3])


def find_fixed_file_version(data: bytes) -> Optional[Tuple[int, int]]:
    """Return the first plausible VS_FIXEDFILEINFO file version, or None."""
    search_from = 0
    while True:
        index = data.find(VS_FIXEDFILEINFO_SIGNATURE, search_from)
        if index < 0:
            return None
        if index + 16 <= len(data):
            version_ms, version_ls = struct.unpack_from("<II", data, index + 8)
            if version_ms or version_ls:
                return (version_ms, version_ls)
        search_from = index + 1


def check_version_strings(data: bytes) -> Check:
    title = "Version resource strings"
    problems: List[str] = []
    details: List[str] = []

    encodings = find_string_encodings(data, PLATFORM_NAME)
    if encodings:
        details.append("%s: %s" % (PLATFORM_NAME, "+".join(encodings)))
    else:
        problems.append("platform name %r not found (ASCII or UTF-16LE)"
                        % PLATFORM_NAME)

    # D-15: the version has two independent sources and both must be present.
    # The UTF-16LE copy proves the VS_VERSIONINFO resource exists (deleting the
    # .rsrc used to leave the ASCII check green); the ASCII copy proves the
    # compiled-in VC_VERSION_DISPLAY literal exists.  Accepting either one was
    # the weakness this check closes.
    version_encodings: List[str] = []
    matched_version = None
    for version in VERSION_STRINGS:
        found = find_string_encodings(data, version)
        for encoding in found:
            if encoding not in version_encodings:
                version_encodings.append(encoding)
        if found and matched_version is None:
            matched_version = version

    missing_encodings = [
        encoding for encoding in ("ASCII", "UTF-16LE")
        if encoding not in version_encodings]
    if not missing_encodings:
        details.append("version %r: ASCII+UTF-16LE (literal + version resource)"
                       % matched_version)
    elif version_encodings:
        problems.append(
            "version %r is present as %s only; it must appear both in the "
            "UTF-16LE version resource and as an ASCII (VC_VERSION_DISPLAY) "
            "string - the version resource and the compiled-in literal must "
            "agree" % (matched_version, "+".join(version_encodings)))
    else:
        expected_ms, expected_ls = expected_fixed_file_version()
        fixed = find_fixed_file_version(data)
        if fixed == (expected_ms, expected_ls):
            details.append(
                "version %s: numeric VS_FIXEDFILEINFO (MS=0x%08X, LS=0x%08X)"
                % (VERSION_STRINGS[0], expected_ms, expected_ls))
        elif fixed is not None:
            problems.append(
                "version string %r not found (ASCII or UTF-16LE) and the "
                "numeric VS_FIXEDFILEINFO version is MS=0x%08X LS=0x%08X, "
                "expected MS=0x%08X LS=0x%08X"
                % (VERSION_STRINGS[0], fixed[0], fixed[1],
                   expected_ms, expected_ls))
        else:
            problems.append(
                "version string %r not found (ASCII or UTF-16LE) and no "
                "VS_FIXEDFILEINFO block" % VERSION_STRINGS[0])

    pin_encodings = find_string_encodings(data, YRPP_PIN)
    if pin_encodings:
        details.append("YRpp pin %s: %s"
                       % (YRPP_PIN[:12] + "...", "+".join(pin_encodings)))
    else:
        problems.append("YRpp pin %r not found (ASCII or UTF-16LE)"
                        % YRPP_PIN)

    if problems:
        return Check(False, title, "; ".join(problems), details)
    return Check(True, title, "; ".join(details))


# ---------------------------------------------------------------------------
# Check 6: feature registration
# ---------------------------------------------------------------------------


def check_feature_registration(data: bytes) -> Check:
    title = "Feature registration"
    needle = FEATURE_ID.encode("ascii")
    if needle in data:
        return Check(True, title,
                     "feature id %r found as an ASCII string literal"
                     % FEATURE_ID)
    return Check(False, title,
                 "feature id %r not found in the image - was the Example "
                 "module compiled in and registered?" % FEATURE_ID)


# ---------------------------------------------------------------------------
# Check 7: log policy (source)
# ---------------------------------------------------------------------------

SRC_DIR = os.path.join(REPO_ROOT, "src")

# Only this call is asserted: it is the release build's debugger I/O.  A comment
# in the sources must spell it exactly like this to be caught, so the check does
# not try to parse comments.
DEBUG_OUTPUT_NEEDLE = "OutputDebugStringA"

# The base owns exactly one debugger helper: the one EmitDebugOutput() in the
# logger.  The needle must not appear anywhere else in src/, even inside a
# DEBUG-gated region - a second helper is a second path to the debugger API that
# would have to be kept in sync by hand (D-14).
DEBUG_OUTPUT_HELPER_FILE = "src/Core/Log.cpp"

# "A DEBUG build is what makes this region exist": the two spellings the project
# uses.  A region opened like this protects everything up to its matching
# #endif, including its #else branch (the one such branch in src/Core/Log.cpp is
# the explicit [Platform] DebugOutput=yes switch - keeping #else gated there is
# deliberate, docs/Logging.md).  An #elif branch is *not* covered by the gate it
# follows: it is re-evaluated from its own condition (a #elif defined(DEBUG)
# opens a DEBUG region even when the #if branch was not a DEBUG gate).
DEBUG_GATE_RE = re.compile(
    r"^\s*#\s*(?:ifdef\s+DEBUG\b|if\s+defined\s*\(?\s*DEBUG\b)")
# The condition half of an #elif: an #elif has no "#if" prefix, so it needs its
# own spelling of the same gate.  Only the defined(DEBUG) form is recognised;
# that is the only shape the project uses and the only one the policy allows.
DEBUG_ELIF_GATE_RE = re.compile(r"^\s*#\s*elif\s+defined\s*\(?\s*DEBUG\b")
CONDITIONAL_RE = re.compile(r"^\s*#\s*(?:if|ifdef|ifndef)\b")
ELIF_RE = re.compile(r"^\s*#\s*elif\b")
ELSE_RE = re.compile(r"^\s*#\s*else\b")
ENDIF_RE = re.compile(r"^\s*#\s*endif\b")


def check_log_policy_source() -> Check:
    """Release builds must not emit debugger output unconditionally."""
    title = "Log policy (source)"
    violations: List[str] = []
    region_violations = 0
    helper_violations = 0
    scanned = 0

    if not os.path.isdir(SRC_DIR):
        return Check(False, title, "source directory not found: %s" % SRC_DIR)

    for dirpath, dirnames, filenames in os.walk(SRC_DIR):
        dirnames.sort()
        for filename in sorted(filenames):
            if not filename.endswith((".cpp", ".h")):
                continue
            path = os.path.join(dirpath, filename)
            try:
                with open(path, "r", encoding="utf-8", errors="replace") as handle:
                    lines = handle.read().splitlines()
            except OSError as exc:
                return Check(False, title, "cannot read %s: %s" % (path, exc))
            scanned += 1
            rel_path = os.path.relpath(path, REPO_ROOT).replace(os.sep, "/")

            # Stack of "is the innermost open region DEBUG-only?" flags.  An
            # #elif recomputes the flag from its own condition (D-14: a
            # '#elif defined(DEBUG)' branch is a DEBUG region even if the #if
            # before it was not a DEBUG gate).  An #else keeps the flag of the
            # region it continues, which is what makes the documented Log.cpp
            # helper acceptable: its release branch is unreachable without the
            # explicit INI opt-in.
            debug_stack: List[bool] = []
            for line_number, line in enumerate(lines, 1):
                if ELIF_RE.match(line):
                    parent_debug = debug_stack[-2] if len(debug_stack) > 1 else False
                    if debug_stack:
                        debug_stack[-1] = parent_debug or bool(DEBUG_ELIF_GATE_RE.match(line))
                    continue
                if ELSE_RE.match(line):
                    # Deliberate: keep the current branch's gate (docs/Logging.md).
                    continue
                if CONDITIONAL_RE.match(line):
                    parent_debug = debug_stack[-1] if debug_stack else False
                    debug_stack.append(parent_debug or bool(DEBUG_GATE_RE.match(line)))
                    continue
                if ENDIF_RE.match(line):
                    if debug_stack:
                        debug_stack.pop()
                    continue
                if DEBUG_OUTPUT_NEEDLE not in line:
                    continue
                in_debug_region = debug_stack[-1] if debug_stack else False
                if rel_path != DEBUG_OUTPUT_HELPER_FILE:
                    helper_violations += 1
                    violations.append(
                        "%s:%d: %s\n        (the only allowed %s helper is %s)"
                        % (rel_path, line_number, line.strip(),
                           DEBUG_OUTPUT_NEEDLE, DEBUG_OUTPUT_HELPER_FILE))
                elif not in_debug_region:
                    region_violations += 1
                    violations.append(
                        "%s:%d: %s" % (rel_path, line_number, line.strip()))

    if violations:
        reasons: List[str] = []
        if region_violations:
            reasons.append(
                "%d call(s) to %s outside a #ifdef DEBUG region"
                % (region_violations, DEBUG_OUTPUT_NEEDLE))
        if helper_violations:
            reasons.append(
                "%d call(s) to %s outside the single allowed helper file %s"
                % (helper_violations, DEBUG_OUTPUT_NEEDLE,
                   DEBUG_OUTPUT_HELPER_FILE))
        extra = violations[:10]
        if len(violations) > 10:
            extra.append("... and %d more" % (len(violations) - 10))
        extra.append("Move the call into the DEBUG-gated helper in src/Core/Log.cpp")
        extra.append("(or under #ifdef DEBUG); see docs/Logging.md.")
        return Check(
            False, title,
            "%s (%d file(s) scanned)" % ("; ".join(reasons), scanned),
            extra)

    return Check(
        True, title,
        "%d source file(s) scanned; every %s call is inside a #ifdef DEBUG / "
        "#if defined(DEBUG) region of %s"
        % (scanned, DEBUG_OUTPUT_NEEDLE, DEBUG_OUTPUT_HELPER_FILE))


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------


def resolve_dll_path(argument: Optional[str]) -> str:
    if argument is None:
        return DEFAULT_DLL
    expanded = os.path.expanduser(argument)
    if os.path.isabs(expanded):
        return expanded
    relative_to_cwd = os.path.abspath(expanded)
    if os.path.exists(relative_to_cwd):
        return relative_to_cwd
    return os.path.join(REPO_ROOT, expanded)


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="Verify a built VentureCore.dll against the source tree "
                    "and docs/HookCatalogue.md.")
    parser.add_argument(
        "dll", nargs="?", default=None,
        help="DLL to inspect (default: Release/VentureCore.dll)")
    parser.add_argument(
        "--catalogue", default=DEFAULT_CATALOGUE,
        help="hook catalogue markdown file "
             "(default: docs/HookCatalogue.md)")
    args = parser.parse_args(argv)

    dll_path = resolve_dll_path(args.dll)
    catalogue_path = args.catalogue
    if not os.path.isabs(catalogue_path):
        catalogue_path = os.path.join(REPO_ROOT, catalogue_path)

    print("VentureCore static build verification")
    print("  DLL      : %s" % dll_path)
    print("  catalogue: %s" % catalogue_path)
    print()

    checks: List[Check] = []

    try:
        pe, data = open_pe(dll_path)
    except PEFormatError as exc:
        if not os.path.isfile(dll_path):
            report = [Check(
                False, "DLL file found",
                "DLL not found: %s - build it first, e.g. "
                "'scripts/linux/build_linux.sh Release --build-type RELEASE'"
                % dll_path)]
        else:
            report = [Check(False, "PE image format", str(exc))]
        # Check 7 inspects the source tree, not the DLL: keep it in the report so
        # a missing or stale binary cannot hide a release-source violation.
        report.append(check_log_policy_source())
        print_report(report)
        return 1

    checks.append(Check(
        True, "PE image format",
        "MZ/PE, PE32 (magic 0x%03X), machine 0x%04X (i386), %d sections, "
        "ImageBase=0x%08X"
        % (pe.magic, pe.machine, pe.num_sections, pe.image_base)))

    # -- check 2 ---------------------------------------------------------
    hook_entries: Optional[List[Tuple[int, int, str]]] = None
    try:
        hook_entries, hook_detail = read_hook_entries(pe)
        checks.append(Check(True, "Hook section .syhks00", hook_detail))
    except PEFormatError as exc:
        checks.append(Check(False, "Hook section .syhks00", str(exc)))

    # -- check 3 ---------------------------------------------------------
    catalogue_rows: Optional[List[Dict[str, object]]] = None
    try:
        catalogue_rows = parse_hook_catalogue(catalogue_path)
    except CatalogueError as exc:
        checks.append(Check(False, "Hook catalogue parseable", str(exc)))

    if catalogue_rows is not None:
        if hook_entries is not None:
            checks.append(check_hook_table(hook_entries, catalogue_rows))
        else:
            checks.append(Check(False,
                                "Hook table matches docs/HookCatalogue.md",
                                "cannot compare: the .syhks00 section was not "
                                "decoded"))

        # -- check 4 -----------------------------------------------------
        checks.append(check_hook_exports(pe, catalogue_rows))
    else:
        checks.append(Check(False, "Hook table matches docs/HookCatalogue.md",
                            "cannot compare: the hook catalogue was not "
                            "parsed"))
        checks.append(Check(False, "Hook handlers exported",
                            "cannot check: the hook catalogue was not parsed"))

    # -- check 5 ---------------------------------------------------------
    checks.append(check_version_strings(data))

    # -- check 6 ---------------------------------------------------------
    checks.append(check_feature_registration(data))

    # -- check 7 ---------------------------------------------------------
    checks.append(check_log_policy_source())

    print_report(checks)
    return 0 if all(check.ok for check in checks) else 1


if __name__ == "__main__":
    sys.exit(main())
