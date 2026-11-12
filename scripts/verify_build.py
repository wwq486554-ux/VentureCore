#!/usr/bin/env python3
"""Static verification of a built VentureCore.dll.

Usage
-----
    python3 scripts/verify_build.py [DLL]
    VCS_GAMEMD_EXE=/path/to/gamemd.exe python3 scripts/verify_build.py [DLL]
    python3 scripts/verify_build.py --gamemd /path/to/gamemd.exe [DLL]

The positional argument is the DLL to inspect and defaults to
``Release/VentureCore.dll`` under the repository root (the parent directory of
``scripts/``).  A relative path is resolved against the current working
directory first and against the repository root second.  A non-default hook
catalogue can be selected with ``--catalogue PATH``.  The engine executable for
the offline hook-window check (check 10) is selected with ``--gamemd PATH`` or
the ``VCS_GAMEMD_EXE`` environment variable; when neither is given that check
reports an explicit SKIP with a warning instead of pretending the windows were
validated.

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
       Every distinct hook handler name from the catalogue must be exported
       under exactly that name (``DEFINE_HOOK`` exports ``extern "C"``, so the
       names are undecorated; a C++-decorated spelling is accepted as a
       fallback).  The only exports allowed outside the catalogue are the
       documented non-hook surface (the three SyringeEx feature flags and the
       two public lineage functions, see ``NON_HOOK_EXPORTS``); anything else is
       reported as a stale/undocumented export, and unnamed exported functions
       are flagged too.  A substring match is deliberately *not* used: a long
       export name used to satisfy a shorter catalogue name.

5. Version resource strings and build provenance
       The platform name, the version and the pinned YRpp commit are expected in
       the image.  The version must be present **twice**: as UTF-16LE (the
       VS_VERSIONINFO resource strings written by ``src/version.rc``) and as
       ASCII (the compiled-in ``VC_VERSION_DISPLAY`` literal used by the startup
       log and the lineage marker).  Requiring both means a build with the
       version resource stripped, or one that only embeds the version in a C++
       literal, fails this check.  A numeric ``VS_FIXEDFILEINFO`` block with the
       expected ``dwFileVersionMS/LS`` is accepted as a fallback when no literal
       version string is present at all.

       The same resource is the build's provenance record, so two more values
       are asserted: ``PlatformBuildType`` (written by ``src/version.rc`` from
       the ``VC_BUILD_TYPE`` macro) must be ``RELEASE`` - a ``LOCAL`` or
       ``NIGHTLY`` development build is rejected - and ``GitCommit`` must equal
       the current working copy's git HEAD, which rejects a stale DLL that was
       built before the current sources (the comparison is only skipped, with an
       explicit detail line, when git itself is unavailable).

6. Feature registration
       The ``example_unit_counter`` feature id is expected as an ASCII string
       literal, proving the Example module was compiled in and registered.

7. Log policy (source)
       The release-source assertion behind the logger's cost policy: calls to
       the debugger API (``OutputDebugString`` in every spelling - ``...A``,
       ``...W``, the bare macro) may only appear inside a ``#ifdef DEBUG`` /
       ``#if defined(DEBUG)`` region **and only in** ``src/Core/Log.cpp`` - the
       base has exactly one gated debugger helper, so a second copy is rejected
       even when it sits inside a DEBUG region.
       The check is line-based: it walks every ``src/**`` and ``include/**``
       ``.cpp`` / ``.c`` / ``.h`` / ``.hpp`` / ``.inl`` file, tracks the
       preprocessor conditional nesting (``#if``/``#ifdef``/``#ifndef`` push,
       ``#elif`` recomputes its branch from its own condition, ``#endif`` pops)
       and reports ``file:line`` for a call outside such a region or outside the
       helper file.
       An ``#else`` branch is the **negation** of its conditional: entering it
       invalidates the DEBUG gate of the region it continues, so a call there is
       only release-safe when an enclosing DEBUG region still gates it.  The
       base's one helper uses exactly that shape - its ``#else`` branch is the
       runtime ``[Platform] DebugOutput=yes`` switch - so the helper's release
       branch must match an explicit whitelist shape: the call is guarded by
       ``if (g_debugOutput)`` (on the same line or on the immediately preceding
       line).  Removing that run-time guard therefore fails the check, which is
       the regression this shape protects (D-14).
       Because it inspects the *sources* this check keeps working when the DLL
       was built from an older state, and its failure tells you exactly which
       line to move.  It is a **lexical hygiene check, not a proof that the
       release branch is unreachable** (see docs/Logging.md).

8. Legacy tier gate removed (source)
       ``2.0`` replaced the host support-level scalar with per-feature
       ``requiredHosts`` / ``anyOfHosts`` declarations.  The removed API
       spellings (``Tier``, ``TierName``, ``minTier``, ``ResolveTier``,
       ``CurrentTier``, a ``tier`` field access and identifier-shaped uses such
       as ``tier = ...``) may no longer appear anywhere in the repository that
       describes the current state: sources, headers (including ``include/``),
       docs, examples, project files.  The scan is case-insensitive for API and
       identifier spellings; the plain English noun is still allowed in prose
       (the documentation deliberately records that the tier concept was
       removed), so the bare ``tier = / tier, / tier)`` shape is only rejected
       in C/C++ sources where it can only be an identifier.
       ``CHANGELOG.md`` is exempt because it records the 1.x history verbatim.
       ``external/`` is skipped explicitly (see ``TIER_SCAN_SKIP_NOTES``): it is
       the vendored third-party dependency, not this repository's current state.

9. Removed closed-source host name absent (source)
       ``2.0`` dropped the fifth, closed-source ecosystem host from the public
       surface (module probe, host bit and every mention).  Its name may not
       appear anywhere in the repository any more - code, docs, historical
       CHANGELOG included.  This verifier is exempt because it defines the
       token.

10. Hook windows vs the engine executable (offline)
       The hook records must be valid instruction windows of the pinned
       ``gamemd.exe``: every address inside the engine's ``.text`` section (via
       the PE ``ImageBase``), every size at least 5 bytes, the instructions
       decoded from the address tiling the window exactly (nothing cut in half),
       ``address + size`` being an instruction start, and the windows not
       overlapping (``scripts/verify_hook_windows.py`` does this with GNU
       ``objdump``; the engine is never executed).  This needs the engine
       binary: pass ``--gamemd PATH`` or set ``VCS_GAMEMD_EXE``.  Without it the
       check is reported as ``[SKIP]`` plus a warning - never as PASS.

11. Release CRT assertions absent (binary)
       A RELEASE build compiles with ``/DNDEBUG``, so no live ``assert`` call
       site exists and the CRT assert machinery is not linked in.  The UTF-16
       string ``Assertion failed`` (the CRT's ``_wassert`` message) must
       therefore not appear in the image; if it does, an assert path is still
       active in the release artifact (TOOL-03).

12. INI live/probe tooling self-checks (source)
       ``scripts/check_live_log.py`` (judges a live ``<Platform>.log``) and
       ``scripts/compare_ini_template.py`` (compares the generated template with
       the shipped example) carry an offline ``--self-check`` mode against a
       repository fixture.  Both are run here, so the tools that the in-game
       acceptance verdict depends on are themselves part of the automatic gate
       (INI-07): a change that breaks their parsing, duplicate-key detection,
       template-freshness rule or platform-name derivation fails this check even
       when no game is available.

Exit status is 0 when no check fails and 1 otherwise.  A skipped check (see
check 10) is reported as ``[SKIP]`` and counted in the result line - it is not a
pass, but it does not fail the run either.  The expected strings and ids live in
the constants below; when the platform is renamed (see docs/Renaming.md) update
them together with the rest of the rename.
"""

