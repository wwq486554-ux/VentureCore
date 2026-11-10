# Engine Bindings

This document describes how VentureCore binds to `gamemd.exe`, SyringeEx and
YRpp: which layers exist, which contracts are enforced (and where), and how the
platform's extension storage hooks into the engine's object lifetime.

Related documents: `docs/HookCatalogue.md` (every installed hook),
`docs/SaveFormat.md` (the savegame frame), `docs/Updating.md` (how to
update the YRpp pin), `docs/HostMatrix.md` (optional hosts).

## Binding layers

| Layer | What it provides | Where the base binds |
| --- | --- | --- |
| `gamemd.exe` 1.001 | the 32-bit game process; all fixed hook addresses refer to this build | hook addresses in `src/Engine/` and the feature modules |
| SyringeEx | DLL injection, hook installation and register preservation | `.syhks00` section records emitted by `DEFINE_HOOK`; `SyringeFeatures` flags read at startup |
| YRpp (`external/YRpp`, pinned) | C++ view of the engine classes and the `DEFINE_HOOK` / `GET` macros | all engine types used by the base and the features |
| VentureCore base | startup, logging, INI, feature registry, extension storage | `src/Core/`, `src/Ext/`, `src/Engine/` |

## SyringeEx contract

### Discovery and injection

SyringeEx scans the game directory for DLLs that carry a **`.syhks00`** section
and injects them; no launcher configuration is needed.  The section is emitted by
the hook macros from the external YRpp `Syringe.h` (`SYR_VER=2`).

### `.syhks00` hook records

The section is an array of 16-byte records (`#pragma pack(push, 16)` plus
`__declspec(align(16))` around the struct):

```c
struct hookdecl
{
    unsigned int hookAddr;   // game VA of the hook window
    unsigned int hookSize;   // bytes overwritten by the jump/trampoline
    const char*  hookName;   // VA (ImageBase + RVA) of the NUL-terminated name
    /* 4 bytes tail padding to the 16-byte record size */
};
```

How the record array is read (both by SyringeEx and by `scripts/verify_build.py`):

* the live range is `min(Misc.VirtualSize, SizeOfRawData)` - the linker pads
  `SizeOfRawData` to the file alignment (0x200 here) while `VirtualSize` stays
  exact, and the section virtual size is a multiple of 16 here;
* records whose name pointer is 0 are alignment padding and are skipped;
* the remaining records are matched by handler name; the same handler can appear
  at several addresses (`DEFINE_HOOK_AGAIN`), and if hooks share one address,
  SyringeEx uses the largest declared size for the overwritten window.

Hook windows and their sizes are listed in `docs/HookCatalogue.md`; that file is
the single source of truth and `scripts/verify_build.py` verifies a built DLL
against it.

### Hook handler ABI

`DEFINE_HOOK` expands to an exported C function:

```c
extern "C" __declspec(dllexport) DWORD __cdecl Handler(REGISTERS* R)
```

`REGISTERS` layout and size (0x28 on `SYR_VER=2`) are part of the ABI: the
compile-time gate asserts `sizeof(REGISTERS) == 0x28`.  Handlers read engine
values with the `GET(...)` macro and return 0 to continue the engine's original
instruction flow.

### `SyringeFeatures` gate

SyringeEx sets the exported feature flags (`ESPModification`, `ZFPreservation`,
`ReladdrInstructionFixup`) at injection time.  VentureCore requires all three and
**refuses to run under an older Syringe**: it shows a message box and terminates
the game instead of risking silent corruption.  There is no degraded mode.

### `.syexe00`

SyringeEx also understands a `.syexe00` section for host-checksum declarations.
The v0 base does not use it (no `declhost` records); optional hosts are detected
at run time instead of being declared to the injector.  See `docs/HostMatrix.md`.

## YRpp pin: three gates

The platform compiles against one exact YRpp revision
(`VC_YRPP_PIN` in `src/Core/EnginePin.h`).  The pin is enforced three times:

