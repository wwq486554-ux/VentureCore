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
3. Install it into `external/YRpp` and update the stamp:

   ```bash
   scripts/linux/fetch_yrpp.sh --from /path/to/YRpp
   ```

   The source must normally be a git checkout whose `HEAD` is the new pin.
   `--commit SHA` is only accepted for a source tree without `.git`; its
   content cannot be verified, so the script warns about that.  Re-running the
   script without arguments when `external/YRpp` is already at the pin is a
   no-op, and the script refuses to use `external/YRpp` as its own source.

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

7. Clean build and verify:

   ```bash
   scripts/linux/build_linux.sh Release --build-type RELEASE --clean
   python3 scripts/verify_build.py
   scripts/linux/load_probe.sh
   ```

8. Record the review in `docs/EngineBindings.md` (pin value and any ABI changes)
   and add a `CHANGELOG.md` entry if behaviour changed.

The build script itself runs `check_yrpp.py` before compiling, so a stale
`external/YRpp` cannot silently produce a binary: it aborts with the expected and
actual revisions.  The compile-time gate in `EngineCompat.h` catches layout drift,
and the runtime log plus the `YrppCommit` version-resource string record what was
built.

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
* `VC_PRERELEASE_SUFFIX` - `"experimental"` for sample builds; comment it out for a
  stable release (the version resource then drops the `VS_FF_PRERELEASE` flag).

Everything else is derived: the log banner (`v0.3.0.0-experimental`), the version
resource strings, the build-type string (`LOCAL` / `NIGHTLY` / `RELEASE`, set by
`--build-type`).  Do not edit the generated strings by hand.

Checklist for a version bump:

1. Bump the numbers (and optionally change/remove the prerelease suffix).
2. Add a `CHANGELOG.md` section for the new version, dated.
3. Rebuild with `--build-type RELEASE` for a release artefact and run
   `python3 scripts/verify_build.py` (it expects the `VERSION_STRINGS` at the top
   of that file - currently `0.3.0.0` / `0.3.0-experimental`; update them in the
   same change as the version bump).
4. Run `scripts/linux/load_probe.sh`.
5. Verify the version resource of the produced DLL, e.g.
   `winedump -j resource Release/VentureCore.dll` (or the Windows Explorer file
   properties), and confirm `PlatformBuildType=RELEASE`.

## 4. Updating feature switches and INI schema

* A feature's INI switch lives under `[Features]`, keyed by its
  `FeatureDescriptor.iniKey`; its internal id is not the switch key.
* New switches default to **on** (`VC_DEFAULT_FEATURE_ENABLED`), so a platform
  keeps working when a user drops in a new DLL without editing the INI.
* Do not rename an existing `iniKey` without a migration note in
  `CHANGELOG.md`: old user INIs would silently fall back to the default.
* `[Platform]` currently has five keys (see `README.md` / `VentureCore.ini.example`):
  `LogLevel`, the legacy `VerboseLogging` alias, `DebugOutput`, `LineagePolicy`
  and `IniTakeover`.
* Keep `VentureCore.ini.example` and `docs/MakeYourOwnPlatform.md` in sync with
  any schema change.

## 5. Updating host detection

When an optional host changes its module name, or a new host is added:

1. Edit the `g_hosts[]` table in `src/Core/HostDetect.cpp`.
2. Update the `Host` enum in `src/Core/HostDetect.h` and, if the tier mapping
   changes, `ResolveTier()` and the `Tier` enum in `include/platform/Feature.h`.
3. Update `docs/HostMatrix.md` and any feature that gates on `minTier`.
4. Add a run-time check to the in-game test: the startup line
   `hosts : ... (tier Tn)` must list the expected hosts.

## 6. Repository hygiene

* Line endings: `.gitattributes` declares CRLF for text files and LF for
  `*.sh` / `*.py`.  Before committing, run
  `scripts/linux/check_eol.py` (report) or `scripts/linux/check_eol.py --fix`
  (rewrite offending files).
* `external/YRpp/` and `external/YRpp.commit` are gitignored - never commit
  YRpp, it is an external dependency without a license declaration (see
  `NOTICE.md`).
* Build output (`Release/`, `Debug/`, `*.pdb`, `*.obj`, ...) is gitignored; do
  not commit artefacts.
* The build generates `Release/IntDir/Generated/VentureCore.Git.h` for both the
  MSBuild and the Linux path; it is generated, not authored.

## 7. Planned increments (roadmap)

The implemented base layers as of `v0.3.0-experimental` are: savegame persistence
(header-first sequential frame, payload v2), event channel (fail-closed sizing),
command registration, lineage detection and logging control - each with its own
document in `docs/`.  Every layer has automated checks on this machine (named in
`README.md` and the per-layer document), but those checks load the DLL or drive
engine-free code; they do not replace an in-game run.  The remaining increments are
**T1-T3 host-specific features**, **Phobos interop** and **AE**; none of these
exist in `v0.3.0-experimental`.  When one lands, this document gains the sections
it needs (host-specific hook selection and AE handling in particular must be
written down before the first release that ships them).

When you change a saveable store's **row layout** (a field added, removed or
reordered), bump that store's `dataVersion` in its `SetSaveable(...)` call and
update `docs/SaveFormat.md`; the row canary and the payload signature both cover
it, so old saves drop their extension state instead of misreading the rows.
Adding, removing or reordering a saveable store is also a format change because
the payload signature covers the store set.  The frame format version
(`src/Core/SaveTrailer.h`) and the payload version (`src/Ext/ExtSave.h`) change
only when the on-stream layout itself changes.