from __future__ import annotations

import argparse
import os
import re
import struct
import subprocess
import sys
from typing import Dict, List, Optional, Sequence, Tuple

import verify_hook_windows

# ---------------------------------------------------------------------------
# Expected values / fixed facts
# ---------------------------------------------------------------------------

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_DLL = os.path.join(REPO_ROOT, "Release", "VentureCore.dll")
DEFAULT_CATALOGUE = os.path.join(REPO_ROOT, "docs", "HookCatalogue.md")

# Platform identity (src/Core/PlatformConfig.h, src/version.rc).
PLATFORM_NAME = "VentureCore"

# The version strings expected in the image (src/Core/PlatformVersion.h).  The
# version resource writes VC_VERSION_DISPLAY ("v2.0.0.0"), which contains the long
# version "2.0.0.0" as a substring (the leading "v" is not part of the string).
# For a non-stable build the display also carries the prerelease suffix
# ("v2.0.0.0-alpha"); add that spelling here too.
# See check_version_strings() for why both encodings of the version are required.
VERSION_STRINGS: Tuple[str, ...] = ("2.0.0.0", "2.0.0")

# The three-gate YRpp pin (src/Core/EnginePin.h).
YRPP_PIN = "c6c522f77b58508588d9a8652c53356e4aef7a96"

# Feature registry id of the Example module (src/Features/).
FEATURE_ID = "example_unit_counter"

# Build provenance: the VS_VERSIONINFO values written by src/version.rc from the
# build configuration.  VC_BUILD_TYPE is "RELEASE" only for a
# --build-type RELEASE build (src/Core/PlatformVersion.h); a LOCAL or NIGHTLY
# development build must not pass the release acceptance gate (TOOL-06/DOC-10).
BUILD_TYPE_KEY = "PlatformBuildType"
EXPECTED_BUILD_TYPE = "RELEASE"
GIT_COMMIT_KEY = "GitCommit"
GIT_REF_KEY = "GitRef"
GIT_COMMAND_TIMEOUT_SECONDS = 15

# The in-game log/template tools carry an offline self-check that is part of the
# automatic acceptance gate (INI-07).  The marker makes a silently-truncated or
# silently-skipped run fail instead of passing on the exit code alone.
INI_TOOL_SELF_CHECKS: Tuple[Tuple[str, str], ...] = (
    ("compare_ini_template.py", "SELF-CHECK: PASS"),
    ("check_live_log.py", "SELF-CHECK: PASS"),
)
INI_TOOL_TIMEOUT_SECONDS = 120

# The 2.0 removal of the legacy host "support tier" (see docs/HostMatrix.md).
# These spellings must not appear anywhere in the repository that describes the
# current state; CHANGELOG.md keeps the 1.x history verbatim, and this verifier
# of course contains the token list itself.
#
# The scan is split into three shapes:
#   * API_TOKENS  - unambiguous legacy API spellings, case-insensitive, any file;
#   * IDENTIFIER  - the capitalised standalone type name (as before) plus the
#                   ``ctx.tier`` / ``ctx->tier`` / ``Tier::tier`` access shape in
#                   any case, any file.  ``.``/``::``/``->`` never precede an
#                   English noun, so prose is unaffected;
#   * CODE_SHAPE  - the bare ``tier = / tier, / tier; / tier)`` shape, only in
#                   C/C++ sources: that is the shape a declaration or assignment
#                   takes (TOOL-05 ``tier = 2;``), while the documentation's
#                   English "no support tier)" must not be rejected.
TIER_API_TOKENS_RE = re.compile(
    r"\b(?:TierName|minTier|ResolveTier|CurrentTier|g_tier|tierSupported)\b",
    re.IGNORECASE)
