# Host Matrix

VentureCore is designed to sit *next to* existing platform DLLs rather than
replace or extend them.  This document defines which hosts the base knows about,
how each one is detected, how a feature declares the hosts it needs, and what the
startup log reports.

Source of truth: `include/platform/HostIds.h` (public built-in bits),
`src/Core/HostRegistry.h` (registry and gate), `src/Core/Hosts.Builtin.cpp`
(built-in registrations) and `src/Core/Hosts.Custom.cpp` (a derived platform's
private hosts).

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
| SyringeEx | `Syringe.exe` | the `SyringeFeatures` flags (`ESPModification`, `ZFPreservation`, `ReladdrInstructionFixup`) set at injection | **required**: if the flags are not all set, the platform shows a message box and terminates the game |
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
* `moduleName` / `displayName` must be static-lifetime strings (literals);
* the base registers no custom host itself, and this file is separate from
  `Hosts.Builtin.cpp`, so a base sync does not overwrite a platform's entries.

The whole custom-host path has an automated check: `scripts/linux/probe_hosts.sh`
builds a stub host module plus a generated source-level registration and
verifies detection, `HasHostName()`, the `(custom)` formatting and the
`requiredHosts` gate while the module is loaded and after it is unloaded.

A feature of that platform declares the dependency with the same bit:

```cpp
requiredHosts = 1u << 3;   // compile-time known custom host
```

A host discovered at run time (the later self-identification protocol) has no
compile-time bit; a feature queries it by name in `Initialize()` with
`FeatureContext::HasHostName`.  Run-time discovery is **not** implemented in 2.0.

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
* run-time host discovery (self-identifying DLLs, the `VentureHost_GetInfo`
  protocol) is not implemented yet; only source-level custom hosts are supported;
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
