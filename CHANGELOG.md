# Changelog

All notable changes to VentureCore are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
The project uses semantic versioning.  The pre-1.0 samples carried an
`-experimental` suffix in the DLL version resource; **`1.0.0` (2026-11-11) is the
first stable release** and the stable contracts are listed in `README.md`.

## [Unreleased]

### Changed (breaking - 2.0 feature API)

- host model: the support-level scalar is gone.  The `Tier` enum/`TierName()` and
  `FeatureDescriptor::minTier` are removed; `FeatureContext::tier` is replaced by
  `hostMask` plus `HasHost(Host)` / `HasHostBit(unsigned)` / `HasHostName(const
  char*)` (public bits in `include/platform/HostIds.h`, registry in
  `src/Core/HostRegistry.*`).  A feature now declares what it needs with
  `FeatureDescriptor::requiredHosts` (AND) and `anyOfHosts` (OR); the startup log
  reports the detected set and the missing hosts instead of a level name.
- host detection is a registry: the built-in hosts are registered in
  `src/Core/Hosts.Builtin.cpp`, and a derived platform registers private hosts in
  `src/Core/Hosts.Custom.cpp` with an explicit custom bit (>= 1<<3, 29 slots).
  The closed-source platform that the 1.x table also probed is no longer detected
  and its bit is unused.
- the startup host line is now `hosts   : Ares, Phobos (2 host(s))` or
  `hosts   : <none>` (no level suffix); the feature lines are `enabled`,
  `disabled by ... [Features] ...` or `disabled - requires host ...`.

### Documentation

- documented the feature-switch contract: `[Features]` has exactly **one switch
  per feature module**, no matter how many keys the module owns (or whether it
  owns any at all); a module's own keys live outside `[Features]` and are read by
  the module.  Clarified in `README.md`, `docs/MakeYourOwnPlatform.md` (new
  section), `docs/Ini.md` (three INI layers), `VentureCore.ini.example` and the
  `FeatureDescriptor::iniKey` comment.  No behaviour change.

## [1.0.0] - 2026-11-11

**First stable release of the base.**  No functional change from
`0.4.0-experimental`: the same hooks, savegame format, event protocol, command
registry and INI contracts.  This release marks the base as stable, finalises the
version scheme (no `-experimental` suffix; the version resource no longer sets
`VS_FF_PRERELEASE`) and gives `README.md` a release-quality description
(stability contract, verification baseline, roadmap wording).

Stable contracts as of 1.0.0: the `include/platform/` feature API, the savegame
frame and payload signature, the `0x70-0x7F` event range/record layout, the hook
table for the supported `gamemd.exe` build, and the `<Platform>.ini` keys.  The
roadmap items (T1-T3 host-specific features, Phobos interop, AE) are additive
1.x increments and are not part of 1.0.

Reference verification (2026-11-11, `--clean`, MSVC 14.44 under Wine): 23
translation units, 0 warnings; `verify_build.py` 7/7; `load_probe.sh` PASS
(exports 7/7); `run_tests.sh` 10/10; `check_eol.py` 17 rules / 0 mismatch;
`abi_probe.sh` 20 types, every `sizeof` assertion matches; real game (YR + Ares)
normal path and lineage-refuse scenario both pass.

## [0.4.0-experimental] - 2026-11-11

Hardening release over 0.3.0: the ten "recommended" findings of the independent
blind review's Batch B are fixed, plus three latent problems found while
implementing them.  Still **no gameplay features**: installing the DLL does not
change the game.

**Behaviour changes for derived platforms and users:**

- a **lineage-refused DLL is now inert**: it keeps its hook windows and exports,
  but every engine-facing handler returns immediately, its command is not added
  to the engine command list, its event types are neither sized nor dispatched,
  the rules-INI takeover purges nothing and the savegame hooks do not touch the
  stream (C-06).  `docs/Lineage.md` documents "inert, not absent".
- command names are compared **case-insensitively**; a second command that
  differs only in case is rejected, and the registration hook warns when a
  command collides with one already in the engine list.  Platform commands must
  carry the platform prefix (`docs/Commands.md`, C-08).
