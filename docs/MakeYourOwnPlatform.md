# Make Your Own Platform

This guide walks through turning the VentureCore base into **your own** platform:
its own name, its own feature modules, its own single DLL.  The model is:

> copy the repository -> rename the platform -> add feature modules under
> `src/Features/` -> build one platform DLL.

You do not modify Ares / Phobos / Kratos, you do not vendor YRpp, and you do not
re-implement startup, logging, INI parsing, host detection, feature switches or
extension storage - the base already provides those.

The compiled reference implementation of everything below is
`src/Features/Example/ExampleFeature.cpp`; read it after this guide.

## 0. Prerequisites

* Visual Studio 2022 (v143 / MSVC 14.44) - or the same toolchain under Wine on
  Linux (`scripts/linux/build_linux.sh`);
* a *Yuri's Revenge* 1.001 installation with SyringeEx;
* the pinned YRpp checkout in `external/YRpp/` (see `README.md`; the build refuses
  to run without the pin matching).

## 1. Copy and rename

1. Copy the repository and start a fresh history for your platform.
2. Rename the platform following `docs/Renaming.md` - at minimum
   `VC_PLATFORM_NAME` and `VC_SAVEGAME_ID` in `src/Core/PlatformConfig.h`, the
   version metadata in `src/Core/PlatformVersion.h`, the literal strings in
   `src/version.rc`, and `ProjectName` in `VentureCore.vcxproj`.
3. Update the expected values at the top of `scripts/verify_build.py` and the
   export list in `scripts/linux/load_probe.sh` if you renamed the hook handlers.

Do not rename the `.syhks00` / `.syexe00` sections or the `external/YRpp` path;
see `docs/Renaming.md`.

## 2. Fetch YRpp and build once

```bash
scripts/linux/fetch_yrpp.sh --from /path/to/YRpp
scripts/linux/build_linux.sh Release --build-type RELEASE
python3 scripts/verify_build.py
scripts/linux/load_probe.sh
```

At this point you have a working platform DLL with the Example module.  Keep it
working - it is your smoke test.

## 3. Decide what your platform adds

A platform built from this base is a set of feature modules.  For each feature,
write down before coding:

* a unique internal `id` (lower_snake_case) - used in logs and lookups;
* an INI `iniKey` for the `[Features]` switch (human-friendly, e.g.
  `MyFeature`; one switch per feature module, however many keys it owns);
* which optional hosts it needs, if any: `requiredHosts` (all of) and/or
  `anyOfHosts` (any of) over the compile-time known host bits
  (`include/platform/HostIds.h` plus your own custom hosts); `0` means "runs
  anywhere";
* the engine objects it needs to extend, and therefore the extension type(s);
* the engine hooks it needs (every hook must also get a row in
  `docs/HookCatalogue.md`).

