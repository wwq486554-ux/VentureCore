# Host Matrix

VentureCore is designed to sit *next to* existing platform DLLs rather than
replace or extend them.  This document defines which hosts are required, how each
one is detected, what v0.2.0-experimental does with it, and which support tier the
detection resolves to.

Source of truth: `src/Core/HostDetect.h`, `src/Core/HostDetect.cpp` and the
`Tier` enum in `include/platform/Feature.h`.

## Matrix

| Host | Module checked | Requirement | Detected by | v0.2.0 behaviour | Tier |
| --- | --- | --- | --- | --- | --- |
| `gamemd.exe` (Yuri's Revenge 1.001) | - | **required** | the process itself; hook addresses are tied to this build | host executable; the platform runs inside it | - |
| SyringeEx | `Syringe.exe` | **required** | the `SyringeFeatures` flags (`ESPModification`, `ZFPreservation`, `ReladdrInstructionFixup`) set by SyringeEx at injection | **gate**: if the flags are not all set, the platform shows a message box and terminates the game - it refuses to run under an older Syringe | T0 base |
| Ares | `Ares.dll` | optional | loaded module (`GetModuleHandleA`) | detected and logged; no functional dependency | T1 |
| Phobos | `Phobos.dll` | optional | loaded module | detected and logged; no functional dependency | T2 |
| Kratos | `Kratos.dll` | optional | loaded module | detected and logged; no functional dependency | T3 |
| Kaleidoscope | `Kaleidoscope.dll` | optional | loaded module | detected and logged; no functional dependency | T3 |

Detection is by **loaded module**, not by file presence: a DLL sitting in the game
directory is not necessarily injected.  SyringeEx loads every DLL that carries a
`.syhks00` section, so for the supported hosts a loaded module means an active
host.

## Support tiers

The detected host set is resolved to one tier, and every feature declares the
lowest tier it can run at (`FeatureDescriptor::minTier`):

| Tier | Resolved when | Meaning |
| --- | --- | --- |
| T0 | SyringeEx only | plain YR + SyringeEx environment |
| T1 | Ares loaded | Ares host available |
| T2 | Phobos loaded | Phobos host available |
| T3 | Kratos or Kaleidoscope loaded | the "best effort" host set |

Resolution is by presence, highest wins: T3 if Kratos or Kaleidoscope is loaded,
otherwise T2 if Phobos is, otherwise T1 if Ares is, otherwise T0.  A feature is
enabled only when its INI switch is on **and** the resolved tier is at least its
`minTier`; if the tier is too low the log records why the feature was disabled.
The switch itself comes from `<Platform>.ini` `[Features] <iniKey>` (`yes` by
default).

Startup logs the whole picture in one line, for example:

```text
hosts   : Ares, Phobos (tier T2)
```

## Lineage mutual exclusion

The matrix above is about *optional hosts*, which are different platforms that
VentureCore sits next to.  A second DLL of the **same lineage** (another platform
built from this very source tree) is a different case and is not allowed: each
copy would build its own extension table and its own savegame trailer in one
process.

Every DLL built from this base exports the `VentureLineage_GetInfo()` marker
(platform name, version, marker ABI and savegame id).  At startup the platform
probes the loaded modules for that marker; a matching second DLL is named in the
log and the configured `[Platform] LineagePolicy` decides what happens:
`refuse` (the default) leaves the platform disabled - the game keeps running
without our features - while `warn` only logs the conflict.  See
`docs/Lineage.md` for the marker, the scan and the exact log lines.

## v0.2.0 limitations

* No functional dependency on any optional host: the base starts at T0 and does
  nothing host-specific.  Host detection exists so derived platforms can gate
  their own features.
* No host-specific (T1-T3) features are implemented yet.
* No Phobos interop and no AE handling.
* The extension-storage layer deliberately does not use the engine's unused
  `AbstractClass` extension slot, because DLLs in the Phobos lineage own it.

## Testing a host combination

1. Install the desired host DLLs into the game directory as you normally would.
2. Install VentureCore as described in `README.md`.
3. Launch the game and open `<Platform>.log`.
4. Check the `hosts : ... (tier Tn)` line and the per-feature
   `[Features] ... disabled - requires tier ...` lines.

Only loaded modules count, so a host that fails to inject looks exactly like a
host that is not installed.

## Event-channel mixed-client constraint

The platform's event channel (`0x70-0x7F`, see `docs/EventChannel.md`) is a
**same-build contract**, not a compatibility matrix entry:

* every peer in a multiplayer game must run a platform build that registered the
  same event types with the same payload sizes; the engine sizes an unknown type
  from the vanilla table, so a peer without the platform (or with a different
  registry) misreads our events and desynchronises;
* there is no automatic peer detection at runtime.  The startup log lists the
  registered event types and the mixed-client warning; a player installing the
  platform on only one side of a network game is misconfigured;
* single-player is unaffected (the event path still runs and is logged, which is
  how the platform's own user test verifies it).

## Planned (roadmap)

* T1-T3 host-specific feature packages (Ares-only, Phobos-aware, ...).
* Phobos interop (opt-in calls into Phobos extensions).
* AE (Ares extensions) support.

None of these are in v0.2.0-experimental; see `README.md` for the full roadmap.
