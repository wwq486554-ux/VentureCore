# Updating

Three different things can be "updated" in this project: the YRpp pin, the
platform code itself, and the version metadata.  Each has its own procedure.

## 1. Updating the YRpp pin (compatibility review)

The platform compiles against one pinned YRpp revision.  Updating the pin is a
**compatibility review, not a version bump**: engine class layouts, hook windows
and macro behaviour may all change.  The procedure, in order:

1. Get the new YRpp revision (a local checkout of the desired commit).
2. Edit `src/Core/EnginePin.h`:
   * `VC_YRPP_PIN` - full 40-hex commit id,
   * `VC_YRPP_PIN_SHORT` - the 8-character form used in the log,
   * and `VC_YRPP_REPO` / `VC_YRPP_BRANCH` if the source moved.
3. Install it into `external/YRpp`, update the stamp and the content manifest:

   ```bash
   scripts/linux/fetch_yrpp.sh --from /path/to/YRpp --write-manifest
   ```

   The source must normally be a git checkout whose `HEAD` is the new pin; a
   dirty checkout (modified or untracked files) is rejected.  `--commit SHA` is
   only accepted for a source tree without `.git`: the function cannot identify
   the commit, so the staged copy must match the committed content manifest
   instead.  `--write-manifest` is the explicit step that accepts a new content
   set and rewrites `external/YRpp.sha256` (commit the manifest together with
   the pin change).  Without it, an unknown content set is refused.  Re-running
   the script without arguments when `external/YRpp` is already at the pin is a
   no-op (it still verifies the manifest), and the script refuses to use
   `external/YRpp` as its own source.

4. Re-measure the ABI and run the probe:

   ```bash
   scripts/linux/abi_probe.sh
   ```

   Copy the numbers into the `static_assert`s in `src/Engine/EngineCompat.h`
   (add asserts for any newly used type or field).  If a value changed, review
   every use of the affected type before accepting the new number - a resize can
   be harmless or a savegame-corrupting layout change.
5. Review the hook catalogue against the new revision: hook addresses and window
   sizes in `docs/HookCatalogue.md` are tied to the executable build the platform
   supports; if the YRpp update implies a different target build, this becomes a
   separate engine-support change.
6. Run the build-time gate:

   ```bash
   scripts/linux/check_yrpp.py
   ```

   The gate checks the commit id **and** the content: `external/YRpp.sha256`
   lists the sha256 of every file plus the exact file set, so a checkout at the
   right commit but with edited/added/removed files is rejected (a dirty git
   checkout is rejected as well).  The same manifest is verified by
   `fetch_yrpp.sh` before it swaps a staged copy into place.  The Linux build runs
   this gate itself before compiling; the Visual Studio build does not, so run
   the script (or add it as a pre-build step) when you build on Windows.

7. Clean build and verify:

   ```bash
   scripts/linux/build_linux.sh Release --build-type RELEASE --clean
   VCS_GAMEMD_EXE=/path/to/gamemd.exe python3 scripts/verify_build.py
   scripts/linux/load_probe.sh
   ```

   `VCS_GAMEMD_EXE` (or `--gamemd`) is the pinned engine executable for the
   offline hook-window check; without it that check is an explicit SKIP, not a
   PASS.  The verifier also rejects a DLL whose `PlatformBuildType` is not
   `RELEASE` or whose git stamp does not match the working copy, so verify the
   artifact you are about to ship, not an older one.

8. Record the review in `docs/EngineBindings.md` (pin value and any ABI changes)
   and add a `CHANGELOG.md` entry if behaviour changed.

The Linux build script runs `check_yrpp.py` before compiling, so a stale or
edited `external/YRpp` cannot silently produce a binary there: it aborts with the
expected and actual revision plus the changed/missing/unexpected files.  The
Visual Studio build does not run that gate (run the script yourself, see step 6).
The compile-time gate in `EngineCompat.h` catches layout drift, and the runtime
log plus the `YrppCommit` version-resource string record what was built.

## 2. Updating the platform code

Normal development loop:

1. Edit source under `src/` (or `include/platform/` for the public feature API).
2. If you added, removed or moved a **hook**, update `docs/HookCatalogue.md` in the
   same change - `scripts/verify_build.py` compares the built `.syhks00` records
   against that table and fails on any difference.
3. If you added a new source file, add it to `VentureCore.vcxproj` (the Linux
   build derives its file list from the project file, so both builds stay in
   sync).