TIER_IDENTIFIER_RE = re.compile(r"\bTier\b")
TIER_ACCESS_RE = re.compile(r"(?:->|\.|::)\s*tier\b", re.IGNORECASE)
TIER_CODE_SHAPE_RE = re.compile(r"\btier\s*[=;,)]", re.IGNORECASE)
TIER_CODE_EXTENSIONS = (".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx",
                        ".inl", ".rc")
TIER_SCAN_SKIP_DIRS = {".git", "external", "Release", "Debug"}
# Directories this check intentionally does not walk, with the reason that makes
# the skip explicit instead of silent.  ``external/`` is the vendored YRpp
# dependency fetched at the pinned commit: its own API may legitimately contain
# a tier concept and it is not this repository's current state.
TIER_SCAN_SKIP_NOTES = {
    ".git": "version-control metadata",
    "external": "vendored YRpp dependency, fetched at the pinned commit",
    "Release": "build output",
    "Debug": "build output",
}
TIER_SCAN_SKIP_FILES = {"CHANGELOG.md", "scripts/verify_build.py"}
TIER_SCAN_EXTENSIONS = (
    ".cpp", ".c", ".cc", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl", ".md",
    ".py", ".sh", ".example", ".rc", ".vcxproj", ".props", ".sln", ".txt",
    ".ini", ".yml", ".yaml", ".json")

# The closed-source ecosystem host removed from the public surface in 2.0.  The
# name must not appear anywhere in the repository, historical CHANGELOG
# included; this verifier is exempt because it defines the token list.
CLOSED_HOST_TOKENS_RE = re.compile(r"kaleido", re.IGNORECASE)
CLOSED_HOST_SCAN_SKIP_DIRS = {".git", "external", "Release", "Debug"}
CLOSED_HOST_SCAN_SKIP_FILES = {"scripts/verify_build.py"}
CLOSED_HOST_SCAN_EXTENSIONS = TIER_SCAN_EXTENSIONS

# Exports that are not hook handlers but are part of the documented public
# surface (source of truth: docs/HookCatalogue.md + external/YRpp/Syringe.h +
# src/Core/Lineage.cpp).  The hook-handler export check below matches the
# catalogue names exactly and reports any export outside both sets as stale /
# undocumented (TOOL-06: the old substring match accepted a longer export name
# in place of a catalogue name, and ignored exports the catalogue does not
# describe).
NON_HOOK_EXPORTS = frozenset({
    # SyringeEx feature flags exported by external/YRpp/Syringe.h.
    "ESPModification",
    "ReladdrInstructionFixup",
    "ZFPreservation",
    # Public lineage API exported by src/Core/Lineage.cpp.
    "VentureLineage_CountConflicts",
    "VentureLineage_GetInfo",
})

# The engine executable for the offline hook-window check #10 (TOOL-06/HOOK-04).
# Command line wins over the environment; nothing is hard-coded because the
# engine binary is an external input.
GAMEMD_ENV_VAR = verify_hook_windows.GAMEMD_ENV_VAR

# A release build compiles with /DNDEBUG, so no live CRT assert path may remain
# (TOOL-03).  The string is wide only - that is how the MSVC CRT formats it.
ASSERTION_FAILED_NEEDLE = "Assertion failed"

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
    """One PASS/FAIL/SKIP line plus optional indented detail lines."""

    __slots__ = ("ok", "title", "detail", "extra", "skipped")

    def __init__(self, ok: bool, title: str, detail: str = "",
                 extra: Optional[Sequence[str]] = None,
                 skipped: bool = False) -> None:
        self.ok = ok
        self.title = title
        self.detail = detail
        self.extra: List[str] = list(extra or ())
        # A skipped check could not run (no input available).  It is reported as
        # [SKIP], never as [PASS], and it does not fail the run - but the result
        # line counts it so a skipped check cannot masquerade as a pass.
        self.skipped = skipped


def print_report(checks: Sequence[Check]) -> None:
    for check in checks:
        if check.skipped:
            status = "SKIP"
        else:
            status = "PASS" if check.ok else "FAIL"
        print("[%s] %s" % (status, check.title))
        if check.detail:
            print("       %s" % check.detail)
        for line in check.extra:
            print("       %s" % line)
    total = len(checks)
    skipped = sum(1 for check in checks if check.skipped)
    failed = sum(1 for check in checks if not check.ok and not check.skipped)
    passed = total - skipped - failed
    if failed == 0:
        summary = "Result: %d/%d checks passed." % (passed, total)
        if skipped:
            summary = ("Result: %d/%d checks passed, %d skipped (see the SKIP "
                       "line above - a skipped check is not a pass)."
                       % (passed, total, skipped))
        print("\n" + summary)
    else:
        summary = "Result: FAILED - %d/%d checks passed" % (passed, total)
        if skipped:
            summary += ", %d skipped" % skipped
        print("\n" + summary + ".")


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


