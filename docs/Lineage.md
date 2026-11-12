# Lineage Marker and Mutual Exclusion

VentureCore is a platform **base**: a derived platform is a DLL built from this
source tree, usually under a different name, with its own feature modules.  All
of them belong to one **lineage** - they share the extension model, the savegame
trailer format and the INI layout.  This document describes the marker every
DLL of the lineage exports and the check that makes sure only one of them runs
in a process.

Source of truth: `include/platform/Lineage.h` (the public C ABI),
`src/Core/Lineage.h` / `src/Core/Lineage.cpp` (the pure decision and the module
scan), the startup wiring in `src/Core/Platform.cpp` and the process-wide
`src/Core/PlatformGate.h` that the engine-facing handlers check.

## Why only one lineage DLL per process

Every copy of the platform would run its own startup path inside the same game
process and keep its own state next to it:

* its own **extension table** - two tables would try to attach an extension to
  the same engine object, and only one `Attach` can win (see
  `docs/EngineBindings.md`), so one of the two modules would silently lose every
  extension it creates;
* its own **savegame trailer** - two writers would append two trailers (or
  fight over one), and a reader could not tell which module wrote what;
* its own scenario-clear, pointer-invalidation and lifecycle hooks, logging
  twice and resetting shared state twice.

None of that is recoverable at run time, so a second DLL of the lineage must not
enable itself.  This is a hard design rule, not a performance hint.

## The marker

Every DLL built from this base exports two functions with C linkage:

```c
struct VentureLineageInfo
{
    unsigned magic;       // VENTURE_LINEAGE_MAGIC = 0x564C4E31 ("VLN1")
    unsigned abiVersion;  // VENTURE_LINEAGE_ABI_VERSION = 1
    const char* name;     // VC_PLATFORM_NAME, e.g. "VentureCore"
    const char* version;  // VC_VERSION_DISPLAY, e.g. "v2.0.0.0"
    unsigned savegameId;  // VC_SAVEGAME_ID, e.g. 0x56433031
};

extern "C" __declspec(dllexport) const VentureLineageInfo* __cdecl VentureLineage_GetInfo();
extern "C" __declspec(dllexport) unsigned __cdecl VentureLineage_CountConflicts();
```

How the fields are produced:

| Field | Value |
| --- | --- |
| `magic` | `VENTURE_LINEAGE_MAGIC` (`0x564C4E31`, "VLN1") - identifies "a DLL built from the VentureCore base" |
| `abiVersion` | `VENTURE_LINEAGE_ABI_VERSION` (`1`) - bump **only** if the marker layout changes |
| `name` | `VC_PLATFORM_NAME` from `src/Core/PlatformConfig.h` - the renamed platform name |
| `version` | `VC_VERSION_DISPLAY` from `src/Core/PlatformVersion.h` |
| `savegameId` | `VC_SAVEGAME_ID` from `src/Core/PlatformConfig.h` - the savegame trailer format of the lineage |

`__cdecl` is written out explicitly and the functions are decorated with
`__declspec(dllexport)` exactly like the SyringeEx hook handlers: the project is
compiled with **`/Gz`** (stdcall is the default calling convention), so a
convention inherited from the build would decorate the names and
`GetProcAddress(module, "VentureLineage_GetInfo")` would never find them.  The
export names in the PE image are undecorated and are covered by the load probe.

`VentureLineage_CountConflicts()` returns the number of *other* loaded modules
that answered the probe with a matching marker.  It exists for the automated
test (`scripts/linux/load_probe.sh`): it makes the scan observable without
starting the game.

## The scan

`venture::ScanForeignLineage()` runs once per process, from `Startup()` after
the log is open and the host matrix has been detected (so a conflict is logged
in the same file as the rest of the startup picture).

1. Take a module snapshot of the current process with kernel32's
   `CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId())`.
   Toolhelp is used on purpose: it needs no extra import library (no `psapi`).