4. Build and verify:

   ```bash
   scripts/linux/build_linux.sh Release --build-type RELEASE
   python3 scripts/verify_build.py
   scripts/linux/load_probe.sh
   ```

5. Do the in-game test on a machine with a writable game directory: launch the
   game with SyringeEx and the DLL installed, enter a skirmish, leave it, check
   `<Platform>.log` for the startup banner, the feature switch lines, scenario
   clear, and (for the Example module) the created/destroyed/live counters.
6. Update the affected documentation (`README.md`, `docs/*.md`) and
   `CHANGELOG.md`.

## 3. Bumping the version

Version metadata lives in `src/Core/PlatformVersion.h`:

* `VC_VERSION_MAJOR`, `VC_VERSION_MINOR`, `VC_VERSION_REVISION`,
  `VC_VERSION_PATCH`,
* `VC_PRERELEASE_SUFFIX` - a non-stable suffix (`"experimental"`, `"alpha"`, ...);
  **commented out for a stable release** (as in 2.1.0: the display has no suffix
  and the version resource does not set `VS_FF_PRERELEASE`).

Everything else is derived: the log banner (`v2.1.0.0`), the version
resource strings, the build-type string (`LOCAL` / `NIGHTLY` / `RELEASE`, set by
`--build-type`).  Do not edit the generated strings by hand.

Checklist for a version bump:

1. Bump the numbers.  For a non-stable build add `VC_PRERELEASE_SUFFIX`; remove
   it for a stable release.
2. Add a `CHANGELOG.md` section for the new version, dated.
3. Rebuild with `--build-type RELEASE` for a release artefact and verify it with
   `VCS_GAMEMD_EXE=<pinned gamemd.exe> python3 scripts/verify_build.py` (the
   verifier expects the `VERSION_STRINGS` at the top of that file - currently
   `2.1.0.0` / `2.1.0`; update them in the same change as the version bump, and
   pass the engine executable so the offline hook-window check runs instead of
   being skipped).
4. Run `scripts/linux/load_probe.sh`.
5. Verify the version resource of the produced DLL, e.g.
   `winedump -j resource Release/VentureCore.dll` (or the Windows Explorer file
   properties), and confirm `PlatformBuildType=RELEASE`.

## 4. Updating feature switches and INI schema

* A feature's INI switch lives under `[Features]`, keyed by its
  `FeatureDescriptor.iniKey`; its internal id is not the switch key.
* Switches default to **on** unless the descriptor declares
  `defaultEnabled = false`; an explicit user value wins.  The optional
  `descriptor.group` adds a `[Groups] <group>=yes|no` master switch (default on).
* `VC_DEFAULT_FEATURE_ENABLED` (`src/Core/PlatformConfig.h`) is a **historical
  name only in 2.0**: it is still defined so existing derived platforms that
  reference it keep compiling, but nothing reads it any more.  Changing it - or
  setting it to `false` - has **no effect**; the default now comes from
  `FeatureDescriptor::defaultEnabled` (default `true`).  A module that should be
  off unless the user opts in must set `defaultEnabled = false` explicitly.
* A module's own `<Platform>.ini` keys are declared at the read site with
  `FeatureContext::IniBool` / `IniInt` / `IniString` (or the `VENTURE_INI_*`
  macros).  The registry validates them, logs effective values and reports a
  duplicate declaration; do not add new reads through the legacy `IniGet*`
  getters.  The legacy getters still compile and a key read through one is
  recorded as known (the template dump lists it as a lenient/legacy read), but
  the value is not validated: it keeps the lenient parsing rules and a bad value
  does not warn or enter the invalid count.  Declare each key unconditionally in
  `Initialize()` - the `[Features]` switch gates behaviour, not registration.
* Do not rename an existing `iniKey` or feature key without a migration note in
  `CHANGELOG.md`: old user INIs would silently fall back to the default.
* `[Platform]` currently has seven keys (see `README.md` /
  `VentureCore.ini.example`): `LogLevel`, the legacy `VerboseLogging` alias,
  `DebugOutput`, `LineagePolicy`, `IniTakeover`, `IniWarnUnknownKeys` and
  `DumpIniTemplate`.
* Keep `VentureCore.ini.example` and `docs/MakeYourOwnPlatform.md` in sync with
  any schema change.  After a run with `[Platform] DumpIniTemplate=yes`,
  `scripts/compare_ini_template.py <generated> VentureCore.ini.example` reports
  keys that exist in only one of the two files.