def _handler_export_match(handler: str, exported: str) -> bool:
    """Exact export-name match; a C++-decorated spelling is the only fallback.

    ``DEFINE_HOOK`` exports the handler with ``extern "C"``, so the regular
    spelling is the bare name.  A decorated name (``?Venture_...@@...``) can only
    appear if the mangling convention changes; accepting it here keeps the check
    about the *handler* rather than about the decoration.  A plain superstring
    export name is NOT accepted (that was the TOOL-06 weakness).
    """
    return exported == handler or (exported.startswith("?") and handler in exported)


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
               if not any(_handler_export_match(handler, exported)
                          for exported in export_names)]
    unexpected = [
        exported for exported in export_names
        if exported not in NON_HOOK_EXPORTS
        and not any(_handler_export_match(handler, exported)
                    for handler in unique_handlers)]

    detail = "%d/%d distinct hook handler names exported under their exact " \
             "name (%d named export%s, %d function%s, %d allow-listed non-hook " \
             "export%s)" % (
                 len(unique_handlers) - len(missing), len(unique_handlers),
                 len(export_names), "" if len(export_names) == 1 else "s",
                 function_count, "" if function_count == 1 else "s",
                 sum(1 for exported in export_names
                     if exported in NON_HOOK_EXPORTS),
                 "" if sum(1 for exported in export_names
                           if exported in NON_HOOK_EXPORTS) == 1 else "s")
    problems: List[str] = []
    if missing:
        problems.append("missing from the export table: " + ", ".join(missing))
    if unexpected:
        problems.append(
            "exported but not described by docs/HookCatalogue.md or the "
            "NON_HOOK_EXPORTS allow-list (stale/undocumented export): "
            + ", ".join(unexpected))
    if function_count != len(export_names):
        problems.append(
            "the DLL has %d exported function(s) but only %d named export(s); "
            "unnamed exports are not described by the hook catalogue"
            % (function_count, len(export_names)))
    if problems:
        return Check(False, title, detail + "; " + "; ".join(problems))
    return Check(True, title, detail)


# ---------------------------------------------------------------------------
# Check 5: version resource strings and build provenance
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


# A VS_VERSIONINFO VALUE block: wLength/wValueLength/wType (6 bytes), szKey as a
# NUL-terminated UTF-16LE string, DWORD alignment padding, then the UTF-16LE
# value.  The resource data starts DWORD-aligned, so the absolute offset of the
# key is enough to find the aligned value start.
VERSION_VALUE_BLOCK_HEADER = 6


def find_version_string_value(data: bytes, key: str) -> Optional[str]:
    """Return the UTF-16LE VS_VERSIONINFO value for `key`, or None.

    ``data`` is the whole image; the first occurrence that decodes as a
    printable, non-empty UTF-16LE string wins.  Returns None when the key does
    not occur at all (an unstripped resource always contains it, so None means a
    build with a stripped/stale version resource).
    """
    key_bytes = key.encode("utf-16-le")
    needle = key_bytes + b"\x00\x00"
    # Offset of the value relative to the VALUE block start: header + key +
    # terminator, rounded up to the DWORD alignment of the block.
    relative_value_offset = (
        VERSION_VALUE_BLOCK_HEADER + len(key_bytes) + 2 + 3) & ~3
    search_from = 0
    while True:
        index = data.find(needle, search_from)
        if index < 0:
            return None
        value_start = index - VERSION_VALUE_BLOCK_HEADER + relative_value_offset
        search_from = index + 1
        if value_start < 0 or value_start + 2 > len(data):
            continue
        # The value itself starts on an even offset from value_start and ends at
        # the first NUL UTF-16 code unit.
        end = value_start
        while end + 1 < len(data) and not (data[end] == 0 and data[end + 1] == 0):
            end += 2
        if end == value_start or end + 1 >= len(data):
            continue
        try:
            value = data[value_start:end].decode("utf-16-le")
        except UnicodeDecodeError:
            continue
        if value and all(0x20 <= ord(character) <= 0x7E for character in value):
            return value


def working_copy_git_stamp() -> Optional[Tuple[str, Optional[str]]]:
    """(short HEAD commit, full ref) of this repository, or None if unavailable.

    The build scripts stamp ``git rev-parse --short HEAD`` and either
    ``git symbolic-ref HEAD`` or the first ``for-each-ref`` pointing at HEAD
    (detached build from a tag), so the comparison uses exactly the same
    commands.  ``None`` means git (or the repository) is unavailable and the
    stamp comparison is skipped with an explicit detail line.
    """
    def _git(*arguments: str) -> Optional[str]:
        try:
            result = subprocess.run(
                ["git", "-C", REPO_ROOT, *arguments],
                capture_output=True, text=True, timeout=GIT_COMMAND_TIMEOUT_SECONDS)
        except (OSError, subprocess.SubprocessError):
            return None
        if result.returncode != 0:
            return None
        return result.stdout.strip() or None

    commit = _git("rev-parse", "--short", "HEAD")
    if not commit:
        return None
    ref = _git("symbolic-ref", "HEAD")
    if not ref:
        ref = _git("for-each-ref", "--points-at", "HEAD", "--format=%(refname)")
        ref = ref.splitlines()[0] if ref else None
    return commit, ref