| Gate | Where | What it enforces |
| --- | --- | --- |
| build time | `scripts/linux/check_yrpp.py`, run by `scripts/linux/build_linux.sh` before compiling | `external/YRpp` matches `VC_YRPP_PIN` (from the checkout's git `HEAD`, or from the `external/YRpp.commit` stamp written by `fetch_yrpp.sh`); a mismatch aborts the build |
| compile time | `src/Engine/EngineCompat.h` | 32-bit build and the engine ABI sizes the base actually touches; a moved field or changed layout fails with a `static_assert` message |
| run time | startup log and `src/version.rc` | the log prints the short and full pin; the version resource records `YrppCommit` |

### ABI assertions

`src/Engine/EngineCompat.h` currently asserts the engine ABI sizes the base
touches (see `src/Engine/EngineCompat.h`; the pinned YRpp layout is the source of
truth, and the build fails if a size moves):

| Type | Expected size |
| --- | --- |
| `void*` | 4 (32-bit build) |
| `AbstractClass` | 0x24 |
| `ObjectClass` | 0xAC |
| `FootClass` | 0x6C0 |
| `TechnoClass` | 0x520 |
| `UnitClass` | 0x8E8 |
| `BulletClass` | 0x160 |
| `BulletTypeClass` | 0x2F8 |
| `UnitTypeClass` | 0xE78 |
| `WeaponTypeClass` | 0x160 |
| `INIClass` | 0x40 |
| `CCINIClass` | 0x58 |
| `INIClass::INIEntry` | 0x28 |
| `INIClass::INISection` | 0x44 |
| `EventClass` | 111 |
| `REGISTERS` | 0x28 |

Rule: whenever a new engine type or field is used, add its assertion here as
well.  A YRpp update that silently moves a field must fail the build, not corrupt
a savegame at run time.

`scripts/linux/abi_probe.sh` prints the sizes of the engine classes it includes
(`AbstractClass`, `ObjectClass`, `FootClass`, `TechnoClass`, `UnitClass`,
`BulletClass`, `UnitTypeClass`, `WeaponTypeClass`, `WarheadTypeClass`,
`CCINIClass`, `REGISTERS` and a few more) so a YRpp update can be re-checked
against the assertions; it does **not** print `INIClass`, `INIClass::INIEntry`,
`INIClass::INISection` or `EventClass` - those four assertions are taken from the
pinned YRpp layout and are enforced by the build, not re-measured by the probe
yet (adding them to the probe is a tracked follow-up).

## Rules / art INI binding

Platform startup (`Venture_ExeRun`) runs before the engine parses `rules(md).ini`,
so rules keys cannot be read from a feature's `Initialize()`.  The binding is:

* **Typed reads** - `src/Core/EngineIni.h` wraps the pinned YRpp INI API
  (`ReadBool` / `ReadInt` / `ReadDouble` / `ReadString` over any `CCINIClass*`, so
  the same call works for the rules INI and the art INI).  Every getter falls back
  to the supplied default on a null INI/section/key.
* **Rules-INI observers** - a feature registers a reader with
  `IniTakeover::RegisterRulesIniObserver()` from its `Initialize()`; the readers
  are called at the rules-INIT hook, i.e. after the engine parsed the file and
  before the engine (and any host platform) consumes it.
* **Takeover** - `DEFINE_HOOK(0x6686C0, Venture_RulesClass_Init, 0x6)` sits at
  the entry of the engine's rules-init function: the caller has already parsed
  `CCINIClass::INI_Rules` and the body has not read it yet (Phobos/Ares hook the
  *inner* `0x668BF0` function, which this function calls, so the platform's purge
  always runs first).  After notifying the observers the handler removes every
  registered platform-owned `(section, key)` pair with the engine's own key
  removal (`INIClass::Clear(section, key)`, `0x5257C0`), so a host platform that
  reads the same key does not see it.  Gated by `[Platform] IniTakeover=yes|no`
  (default yes: off keeps the keys visible for hosts and only reads the values).
* **Not implemented yet** - route B (a read-layer filter at
  `INIClass::ReadString`, `0x528A10`) is deliberately deferred: Ares occupies that
  site (size 5) and chaining there charges every INI read of the whole load.  It
  is the planned route for gamemode/map INIs, which are parsed after this hook.
  Only the single parsed rules file is purged today; `docs/Ini.md` has the
  details, the future type-level sites and the limitation list.

The INI container layouts the takeover walks are asserted in
`src/Engine/EngineCompat.h` (`INIClass` 0x40, `CCINIClass` 0x58, `INIEntry` 0x28,
`INISection` 0x44).

## Hook timing and the feature contract

SyringeEx installs the hooks before `Startup()` runs (startup itself runs from the
`Venture_ExeRun` hook).  Therefore:

* a hook can fire before, or without, the feature being enabled - hook bodies that
  change game behaviour must gate on the feature's `enabled` flag, while cleanup
  paths (removing an extension that was allocated while the feature was on) must
  still run after the switch is turned off;
* the base resolves the switch (INI `[Features] <iniKey>`, default on) and the
  `minTier` gate, writes the result to `*descriptor.enabled`, and only then calls
  the feature's `Initialize()`;
* `Initialize()` runs in both switch states, so a feature creates its stores and
  cleanup hooks regardless of the switch.

See `include/platform/Feature.h` for the exact `FeatureDescriptor` /
`FeatureContext` fields (`id` is the internal identifier, `iniKey` is the INI
switch key, `minTier`, `enabled`, `Initialize`).

## Extension storage binding

### Why a side table

VentureCore keeps extension data **outside** the engine objects, in its own maps
keyed by the owner pointer.  It deliberately does not use the engine's unused
`AbstractClass` extension slot: DLLs in the Phobos lineage own that slot, and the
base must coexist with them.  The side table also keeps the data typed and lets a
derived platform drop a module without touching engine class layouts.

### Extension model

* **One extension per owner.**  An engine object carries at most one platform
  extension instance - the *most derived* class the platform declares for that
  owner class.  This mirrors the call shape of the Phobos-style code the platform
  is meant to host (`UnitExt::Fetch(unit)`, `TechnoExt::Fetch(techno)`) without
  reusing its pointer-slot / container implementation.
* **Hierarchy + runtime type ids.**  `AbstractExt` is the root;
  `src/Ext/ExtHierarchy.h` declares the `TechnoExt -> FootExt -> UnitExt`
  skeleton, and a platform's extension class derives from one of them.  Every
  class declares a `TypeId` (base hierarchy 0x0001-0x00FF; platform classes start
  at `AbstractExt::FirstUserTypeId` = 0x0100) and a `BaseTypeId` (its direct
  base).  `FetchExtAs<T>` walks that chain before the `static_cast`; the DLL is
  built with `/GR-`, so there is no RTTI and no `dynamic_cast`.  The pairs are
  registered with `RegisterExtType` - the built-in hierarchy is seeded in
  `src/Ext/AbstractExt.cpp`, a feature class registers itself from its store or
  its first `Fetch`.
* **Global index** (`src/Ext/ExtensionIndex.h`): `unordered_map<owner,
  AbstractExt*>` for polymorphic attach/fetch/detach, plus the registry of
  stores.  A store registers a type-erased `ExtStoreDesc` on first use (type id,
  diagnostic name, saveable flag, clear/count callbacks), so scenario clear - and
  the savegame layer - walk every store without knowing its C++ type.
* **Store** (`src/Ext/ExtensionStore.h`): one store per (game class, most
  derived extension class) pair, with
  * an **intrusive doubly linked list** of its instances in allocation order
    (`AbstractExt::StoreNext/StorePrev`; no separate container node),
  * an `unordered_map<owner, TExt*>` for O(1) typed `Fetch`/`Remove`
    (O(1) removal = unlink + map erase + index detach),
  * a single-entry `Fetch` cache (`(owner, instance)` of the last lookup),
  * an optional `Reserve(expectedLiveCount)` hint for the map.

  A second store trying to claim an owner that already has an extension fails
  cleanly (`Allocate` returns `nullptr`): that is a feature-composition error, so
  a platform with several modules for the same owner class puts their fields into
  **one** extension class.
* Feature code works with `AbstractExt::Owner()`, `ExtTypeId()` and the optional
  `Save` / `Load` / `PostLoad` / `OnDeferredAllocation` virtuals (the savegame
  layer drives the latter three; see `docs/SaveFormat.md`).

Extension instances are **owned by the platform's lifecycle hooks** - a feature
never deletes them itself.

### Lifecycle binding

| Engine event | Hook | Effect |
| --- | --- | --- |
| `UnitClass` constructed | `Venture_Example_UnitClass_CTOR` (0x7353D3) | Example store `Allocate`s the `ExampleUnitExt` instance (type id 0x0101, base `UnitExt`); index `Attach`es it, so `UnitExt` / `FootExt` / `TechnoExt` views reach the same instance |
| `UnitClass` destroyed (two sites) | `Venture_Example_UnitClass_DTOR` (0x735967, 0x7359DA) | Example store `Remove`s the extension; index `Detach`es it |
| scenario teardown | `Venture_ScenarioClear` (0x685659) | `ExtensionIndex::ClearAll()` - every registered store clears itself, then the index is cleared |
| pointer invalidated | `Venture_AnnounceInvalidPointer` (0x7258D0) | `ExtensionIndex::NotifyInvalidPointer()` - increments a diagnostic counter and drops a deferred allocation recorded for the invalidated owner; it does **not** delete extensions (removal stays owned by the destructor hook) and there is **no subscriber/log API** yet, so a feature cannot register a pointer-keyed cache on it today |

Consequences for feature authors:

* never hold an extension pointer across a scenario boundary - scenario clear
  destroys all of them;
* a base-class view and the derived `Fetch` return the same instance; do not keep
  a base view cached across frames (the pointer-invalidation notification is not
  exposed to features yet - see the lifecycle table above);
* extension state **is** persisted by the savegame frame, but only for
  stores that opted in (`SetSaveable(true, dataVersion)`); see
  `docs/SaveFormat.md` for the feature-author rules;
* do not cache engine pointers without a validity rule you control: the engine's
  pointer-invalidation announcement is only used internally today.

### Store cost (measured)

The store container was chosen against a microbenchmark
(`tests/bench_store.cpp`, `scripts/linux/bench_store.sh`; Wine + MSVC 14.44 with
the **benchmark's own** compile flags - `/W4 /MT /EHsc- /Gz`, no `/O2` or `/Oy`,
so the numbers are *not* comparable to the optimised release DLL - live instance
counts 1k / 5k / 20k).  "Before" is the flat `vector<unique_ptr>` + linear removal
the base started with; those numbers were recorded during the container rework
and cannot be re-measured from this tree (the old container is gone).  "After" is
the intrusive allocation-order list + `Fetch` cache + `Reserve` hint, measured
twice on this machine on 2026-11-11; the ranges span both runs because Wine
timing is noisy (the 1k-live case in particular):

| Operation | Before (historical) | After (measured) |
| --- | --- | --- |
| `Allocate` (per instance) | 512-538 ns | 426-988 ns (noisy) |
| `Fetch`, changing owners (hash path) | 41-45 ns | 38-91 ns (noisy, unchanged within noise) |
| `Fetch`, same owner repeatedly (cache) | 34-52 ns | **1.7-2.8 ns** |
| `Remove` (random order, per instance) | 4.3-4.7 / 21 / 83-84 µs @ 1k/5k/20k | **0.20-0.29 µs @ every size** |
| `Clear` (per instance) | 107-118 ns | 107-166 ns (unchanged) |

Memory: the intrusive links cost two pointers (8 bytes) per extension instance
but remove the former container node (~24 bytes) and the vector's growth slack,
so the per-instance footprint stays at roughly the same level; the `Reserve` hint
trades a small upfront allocation for no rehash spikes during a battle.

Why this matters: `Remove` runs once per destroyed object and used to be linear
in the live count (the historical before-column: 4.3 → 84 µs across the range
above - an O(n) scan per unit destroyed, i.e. quadratic work while an army
dies).  `Fetch` on the same owner is
the common shape in per-frame/per-shot feature code, where the cache removes the
hash lookup entirely.  The allocation-order list keeps the savegame row order
deterministic.

## Lineage marker and mutual exclusion

Only one DLL of the VentureCore lineage may run in the process: a second one
would build its own extension table and its own savegame trailer on top of the
same engine state.  The DLL therefore exports a small C-ABI marker (not a hook):

* `VentureLineage_GetInfo` - magic `0x564C4E31`, marker ABI version, platform
  name, platform version, `VC_SAVEGAME_ID`;
* `VentureLineage_CountConflicts` - diagnostic: how many other lineage DLLs the
  scan currently finds (used by `scripts/linux/load_probe.sh`, which loads a
  second copy under a different file name and expects a conflict).

Both are explicit `__cdecl`: the project is built with `/Gz` (stdcall default),
which would otherwise decorate the export names and hide them from
`GetProcAddress`.

Startup scans the loaded modules with the kernel32 Toolhelp snapshot (skipping
this DLL and the Windows directory), `GetProcAddress`es the marker on every
remaining module and applies `[Platform] LineagePolicy`: `refuse` (default) logs
the conflicting modules and leaves the platform disabled (no message box; the
game keeps running without the platform), `warn` logs and continues.  Details and
limitations: `docs/Lineage.md`.

## Event channel binding

The platform reserves the event-type range `0x70-0x7F` (vanilla `0x00-0x2E` - the
47-entry payload-size table - CnCNet `0x30-0x3F`, Phobos `0x40-0x45`, Kratos
`0x50-0x51`, Ares `0x60-0x61`).
A feature registers a type with a fixed payload size (1..104 bytes) and a
responder; the platform serializes it into the engine's event record, queues it
and routes it back on every machine.

| Engine site | Handler | Effect |
| --- | --- | --- |
| `0x4C6CC8` (event dispatch, size 5) | `Venture_Event_Dispatch` | routes our types to their registered responder; always returns 0, so the engine's own event switch still runs |
| `0x64B6FE` (GetEventSize, size 6) | `Venture_Event_GetSize1` | returns the registered payload size for our types and resumes at `0x64B71D` |
| `0x64BE7D` (GetEventSize, size 6) | `Venture_Event_GetSize2` | same, plus the caller frame's size slot; resumes at `0x64BE97` |
| `0x64C30E` (GetEventSize, size 6) | `Venture_Event_GetSize3` | same with the type in `ESI`; resumes at `0x64C321` |

Wire format: an event is the engine's 111-byte `EventClass` record (type, executed
flag, house index, frame, 104-byte data union); the platform writes its payload
into the data union and queues the record with `EventClass::OutList.Add` - the
same mechanism the rest of the ecosystem uses for custom event types.  The
record layout is asserted in `src/Engine/EngineCompat.h`.