## 5. Updating host detection

Host detection is a registry, not a hard-coded table:

1. Built-in hosts live in `src/Core/Hosts.Builtin.cpp` (registration) with their
   public bits in `include/platform/HostIds.h`.  Changing a module name or adding
   a built-in host means editing both, plus `docs/HostMatrix.md`.
2. A derived platform's private hosts are registered in
   `src/Core/Hosts.Custom.cpp` with
   `VENTURE_REGISTER_HOST(uniqueId, bit, "Module.dll", "Display")`.  The bit must
   be >= 1<<3 and must not collide; the first definition wins and a conflict is
   logged at startup.
3. A platform that identifies itself at run time exports the
   `VentureHost_GetInfo` marker from `include/platform/HostMarker.h`; it is
   discovered by the startup scan and gets the lowest free custom bit.  Changing
   the marker layout or the validation rules requires bumping
   `VENTURE_HOST_MARKER_ABI_VERSION` and updating the producer and
   `docs/HostMatrix.md`; the scanner's POD-only SEH wrapper and its length caps
   must stay.
4. Detection is by loaded module only: a host that fails to inject looks exactly
   like one that is not installed.  There is no environment ranking - features
   declare what they need with `requiredHosts` / `anyOfHosts`.
5. Re-check the declared host requirements of every feature when a host's
   presence semantics change.
6. Add a run-time check to the in-game test: the startup line
   `hosts   : Ares, Phobos (2 host(s))` must list the expected hosts, and the
   per-feature `disabled - requires ...` lines must name the missing ones.
7. Run `scripts/linux/probe_hosts.sh` after changing the registry: it builds a
   stub custom host and verifies registration, detection, formatting and the
   `requiredHosts` gate under Wine (no game process needed).

## 6. Repository hygiene

* Line endings: `.gitattributes` declares CRLF for text files and LF for
  `*.sh` / `*.py`.  Before committing, run
  `scripts/linux/check_eol.py` (report) or `scripts/linux/check_eol.py --fix`
  (rewrite offending files).
* `external/YRpp/` and `external/YRpp.commit` are gitignored - never commit
  YRpp, it is an external dependency without a license declaration (see
  `NOTICE.md`).  `external/YRpp.sha256` **is** committed: it is the content
  manifest (file hashes only, no YRpp content) and must be regenerated with
  `fetch_yrpp.sh --write-manifest` whenever the pin changes.
* Build output (`Release/`, `Debug/`, `*.pdb`, `*.obj`, ...) is gitignored; do
  not commit artefacts.
* The build generates `Release/IntDir/Generated/VentureCore.Git.h` for both the
  MSBuild and the Linux path; it is generated, not authored.

## 7. Base scope (what is not included)

The implemented base layers are: savegame persistence (header-first sequential
frame, payload v2), event channel (fail-closed sizing), command registration,
lineage detection and logging control - each with its own document in `docs/`.
Every layer has automated checks on this machine (named in `README.md` and the
per-layer document), but those checks load the DLL or drive engine-free code;
they do not replace an in-game run.

**Host-specific feature packages, Phobos interop and AE are not part of the base.**
If any of them is added, this document must gain the sections it needs first
(host-specific hook selection and AE handling in particular) before a release
ships it.

When you change a saveable store's **row layout** (a field added, removed or
reordered), bump that store's `dataVersion` in its `SetSaveable(...)` call and
update `docs/SaveFormat.md`; the row canary and the payload signature both cover
it, so old saves drop their extension state instead of misreading the rows.
Adding, removing or reordering a saveable store is also a format change because
the payload signature covers the store set.  The frame format version
(`src/Core/SaveTrailer.h`) and the payload version (`src/Ext/ExtSave.h`) change
only when the on-stream layout itself changes.


## 8. Migrating a 1.x platform to 2.0

2.0 is a **breaking** base release: the host model and the platform INI layer
changed, so a derived platform must be updated and everything recompiled against
the new `include/platform/` headers.  Contracts that did **not** change: the
savegame frame/payload, the event channel, the command registry and the hook
table; a 1.x save keeps loading and its extension state is preserved.

1. **Feature API** - remove the old host support-level field and the per-feature
   minimum level from the descriptor/context (the `[2.0.0]` changelog entry
   names the removed API).  Set `requiredHosts` (AND) and `anyOfHosts` (OR)
   instead; `0, 0` means "no host dependency".  Use `FeatureContext::HasHost` /
   `HasHostBit` / `HasHostName` for run-time checks.