2. For every module except this DLL:
   * skip modules whose path is inside the Windows directory - that cheaply
     removes kernel32, user32, the CRT and the rest of the system DLLs; the
     `GetProcAddress` step below filters everything else (the probe does not
     need a name list).  The path is matched on a **directory boundary**:
     `C:\Windows\System32\...` is skipped, but `C:\WindowsApps\...`,
     `C:\Windows.old\...` and `C:\Windows10\...` are not - a game installed in
     such a directory must not hide a lineage conflict; the match is
     case-insensitive and accepts both `\` and `/`;
   * call `GetProcAddress(module, "VentureLineage_GetInfo")`;
   * when it resolves, call it and compare the returned `magic` with
     `VENTURE_LINEAGE_MAGIC`.  A module that exports a function under that name
     but returns another magic is ignored - only a real lineage DLL counts;
   * record the module file name, its path, and the marker's name/version/ABI/
     savegame id.
3. The running DLL itself is skipped by comparing the resolved function pointer
   with its own `VentureLineage_GetInfo`: the module that resolves to *our* code
   is the current copy.  A copy loaded under another file name is a different
   module and is exactly the conflict to report.

The scan does not throw C++ exceptions and does not terminate the process on its
own: if the toolhelp snapshot is unavailable, the scanner returns "no conflict"
and the platform starts normally - the mutual-exclusion check is a safety net,
not a startup dependency.

The scan is **not** a security boundary and does not contain faults raised inside
a foreign module: when a module exports the probe symbol, its
`VentureLineage_GetInfo()` is called directly, so a module that exports the
symbol but crashes inside it can still take the process down.  (The host-marker
scan protects its call; containment of a faulting lineage export is a known
limitation, see "Limits and residual risks".)

## Policy and startup behaviour

The behaviour is configured in `<Platform>.ini`:

```ini
[Platform]
LineagePolicy=refuse
```

| Value | Meaning |
| --- | --- |
| `refuse` (default) | log the conflict, then leave the platform disabled |
| `warn` | log the conflict and continue with the features enabled |

The value is case-insensitive; a missing value falls back to `refuse` (the safe
choice).  An unrecognised value falls back to `refuse` as well, and the startup
block records the mis-spelling instead of silently enforcing the fallback:

```text
[WARN ] lineage : unrecognised LineagePolicy 'warm' - using refuse
```

The resolved policy is applied **before** any feature is initialized.

Log lines, in the same shape as the rest of the startup block:

```text
lineage : no other VentureCore-lineage DLL loaded
```

```text
lineage : conflicting VentureCore-lineage DLL loaded: OtherPlatform.dll (VentureCore v2.0.0.0, abi 1, savegame 0x56433031) (LineagePolicy=warn - continuing)
```

```text
lineage : conflicting VentureCore-lineage DLL loaded: OtherPlatform.dll (VentureCore v2.0.0.0, abi 1, savegame 0x56433031) (LineagePolicy=refuse - platform stays disabled)
```

When a conflicting module reports a **different** `savegameId`, the details
append `[savegame id differs from this build: 0x...]`: even under `warn` the
mismatch means the two modules cannot share savegame data, so it must be
recorded.

What `refuse` does:

* no message box, no `ExitProcess`, no crash - the game keeps running with the
  platform's feature initialization skipped;
* the platform log is written immediately (one synchronous `WriteFile` per line;
  no `FlushFileBuffers`) and is closed after the error line, so the reason is on
  disk;
* the startup gate (`src/Core/PlatformGate.h`) becomes **Refused**, and every
  engine-facing handler checks it and returns immediately.  A refused copy does
  **not** act on the engine:
  * its command is not appended to the engine command list (it does not appear in
    Options -> Keyboard);
  * its event types are neither sized nor dispatched by the channel hooks, and the
    event channel startup block is not logged;
  * the rules-INI takeover does not purge anything - a refused DLL must not delete
    keys a host platform is about to read;
  * the savegame hooks return before touching the stream, and the swizzle
    adapters are not installed into the engine services;
  * `Shutdown()` and the scenario-clear / pointer-invalidation entry points are
    no-ops, no feature was initialized, and no *saveable* store is registered.

It is *not* identical to not installing the DLL: SyringeEx still injects it and
installs every engine hook, the manifest's hook windows are patched, and the
DLL's C exports (the lineage marker) are visible.  The static initializers also
still populate the process's own command/event registries - but the registries
reject new entries once the gate is Refused and the engine-facing consumers are
gated, so none of that reaches the game.  Treat a refused DLL as inert rather
than absent: it costs the hook windows and the module, and it does nothing else.

`warn` is meant for diagnosis only: the second DLL's features *will* fight over
the extension table and the savegame trailer, so do not ship a configuration
where two lineage DLLs of different builds are meant to coexist.

## Process lifetime

Startup is once per process.  `Platform::Startup()` runs from the engine's
`ExeRun` hook; `Platform::Shutdown()` is a teardown entry point for a derived
platform's own lifecycle - the base never calls it (no engine hook triggers it).
Shutdown is final for the process and there is **no supported restart path**: the
startup gate becomes `PlatformGate::State::Finished`, the log is closed and the
extension state is cleared, and a later `Startup()` returns false instead of
initializing every feature a second time (feature initialization is once-only,
and the engine hooks stay installed but inert).  Calling `Shutdown()` before a
successful startup is a no-op; a refused platform never initialized its features,
so there is nothing to shut down either.

## How it is verified

`scripts/linux/load_probe.sh` loads the built DLL into a tiny Win32 host under
Wine and additionally:

1. resolves both lineage exports (they are part of the probe's export count);
2. checks `VentureLineage_GetInfo()` returns `0x564C4E31`, ABI `1`, a non-empty
   name and version, and `savegameId == 0x56433031`;
3. copies the DLL to a second file name inside the probe work directory, loads
   both copies into the probe process, and requires
   `VentureLineage_CountConflicts()` on the original copy to report **at least
   one** conflicting module - then frees the copy and requires the count to drop
   back to zero.

The synthetic-module decision branches (no conflict, warn, refuse, several
conflicts, magic mismatch, savegame id mismatch, policy parsing) and the
Windows-directory path boundary (`C:\Windows` vs `C:\WindowsApps`,
`C:\Windows.old`, drive-root Windows directories, case and separator variants)
are covered by `tests/test_lineage.cpp`; run it with
`scripts/linux/run_tests.sh lineage`.

## Limits and residual risks

* The scan detects *this lineage only*.  A different platform DLL that does not
  export the marker is invisible to it; that is intentional - this is a safety
  net for copies of the same base, not a security boundary.
* The scan runs once at startup.  A lineage DLL loaded later (by the game or by
  another injector) is not seen; SyringeEx-style injection happens before the
  startup hook, so this covers the supported installation model.
* `CreateToolhelp32Snapshot` is a kernel32 API; it works under Wine and Windows
  alike.  If a host blocks it, the scan degrades to "no conflict" (the platform
  still starts) - that residual risk is accepted for a startup-time diagnostic.
* Loading the *same file* twice with `LoadLibraryA` returns the same module and
  increments its reference count, so it is not reported as a conflict - it is
  one copy of the DLL state, not two.
* The scan calls an exported function of another module directly (`GetProcAddress`
  then a call).  A module that exports the probe symbol but raises inside the
  call can take the process down; containing a faulting foreign export is a known
  limitation of this scan (the host-marker scan has the protected path).
* The marker layout is ABI: append fields only with an
  `VENTURE_LINEAGE_ABI_VERSION` bump, and keep the `__cdecl` exports
  undecorated.

## See also

* `docs/HostMatrix.md` - the other, optional platform DLLs the base can sit next
  to (Ares, Phobos, ...): those are hosts, not lineage copies.
* `docs/HookCatalogue.md` - this feature installs no engine hooks; the marker is
  a PE export, not a hook.
* `scripts/linux/load_probe.sh`, `scripts/verify_build.py` - automatic probes.