The payload contract is a **same-build requirement**: every peer must run a
platform build that registered the same event type with the same payload size.
A peer without the platform (or with a different registry) sizes our events from
the vanilla table and desynchronises; there is no automatic peer detection - the
startup log lists the registered types and warns about mixed clients.  See
`docs/EventChannel.md` and the mixed-client note in `docs/HostMatrix.md`.

## Command binding

`DEFINE_HOOK(0x533066, Venture_Commands_Register, 0x6)` is the engine's command
registration callback.  The platform creates one `CommandClass` adapter per
enabled command descriptor (engine name, wide UI name/category/description,
handler) and appends it with `CommandClass::Array.AddItem`, which is the same
mechanism the rest of the ecosystem uses; the command then appears in the game's
keyboard options dialog where the player binds a key.  The example command logs
and raises the platform's demo event, so one key press exercises the command path,
the event sender, the engine's event queue and the event responder.

The engine reloads `CommandClass::Hotkeys` from `KEYBOARDMD.INI` right after the
callback (`0x533D20`), so a default hotkey set from the registration hook would be
discarded: commands ship unbound and the player binds them.  Details:
`docs/Commands.md`.

## Savegame trailer binding

Extension state is persisted through the platform's framed block, which is
appended and read strictly sequentially (the engine's savegame stream is not
seekable); the format and the load sequence are documented in
`docs/SaveFormat.md`.  The binding side:

| Engine site | Handler | Effect |
| --- | --- | --- |
| `0x67D1B4` (after the engine's object-state writer, before the finalize/Commit) | `Venture_SaveGame_Trailer` | builds the extension payload and appends the header-first frame `[header][payload]` sequentially - no `Seek`: this site runs after every ecosystem writer, so the current position is the end of the save data; skipped when the writer's BOOL result (`AL` of `EAX`) is 0 or when no saveable store is registered |
| `0x67E44D` (load window open) | `Venture_LoadGame_Begin` | defers allocations driven by object constructors |
| `0x67F7C8` (end of the engine's load-data routine) | `Venture_LoadGame_Trailer` | sequential read of the frame from the current position (our handler runs after every ecosystem reader, so this is exactly our block; no `Seek`), header/CRC validation, payload read |
| `0x67E685` (after the swizzle pass) | `Venture_LoadGame_PostSwizzle` | adopts the restored instances (owner pointers remapped), `PostLoad()`, deferred allocations |
| `0x67E68A` (load window close) | `Venture_LoadGame_End` | resumes normal allocation |

The engine's swizzle manager is reached through the `Ext/ExtSwizzle.h` adapter,
which `src/Engine/Hooks.Save.cpp` installs on the first save/load hook - the
extension layer itself stays engine-free (and unit-testable with a fake swizzle
manager).

Order is the critical constraint on both sides, and the stream cannot seek, so
both are sequential: the writer is installed at `0x67D1B4`, a site no known
platform hooks that runs after the engine's object-state writer - the routine
containing the tail site Ares owns (`0x67E42E`, whose handler always jumps away) -
so the append puts our block last regardless of handler order.  The reader runs at
`0x67F7C8` after every ecosystem sequential reader (the known platform modules
sort before `VentureCore.dll`), so it starts exactly at our frame.  `docs/SaveFormat.md` records
the failure mode for a platform outside that order.

## Logging

The platform writes `<Platform>.log` next to `rules.ini`; `docs/Logging.md` has
the level semantics, the measured cost and the hot-path rules.  The binding side:

* `[Platform] LogLevel=error|warn|info|debug` selects the filter; the legacy
  `VerboseLogging=yes` remains an alias for `debug` (and wins when both are set,
  because it is the older key);
* a RELEASE build writes to the debugger only with `[Platform] DebugOutput=yes`
  (a DEBUG build always does), so a shipped DLL does not pay
  `OutputDebugStringA` per line - the source-hygiene check in
  `scripts/verify_build.py` enforces that no unconditional call is left;
* `Log::Open()` writes one per-process separator header (platform, version,
  build type, PID, start time), so an append-only log stays readable across
  sessions; all historical line formats below it are unchanged for users and
  grep-based tooling;
* the logger is best-effort: a failed file open disables file logging (the gated
  debugger helper carries one explanation line) and a failed write drops the
  line after reporting it once; no file-system failure aborts the platform (see
  `docs/Logging.md` for the exact guarantees).

## Adding a new engine binding

1. Use a YRpp type or field rather than a raw address whenever possible.
2. Add a `static_assert` for every new layout assumption in
   `src/Engine/EngineCompat.h`, measured with `scripts/linux/abi_probe.sh`.
3. If a new hook is needed, add it to `docs/HookCatalogue.md` in the same change;
   `scripts/verify_build.py` fails otherwise.
4. Keep the hook body self-contained, gate it on the owning feature's `enabled`
   flag, and never cut an instruction: the window must contain complete
   instructions and `address + size` must be the next instruction.
5. When the YRpp pin changes, follow `docs/Updating.md`.

## Limits in v0.3.0-experimental

* The savegame trailer persists extension state (see `docs/SaveFormat.md`); it is
  not a general savegame fork - the engine's own object format is untouched.
* Event channel and command registration are implemented; their protocol (event
  numbers/payload sizes, command names) is a same-build contract (see
  `docs/HostMatrix.md` and `docs/Commands.md`).
* No host-specific (T1-T3) bindings, no Phobos interop, no AE.
* The lineage marker detects this platform's own lineage only (same magic); a
  DLL loaded after startup is not detected (see `docs/Lineage.md`).
* No runtime plugin ABI: features are compiled into the single platform DLL.
* No unload hook: the platform does not try to restore engine memory; the game
  process terminates normally.