def check_build_provenance(data: bytes) -> Tuple[List[str], List[str]]:
    """Validate PlatformBuildType/GitCommit against the working copy.

    Returns (problems, details) and is folded into the version-resource check so
    that a LOCAL/NIGHTLY build or a DLL built from older sources is rejected
    (TOOL-06/DOC-10).
    """
    problems: List[str] = []
    details: List[str] = []

    build_type = find_version_string_value(data, BUILD_TYPE_KEY)
    if build_type is None:
        problems.append(
            "the version resource has no %r value (src/version.rc writes "
            "VC_BUILD_TYPE); cannot prove this is a RELEASE build"
            % BUILD_TYPE_KEY)
    elif build_type != EXPECTED_BUILD_TYPE:
        problems.append(
            "PlatformBuildType is %r, expected %r - LOCAL/NIGHTLY development "
            "builds are not acceptance artifacts; rebuild with "
            "'--build-type RELEASE'"
            % (build_type, EXPECTED_BUILD_TYPE))
    else:
        details.append("build type %s (%s)" % (EXPECTED_BUILD_TYPE, BUILD_TYPE_KEY))

    git_commit = find_version_string_value(data, GIT_COMMIT_KEY)
    stamp = working_copy_git_stamp()
    if git_commit is None:
        problems.append(
            "the version resource has no %r value - the DLL is not git-stamped "
            "(built without git?), so it cannot be tied to the sources it came "
            "from" % GIT_COMMIT_KEY)
    elif stamp is None:
        details.append(
            "git stamp %s %s (working-copy git unavailable - comparison "
            "skipped)" % (GIT_COMMIT_KEY, git_commit))
    elif git_commit != stamp[0]:
        problems.append(
            "stale or foreign build: the DLL records %s %r but the working copy "
            "is at %r - rebuild after the current sources changed (the stamp is "
            "the freshness assertion, not a file hash)"
            % (GIT_COMMIT_KEY, git_commit, stamp[0]))
    else:
        details.append("git stamp %s %s matches the working copy"
                       % (GIT_COMMIT_KEY, git_commit))
        git_ref = find_version_string_value(data, GIT_REF_KEY)
        if git_ref and stamp[1] and git_ref != stamp[1]:
            # Same commit, different name (e.g. built on a tag, verified on a
            # branch): this is informational, the commit comparison above is the
            # freshness assertion.
            details.append("note: %s %r differs from the working-copy ref %r "
                           "(same commit - tag/detached build?)"
                           % (GIT_REF_KEY, git_ref, stamp[1]))

    return problems, details


def check_version_strings(data: bytes) -> Check:
    title = "Version resource strings and build provenance"
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

    # TOOL-06/DOC-10: the version resource is also the build's provenance record
    # - a RELEASE build and the current git HEAD are part of "this DLL is the
    # artifact the sources say it is".
    provenance_problems, provenance_details = check_build_provenance(data)
    problems.extend(provenance_problems)
    details.extend(provenance_details)

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

# TOOL-04: scan src/ *and* include/ (public headers can call the API too), and
# every C/C++ extension that can reach the compiler - a helper in a .inl is just
# as live as one in a .cpp.
LOG_POLICY_SCAN_ROOTS = ("src", "include")
LOG_POLICY_SCAN_EXTENSIONS = (
    ".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl")

# Every spelling of the debugger API: OutputDebugStringA / ...W, the bare macro,
# and any future suffix.  The scan is lexical on purpose - a comment that spells
# the name is caught as well, and the sources must spell it exactly like this if
# the check is to see it.
DEBUG_OUTPUT_CALL_RE = re.compile(r"\bOutputDebugString[A-Za-z0-9_]*\b")

# The base owns exactly one debugger helper: EmitDebugOutput() in the logger.
# The call must not appear anywhere else, even inside a DEBUG-gated region - a
# second helper is a second path to the debugger API that would have to be kept
# in sync by hand (D-14).
DEBUG_OUTPUT_HELPER_FILE = "src/Core/Log.cpp"

# The helper's release branch is the one call that is *not* DEBUG-gated, so it
# must match an explicit whitelist shape: the call is behind the run-time
# [Platform] DebugOutput switch.  Two shapes are accepted - the guard on the same
# line (``if (g_debugOutput) OutputDebugStringA(x);``) or on the immediately
# preceding line (``if (g_debugOutput)`` followed by the call), plus the
# early-exit variant (``if (!g_debugOutput) return;``).  Removing the guard
# fails the check; that is exactly the TOOL-04 regression (a call still compiled
# into release builds).
DEBUG_OUTPUT_GUARD_RE = re.compile(r"\bif\s*\(\s*g_debugOutput\s*\)")
DEBUG_OUTPUT_EARLY_RETURN_RE = re.compile(
    r"\bif\s*\(\s*!\s*g_debugOutput\s*\)\s*return\b")

# "A DEBUG build is what makes this region exist": the two spellings the project
# uses.  A region opened like this is DEBUG-only.  An #elif branch is
# re-evaluated from its own condition (a '#elif defined(DEBUG)' opens a DEBUG
# region even when the #if branch was not a DEBUG gate).  An #else branch is the
# **negation** of the condition(s) before it: entering it invalidates the DEBUG
# gate of the region it continues, so only an enclosing DEBUG region can still
# protect it (that is why the Log.cpp helper's #else branch needs the explicit
# run-time guard).
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


def _debug_output_guard_applies(lines: Sequence[str], line_index: int,
                                match_start: int) -> bool:
    """Does the whitelisted run-time guard protect this helper call?"""
    if DEBUG_OUTPUT_GUARD_RE.search(lines[line_index][:match_start]):
        return True
    # Walk back over blank lines only: the whitelist shape is the adjacent guard.
    previous = line_index - 1
    while previous >= 0 and not lines[previous].strip():
        previous -= 1
    if previous < 0:
        return False
    previous_line = lines[previous]
    if DEBUG_OUTPUT_CALL_RE.search(previous_line):
        return False  # two calls on one line: the guard only wraps the first
    if DEBUG_OUTPUT_GUARD_RE.search(previous_line):
        return True
    return bool(DEBUG_OUTPUT_EARLY_RETURN_RE.search(previous_line))