2. **Descriptor defaults/groups** - the optional trailing fields `group` and
   `defaultEnabled` may be omitted.  A module that should be off unless the user
   opts in sets `defaultEnabled = false`; a group master switch is
   `[Groups] <group>=yes|no` in `<Platform>.ini`.  The 1.x
   `VC_DEFAULT_FEATURE_ENABLED` macro is **not read** in 2.0 - set
   `defaultEnabled` explicitly instead (see §4).
3. **Host registration** - the built-in table now lives in
   `src/Core/Hosts.Builtin.cpp` with the public bits in
   `include/platform/HostIds.h`.  A private host is registered in
   `src/Core/Hosts.Custom.cpp` with `VENTURE_REGISTER_HOST` and an explicit bit
   `>= 1u<<3`, or an independent platform exports the `VentureHost_GetInfo`
   marker (`include/platform/HostMarker.h`) and is discovered at startup on the
   lowest free custom bit.
4. **Platform INI reads** - replace `IniGetBool/IniGetInt/IniGetString` with
   `IniBool/IniInt/IniString` (or the `VENTURE_INI_*` macros) and pass the
   description/default/range.  The base then validates the value, warns about
   invalid values and unknown keys, and includes the key in the
   `DumpIniTemplate` output.  The legacy getters still compile and record the
   key as known (the template dump lists it as a lenient/legacy read), but they
   bypass validation: the value keeps the lenient parsing rules and a bad value
   does not warn or count as invalid.
5. **New `[Platform]` keys** are optional: `IniWarnUnknownKeys` and
   `DumpIniTemplate`.  Existing user INIs keep working; a mis-spelled key now
   produces a startup warning (that is the point of the registry).
6. **Removed closed-source host** - remove any dependency on its bit or module;
   the base no longer detects it.
7. **Recompile everything** - the `FeatureDescriptor` / `FeatureContext` layouts
   changed, so a stale object file is an ABI mismatch rather than a warning.
8. **Re-run the checklist**: `--clean` build, `VCS_GAMEMD_EXE=<pinned
   gamemd.exe> python3 scripts/verify_build.py`,
   `scripts/linux/run_tests.sh`, `scripts/linux/load_probe.sh`,
   `scripts/linux/probe_hosts.sh`, then the in-game smoke test from your own
   documentation.

## 9. Migrating a 2.0 platform to 2.1

2.1 is a remediation release over 2.0: no savegame or wire-format changes, but one
source-level recompile requirement and a set of INI/host/command diagnostics
changes.  A derived platform should:

1. **Recompile everything** - the feature-API callbacks are pinned `__cdecl`.  A
   callback defined without an explicit `__cdecl` in a translation unit built
   with the project's `/Gz` (stdcall default) now fails to compile instead of
   silently disagreeing with the base about the calling convention.  Keep the
   feature code on `VentureCore.props` or spell `__cdecl` on your own callbacks.
2. **Review the INI behaviour changes** listed in `CHANGELOG.md` (the first
   declaration wins for duplicate keys, the template dumps declared defaults, the
   startup `invalid` count includes switch values, `[Groups]` is always a
   platform-owned section, a group switch is read once per pass, duplicate
   `[Features]` switches are de-duplicated, `BeginFeature`/`EndFeature` must
   pair, legacy getters mark keys known).  Mis-typed keys can now produce more
   startup warnings; existing user INI files keep working.
3. **Update hook-aware tooling**: the table is now 21 records / 20 distinct
   handlers (one diagnostic post-registration command hook, `0x533C3E`); all
   existing addresses and sizes are unchanged.  `docs/HookCatalogue.md` is the
   current table.
4. **Re-run the acceptance checklist** from §2/§8 with
   `VCS_GAMEMD_EXE=<pinned gamemd.exe>`, then the in-game retest list from the
   release notes (normal startup, co-load, save/load, event numbers).
5. **Savegames stay compatible in both directions**: a 2.0 save loads under 2.1
   and a 2.1 save loads under 2.0, extension state preserved.  The 2.1 loader
   additionally rejects structurally impossible payloads (per-store row count,
   per-row data size and payload-size limits).
6. **Scope decisions**: savegames are treated as trusted input (no
   anti-tamper/malicious-save guarantee), and the lineage marker remains a
   VENTURE-lineage internal contract whose scan does not contain crashes inside
   foreign DLLs.  See `docs/SaveFormat.md` and `docs/Lineage.md`.