- a platform-owned INI key in an **engine core section** (`[General]`, ...) is
  refused with a warning instead of being registered and purged (C-09); own
  keys belong in a platform-namespaced section (`docs/Ini.md`).
- extension type registrations are validated; a rejected type id is poisoned and
  can no longer be fetched through a wrong `static_cast` (D-22/A-18).

### Added

- `src/Core/PlatformGate.h`: process-wide `Pending` / `Running` / `Refused`
  startup gate checked by the engine-facing handlers and by the command/event
  registries; `Platform::IsStarted()`.
- `RegisterExtType` validation (type-id range, self-reference, conflicting base,
  cyclic/deep chain; forward references stay legal), a problem sink routed to
  the platform logger, poisoned type ids and a bounded parent-chain walk.
- `CommandNameEquals` (ASCII case fold), case-insensitive registry lookup and a
  registration-hook summary/collision log.
- INI takeover deny-list of engine core sections (`IsReservedEngineSection`), a
  warn sink and `RefusedKeyCount()` reported in the takeover startup line.
- Tooling: `run_tests.sh` fails on a missing/empty `tests/` and runs every test
  under a per-test timeout (`VCS_TEST_TIMEOUT`, default 120 s); `verify_build.py`
  log-policy scan understands `#elif` and enforces the single debugger helper in
  `src/Core/Log.cpp`; `abi_probe.sh` takes the toolchain lock and prints the four
  previously omitted `EngineCompat.h` types; `build_linux.sh` warns about a
  `src/**/*.cpp` that is not in `VentureCore.vcxproj`; the benchmark builds with
  the release `/O2 /Oy` flags and rebuilds on any Ext source/header change.
- 75 new test assertions across `test_ext_hierarchy`, `test_lineage`,
  `test_commands`, `test_event_channel` and `test_engine_ini`.

### Changed

- Parent-chain walks are depth-limited (a broken table fails loudly instead of
  hanging); `ExtStoreDesc` lost the dead `count` callback.
- `docs/Lineage.md`, `docs/Commands.md`, `docs/Ini.md`, `docs/EngineBindings.md`,
  `docs/Logging.md` and `scripts/linux/README.md` describe the new behaviour.
- Store benchmark numbers re-measured with `/O2 /Oy` (faster across the board;
  the "Before" column is historical and directional only).

### Fixed

- D-22: a copy-pasted `TypeId`/`BaseTypeId` pair could silently capture the
  built-in hierarchy mapping and produce a wrong `static_cast` (the build has no
  RTTI).
- A-18: a cyclic parent-chain registration could hang a lookup; dead field.
- D-06: the lineage scan's Windows-directory skip matched a bare string prefix,
  so modules under `C:\WindowsApps\` or `C:\Windows.old\` were skipped - a
  false negative for the mutual-exclusion safety net.  The match is now on a
  directory boundary.
- D-12/D-13/D-14/D-16/D-17: the verification tooling no longer reports a pass
  while doing nothing (empty test directory, `#elif` blind spot, unlocked ABI
  probe, unoptimised benchmark with stale inputs, silently uncompiled sources).
- Two latent `TEST_DEPS` issues: a test/benchmark source without the comment
  aborted the runner under `set -o pipefail` (found while implementing D-13/D-17).

### Real-game verification (2026-11-11, YR + Ares)

- normal path, one match + save/load + the example command: command and event
  path live, savegame trailer appended, load restored 44/44 adopted extensions;
- lineage-refuse, two copies of the DLL: both copies log the conflict and stay
  disabled - no command registration, no event-channel answers, no rules-INI
  purge, no savegame writes; the game itself runs normally.

## [0.3.0-experimental] - 2026-11-11

Hardening release over 0.2.0: every Batch-A finding of the independent blind
review that blocked a public release is fixed (one BLOCKER, eight MAJOR, the
related MINORs), the savegame trailer is now a header-first strictly sequential
frame with a **v2** payload, and the README/docs claims that did not match the
code were corrected.  Still **no gameplay features**: installing the DLL does not
change the game.