def check_log_policy_source() -> Check:
    """Debugger output must stay behind the DEBUG gate / the release helper."""
    title = "Log policy (source)"
    violations: List[str] = []
    region_violations = 0
    helper_violations = 0
    guarded_calls = 0
    scanned = 0

    root_paths: List[Tuple[str, str]] = []
    missing_roots: List[str] = []
    for root in LOG_POLICY_SCAN_ROOTS:
        path = os.path.join(REPO_ROOT, root)
        if os.path.isdir(path):
            root_paths.append((root, path))
        else:
            missing_roots.append(root)
    if missing_roots:
        return Check(False, title,
                     "source directories not found: %s"
                     % ", ".join(missing_roots))

    for root, root_path in root_paths:
        for dirpath, dirnames, filenames in os.walk(root_path):
            dirnames.sort()
            for filename in sorted(filenames):
                if not filename.endswith(LOG_POLICY_SCAN_EXTENSIONS):
                    continue
                path = os.path.join(dirpath, filename)
                try:
                    with open(path, "r", encoding="utf-8",
                              errors="replace") as handle:
                        lines = handle.read().splitlines()
                except OSError as exc:
                    return Check(False, title,
                                 "cannot read %s: %s" % (path, exc))
                scanned += 1
                rel_path = os.path.relpath(
                    path, REPO_ROOT).replace(os.sep, "/")

                # Stack of (enclosing gate, current branch gate) pairs.  The
                # current branch gate answers "is this line only compiled in a
                # DEBUG build?"; #elif recomputes it from its own condition and
                # #else drops back to the enclosing gate (its condition is the
                # negation of the branch that preceded it).
                debug_stack: List[Tuple[bool, bool]] = []
                for line_index, line in enumerate(lines):
                    line_number = line_index + 1
                    if ELIF_RE.match(line):
                        if debug_stack:
                            enclosing = debug_stack[-1][0]
                            debug_stack[-1] = (
                                enclosing,
                                enclosing or bool(
                                    DEBUG_ELIF_GATE_RE.match(line)))
                        continue
                    if ELSE_RE.match(line):
                        if debug_stack:
                            enclosing = debug_stack[-1][0]
                            debug_stack[-1] = (enclosing, enclosing)
                        continue
                    if CONDITIONAL_RE.match(line):
                        enclosing = (debug_stack[-1][1]
                                     if debug_stack else False)
                        debug_stack.append(
                            (enclosing,
                             enclosing or bool(DEBUG_GATE_RE.match(line))))
                        continue
                    if ENDIF_RE.match(line):
                        if debug_stack:
                            debug_stack.pop()
                        continue
                    matches = list(DEBUG_OUTPUT_CALL_RE.finditer(line))
                    if not matches:
                        continue
                    in_debug_region = (debug_stack[-1][1]
                                       if debug_stack else False)
                    for match in matches:
                        spelling = match.group(0)
                        # The helper file is the *only* place allowed to talk to
                        # the debugger, even inside a DEBUG-gated region: a
                        # second helper would have to be kept in sync by hand
                        # (D-14).
                        if rel_path != DEBUG_OUTPUT_HELPER_FILE:
                            helper_violations += 1
                            violations.append(
                                "%s:%d: %s\n        (%s must stay inside the "
                                "single DEBUG-gated helper %s)"
                                % (rel_path, line_number, line.strip(),
                                   spelling, DEBUG_OUTPUT_HELPER_FILE))
                            continue
                        if in_debug_region:
                            continue
                        if _debug_output_guard_applies(
                                lines, line_index, match.start()):
                            guarded_calls += 1
                            continue
                        region_violations += 1
                        violations.append(
                            "%s:%d: %s\n        (%s in the helper is not "
                            "behind the `if (g_debugOutput)` run-time guard)"
                            % (rel_path, line_number, line.strip(), spelling))

    if violations:
        reasons: List[str] = []
        if region_violations:
            reasons.append(
                "%d call(s) to OutputDebugString* compiled in without a DEBUG "
                "gate or the g_debugOutput run-time guard"
                % region_violations)
        if helper_violations:
            reasons.append(
                "%d call(s) to OutputDebugString* outside the single allowed "
                "helper file %s" % (helper_violations,
                                    DEBUG_OUTPUT_HELPER_FILE))
        extra = violations[:10]
        if len(violations) > 10:
            extra.append("... and %d more" % (len(violations) - 10))
        extra.append("Move the call into the DEBUG-gated helper in "
                     "src/Core/Log.cpp (or under #ifdef DEBUG); the helper's "
                     "release branch must keep the `if (g_debugOutput)` guard.")
        extra.append("See docs/Logging.md - this is a lexical hygiene check.")
        return Check(
            False, title,
            "%s (%d file(s) scanned)" % ("; ".join(reasons), scanned),
            extra)

    return Check(
        True, title,
        "%d source file(s) scanned (src/ + include/); every OutputDebugString* "
        "call is DEBUG-gated except the whitelisted `if (g_debugOutput)` helper "
        "in %s (%d whitelisted call(s)) - lexical hygiene, not a proof that the "
        "release branch is unreachable (see docs/Logging.md)"
        % (scanned, DEBUG_OUTPUT_HELPER_FILE, guarded_calls))


# ---------------------------------------------------------------------------
# Check 8: the 2.0 removal of the host support tier (source)
# ---------------------------------------------------------------------------


