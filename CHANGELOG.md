# Changelog

All notable changes to VentureCore are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
The project uses a pre-1.0 version scheme: `0.<MINOR>.<PATCH>-<suffix>`, where the
suffix (`experimental`, `alpha`, ...) marks a non-stable sample and is carried in
the DLL version resource (`v0.2.0.0-experimental`).

## [0.2.0-experimental] - 2026-11-10

Base-completion release.  The empty skeleton from 0.1.0 gained the platform
layers a feature pack actually needs, and every layer has automated verification
on this machine.  Still **no gameplay features**: installing the DLL does not
change the game.

### Added

- **Extension hierarchy** (`src/Ext/ExtHierarchy.h`, `src/Ext/AbstractExt.{h,cpp}`):
  `AbstractExt -> TechnoExt -> FootExt -> UnitExt` with runtime type ids, so the
  migrated feature code can keep using `UnitExt::Fetch(unit)` /
  `TechnoExt::Fetch(techno)`; one most-derived instance per owner object, a second
  store claiming the same owner fails cleanly.
- **Savegame tail block** (`src/Core/SaveTrailer.{h,cpp}`,
  `src/Ext/ExtSave.{h,cpp}`, `src/Ext/ExtStream.h`, `src/Ext/ExtSwizzle.h`,
  `src/Engine/Hooks.Save.cpp`): extension state is appended after every other
  platform's block (`[payload][magic VNC1][formatVersion][payloadSize][crc][magic]`),
  read by scanning backwards from the end of the stream (old saves without a
  trailer self-heal), owner pointers relinked through the engine's swizzle pass,
  per-store data versions and unknown-store skipping; `docs/SaveFormat.md`.
- **Rules/art INI layer** (`src/Core/EngineIni.{h,cpp}`,
  `src/Core/IniTakeover.{h,cpp}`, `src/Engine/Hooks.Ini.cpp`): typed reads over the
  engine-parsed INI files, rules-INI observers, and the rules-INI takeover that
  removes platform-owned keys from the parsed rules file before a host platform can
  read them (`[Platform] IniTakeover`, default on); `docs/Ini.md`.
- **Event channel** (`src/Event/EventChannel.{h,cpp}`,
  `src/Engine/Hooks.Events.cpp`): platform event types `0x70-0x7F` with fixed
  payloads, a responder registry, the engine's `EventClass` record as wire format,
  dispatch + three `GetEventSize` hooks, startup occupancy log and mixed-client
  warning; `docs/EventChannel.md`.
- **Command registration** (`src/Command/Commands.{h,cpp}`,
  `src/Engine/Hooks.Commands.cpp`): a command registry plus a `CommandClass`
  adapter registered at the engine's callback; the example command
  "Venture Test Command" logs and raises the example event, so one key press
  exercises both paths; `docs/Commands.md`.
- **Lineage mutual exclusion** (`include/platform/Lineage.h`,
  `src/Core/Lineage.{h,cpp}`): exported `VentureLineage_GetInfo` marker and
  `VentureLineage_CountConflicts` diagnostic, startup scan of loaded modules and
  `[Platform] LineagePolicy=refuse|warn` (default refuse); the probe loads a copy
  of the DLL under a second name and proves the detection; `docs/Lineage.md`.
- **Logging control** (`src/Core/LogPolicy.h`, `src/Core/Log.{h,cpp}`):
  `[Platform] LogLevel=error|warn|info|debug` (with `VerboseLogging` kept as the
  debug alias), debugger output only in DEBUG builds or with
  `[Platform] DebugOutput=yes`, one per-process separator header, and a
  source-hygiene check that no unconditional `OutputDebugStringA` remains;
  `docs/Logging.md`.
- **Extension-store hot paths** (`src/Ext/AbstractExt.h`,
  `src/Ext/ExtensionStore.h`): intrusive allocation-order list with O(1) removal,
  owner map and a single-entry `Fetch` cache, `Reserve()` hint; measured in
  `tests/bench_store.cpp` / `scripts/linux/bench_store.sh` (Remove 4.3-84 µs ->
  0.19-0.25 µs, same-owner Fetch 34-52 ns -> ~2 ns; numbers in
  `docs/EngineBindings.md`).
- **Test and tooling** : `tests/` with seven engine-free test programs,
  `scripts/linux/run_tests.sh` (MSVC under Wine), the extension-store benchmark
  script, a Wine-toolchain flock shared by build/tests/probes, and a generated
  git-stamp header helper so tests compile the platform sources directly.

### Changed

- `scripts/verify_build.py` now runs **7 checks** (adds the log-policy source
  hygiene check) and follows the hook catalogue (17 hook records, 16 distinct
  handlers).
- `scripts/linux/load_probe.sh` resolves **7 exports** (base hook handlers plus the
  two lineage markers), validates the marker payload and runs the two-copy lineage
  conflict test.
- The Example module is now an `ExampleUnitExt` derived from `UnitExt`, verifies the
  base-class views, persists a per-unit serial through the savegame trailer and
  moved its per-unit logging to DEBUG with a sampled INFO progress line; a second
  module `example_ini_key` reads one rules key.