**Breaking format change for extension state**: the trailer is
`[16-byte header][payload]` and the payload carries a signature of the platform's
saveable store set.  Saves written by `v0.2.0-experimental` load the game
normally, but their extension state is dropped once - the version-1 payload is
rejected with a log warning, there is no migration (decision D19).

### Added

- Savegame load-risk coverage: three hooks on the engine's abort exits
  (`0x67E4B1` / `0x67E53A` / `0x67E595`) close the extension load window (W3,
  A-04); a scenario-clear fallback resets the window and the deferred records
  when no load is in progress (W3/W4c).
- Extension-layer load lifecycle: `ExtensionIndex` load window, deferred
  allocations for object constructors, staged swizzle registration that the
  engine only sees after the whole payload validated
  (`ExtSwizzle::BeginStaging/CommitStaged/DiscardStaged`, A-01), and a
  load-window-is-open guard in the scenario-clear path (W4c, found by the real
  game G1 test).
- Savegame payload v2: store-set signature (`typeId`, `dataVersion`, order) that
  must match the platform's registered saveable stores exactly; a mismatch
  rejects the whole payload instead of skipping a block (A-05).
- Payload robustness: size fields are validated against the remaining bytes and
  the 16 MiB cap **before** allocation (A-02/A-11); rows, blocks and the payload
  must be consumed exactly (A-09); duplicate store type ids are refused (A-10);
  the row canary includes the store `dataVersion`; read loops tolerate legal COM
  short reads and report no-progress/failure instead of spinning (A-13/A-14).
- Event-channel fail-closed size policy
  (`EventChannel::GetWireSize`: registered size | vanilla `0x00-0x2E` |
  one byte for everything unregistered above `0x2E`) and a real
  `Dispatch(type, data, availableSize)` buffer-length check (C-01/C-02).
- Test/tooling: `tests/test_load_window.cpp`, `tests/test_payload_safety.cpp`,
  `tests/test_logging.cpp` (real log file I/O), `scripts/linux/test_fetch_yrpp.sh`
  (self-copy guard / atomic install / pin gate) and
  `scripts/linux/verify_incremental.sh` (real toolchain incremental-build
  matrix); `run_tests.sh` now summarises compiler warnings of the test builds.
- `Log::OpenAt(path)` (public file-I/O entry point used by the new test and by
  derived platforms).

### Changed

- **Savegame trailer rewritten** (`src/Core/SaveTrailer.{h,cpp}`,
  `src/Engine/Hooks.Save.cpp`): the writer moved from the shared ecosystem tail
  site `0x67E42E` (owned by Ares, whose handler always jumps away - the block was
  never written with Ares installed) to `0x67D1B4`, after the engine's
  object-state writer returned and before the finalize/Commit.  The frame is
  header-first (`[magic "VNC1"][formatVersion=2][payloadSize][crc32][payload]`)
  and both sides are strictly sequential: the engine's savegame `IStream` is a
  streaming wrapper that does **not** implement `Seek` (proven by the real-game
  G1 test), so the 0.2.0 "tail-anchored, seek-to-end" design could never work in
  the game.  Writes retry short writes; the read position is the end of the
  platform-ordered blocks because our handler runs after every known ecosystem
  reader.
- Version bump to `0.3.0-experimental` (`src/Core/PlatformVersion.h`,
  `scripts/verify_build.py`); minor bump because the savegame payload and the
  save/load behaviour changed incompatibly.
- Build tooling: `fetch_yrpp.sh` installs through a staging directory, refuses to
  copy the installed tree onto itself and no longer lets `--commit` override a
  detected git `HEAD` (D-09, S-01); the incremental-build inputs now include
  `include/`, the project/props files and the link arguments (D-10/D-11).
- `scripts/verify_build.py` version check hardened: the version must appear both
  as UTF-16LE (version resource) and as ASCII (compiled-in `VC_VERSION_DISPLAY`
  literal), and the numeric `VS_FIXEDFILEINFO` fallback derives the expected
  `dwFileVersionMS/LS` from `VERSION_STRINGS` (D-15).