def check_legacy_tier_removed() -> Check:
    """The 2.0 feature API has no host support tier: no tier token may survive."""
    title = "Legacy tier gate removed (source)"
    hits: List[str] = []
    skipped_dirs: List[str] = []
    scanned = 0

    for dirpath, dirnames, filenames in os.walk(REPO_ROOT):
        kept_dirs: List[str] = []
        for directory in sorted(dirnames):
            if directory in TIER_SCAN_SKIP_DIRS:
                note = TIER_SCAN_SKIP_NOTES.get(directory)
                if note:
                    skipped_dirs.append("%s/ (%s)" % (directory, note))
                continue
            kept_dirs.append(directory)
        dirnames[:] = kept_dirs
        for filename in sorted(filenames):
            if not filename.endswith(TIER_SCAN_EXTENSIONS):
                continue
            path = os.path.join(dirpath, filename)
            rel_path = os.path.relpath(path, REPO_ROOT).replace(os.sep, "/")
            if rel_path in TIER_SCAN_SKIP_FILES:
                continue
            try:
                with open(path, "r", encoding="utf-8", errors="replace") as handle:
                    lines = handle.read().splitlines()
            except OSError as exc:
                return Check(False, title, "cannot read %s: %s" % (rel_path, exc))
            scanned += 1
            patterns = [TIER_API_TOKENS_RE, TIER_IDENTIFIER_RE, TIER_ACCESS_RE]
            if filename.endswith(TIER_CODE_EXTENSIONS):
                # The bare `tier = / tier, / tier)` shape is only an identifier
                # in C/C++ sources; the English noun in documentation prose
                # ("no support tier)") is how the 2.0 design is documented and
                # must stay allowed.
                patterns.append(TIER_CODE_SHAPE_RE)
            for line_number, line in enumerate(lines, 1):
                if any(pattern.search(line) for pattern in patterns):
                    hits.append("%s:%d: %s"
                                % (rel_path, line_number,
                                   line.strip()[:140]))

    if hits:
        extra = hits[:10]
        if len(hits) > 10:
            extra.append("... and %d more" % (len(hits) - 10))
        extra.append("2.0 replaced the tier gate with requiredHosts/anyOfHosts - "
                     "see docs/HostMatrix.md")
        return Check(
            False, title,
            "%d occurrence(s) of the removed tier gate (%d file(s) scanned)"
            % (len(hits), scanned), extra)

    detail = ("%d file(s) scanned; no tier-gate token left (CHANGELOG.md "
              "history is exempt)" % scanned)
    if skipped_dirs:
        unique = sorted(set(skipped_dirs))
        detail += "; skipped by design: " + ", ".join(unique)
    return Check(True, title, detail)


# ---------------------------------------------------------------------------
# Check 9: the removed closed-source host name (source)
# ---------------------------------------------------------------------------


def check_closed_source_host_removed() -> Check:
    """The removed closed-source host must not be named anywhere any more."""
    title = "Removed closed-source host name absent (source)"
    hits: List[str] = []
    scanned = 0

    for dirpath, dirnames, filenames in os.walk(REPO_ROOT):
        dirnames[:] = sorted(
            directory for directory in dirnames
            if directory not in CLOSED_HOST_SCAN_SKIP_DIRS)
        for filename in sorted(filenames):
            if not filename.endswith(CLOSED_HOST_SCAN_EXTENSIONS):
                continue
            path = os.path.join(dirpath, filename)
            rel_path = os.path.relpath(path, REPO_ROOT).replace(os.sep, "/")
            if rel_path in CLOSED_HOST_SCAN_SKIP_FILES:
                continue
            try:
                with open(path, "r", encoding="utf-8", errors="replace") as handle:
                    lines = handle.read().splitlines()
            except OSError as exc:
                return Check(False, title, "cannot read %s: %s" % (rel_path, exc))
            scanned += 1
            for line_number, line in enumerate(lines, 1):
                if CLOSED_HOST_TOKENS_RE.search(line):
                    hits.append("%s:%d: %s"
                                % (rel_path, line_number, line.strip()[:140]))

    if hits:
        extra = hits[:10]
        if len(hits) > 10:
            extra.append("... and %d more" % (len(hits) - 10))
        extra.append("the host was removed in 2.0 - keep the audit fact, drop the "
                     "name (see CHANGELOG.md / docs/HostMatrix.md)")
        return Check(
            False, title,
            "%d occurrence(s) of the removed host name (%d file(s) scanned)"
            % (len(hits), scanned), extra)

    return Check(
        True, title,
        "%d file(s) scanned; the removed closed-source host is not named anywhere"
        % scanned)


# ---------------------------------------------------------------------------
# Check 10: offline hook-window validation against the engine executable
# ---------------------------------------------------------------------------


def resolve_engine_path(argument: Optional[str]) -> Optional[str]:
    """Resolve --gamemd / $VCS_GAMEMD_EXE to a path, or None when not supplied."""
    candidate = argument or os.environ.get(GAMEMD_ENV_VAR)
    if not candidate:
        return None
    expanded = os.path.expanduser(candidate)
    if not os.path.isabs(expanded):
        expanded = os.path.abspath(expanded)
    return expanded


def check_hook_windows(dll_path: str,
                       expected_count: Optional[int],
                       engine_path: Optional[str]) -> Check:
    """Validate every .syhks00 window against the engine's instructions.

    The engine executable is an explicit input (``--gamemd`` / ``$VCS_GAMEMD_EXE``)
    because it is not part of this repository.  Without it this check is reported
    as an explicit SKIP with a warning - never as a PASS, so a machine without
    the engine cannot make the acceptance look green.
    """
    title = "Hook windows vs gamemd.exe (offline)"
    if engine_path is None:
        return Check(
            True, title,
            "SKIPPED - no engine executable supplied (pass --gamemd PATH or set "
            "%s); the hook windows were NOT validated against the engine"
            % GAMEMD_ENV_VAR,
            extra=["WARNING: this is a skip, not a pass - run the verifier with "
                   "the pinned gamemd.exe before shipping."],
            skipped=True)
    if not os.path.isfile(engine_path):
        return Check(False, title,
                     "engine executable not found: %s (from --gamemd/"
                     "$%s)" % (engine_path, GAMEMD_ENV_VAR))
    try:
        report = verify_hook_windows.validate(
            dll_path, engine_path, expected_count=expected_count)
    except verify_hook_windows.HookWindowError as exc:
        return Check(False, title, str(exc))

    if report.ok:
        return Check(True, title, report.summary())

    extra = ["engine: %s" % engine_path]
    extra.extend(report.failures[:10])
    if len(report.failures) > 10:
        extra.append("... and %d more" % (len(report.failures) - 10))
    extra.append("Re-derive the window from the engine instruction listing (see "
                 "docs/HookCatalogue.md) or run "
                 "'python3 scripts/verify_hook_windows.py --gamemd PATH'.")
    return Check(False, title, report.summary(), extra)


