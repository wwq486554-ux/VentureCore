# INI Layer: Typed Reads and Takeover

This document describes the platform's INI-facing layer:

* `src/Core/IniRegistry.h` - the 2.0 registry for the platform's own
  `<Platform>.ini` keys (declaration, validation, [Groups] switches, unknown-key
  scan, template dump);
* `src/Core/EngineIni.h` - typed reads over the engine's parsed `rules(md).ini` /
  `art(md).ini`;
* `src/Core/IniTakeover.h` - the owned-key registry and the takeover purge that
  hides the platform's own keys from host platforms loaded next to it;
* `src/Engine/Hooks.Ini.cpp` - the engine adapter and the takeover hook;
* `src/Features/Example/ExampleIniDemo.cpp` - the reference feature module that
  reads one rules key and registers it as owned.

Related documents: `docs/HookCatalogue.md` (hook address table),
`docs/EngineBindings.md` (binding layers), `docs/MakeYourOwnPlatform.md` (writing
a feature module). The `[Platform] IniTakeover` switch is documented in
`README.md` and `VentureCore.ini.example`.

## The three INI layers a feature sees

A platform build touches three different INI layers; keep them apart when
designing a feature:

| Layer | File | Read by | Purpose |
| --- | --- | --- | --- |
| `[Platform]` configuration | `<Platform>.ini` | the base | platform-wide options (`LogLevel`, `LineagePolicy`, `IniTakeover`, ...) |
| `[Features]` switches | `<Platform>.ini` | the base, one line per feature module | turn a whole feature module on/off; defaults to on |
| feature keys | `<Platform>.ini` (the module's own section) or `rules(md).ini` / `art(md).ini` | the feature itself | the module's parameters; the module supplies the defaults |

The middle layer is deliberately coarse: one switch per feature module, not one
per key.  A module with any number of keys - including none - still has exactly
one `[Features]` line, and its keys are read by the module (see "One switch per
feature module, not per key" in `docs/MakeYourOwnPlatform.md`).

## The platform key registry (2.0)

`<Platform>.ini` is both a base configuration file and the place where a feature
module declares its own keys.  Before 2.0 each module read its keys with a raw
getter and carried a default in the call; a mis-spelled key or an invalid value
was invisible.  The registry (`src/Core/IniRegistry.h`) makes the declaration
explicit:

```cpp
// Inside FeatureContext, from Initialize() (runs in both switch states):
bool logEveryUnit = VENTURE_INI_BOOL(context, "MyFeature", "LogEveryUnit", false,
    "log one line per handled unit");
int progressEvery = VENTURE_INI_INT(context, "MyFeature", "ProgressEvery", 100, 0, 10000,
    "log one progress line every N units");
char mode[32] = {};
VENTURE_INI_STRING(context, "MyFeature", "Mode", "safe", mode, sizeof(mode),
    "which strategy to use");
```

The macros (in `include/platform/Feature.h`) pass `__FILE__` as the registration
site; the equivalent function-pointer calls take `sourceFile` as the last
argument.  Every declaration records `section / key / type / default / range /
description / owning feature / source file` and returns the validated value.

### Registry rules

* **Valid value** - the key is read with the same boolean tokens as before
  (`yes/no`, `true/false`, `on/off`, `1/0`, whole token, case-insensitive).
  Integers must be one complete base-10 number (the legacy getter accepted a
  prefix such as `12abc`; the registry rejects it) inside the declared
  `[minValue, maxValue]` range (`minValue > maxValue` = no range).
* **Invalid value** - falls back to the declared default and produces a warning:
  `key [MyFeature] ProgressEvery: '12abc' is not a valid integer - using the
  default '100'`.  Invalid values are never silent, and there is no switch to
  turn the warning off.
* **Duplicate declaration** - two modules declaring the same `(section, key)`
  produce a warning naming both and keep the first declaration; a module
  declaring a base `[Platform]` key is warned that the key is reserved.
* **Effective values** - each declaration logs one Debug line; the startup block
  ends with one Info summary: `ini keys: 9 registered, 1 invalid, 2 unknown`.
* **Missing vs empty** - a missing key keeps the default without a warning; a key
  present with an empty value is validated like any other token (so an empty
  bool is invalid).

### Unknown keys at startup

After the feature modules are initialized the base enumerates `<Platform>.ini`
once and classifies every entry:

| Where | Result |
| --- | --- |
| registered key / built-in `[Platform]` key / known `[Groups]` switch | known, no message |
| unknown `[Features]` name | **Warn** with a suggestion: `unknown feature switch [Features] RaidalIndicator - ignored (did you mean RadialIndicator?)` |
| unknown key in `[Platform]`, `[Groups]` or a module section | **Warn**: `unknown key [RadialIndicator] LargeRagne - ignored` |
| a section the platform does not own | Debug only, one line per section: `ignoring user-defined section [User] (...)` |

`[Platform] IniWarnUnknownKeys=no` turns the unknown-**key** warnings off (the
entries are then Debug-logged instead); invalid values still warn.  The file is
never rewritten or auto-repaired.

### Feature groups

`FeatureDescriptor::group` (optional trailing field) names a group, and
`[Groups] <group>=yes|no` (default `yes`) is an explicit master switch for the
whole group.  A module is enabled when its `[Features]` switch, its group switch
and its declared host requirements all allow it; the per-module switch is not
replaced.  The group name is only needed for this gate and for the template
dump - it does not change the feature API.

### R1 template dump

`[Platform] DumpIniTemplate=yes` (development only) writes
`<Platform>.ini.generated` next to the log at startup: the registry rendered as
INI text ([Platform], [Groups], [Features] and one section per module key, with
type/default/range/description annotations).  Compare it with the shipped
`VentureCore.ini.example` (or run `python3 scripts/compare_ini_template.py`
against a generated file) when a key changed.  The dump is the R1 route; the
build-time manifest route (R2) is deliberately not implemented.

The rest of this document is about the third layer's engine half (typed reads
over the parsed rules/art INI) and the takeover that keeps our own rules keys
away from a host platform.

## Why read through the engine

`rules(md).ini` and `art(md).ini` are parsed by the engine into `CCINIClass`
objects; YRpp exposes them as `CCINIClass::INI_Rules` (a pointer) and
`CCINIClass::INI_Art` (a reference). A feature must read those parsed objects,
not re-open the files:

* the engine's view is what every other subsystem uses, including its own
  merging/resolution rules; re-parsing would diverge from it;
* the takeover layer removes keys from that parsed object, so it is the only view
  the platform can consistently filter;
* the file may not even exist (defaults compiled into the engine), while the
  parsed object always exists once the engine has loaded it.

## Typed reads (`Core/EngineIni.h`)

| Function | Returns |
| --- | --- |
| `EngineIni::ReadBool(ini, section, key, def)` | `def` when the INI/section/key is missing, otherwise the engine's boolean parse (`yes`/`no`/`true`/`false`/`on`/`off`/`1`/`0`) |
| `EngineIni::ReadInt(ini, section, key, def)` | `def` when missing, otherwise the engine's integer parse |
| `EngineIni::ReadDouble(ini, section, key, def)` | `def` when missing, otherwise the engine's floating-point parse |
| `EngineIni::ReadString(ini, section, key, def, buffer, size)` | the value, or `def`, copied into `buffer` (always NUL-terminated) |

Every getter tolerates a null INI pointer, section or key and returns the default
(`EngineIni.h` only forward-declares `CCINIClass`; the call site includes
`Engine/EngineCompat.h` when it needs `CCINIClass::INI_Rules` / `INI_Art`):

```cpp
#include "Core/EngineIni.h"
#include "Engine/EngineCompat.h"   // CCINIClass::INI_Rules

const bool logUnits = venture::EngineIni::ReadBool(
    CCINIClass::INI_Rules, "Examples", "UnitCounterLog", false);
```

**Boolean and integer parsing is not shared with the platform INI.**  `ReadBool`
here hands the value to the engine's `CCINIClass::ReadBool`, which resolves the
value from the **first character** of the raw text (`1` / `T` / `Y` → true,
`0` / `F` / `N` → false, anything else → the supplied default; so `on`/`off` fall
back to the default).  The platform's own `<Platform>.ini` registry
(`src/Core/IniRegistry.cpp`) is stricter: it compares the whole token
case-insensitively (`yes`/`no`, `true`/`false`, `on`/`off`, `1`/`0`) and rejects
an invalid value with a warning; a declared integer must be one complete number
inside its range.  The legacy `Ini::GetInt` still parses a leading decimal number
and ignores trailing text (`12abc` → 12); do not rely on that - declare the key
with `VENTURE_INI_INT`.  Do not assume a spelling accepted by one parser is
accepted by the other when documenting a key; the full set of `<Platform>.ini`
keys is documented in `README.md` and `VentureCore.ini.example`.

The same functions work for the art INI:

```cpp
const int frameCount = venture::EngineIni::ReadInt(
    &CCINIClass::INI_Art, "SomeAnim", "FrameCount", 1);
```

### Reading rules keys at the right time

The platform starts from the `ExeRun` hook, which runs **before** the engine
parses `rules(md).ini`. A feature that needs a value from the rules INI therefore
cannot reliably read it inside `Initialize()`; instead it registers a rules-INI
observer (`Core/IniTakeover.h`):

```cpp
void OnRulesIniReady(venture::IniTakeover::IReader& rules)
{
    const bool logUnits = rules.ReadBool("Examples", "UnitCounterLog", false);
    // ...
}

void Initialize(const venture::FeatureContext& context)
{
    venture::IniTakeover::RegisterOwnedKey("Examples", "UnitCounterLog");
    venture::IniTakeover::RegisterRulesIniObserver(&OnRulesIniReady);
}
```

The observer runs when the engine has parsed the rules INI and is about to read
it, once per rules load, **before** the takeover purge - so it sees exactly the
values the game itself will use. `ExampleIniDemo.cpp` is the working example
(`[Examples] UnitCounterLog=yes|no`, logged when a game starts).

## INI takeover

### Contract

Features define INI keys. When a host platform such as official Phobos is loaded
next to VentureCore, a key that both sides understand must not be consumed by
both (a double-count / conflicting-semantics hazard, D33). The platform therefore
declares which `(section, key)` pairs it owns and removes them from the parsed
rules INI before any other consumer reads it.

* **Registration** - a feature calls `IniTakeover::RegisterOwnedKey(section, key)`
  from `Initialize()` while its feature switch is on. Matching is
  case-insensitive and duplicate registrations are collapsed. When a feature is
  switched off it registers nothing, so its keys stay visible to hosts (that is
  what switching the feature off means).
* **Namespace guard (C-09)** - platform-owned keys live in a
  **platform-namespaced section** (`[VentureCore]`, or a feature namespace such as
  `[Examples]`), never in one of the engine's own rule sections. Registering a key
  in a reserved engine section is **refused**: the key is not registered, nothing
  is purged for it, a `Log::Warn` line is written and the refusal is counted in
  the takeover startup line. The reserved list is the engine's core rules
  sections: `[General]`, `[Basic]`, `[AudioVisual]`, `[MultiPlayer]`,
  `[Maximums]`, `[AI]`, `[Colors]`, `[Countries]`, `[Sides]`, `[CrateRules]`,
  `[CombatDamage]`, `[Radiation]`, `[ElevationModel]`, `[WallModel]`,
  `[SpecialFlags]`, `[Powerups]`, `[LandCharacteristics]`, `[MouseCursors]`
  (case-insensitive, `IniTakeover::IsReservedEngineSection`).  Without the guard
  a mis-registered vanilla key would be silently deleted from the parsed INI
  before the engine reads it - the takeover would break the game instead of
  protecting it.
* **Purge** - the engine hook removes the registered pairs from the parsed rules
  INI. Only registered pairs are touched; every other key, including an unowned
  key in the same section, is left alone. The purge is idempotent, and it runs
  again on every rules load (a second scenario start re-parses and re-reads the
  INI through the same object).
* **Case** - both the registry and the removal are case-insensitive, because INI
  keys/sections are.
* **Switch** - `[Platform] IniTakeover=yes|no` (default `yes`), read lazily from
  `<Platform>.ini` when the rules INI is first loaded. With `no`, the read APIs
  and the rules-INI observers still work but nothing is purged - the user opted
  out of the interop.

### Route A: purge after parse, at `RulesClass::Init` (implemented)

Hook: **`0x6686C0`, size `0x6`, handler `Venture_RulesClass_Init`**.

The engine parses `rules(md).ini` into `CCINIClass::INI_Rules` and then calls
`RulesClass::Init(CCINIClass*)` to read it. Both call sites of that function pass
`CCINIClass::INI_Rules`, and its body reads the INI itself shortly after entry
(`GetSection` at `0x6689F6`, `ReadInteger` at `0x668A19`), so at the function
entry the INI is already parsed and nothing has read the platform's keys from it
yet.

Verified instruction window:

```
006686C0: 81 ec d0 00 00 00   sub  esp, 0xd0      (6 bytes)
006686C6: a1 e0 54 b0 00      mov  eax, ds:0xb054e0   <- next instruction
```

The function is `__thiscall` with one 4-byte stack argument and returns with
`ret 0x4`; the hook reads the INI pointer with `GET_STACK(CCINIClass*, pINI, 0x4)`.
No other platform in the supported host set hooks `0x6686C0`; the nearest
ecosystem site is `0x668BF0` (`RulesClass_Addition`), which is a *different*
function called from inside `RulesClass::Init` (`0x668A27`, `0x668B05`,
`0x668BAA`), i.e. **after** this hook. A host that reads its rules extensions
there (and every later rules/type read) therefore never sees the platform-owned
keys.

The adapter removes a key with the engine's own single-entry removal path
(`YRpp INIClass::Clear(section, key)`, gamemd `0x5257C0`): it looks up the stored
section/entry case-insensitively in the parsed object, then calls the engine
function with the stored spelling, which drops the entry from the section's
`EntryIndex`, unlinks it and deletes it with the game's deleting destructor.
Using the engine's own removal avoids hand-rolled index/list surgery and keeps
the INI object internally consistent.

Log lines (from the takeover hook):

* `[IniTakeover] [Platform] IniTakeover=yes|no (N owned key(s) registered,
  M refused by the deny-list)` - info, once, at the first rules load;
* `[IniTakeover] ... refused as platform-owned - '<section>' is an engine core
  section; put platform keys in a platform-namespaced section` - warn, once per
  refused registration;
* `[IniTakeover] rules INI purge: N owned key(s) removed (...)` - debug, only
  when something was actually removed (enable `[Platform] VerboseLogging=yes` or
  a debug log level to see it).

### Route B: read-layer filtering - deliberately deferred

The alternative is a hook on `INIClass::ReadString` (`0x528A10`) that returns the
default for owned `(section, key)` pairs at read time, with a thread-local bypass
so the platform's own reads still work. It is **not** implemented, for three
reasons:

1. **The site is already taken.** Ares hooks `0x528A10` with size `5`; a second
   hook at the same address shares the overwritten window (SyringeEx uses the
   largest declared size) and would filter every string read process-wide.
2. **It is far more invasive.** Route B changes the semantics of every consumer
   of the engine INI - including map/gamemode INIs that do not exist yet at
   `RulesClass::Init` time - and needs the bypass mechanism to be correct for
   every nested read order.
3. **Route A is sufficient for the contract.** Purging the single parsed rules
   object covers every later reader of that object (engine, host platforms and
   the platform itself), without touching global read behaviour.

Route B is the fallback for the INIs route A cannot own: map/gamemode INIs and
`art(md).ini`-based extension keys. It should be revisited together with those
loaders (a per-INI purge hook is the preferred first option; `0x528A10` only if
no clean load site exists).

### Limitations

* **One INI object.** Route A covers the parsed `rules(md).ini` only. Gamemode
  and map INIs have their own load path and are not purged yet; `art(md).ini`
  keys can be read with `EngineIni` but are not hidden from hosts yet.
* **Only registered pairs.** A key that a feature forgets to register, or
  registers after the rules INI has been parsed, stays visible to hosts. Register
  from `Initialize()`.
* **Engine core sections are never owned.** See the namespace guard above: a
  platform that wants to change a vanilla rule has to read it and act on it, not
  hide it from the engine.
* **Concrete entries only.** The purge removes the concrete entry. If a mod
  uses an INI inheritance mechanism (a host platform's `$Inherits` read-layer
  feature) to alias the same key from a parent section, that read-layer resolution
  is outside route A's reach.
* **Single-threaded.** The registry and the observers are populated/consumed on
  the game's main thread; there are no locks.
* **Observers see the real value only before the purge.** Any later read of an
  owned key from the same INI returns the default.

## Hook sites reused from the ecosystem (not installed)

These are the type-loading sites the wider Phobos / YRpp ecosystem hooks for
per-type INI reading.  The base does **not** install them; they are listed so a
type-extension layer can reuse the same verified sites instead of re-deriving
them:

| Address | Engine function | Typical use |
| --- | --- | --- |
| `0x747B49` | `UnitTypeClass` INI read | unit type extension keys |
| `0x524734` | `InfantryTypeClass` INI read | infantry type extension keys |
| `0x41CD54` | `AircraftTypeClass` INI read | aircraft type extension keys |
| `0x715A4D` | `TechnoTypeClass` INI read | shared techno-type extension keys |

The rules-INI purge already removes owned keys from the parsed INI *before* those
readers run, so a type-level hook is only needed when a feature wants to
intercept the read itself (for example to log or transform it), not to hide a
key.

## Verification

`tests/test_engine_ini.cpp` is a pure console test (no game) and covers:

* typed reads over an in-memory INI before the purge, including defaults for
  missing keys;
* the registry is case-insensitive, de-duplicating and ignores null/empty
  registrations;
* observers run before the purge and see the real value;
* only owned keys are removed (same-named key in another section and unowned key
  in the same section survive);
* removal matches the INI case-insensitively;
* the purge is idempotent and `Apply` can run without a reader;
* `IniTakeover=no` semantics: reads (and observers) still work, nothing is
  purged.

`tests/test_ini_registry.cpp` covers the 2.0 key registry with an in-memory
"file": bool/int/string reads and defaults, the strict integer parser and the
range check, invalid values falling back with a warning, duplicate declarations,
the built-in `[Platform]` keys, `defaultEnabled` and `[Groups]` gates, the
unknown-key scan (suggestions, known-section keys, Debug-only user sections,
`IniWarnUnknownKeys=no`) and the R1 template rendering.

The hook boundary itself is verified with `objdump -d -M intel
--start-address=0x6686C0 --stop-address=0x6686E0 gamemd.exe` (window above), the
hook table is checked by `scripts/verify_build.py` against
`docs/HookCatalogue.md`, and `0x6686C0` was cross-checked against the five host
platform `.syhks00` hook tables used for host compatibility (none uses it).

## Files

| Path | Contents |
| --- | --- |
| `src/Core/IniRegistry.h` / `.cpp` | 2.0 platform-INI key registry, switches/groups, unknown-key scan, template dump (pure C++, unit-testable) |
| `src/Core/Ini.h` / `.cpp` | raw `<Platform>.ini` access (getters + entry enumeration for the scan) |
| `src/Core/EngineIni.h` / `.cpp` | typed reads over a `CCINIClass` |
| `src/Core/IniTakeover.h` / `.cpp` | owned-key registry, observers, purge, `[Platform] IniTakeover` gating (pure C++, unit-testable) |
| `src/Engine/Hooks.Ini.cpp` | engine adapter over `CCINIClass` and the `0x6686C0` hook |
| `src/Features/Example/ExampleIniDemo.cpp` | example module: read + register an owned key |
| `tests/test_engine_ini.cpp` | unit test of the takeover core |
| `tests/test_ini_registry.cpp` | unit test of the 2.0 key registry |