- Event channel: the three `GetEventSize` hooks answer one byte fail-closed for
  every unregistered number above `0x2E`; only the vanilla range falls through to
  the engine table.  Startup log lists the registered types and the mixed-client
  warning says "consumed as one byte fail-closed, may desync but cannot
  overrun" (C-01).
- Logging: `WriteFile` failures are detected and reported once (the line is
  dropped), a NULL format string is rejected before the CRT, and the
  docs/README claims about flush/durability and "crash-proof" were replaced by
  the actual guarantees (D-01/D-03/D-04, L-04).
- Hook catalogue: 17 -> **20 hook records, 19 distinct handler names**; all
  documents now describe the `0x67D1B4` write site and the three failure-exit
  hooks.
- Documentation truth pass: claims that the recheck proved false or stale were
  corrected in `README.md`, `CHANGELOG.md` and `docs/` - `IniTakeover` API name,
  ABI-probe coverage wording, benchmark flags, pointer-invalidation capability,
  savegame layout (intrusive list, no backwards read), `VC_SAVEGAME_ID` usage,
  load-probe export count, `[Platform]` key list, event range (`0x00-0x2E`),
  lineage-refuse wording, command string lifetime, host/tier wording, log flush
  wording and the save-load step labels.

### Fixed

- B-03: the savegame trailer never reached the file with Ares installed (the
  released 0.2.0 "core persistence" was silently dead on the main supported
  host); fixed by the new write site plus sequential I/O (also A-12).
- A-04: a failed/cancelled savegame load left the load window open for the rest
  of the session, silently disabling every later extension allocation.
- A-06/A-07: invalidated owners are removed from the deferred list; the window
  closes before the post-swizzle engine call so allocations cannot be deferred
  after the drain.
- Real-game G1 findings: the load's own scenario reset must not close the load
  window (`0x67E739` inside `LoadGame` - the trailer rows could not be adopted,
  `0 adopted / 44 failed`), and the save stream cannot `Seek` (the trailer was
  not appended at all).  Both fixed and re-verified in the game.
- C-01: unregistered event numbers above the vanilla table range could make the
  multiplayer decoder copy up to 232 bytes into a 104-byte stack buffer
  (return-address overwrite with a hostile/mismatched peer).
- D-01: the logger's file I/O had no automated test while the docs claimed it
  was verified; `tests/test_logging.cpp` now drives `Log::OpenAt`, the separator
  header, filtering, line format, repeated open/close, failed open and
  NULL/`%` format strings.
- D-09: re-running `fetch_yrpp.sh` could delete the installed `external/YRpp`
  and then fail (data loss); the script now refuses self-copies and installs
  atomically.
- D-10/D-11: an incremental build could silently keep a stale DLL after editing
  `include/`, project files or link arguments when the work tree was dirty.
- D-15: `verify_build.py` accepted a build with the version resource removed
  (the C++ ASCII literal satisfied the old "either encoding" check) and its
  numeric fallback compared against a wrong constant; both fixed.
- Documentation/comments: stale `VC_SAVEGAME_ID` "unused" note, "Phobos hook
  name" clarification at `0x668BF0`, dead store-descriptor `count` field comment,
  `0x2F`/`0x00-0x2F` event-range wording that should be `0x00-0x2E`.

### Security

- **A-01**: a corrupt/drifted payload could delete extensions whose swizzle slots
  were already registered, letting the engine write through freed memory.  The
  whole payload is now validated before the engine's swizzle manager is touched;
  on failure staged registrations are discarded before the partial instances are
  deleted.
- **A-02**: savegame-controlled size fields allocated unbounded memory (a crafted
  save could terminate the 32-bit process); every size is now range-checked
  before allocation and capped at 16 MiB.
- **A-05**: skipping an unknown store block could leave saved cross-store
  pointers unmapped and trap the engine (division-by-zero); the v2 store-set
  signature rejects the whole payload instead.
- **C-01**: a hostile/mismatched multiplayer peer could control the event
  decoder's copy length above the vanilla table range and overwrite the return
  address; the size hooks are now fail-closed above `0x2E`.