# ---------------------------------------------------------------------------
# Check 11: release CRT assertions absent (binary)
# ---------------------------------------------------------------------------


def check_release_assertions(data: bytes) -> Check:
    """A /DNDEBUG RELEASE build must not carry the CRT assert machinery."""
    title = "Release CRT assertions absent (binary)"
    needle = ASSERTION_FAILED_NEEDLE.encode("utf-16-le")
    if needle in data:
        return Check(
            False, title,
            "found the wide string %r in the image - a live assert() path is "
            "still compiled into the release artifact (build with /DNDEBUG; "
            "see src/version.rc and the Release build flags)"
            % ASSERTION_FAILED_NEEDLE)
    return Check(
        True, title,
        "no wide %r string in the image (release CRT assertions are compiled "
        "out)" % ASSERTION_FAILED_NEEDLE)


# ---------------------------------------------------------------------------
# Check 12: INI live/probe tooling self-checks (source)
# ---------------------------------------------------------------------------


def check_ini_tooling_self_checks() -> Check:
    """Run the offline self-check of both in-game INI tools (INI-07)."""
    title = "INI live/probe tooling self-checks"
    details: List[str] = []
    failures: List[str] = []
    for script, marker in INI_TOOL_SELF_CHECKS:
        path = os.path.join(REPO_ROOT, "scripts", script)
        if not os.path.isfile(path):
            failures.append("%s: missing from scripts/" % script)
            continue
        try:
            result = subprocess.run(
                [sys.executable, path, "--self-check"],
                cwd=REPO_ROOT, capture_output=True, text=True,
                timeout=INI_TOOL_TIMEOUT_SECONDS)
        except (OSError, subprocess.TimeoutExpired) as exc:
            failures.append("%s: could not run the self-check: %s"
                            % (script, exc))
            continue
        # The marker proves the self-check really ran its cases; the exit code
        # alone would also accept a partial or gutted implementation.
        combined = (result.stdout or "") + (result.stderr or "")
        marker_line = next((line.strip() for line in combined.splitlines()
                            if marker in line), None)
        if result.returncode != 0 or marker_line is None:
            tail = [line for line in combined.splitlines() if line.strip()][-6:]
            failures.append(
                "%s: self-check failed (exit %d, marker %r %s)"
                % (script, result.returncode, marker,
                   "found" if marker_line else "missing"))
            failures.extend("    " + line for line in tail)
        else:
            details.append("%s: %s" % (script, marker_line))
    if failures:
        return Check(False, title,
                     "an in-game INI tool self-check failed", extra=failures)
    return Check(True, title, "; ".join(details))


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
    parser.add_argument(
        "--gamemd", default=None,
        help="gamemd.exe used by the offline hook-window check #10; when "
             "omitted the environment variable $%s is used, and when neither is "
             "set the check is reported as SKIP (default: unset)"
             % GAMEMD_ENV_VAR)
    args = parser.parse_args(argv)

    dll_path = resolve_dll_path(args.dll)
    engine_path = resolve_engine_path(args.gamemd)
    catalogue_path = args.catalogue
    if not os.path.isabs(catalogue_path):
        catalogue_path = os.path.join(REPO_ROOT, catalogue_path)

    print("VentureCore static build verification")
    print("  DLL      : %s" % dll_path)
    print("  catalogue: %s" % catalogue_path)
    print("  engine   : %s"
          % (engine_path if engine_path is not None
             else "(not supplied - check 10 will be SKIPPED)"))
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
        # The source checks (7/8/9/12) inspect the source tree, not the DLL:
        # keep them in the report so a missing or stale binary cannot hide a
        # release-source violation.
        report.append(check_log_policy_source())
        report.append(check_legacy_tier_removed())
        report.append(check_closed_source_host_removed())
        report.append(check_ini_tooling_self_checks())
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

    # -- check 8 ---------------------------------------------------------
    checks.append(check_legacy_tier_removed())

    # -- check 9 ---------------------------------------------------------
    checks.append(check_closed_source_host_removed())

    # -- check 10 --------------------------------------------------------
    # Independent engine-side proof that the declared windows are valid
    # instruction windows of the pinned gamemd.exe (TOOL-06/HOOK-04).  SKIP when
    # no engine executable was supplied.
    expected_count = len(catalogue_rows) if catalogue_rows is not None else None
    checks.append(check_hook_windows(dll_path, expected_count, engine_path))

    # -- check 11 --------------------------------------------------------
    checks.append(check_release_assertions(data))

    # -- check 12 --------------------------------------------------------
    # The in-game log/template tools are what the human probe run is judged
    # with; their offline self-checks belong to the automatic gate (INI-07).
    checks.append(check_ini_tooling_self_checks())

    print_report(checks)
    return 0 if all(check.ok for check in checks) else 1


if __name__ == "__main__":
    sys.exit(main())
