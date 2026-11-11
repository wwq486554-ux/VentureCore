# VentureCore

VentureCore is an MIT-licensed **starter base** for building your own platform DLL
for *Command & Conquer: Yuri's Revenge* on top of **SyringeEx**.  You copy the
repository, change the platform identity, add feature modules under
`src/Features/`, and compile a single platform DLL of your own - without
modifying Ares, Phobos or Kratos, and without re-implementing the plumbing every
platform needs (startup, logging, INI, host detection, feature switches,
extension storage and persistence, events, commands, the YRpp binding gate, build
scripts).

It is a template, not a mod: it deliberately contains **no gameplay features**.

> **Status: v1.0.0 (stable, 2026-11-11)** - a feature-complete starter base plus two
> Example modules.  The base machinery (startup, logging, config, extension
> storage with savegame persistence, event channel, command registration, lineage
> detection, host detection, the YRpp binding gate and the verification tooling)
> is implemented, in-game verified, and was hardened by an independent blind
> review before this release: every blocking and recommended finding was fixed
> (`CHANGELOG.md`).  The automated checks for each layer are named in this README
> and in `docs/`; they do not replace an in-game check.

## What this base provides

Everything below is implemented; the automated checks that cover a layer are
named with it (an in-game run is still the only end-to-end check - see
[Verifying a build](#verifying-a-build)).

* **Platform identity and versioning** - `src/Core/PlatformConfig.h` is the single
  header that renames the platform; the DLL, INI, log, savegame id and version
  resource follow it.  `src/version.rc` carries name, version, build type and the
  YRpp pin, and the build stamps in the git commit/ref.
* **Startup pipeline** - the `Venture_ExeRun` hook verifies SyringeEx, opens the
  log, loads `<Platform>.ini`, detects optional hosts, resolves the support tier,
  applies the lineage check and initializes every registered feature.
* **File logging** - `<Platform>.log` next to `rules.ini`, level-filtered
  (`[Platform] LogLevel=error|warn|info|debug`), one per-process separator header,
  no unconditional `OutputDebugString` in release builds; a failed open falls
  back to the gated debugger helper and a failed write is reported once and the
  line dropped.  `tests/test_logging.cpp` covers the file I/O and
  `scripts/verify_build.py`'s source check covers where the debugger API may be
  called; neither is a game run (see `docs/Logging.md`).
* **INI layer** - typed getters over `<Platform>.ini`, typed reads over the
  engine-parsed `rules(md).ini` / `art(md).ini`, and a **rules-INI takeover**: when
  a feature switch is on, the platform-owned keys are removed from the parsed
  rules INI so a host platform loaded next to us (Phobos) cannot read them too
  (see `docs/Ini.md`).
* **Host detection matrix** - optional hosts (Ares, Phobos, Kratos,
  Kaleidoscope) are detected by loaded module, logged, and mapped to support
  tiers T0-T3.  No functional dependency on any of them in this base.
* **Lineage mutual exclusion** - the DLL exports a lineage marker; a second DLL of
  the same lineage is detected at startup, named in the log and (by default)
  refused, so the second copy never initializes features and there is no second
  extension table or trailer writer.  A refused copy is **inert**: its command is
  not added to the engine command list, its event types are neither sized nor
  dispatched, the rules-INI takeover purges nothing and the savegame hooks do not
  touch the stream (see `docs/Lineage.md`).
* **Compile-time feature registry** - `FeatureDescriptor`, `FeatureContext`,
  `FeatureRegistrar`, `VENTURE_REGISTER_FEATURE` (`include/platform/Feature.h`);
  one `[Features]` switch per feature (`iniKey`, default on) plus `minTier`
  gating.
* **Extension storage** - an extension hierarchy (`AbstractExt ->
  TechnoExt -> FootExt -> UnitExt`, mirroring the Phobos call shape), one
  most-derived instance per owner object, a typed
  `ExtensionStore<TOwner, TExt>` (O(1) owner map + intrusive allocation-order
  list + last-owner fetch cache) and a type-erased global `ExtensionIndex`;
  scenario-clear and savegame plumbing included.  The pointer-invalidation hook
  is wired (it counts the engine's notifications and drops a stale deferred
  allocation), but there is no subscriber API for feature caches yet - do not
  build cache invalidation on it (see `docs/EngineBindings.md`).
* **Savegame persistence** - extension state is appended to the engine's
  savegame stream as a header-first, strictly sequential frame
  (`[magic VNC1][formatVersion][payloadSize][crc32][payload]`) written after every
  known platform's block; the load side reads it at the end of the ordered
  ecosystem blocks.  The payload carries a signature of the platform's saveable
  store set, so a changed feature set or a bumped store version drops the
  extension state (one-time for `v0.2.0` saves) instead of misreading it; format
  in `docs/SaveFormat.md`.
* **Event channel** - platform-private network event types `0x70-0x7F` with fixed
  payloads, a responder registry, fail-closed sizing for unregistered numbers
  above the engine's `0x00-0x2E` table, and the engine's 111-byte `EventClass`
  record as the in-process event record (see `docs/EventChannel.md`).
* **Command registration** - new player commands with UI name/category/description
  that the player binds in the game's keyboard dialog (see `docs/Commands.md`).
* **Lifecycle hooks** - `UnitClass` construction, both `UnitClass` destruction
  sites, scenario clear and the engine's pointer-invalidation announcement.
* **Example modules** - `example_unit_counter` allocates/removes a `UnitClass`
  extension (an `ExampleUnitExt` derived from `UnitExt`), counts
  created/destroyed/live units, verifies the base-class views, persists a per-unit
  serial through the savegame trailer and logs sparsely; `example_ini_key` reads
  and takes over one `rules(md).ini` key.  Neither has a gameplay effect; they
  demonstrate the whole feature contract and are meant to be copied.
* **Engine binding gate** - YRpp pinned by commit; enforced at build time
  (`scripts/linux/check_yrpp.py`), compile time (`static_assert`s in
  `src/Engine/EngineCompat.h`) and run time (log + version resource).
* **Hook catalogue + static verification** - `docs/HookCatalogue.md` lists every
  hook (20 records, 19 distinct handlers); `scripts/verify_build.py` parses that
  table and verifies a built DLL against it without hard-coding a second copy.
* **Unit tests and a benchmark** - `tests/` with engine-free test programs (run
  with `scripts/linux/run_tests.sh`) and the extension-store microbenchmark
  (`scripts/linux/bench_store.sh`).
* **Build system** - Windows `VentureCore.sln` (VS2022, v143) and Linux scripts
  that drive the real MSVC toolchain under Wine.

## Stability and versioning

`v1.0.0` is the first stable release of the base.  Within 1.x the following are
**stable contracts** (changing one requires a major release or a documented
migration):

* the source-level feature API in `include/platform/` (`FeatureDescriptor`,
  `FeatureContext`, `VENTURE_REGISTER_FEATURE`, and the lineage marker ABI);
* the savegame frame layout `[magic][formatVersion=2][size][crc32][payload]` and
  the payload's store-set signature (bumping one store's `dataVersion` is a
  *store* change, not a base change);
* the event type range `0x70-0x7F` and the event record layout (this is a
  same-build contract - every peer must run a build that agrees with it);
* the hook table in `docs/HookCatalogue.md` for the supported `gamemd.exe` build;
* the `<Platform>.ini` keys documented below.

Changing a contract above requires a major release or a documented migration.

## Requirements

| Requirement | Why |
| --- | --- |
| *Yuri's Revenge* 1.001 with `gamemd.exe` | the host executable the hooks are addressed for; not distributed |
| SyringeEx (`Syringe.exe` in the game directory) | injects the DLL; VentureCore **refuses to run under an older Syringe** (a message box explains this and the game is terminated) |
| YRpp checkout at the pinned commit | build dependency, not vendored; fetched into `external/YRpp/` (see below) |
| Visual Studio 2022 (v143) / MSVC 14.44 toolset | build toolchain (on Linux, run under Wine) |
| Ares / Phobos / Kratos / Kaleidoscope | **optional**; detected and logged only |

The DLL is discovered through its `.syhks00` section: SyringeEx scans the game
directory for DLLs that carry that section and loads them.  No registry edits or
launcher changes are needed.

## Building

### YRpp first (both platforms)

YRpp is not distributed with this repository.  Fetch it into `external/YRpp/`
once per checkout:

```bash
# Linux: copy a local YRpp checkout (the helper also writes external/YRpp.commit)
scripts/linux/fetch_yrpp.sh --from /path/to/YRpp
```

On Windows, clone the YRpp repository and check out the commit named by
`VC_YRPP_PIN` in `src/Core/EnginePin.h` into `external/YRpp` (or run the same
helper from Git Bash).  The build fails if the revision does not match the pin.

### Linux (MSVC under Wine)

```bash
cd VentureCore
scripts/linux/build_linux.sh Release --build-type RELEASE
```

This drives the real MSVC 14.44 toolchain and Windows SDK under Wine (the same
toolset as the Windows build).  A successful build:

* exits with code 0 and prints a line containing `[linux-build] output:`,
* produces `Release/VentureCore.dll` and `Release/VentureCore.pdb`,
* `file Release/VentureCore.dll` reports `PE32 ... Intel i386`.

Useful options: `--clean`, `-v`, `--jobs N`, `--deploy DIR` (copy into a game
directory), `--fix-eol` (normalise line endings first).  `--build-type` selects
the `RELEASE` / `NIGHTLY` preprocessor define; a build without it is a local
build and is labelled `LOCAL` in the log and version resource.

The build, the unit tests, the benchmark, the ABI probe and the load probe share
one Wine toolchain, so they take an flock in the prefix and serialise against
each other (including across git worktrees that share the prefix).

### Windows

Open `VentureCore.sln` in Visual Studio 2022 and build `Release | Win32`.  The
project uses toolset v143 and expects `external/YRpp/` to be populated as
described above.

## Verifying a build

These automatic checks cover this machine; the in-game run is a human step:

```bash
python3 scripts/verify_build.py         # static: PE image, hook table, exports,
                                        # version strings, feature id, log policy
scripts/linux/load_probe.sh             # dynamic: LoadLibrary, exports, lineage conflict
scripts/linux/run_tests.sh              # engine-free unit tests, same toolchain
python3 scripts/linux/check_eol.py      # line endings vs .gitattributes
scripts/linux/abi_probe.sh              # optional: engine ABI sizes pinned by EngineCompat.h
```

The v1.0.0 source passes these checks.  `scripts/linux/bench_store.sh` runs the
extension-store microbenchmark and `scripts/linux/test_fetch_yrpp.sh` /
`scripts/linux/verify_incremental.sh` cover the tooling.  A rebuilt DLL is not
byte-identical to a released one (embedded timestamp / debug path), so the
checks - not a file hash - are the provenance.

`verify_build.py` checks the PE format (PE32/i386), the `.syhks00` hook records
against the catalogue, the distinct hook handler exports, the version strings
(platform name, the version - required both in the UTF-16LE version resource and
as the compiled-in ASCII literal - and the YRpp pin), the registered Example
feature id and the log-policy source hygiene.  It exits 0 only when every check
passes.

`load_probe.sh` performs `LoadLibrary` -> `GetProcAddress` for the base hook
handlers and the lineage markers, validates the marker payload and prints the resolved export count, then loads
a second copy of the DLL under another file
name and expects the conflict counter to become non-zero.  It loads the DLL (its
imports and CRT constructors run) but does **not** run `Startup`, so the engine
hooks, logging, INI and savegame code paths are not exercised by it; an in-game
run is still required for those.

The real test - launching the game with SyringeEx and the DLL installed - must be
done by a human.  See `docs/HookCatalogue.md` for what each hook does and
`docs/HostMatrix.md` for the host combinations.

## Installing

1. Install SyringeEx in the game directory (replacing the old `Syringe.exe`).
2. Copy `Release/VentureCore.dll` next to `gamemd.exe`.
3. Optionally copy `VentureCore.ini.example` to `VentureCore.ini` and edit it.
4. Start the game.  `<Platform>.log` appears next to `rules.ini` and records the
   startup sequence: process header, platform/version, git revision, YRpp pin, INI
   status, log level, hosts and tier, lineage, feature switches, registered
   commands and event types, then `startup complete`.

Upgrading from `0.4.0-experimental` is a drop-in replacement: 1.0.0 contains no
hook, savegame, event, command or INI changes (only the version string and the
documentation were finalised).  Saves written by 0.3.0+ keep their extension
state.

The base has no unload hook; every line is written immediately with a synchronous
`WriteFile` (no user-mode buffering), so a normal process exit does not lose
lines.  There is no `FlushFileBuffers` call: the data sits in the OS file cache,
which a normal exit flushes, and only a power loss / blue screen can drop it.

## Configuration

`VentureCore.ini` (next to `rules.ini`) has two sections:

* `[Platform]` - platform-wide options:
  * `LogLevel=error|warn|info|debug` (default `info`); the legacy
    `VerboseLogging=yes` is an alias for `debug`;
  * `DebugOutput=no|yes` (default `no`; a release build writes to the debugger
    only when enabled);
  * `LineagePolicy=refuse|warn` (default `refuse`: a second DLL of the same
    lineage is detected and the later one stays disabled; `warn` only logs);
  * `IniTakeover=yes|no` (default `yes`: hide platform-owned `rules(md).ini` keys
    from other platforms loaded next to us).
* `[Features]` - one switch per feature, keyed by the feature's
  `FeatureDescriptor.iniKey`, value `yes`/`no`; features default to on.  The
  internal feature id is not the switch key.  A feature with a higher `minTier`
  is disabled when the detected host set does not reach that tier.

Booleans accept `yes/no`, `true/false`, `on/off` and `1/0`.  See
`VentureCore.ini.example`.

## Repository layout

| Path | Contents |
| --- | --- |
| `include/platform/` | public source-level API: `Feature.h`, `Lineage.h` |
| `src/Core/` | identity/version, log, INI, rules-INI takeover, host detection, lineage scan, feature registry, save trailer, startup |
| `src/Engine/` | engine ABI assertions and the base hooks (lifecycle, INI, savegame) |
| `src/Ext/` | extension storage (`AbstractExt`, `ExtHierarchy`, `ExtensionStore`, `ExtensionIndex`, savegame streams) |
| `src/Event/` | the platform event channel |
| `src/Command/` | player-command registry |
| `src/Features/Example/` | the two Example feature modules |
| `src/version.rc` | VERSIONINFO resource |
| `tests/` | engine-free unit tests and the store microbenchmark |
| `docs/` | the documentation set (see below) |
| `scripts/verify_build.py` | static verification of a built DLL |
| `scripts/linux/` | MSVC-under-Wine build, tests, benchmark, YRpp check/fetch, probes |
| `external/` | external dependencies; `YRpp/` is fetched locally and not committed |
| `VentureCore.sln`, `VentureCore.vcxproj`, `VentureCore.props` | Windows build |

## Documentation

| Document | What it covers |
| --- | --- |
| `docs/MakeYourOwnPlatform.md` | step-by-step: turn this base into your own platform |
| `docs/Renaming.md` | rename the platform (DLL/INI/log/version/savegame id) safely |
| `docs/EngineBindings.md` | how the base binds to gamemd / SyringeEx / YRpp; storage, INI, savegame, event, command, lineage and logging layers |
| `docs/HookCatalogue.md` | every installed hook: address, window size, purpose |
| `docs/HostMatrix.md` | required vs optional hosts, detection, support tiers, mixed-client constraints |
| `docs/SaveFormat.md` | the savegame frame: stream layout, payload, load sequence |
| `docs/Ini.md` | rules/art INI reads and the rules-INI takeover |
| `docs/EventChannel.md` | event type range, payload format, responders, mixed-client risk |
| `docs/Commands.md` | registering player commands and binding hotkeys |
| `docs/Lineage.md` | the lineage marker and the mutual-exclusion check |
| `docs/Logging.md` | log levels, debugger output gating, sampling, measured cost |
| `docs/Licensing.md` | MIT base, GPL-3.0 feature packs, derived-platform rules |
| `docs/Updating.md` | updating the YRpp pin, the platform and the version |
| `CHANGELOG.md` | release history |
| `NOTICE.md` | third-party components and their terms |

## Licensing

The base is **MIT** (`LICENSE.md`, `NOTICE.md`).  YRpp is not distributed and has
no license declaration; SyringeEx and the optional host platforms are not
distributed either.  Feature packs under the Venture umbrella are **GPL-3.0**; a
derived platform may pick its own license, but copying GPL-3.0 modules into it
makes the combined work GPL-3.0.  Details:
`docs/Licensing.md`.

## 中文简介

VentureCore 是一个 MIT 授权的“起始基座/模板”：复制仓库 → 改
`src/Core/PlatformConfig.h` 的平台名 → 在 `src/Features/` 下加自己的功能模块 →
编译成你自己的单一平台 DLL，不需要改 Ares / Phobos / Kratos，也不用重复造
启动、日志、INI、宿主检测、功能开关、扩展存储、存档、事件、命令这些基础设施。

当前版本 **`v1.0.0`（首个正式版，2026-11-11）** 是**空基座 + 两个示例模块**：示例只统计
UnitClass 的创建/销毁、读取一个 rules 键，没有任何玩法功能。基座各层均已实现，并经独立盲审
（全部阻塞/建议项已修）与实机验证；每层对应的自动化检查写在本 README 与 `docs/` 里，
这些检查不能替代一次实机运行：
日志分级、`<Platform>.ini`、rules/art INI 读取与同名键接管、宿主矩阵、血统互斥、
扩展存储（继承链 + 存档持久化，顺序帧 + store 集签名）、事件通道 `0x70–0x7F`、
玩家命令注册。指针失效通知目前只有计数/清理，没有订阅 API。
硬依赖只有 `gamemd.exe` + SyringeEx；Ares / Phobos / Kratos / Kaleidoscope 只做检测和日志。
构建与自检：

```bash
scripts/linux/build_linux.sh Release --build-type RELEASE   # Wine 里的真 MSVC
python3 scripts/verify_build.py                            # 静态自检
python3 scripts/linux/check_eol.py                         # 行尾检查
scripts/linux/load_probe.sh                                # Wine 下 LoadLibrary 探针
scripts/linux/run_tests.sh                                 # 单元测试（引擎无关）
scripts/linux/bench_store.sh                               # 扩展存储微基准
```