The base layers are available: savegame persistence (a stateful store calls
`SetSaveable(true, dataVersion)` and its extension implements `Save`/`Load`, see
`docs/SaveFormat.md`), rules/art INI reads and rules-INI takeover
(`docs/Ini.md`), the event channel for networked feature events
(`docs/EventChannel.md`), command registration (`docs/Commands.md`) and log
control (`docs/Logging.md`).  A savegame payload carries a signature of the
platform's saveable store set, so adding, removing, reordering or re-versioning a
saveable store makes older saves incompatible: their extension state is dropped
(the game's own state still loads, there is no migration).  Do **not** plan around
systems the base does not have: host-specific feature packages, Phobos interop
and AE are not part of the base.

## 4. Add a feature module

Create `src/Features/<Name>/<Name>Feature.cpp` (and a small header if other files
need to call into it).  The minimum shape, matching the current
`include/platform/Feature.h`:

```cpp
#include "Core/Log.h"
#include "Engine/EngineCompat.h"
#include "Ext/ExtensionStore.h"

#include <platform/Feature.h>

namespace venture::features::myfeature
{
namespace
{
bool g_enabled = false;   // written by the base before Initialize()

// One extension instance per owner object; carries your data.  Derive from the
// base hierarchy (UnitExt here) and declare the type-id pair: that is what makes
// UnitExt::Fetch / TechnoExt::Fetch reach this exact instance (no RTTI in the
// build).  Type ids start at AbstractExt::FirstUserTypeId (0x0100).
class MyUnitExt final : public UnitExt
{
public:
    static constexpr std::uint32_t TypeId = 0x0101u;
    static constexpr std::uint32_t BaseTypeId = UnitExt::TypeId;

    explicit MyUnitExt(AbstractClass* const owner) : UnitExt(owner) {}

    std::uint32_t ExtTypeId() const override { return TypeId; }

    // ... feature-specific fields ...
};

using MyStore = ExtensionStore<UnitClass, MyUnitExt>;
MyStore& g_store = MyStore::Instance();

void Initialize(const FeatureContext& context)
{
    (void)context;
    g_store.SetName("UnitClass/MyUnitExt");   // shows up in the docs/logs
    Log::Info("[MyFeature] initialized (enabled=%d, live=%zu)",
        g_enabled ? 1 : 0, g_store.Size());
}
}

// Called by your UnitClass hooks.
void OnUnitCreated(UnitClass* const pUnit)
{
    if (!g_enabled || !pUnit)          // activation gates on the switch
        return;
    if (g_store.Fetch(pUnit) != nullptr)
        return;
    g_store.Allocate(pUnit);
}

void OnUnitDestroyed(UnitClass* const pUnit)
{
    if (!pUnit)
        return;
    g_store.Remove(pUnit);             // cleanup always runs, even when disabled
}
}

// --- feature registration ---------------------------------------------------
namespace venture::features::myfeature
{
namespace
{
const FeatureDescriptor kDescriptor{
    "my_feature",       // id: unique internal identifier (logs, lookup)
    "MyFeature",        // iniKey: switch under [Features] in <Platform>.ini
    "My feature",       // human-readable name for logs
    0,                  // requiredHosts (AND): no host dependency
    0,                  // anyOfHosts (OR): no host dependency
    &g_enabled,         // the base writes the resolved switch here
    &Initialize,        // called from Startup(), on or off
    nullptr,            // group: optional [Groups] master switch (default: none)
    true,               // defaultEnabled: optional [Features] default (default: true)
};
}

VENTURE_REGISTER_FEATURE(kDescriptor);
}

// --- hooks -------------------------------------------------------------------
// The size must cover complete instructions; see docs/HookCatalogue.md.
DEFINE_HOOK(0x7353D3, MyFeature_UnitClass_CTOR, 0x7)
{
    GET(UnitClass*, pItem, ESI);
    venture::features::myfeature::OnUnitCreated(pItem);
    return 0;
}
```

Then:

1. **Register the source file** in `VentureCore.vcxproj` (`<ClCompile Include=...>`).
   The Linux build derives its file list from the project file.
2. **Add the switch** to `VentureCore.ini.example` (or your platform's own
   example) under `[Features] MyFeature=yes`, and document it for your users.
   The module's own keys (if any) go in the module's own section, not in
   `[Features]` - see "One switch per feature module, not per key" below.
3. **Add every new hook row** to `docs/HookCatalogue.md` (address, size, handler
   name, purpose, notes).  `scripts/verify_build.py` fails if the built DLL and
   the table disagree, and it is the only place hook windows are recorded.
4. **Log the resolved state** from `Initialize()` (the Example module does this),
   so a user's log shows which features are on/off and why.
5. **Handle lifetime correctly**: extension instances are owned by the platform's
   lifecycle hooks - ctor allocates, dtor removes, scenario clear wipes
   everything.  Never delete an extension yourself and never keep one across a
   scenario boundary.  Savegame persistence is opt-in per store
   (`SetSaveable(true, dataVersion)` + `Save`/`Load`): a non-saveable extension
   is recreated empty after a load, a saveable one is restored with its data (see
   `docs/SaveFormat.md`).
6. **One extension per owner object**: if a second module extends the same engine
   class, merge its fields into the same extension class instead of adding a
   second store - otherwise the second `Allocate` returns `nullptr` and logs the
   composition error.

### One switch per feature module, not per key

`[Features]` holds **exactly one switch per feature module** - one
`FeatureDescriptor`, i.e. one module under `src/Features/<Name>/`.  A module may
span several source files; they all share that one switch.  The switch is not a
list of the module's keys and it does not grow with them:

* a module that owns twenty keys still has **one** `[Features]` line;
* a module that owns **no** keys at all still has its switch - and should have
  one: it is how a user turns the behaviour off without deleting the DLL, and
  what makes an A/B test or a bug report reproducible;
* the module's own keys are not in `[Features]`.  They live in the module's own
  section of `<Platform>.ini` (for example `[MyFeature]`) or in
  `rules(md).ini` / `art(md).ini`.  In `<Platform>.ini` the module declares them
  at the read site with the 2.0 INI registry (next subsection); for the
  engine-parsed INI use `Core/EngineIni.h`.  A declared key gets its value
  validated and logged, and a mis-spelled key is reported by the startup
  unknown-key scan; an undeclared read through the legacy getters is invisible
  to both.

`[Features]` exists to give the user one uniform kill switch per feature system,
with the same spelling and the same log line for every module.  If a
sub-behaviour needs its own visible switch, make it a separate feature module
with its own descriptor and `[Features]` key - do not expect the base to
discover keys by itself.

A `[Features]` switch defaults to **on** unless the descriptor sets the optional
trailing field `defaultEnabled = false`; an explicit `[Features] <iniKey>=yes|no`
always wins.  The optional `group` field names a feature group whose
`[Groups] <group>=yes|no` master switch disables every module of the group (the
per-module switch still exists; the group is an extra, explicit gate).

### Declaring keys: the INI registry (2.0)

Declare each `<Platform>.ini` key where the module reads it.  The base reads,
validates, records and logs it, and returns the effective value:

```cpp
#include <platform/Feature.h>

bool g_logEveryUnit = false;
int g_progressEvery = 100;

void Initialize(const venture::FeatureContext& context)
{
    // Read the key regardless of `g_enabled` (Initialize runs in both switch
    // states) - the declaration is what makes the key known to the base.
    g_logEveryUnit = VENTURE_INI_BOOL(context, "MyFeature", "LogEveryUnit", false,
        "log one line per handled unit");
    g_progressEvery = VENTURE_INI_INT(context, "MyFeature", "ProgressEvery", 100, 0, 10000,
        "log one progress line every N units (0 = every unit)");
}
```

* an invalid bool / non-integer / out-of-range integer falls back to the
  declared default and produces a **warning** (never silently);
* a duplicate `(section, key)` declaration by two modules warns and keeps the
  first declaration;
* every effective value is logged at Debug level, and the startup block ends
  with `ini keys: N registered, M invalid, K unknown`;
* an unknown `[Platform]`/`[Groups]` key, an unknown key in a module section and
  an unknown `[Features]` switch produce a warning (with a "did you mean ...?"
  suggestion for switches); `[Platform] IniWarnUnknownKeys=no` turns the
  unknown-key warnings off.  User-defined sections are only Debug-logged;
* `[Platform] DumpIniTemplate=yes` (development only) writes
  `<Platform>.ini.generated` next to the log from the registry, for comparing a
  shipped `.ini.example` with what the code really reads;
* the legacy `FeatureContext::IniGetBool/IniGetInt/IniGetString` functions still
  exist for existing modules but bypass all of the above - do not use them in
  new code.

See `docs/Ini.md` for the full contract.

A feature that **only changes vanilla behaviour** is an ordinary feature: no new
keys, no new extension, just hooks that alter what the engine does.  Declare the
descriptor, give it a switch, gate every hook on `enabled`, and it works exactly
like a feature with keys.  Reading or reinterpreting an existing
`rules(md).ini` key needs no INI entry either - read it through
`Core/EngineIni.h` (from a rules-INI observer when the engine parses the file
after `Startup()`) and act on it.  Do **not** register a vanilla key as
platform-owned to "take it over": the takeover hides *our* keys from a host
platform, and registering an engine key only makes the engine lose it (see
`docs/Ini.md`).

### Optional: declare host dependencies

If the feature only makes sense with an optional host platform, declare it in the
descriptor instead of checking at run time:

* `requiredHosts` - AND: every bit must be present.  Ares only:
  `venture::HostBit(venture::Host::Ares)`.
* `anyOfHosts` - OR: at least one bit must be present; `0` means "no
  constraint".  Phobos or Kratos:
  `HostBit(Host::Phobos) | HostBit(Host::Kratos)`.

The base enables the feature only when the `[Features]` switch is on **and** the
declarations are satisfied by the detected host set; otherwise it disables the
feature and logs which host is missing and which hosts are present (for example
`disabled - requires host Kratos (present: Ares, Phobos)`).  Declarations may only
reference compile-time known bits - the built-in hosts in
`include/platform/HostIds.h` plus your platform's own custom hosts
(`src/Core/Hosts.Custom.cpp`).  For a host known only by name, or when the host
set should be checked inside the feature, use `FeatureContext::HasHost` /
`HasHostBit` / `HasHostName` in `Initialize()`.

There is **no** environment-level scalar any more (2.0 removed the old support
levels): a set of hosts is a set, and "Phobos is present" never implies "Ares is
present" - the bits come from the modules actually loaded.  See
`docs/HostMatrix.md`.

### Optional: get detected without editing the base (host marker)

If your platform must be detected by an unmodified base build, do not edit
`Hosts.Custom.cpp`; export the marker instead (`include/platform/HostMarker.h`,
ABI documented in `docs/HostMatrix.md`):

```cpp
#include <platform/HostMarker.h>

static const VentureHostInfo g_info{
    VENTURE_HOST_MARKER_MAGIC, VENTURE_HOST_MARKER_ABI_VERSION,
    "MyPlatform",  // name: static lifetime, 1..63 characters
    "v1.2.0",      // version: null or <= 63 characters; logged only
    0u, 0u,        // capabilities / flags: reserved
};

extern "C" __declspec(dllexport) const VentureHostInfo* __cdecl
VentureHost_GetInfo()
{
    return &g_info;
}
```

The base scans the loaded modules once at startup and registers the platform as a
dynamic custom host (lowest free bit of the shared custom range); the name and
version are recorded for the log only - there is no version gating, and the
`capabilities`/`flags` words are reserved.  A feature of the base then checks it
by name (`FeatureContext::HasHostName("MyPlatform")`).  A DLL built from this
base is a lineage copy, not an independent host: it is refused by the lineage
mutual-exclusion check and cannot masquerade as a host through this protocol.
`scripts/linux/probe_hosts.sh` is the working example (valid marker plus three
malformed markers that must be rejected without crashing).

## 4a. Feature-author conventions (enforced by the base)

* **Command names** are compared case-insensitively and must carry the platform
  prefix (for example `Venture Test Command`); a name that differs only in case
  is rejected, and a collision with an already registered engine command is
  logged (`docs/Commands.md`).
* **Owned rules-INI keys** must live in a platform-namespaced section
  (`[VentureCore]`, `[Examples]`, ...).  Registering a key in an engine core
  section (`[General]`, `[Basic]`, `[AudioVisual]`, `[MultiPlayer]`, ...) is
  refused with a warning (`docs/Ini.md`).
* **Extension type ids** must be `>= AbstractExt::FirstUserTypeId` (0x100); a
  conflicting base or a cyclic chain is rejected and the type id is poisoned -
  `Fetch` returns `nullptr` instead of risking a wrong cast
  (`docs/EngineBindings.md`).

## 5. Make it rebuildable and reviewable

Before declaring a feature done:

* build clean on both paths (`--clean` on Linux, rebuild in Visual Studio);
* `python3 scripts/verify_build.py` passes;
* `scripts/linux/load_probe.sh` prints `resolved exports: 7/7` (or your updated
  list if handler names changed);
* `scripts/linux/run_tests.sh` passes, and your layout/format changes have a test
  (`tests/test_*.cpp`); a performance-sensitive change should also run
  `scripts/linux/bench_store.sh`;
* in-game test: launch with SyringeEx + your DLL, open a skirmish, leave it, and
  check `<Platform>.log` for the startup banner, the feature switches, scenario
  clear and your feature's lines;
* `docs/HookCatalogue.md`, `README.md` and `CHANGELOG.md` updated;
* no new warnings in the build.

## 6. Ship it

1. Bump the version (`src/Core/PlatformVersion.h`) and add a `CHANGELOG.md` entry
   (`docs/Updating.md` has the full checklist).
2. Build with `--build-type RELEASE` so the version resource and log report
   `RELEASE` instead of `LOCAL`.
3. Detect the platform name and version in `scripts/verify_build.py` still match
   your platform (update its constants if you renamed anything).
4. Publish the DLL, optionally the `.pdb`, and your documentation.  Do not publish
   `external/YRpp` or any game files.
5. State your license.  The base is MIT; your own modules can be any license as
   long as you keep the MIT notice.  If you copy GPL-3.0 modules, the combined
   work must be GPL-3.0 - see `docs/Licensing.md`.

## Don't

* don't modify or vendor Ares / Phobos / Kratos or any other host platform;
* don't vendor YRpp or commit `external/YRpp/`;
* don't use the engine's `AbstractClass` extension slot (Phobos-lineage DLLs own
  it) - use the base's extension storage;
* don't add hooks without a `docs/HookCatalogue.md` row;
* don't document capabilities the base does not have (host-specific T1-T3
  features, Phobos interop, AE);
* don't rename `.syhks00` / `.syexe00` or the pinned YRpp path.