- **D-09**: the YRpp fetch helper could destroy a required build input.
- **S-01**: `--commit` could override a detected git `HEAD`, letting a wrong tree
  pass the pin gate as long as its commit string matched; only `.git`-less source
  trees are accepted through `--commit`, with a warning.

### Known limitations

- The savegame order contract is unchanged in nature: the reader assumes every
  known platform's sequential reader runs before ours (all known modules,
  including the Mix fork's `Phobos.dll`, sort before `VentureCore.dll`), and no
  platform appends after `0x67D1B4`.  A platform outside that order degrades
  gracefully (extension state empty) but is not supported.
- `v0.2.0-experimental` saves keep working for the game itself; their extension
  state is dropped once (payload v1 rejected, no migration).
- The real-game E1 (Ares present) and E6 (failed load -> new scenario) checks
  passed on 2026-11-10; E2 (T0, no Ares) could not be executed on this machine
  because SyringeEx does not load the plugin without Ares there.
- Host-specific (T1-T3) features, Phobos interop and AE are still not
  implemented.
- The remaining advisory (non-blocking) Batch-B hardening items - command
  namespace checks, INI takeover deny-list, ABI-probe locking/coverage,
  benchmark optimisation flags, empty-`tests/` guard - are not part of this
  release.

## [0.2.0-experimental] - 2026-11-10

Base-completion release.  The empty skeleton from 0.1.0 gained the platform
layers a feature pack actually needs.  The layers are covered by automated checks
on this machine (listed per layer in `README.md` / `docs/`); those checks do not
replace an in-game run.  Still **no gameplay features**: installing the DLL does
not change the game.

### Added

- **Extension hierarchy** (`src/Ext/ExtHierarchy.h`, `src/Ext/AbstractExt.{h,cpp}`):
  `AbstractExt -> TechnoExt -> FootExt -> UnitExt` with runtime type ids, so the
  migrated feature code can keep using `UnitExt::Fetch(unit)` /
  `TechnoExt::Fetch(techno)`; one most-derived instance per owner object, a second
  store claiming the same owner fails cleanly.
- **Savegame tail block** (`src/Core/SaveTrailer.{h,cpp}`,
  `src/Ext/ExtSave.{h,cpp}`, `src/Ext/ExtStream.h`, `src/Ext/ExtSwizzle.h`,
  `src/Engine/Hooks.Save.cpp`): extension state is appended after every other
  platform's block, owner pointers relinked through the engine's swizzle pass,
  per-store data versions; `docs/SaveFormat.md`.
  *Correction (0.3.0): the 0.2.0 trailer was anchored at the end of the stream
  and read back by seeking; the engine's savegame stream does not implement
  `Seek`, so in a real game the block was never written or read.  0.3.0 replaces
  it with a header-first strictly sequential frame at a non-Ares write site.*
- **Rules/art INI layer** (`src/Core/EngineIni.{h,cpp}`,
  `src/Core/IniTakeover.{h,cpp}`, `src/Engine/Hooks.Ini.cpp`): typed reads over the
  engine-parsed INI files, rules-INI observers, and the rules-INI takeover that
  removes platform-owned keys from the parsed rules file before a host platform can
  read them (`[Platform] IniTakeover`, default on); `docs/Ini.md`.
- **Event channel** (`src/Event/EventChannel.{h,cpp}`,
  `src/Engine/Hooks.Events.cpp`): platform event types `0x70-0x7F` with fixed
  payloads, a responder registry, the engine's 111-byte `EventClass` record as
  the in-process event record (the network encoder sends type + payload only),
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
  script, a Wine-toolchain flock shared by the build, the tests, the benchmark and
  the load probe, and a generated git-stamp header helper so tests compile the
  platform sources directly.  (The ABI probe does not take that lock.)

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
- Savegame persistence applies to stores that opt in (`SetSaveable`); the
  trailer order contract assumes no platform DLL reads the stream after this one
  (its name sorts after `VentureCore.dll`) and no writer appends after the
  engine's object-state writer.
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
