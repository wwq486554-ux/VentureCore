# Host Matrix

VentureCore is designed to sit *next to* existing platform DLLs rather than
replace or extend them.  This document defines which hosts the base knows about,
how each one is detected, how a feature declares the hosts it needs, and what the
startup log reports.

Source of truth: `include/platform/HostIds.h` (public built-in bits),
`include/platform/HostMarker.h` (the self-identification ABI),
`src/Core/HostRegistry.h` (registry, discovery and gate),
`src/Core/Hosts.Builtin.cpp` (built-in registrations) and
`src/Core/Hosts.Custom.cpp` (a derived platform's source-level private hosts).

## The model: facts + declarations (no support tier)

2.0 removed the old level model: it compressed a *set* of loaded hosts into a
single scalar and produced false positives (a Kratos-only environment resolved to
the highest level, so an Ares-dependent feature was let through).  The removed
level enum and its per-feature minimum field must not come back; the model is
now:

| Layer | Content |
| --- | --- |
| facts | `HostRegistry` detects which registered hosts are loaded; the result is a **set** (`hostMask` plus the per-host details).  It answers "who is present", never "what environment level is this". |
| declaration | `FeatureDescriptor::requiredHosts` (AND: every bit must be present) and `anyOfHosts` (OR: at least one bit; `0` = no constraint).  A feature is enabled when its `[Features]` switch is on **and** both host conditions hold. |
| queries | `FeatureContext::hostMask` plus `HasHost(Host)` / `HasHostBit(unsigned)` / `HasHostName(const char*)`; see `include/platform/Feature.h`. |

Do not reintroduce an environment ranking: the base records only *which* hosts
are loaded, and each feature states the hosts it actually needs.  "Any known
host" is expressed as an OR list, for example
`anyOfHosts = HostBit(Host::Ares) | HostBit(Host::Phobos) | HostBit(Host::Kratos)`.

## Built-in hosts

| Host | Bit | Module checked | Detection |
| --- | --- | --- | --- |
| `gamemd.exe` (Yuri's Revenge 1.001) | - | the process itself | **required** host executable; the hook addresses are tied to this build |
| SyringeEx | - (not a host bit; hard requirement) | `Syringe.exe` | **required**: the `SyringeFeatures` flags (`ESPModification`, `ZFPreservation`, `ReladdrInstructionFixup`) must all be set, otherwise the platform shows a message box and terminates the game |
| Ares | `1u << 0` (`Host::Ares`) | `Ares.dll` | loaded module (`GetModuleHandleA`) |
| Phobos | `1u << 1` (`Host::Phobos`) | `Phobos.dll` | loaded module |
| Kratos | `1u << 2` (`Host::Kratos`) | `Kratos.dll` | loaded module |

Detection is by **loaded module**, not by file presence: a DLL sitting in the
game directory is not necessarily injected.  SyringeEx loads every DLL that
carries a `.syhks00` section, so for the supported hosts a loaded module means an
active host.  A host that fails to inject looks exactly like a host that is not
installed.

Ares is not implied by Phobos: the bits are set only by the modules actually
loaded, so an "only Phobos" environment has `hostMask == Phobos` and a feature
with `requiredHosts = Ares` stays disabled.  (That is exactly the case the removed
tier gate used to get wrong.)

## Custom (private) hosts

A platform built from this base can register its own optional platform without
editing the base's built-in table.  Add the host to
`src/Core/Hosts.Custom.cpp` with an explicit bit in the custom range:

```cpp
VENTURE_REGISTER_HOST(MyPlatform, 1u << 3, "MyPlatform.dll", "MyPlatform");
```

* the bit must be `>= 1u << 3`; bits 0-2 belong to the built-in hosts, and a
  custom registration below that range is rejected;
* the custom range is `1u << 3` .. `1u << 31` - 29 slots, shared with any future
  run-time discovery;
* the first definition wins; a duplicate bit, module name or display name is
  rejected at load time and reported once in `<Platform>.log` as an error
  (`hosts   : host registration rejected: ...`);
* `moduleName` / `displayName` must be static-lifetime strings (literals) and are
  limited to 63 characters - the same cap as the marker ABI; a longer name is
  rejected at load time and reported;
* registration is only accepted before the first detection pass: the known bit
  set and the detected set are frozen at startup, and a host registered after
  detection is rejected and reported as a problem (an error in the startup
  block) instead of silently joining (or missing from) the set the features were
  gated on;
* **the built-in hosts take priority by contract** (HOST-09): bits 0-2 are
  reserved to the built-in definitions, and a derived platform must not reuse a
  built-in module or display name (`Ares.dll`/`Ares`, ...).  Cross-translation-
  unit static registration order is unspecified in C++, so a custom definition
  that shadows a built-in name makes one of the two registrations fail with a
  problem and leaves the feature gate with only the winner - which one depends on
  the unspecified initialisation order.  Treat it as a declaration error in
  `Hosts.Custom.cpp`, never as a way to override a built-in.  A dynamic
  (marker-discovered) host is registered during detection, after every static
  registration has run, so it can never displace or duplicate a static
  definition;
* the base registers no custom host itself, and this file is separate from
  `Hosts.Builtin.cpp`, so a base sync does not overwrite a platform's entries.

A feature of that platform declares the dependency with the same bit:

```cpp
requiredHosts = 1u << 3;   // compile-time known custom host
```

The whole source-level custom-host path has an automated check:
`scripts/linux/probe_hosts.sh` builds a stub host module plus a generated
registration and verifies detection, `HasHostName()`, the `(custom)` formatting
and the `requiredHosts` gate while the module is loaded and after it is
unloaded.

## Self-identifying hosts (marker protocol, second phase)

A platform that does **not** want to edit the base source can identify itself at
run time: export one function from the platform DLL and the base discovers it
during startup.  The ABI is in `include/platform/HostMarker.h`:

```cpp
#include <platform/HostMarker.h>

static const VentureHostInfo g_info{
    VENTURE_HOST_MARKER_MAGIC,        // 0x54534856 ('VHST', little endian)
    VENTURE_HOST_MARKER_ABI_VERSION,  // 1
    "MyPlatform",                     // name: static lifetime, 1..63 chars
    "v1.2.0",                         // version: null or <= 63 chars (log only)
    0u, 0u,                           // capabilities / flags: reserved (P3)
};

extern "C" __declspec(dllexport) const VentureHostInfo* __cdecl
VentureHost_GetInfo()
{
    return &g_info;
}
```

How the base handles it (all at startup, once):

* enumerates the process modules (`Toolhelp32`), skips the modules in the Windows
  directory, the DLLs of this lineage (they are refused by the lineage check,
  see below) and our own DLL, then resolves
  `VENTURE_HOST_MARKER_EXPORT_GET_INFO` (`VentureHost_GetInfo`) on the rest;
* takes a temporary module reference (`GetModuleHandleEx`, not a permanent pin)
  while it probes a module, so a concurrent unload cannot pull the module out
  from under the scan;
* calls the marker export and copies its `magic`/`abiVersion` and the
  `name`/`version` strings into local fixed-size copies inside one **POD-only
  `__try/__except` region**: the untrusted pointers are never dereferenced
  outside it, so a marker export that raises, or returns a bad pointer, is
  caught by the same guard, recorded as a problem and skipped;
* validates the copied marker: `magic` and `abiVersion` must match, `name` must
  be NUL-terminated within 1..63 characters, `version` null or NUL-terminated
  within 63 characters.  Anything else is logged as a problem and skipped;
* registers the platform as a **dynamic custom host**: the bit is the lowest free
  bit of the shared custom range (`1u << 3` .. `1u << 31`, 29 slots; source-level
  custom hosts and discovered hosts share it).  Candidates are sorted by module
  file name first, so allocation and the "first definition wins" rule are
  deterministic; a duplicate name (ASCII case-insensitive) or an exhausted range
  is logged and the host is ignored;
* copies the name/version strings immediately (the exporting module may be
  unloaded later), records the marker version and logs it at Debug level
  (`hosts   : SelfHost -> ...SelfHostMarker.dll (marker version v1.2.3)`);
* builds the startup diagnostics (`hosts   : ...` and the per-feature
  `disabled - requires ...` lines) with a bounded append: when the detected-host
  list does not fit, the line is truncated (the cut ends with `...`), a warning
  says the list was truncated, and the detected count is still printed - a long
  or numerous host set cannot overflow the startup buffer.

A discovered host has **no compile-time bit**, so `requiredHosts` / `anyOfHosts`
cannot name it; a feature queries it by name:

```cpp
void Initialize(const venture::FeatureContext& context)
{
    const bool selfHost = context.HasHostName("MyPlatform");   // case-insensitive
    Log::Info("[MyFeature] MyPlatform present: %s", selfHost ? "yes" : "no");
}
```

Reserved for a later phase (P3): the `capabilities`/`flags` words and any version
negotiation - 2.0 records the version for logs only and never gates on it.  A
marker is **not** a license to impersonate a host: a DLL derived from this base
is a lineage copy, not an independent host, and the lineage mutual-exclusion
check refuses it (the scan skips modules that export the lineage marker).

`scripts/linux/probe_hosts.sh` builds marker-exporting stub DLLs and exercises
the whole path under Wine, including five malformed markers (wrong magic, null
name, a marker export that raises an access violation, a wild name pointer and
an over-long name); all five must be rejected without a crash, while the valid
host is detected, gets the next custom bit and is visible through
`HasHostName()` to a feature.  The guarded marker reader itself is also covered
by `tests/test_host_registry.cpp` (wild marker pointer, exact 63-character
boundary, truncated formatting buffers).

## Gate semantics

```text
enabled = [Features] switch
        && (requiredHosts == 0 || (hostMask & requiredHosts) == requiredHosts)   // AND
        && (anyOfHosts   == 0 || (hostMask & anyOfHosts)   != 0)                 // OR
```

`requiredHosts` / `anyOfHosts` may only reference compile-time known bits (the
built-in hosts plus the platform's own custom hosts).  A host discovered at run
time has no compile-time bit; the feature checks it by name in its own body and
logs the consequence itself.

After host detection the base validates every registered descriptor's
`requiredHosts` / `anyOfHosts` against the known bit set, and a bit that belongs
to no registered host is reported in the startup log (a `feature : ...` error
naming the feature and the unknown bit): a mis-spelled or removed host bit must
not silently disable a feature.

Startup log examples:

```text
hosts   : Ares, Phobos (2 host(s))
hosts   : Ares, MyPlatform (custom) (2 host(s))
hosts   : <none>
[Features] rof_curve (ROF curve): enabled
[Features] kratos_hooks (Kratos hooks): disabled - requires host Kratos (present: Ares, Phobos)
[Features] phobos_interop (Phobos interop): disabled - requires any of [Phobos, Kratos] (present: Ares)
[Features] example_unit_counter (Example: UnitClass counter): disabled by VentureCore.ini [Features] Example
```

With `[Platform] LogLevel=debug` every detected host also gets a line with the
resolved module path (`hosts   : Ares -> Z:\game\Ares.dll`) - the first thing to
check when a host does not seem to inject.

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
without our features - while `warn` only logs the conflict.  A refused copy is
**inert**: it does not register commands, does not answer or dispatch its event
types, purges no rules-INI keys and does not touch the savegame stream.  See
`docs/Lineage.md` for the marker, the scan and the exact log lines.

Host detection and lineage detection are independent: a host is another platform
the base runs next to (optional, possibly several), a lineage conflict is another
DLL built from this source tree (never allowed).

## Limitations and what is not in the base

* no functional dependency on any optional host: the base itself enables no
  host-specific behaviour, and there are no host-specific feature packages;
* no Phobos interop and no AE handling;
* run-time host discovery reads name/version only: no version gating, no
  capabilities negotiation (those are reserved for a later phase);
* the extension-storage layer deliberately does not use the engine's unused
  `AbstractClass` extension slot, because DLLs in the Phobos lineage own it.

## Testing a host combination

1. Install the desired host DLLs into the game directory as you normally would.
2. Install VentureCore as described in `README.md`.
3. Launch the game and open `<Platform>.log`.
4. Check the `hosts   : ...` line and the per-feature
   `[Features] ... disabled - requires ...` lines.

Only loaded modules count, so a host that fails to inject looks exactly like a
host that is not installed.

## Event-channel mixed-client constraint

The platform's event channel (`0x70-0x7F`, see `docs/EventChannel.md`) is a
**same-build contract**, not a compatibility matrix entry:

* every peer in a multiplayer game must run a platform build that registered the
  same event types with the same payload sizes.  A peer **without** the platform
  asks the vanilla engine for the size of an out-of-range number (undefined
  behaviour); a peer with the platform but a different registry is answered one
  byte fail-closed by its own hooks.  Both misparse our events and desynchronise;
  the fail-closed case is at least memory-safe;
* there is no automatic peer detection at runtime.  The startup log lists the
  registered event types and the mixed-client warning; a player installing the
  platform on only one side of a network game is misconfigured;
* single-player is unaffected (the event path still runs and is logged, which is
  how the platform's own user test verifies it).