- Extension storage internals changed (hierarchy, intrusive list, fetch cache); the
  feature-facing API (`Allocate` / `Remove` / `Fetch` / `Clear` / `Size`) is
  unchanged.
- Documentation set grew with `docs/SaveFormat.md`, `docs/Ini.md`,
  `docs/EventChannel.md`, `docs/Commands.md`, `docs/Lineage.md` and
  `docs/Logging.md`; `docs/EngineBindings.md`, `docs/HookCatalogue.md`,
  `docs/HostMatrix.md`, `docs/MakeYourOwnPlatform.md`, `docs/Updating.md` and
  `README.md` were brought up to date.

### Known limitations

- Host-specific (T1-T3) features, Phobos interop and AE are still not implemented.
- The event channel and the command names are a same-build contract in
  multiplayer: every peer must run a build registering the same event types,
  payload sizes and commands (no automatic peer detection).
- Savegame persistence applies to stores that opt in (`SetSaveable`); the trailer
  is the last block in the savegame stream, which assumes no platform DLL whose
  name sorts after this one starts appending blocks.
- The lineage marker detects this platform's own lineage only, and modules loaded
  after startup are not detected.
- The base does not implement a clean unload path; the game process terminates
  normally.

## [0.1.0-experimental] - 2026-11-09

First sample release of the base: an empty, MIT-licensed platform skeleton plus
one Example feature module.  There are **no gameplay features** in this release;
installing the DLL does not change the game.

### Added

- Platform identity and version metadata: `src/Core/PlatformConfig.h`,
  `src/Core/PlatformVersion.h`, `src/version.rc` (VERSIONINFO with platform name,
  version, build type and the pinned YRpp commit), git commit/ref stamped at build
  time.
- Startup pipeline: SyringeEx gate (refuses to run under an older Syringe),
  file log, `<Platform>.ini` load, host detection, support-tier resolution,
  feature initialization; the log records the resolved configuration.
- File logger: `<Platform>.log` next to `rules.ini`, four levels, falls back to
  `OutputDebugString` if the file cannot be opened; `[Platform] VerboseLogging`
  enables debug lines.
- INI reader over the game directory's `<Platform>.ini` with typed getters and
  graceful defaults; a missing INI is not an error.
- Host detection matrix: optional hosts (Ares, Phobos, Kratos, Kaleidoscope) are
  detected by loaded module, logged, and mapped to support tiers T0-T3.  No
  functional dependency on any of them in this release.
- Source-level feature registry: `FeatureDescriptor`, `FeatureContext`,
  `FeatureRegistrar` and `VENTURE_REGISTER_FEATURE` (`include/platform/Feature.h`).
  One `[Features]` switch per feature `iniKey`, default on, plus `minTier` gating;
  `Initialize()` runs in both switch states.
- Extension-storage layer: `AbstractExt` + typed `ExtensionStore<TOwner, TExt>`
  (an `unordered_map` for lookup plus an allocation-order `vector`) and the
  type-erased global `ExtensionIndex`; stores register a scenario-clear callback
  on first use.
- Lifecycle hooks for the storage layer: `UnitClass` constructor, both
  `UnitClass` destructor sites, scenario clear and pointer-invalidation
  notification (notification only - extension removal is owned by the destructor
  hook).
- Example feature module (`example_unit_counter`): allocates/removes a
  `UnitClass` extension, counts created/destroyed/live objects and logs the
  transitions; switchable from the INI.  It has no gameplay effect and exists to
  prove the base machinery works.
- Engine binding gate: YRpp pin in `src/Core/EnginePin.h`,
  build-time check `scripts/linux/check_yrpp.py`, compile-time `static_assert`s on
  engine ABI sizes in `src/Engine/EngineCompat.h`, run-time pin logging and the
  `YrppCommit` version-resource string.
- Hook catalogue `docs/HookCatalogue.md` (six hook records, five distinct handler
  names) and the static verifier `scripts/verify_build.py`, which parses the
  catalogue and checks the PE image, the `.syhks00` hook records, the exported
  handlers, the version strings and the registered feature id.
- Linux build scripts for the real MSVC toolchain under Wine
  (`scripts/linux/build_linux.sh`, `common.sh`, `setup_wineprefix.sh`), plus
  `fetch_yrpp.sh`, `check_yrpp.py`, `abi_probe.sh`, `load_probe.sh` and
  `check_eol.py`; Windows build via `VentureCore.sln` (v143).
- Documentation set: README, licensing, host matrix, engine bindings, hook
  catalogue, renaming guide and "make your own platform" guide.

### Known limitations

- No savegame persistence (the save/trailer block is the next increment), no
  event channel, no command registration.
- No host-specific (T1-T3) features, no lineage mutual-exclusion detection, no
  Phobos interop, no AE handling - all planned for later increments.
- The Example module is a counter/smoke test, not a gameplay feature.
- The base does not implement a clean unload path; the game process terminates
  normally.
