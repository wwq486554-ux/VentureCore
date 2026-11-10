# Renaming the Platform

The base ships as `VentureCore`.  A derived platform normally renames it to its own
name so that the DLL, INI, log and version resource are distinct from every other
platform installed in the same game directory.

Renaming is a mechanical checklist, but it touches four places (identity header,
version resource, project/build output name, verification constants) - missing one
leaves a half-renamed platform.  Read `docs/MakeYourOwnPlatform.md` first if you
are starting a new platform from scratch.

## What is derived from what

| Name | Derived from | Notes |
| --- | --- | --- |
| `<Platform>.dll` | `ProjectName` in `VentureCore.vcxproj`, used as `TargetName` by MSBuild and by `scripts/linux/build_linux.sh` | the file name SyringeEx sees in the game directory |
| `<Platform>.ini` | `VC_PLATFORM_NAME` + `VC_PLATFORM_INI_NAME` in `src/Core/PlatformConfig.h` | next to `rules.ini` |
| `<Platform>.log` | `VC_PLATFORM_NAME` + `VC_PLATFORM_LOG_NAME` in `src/Core/PlatformConfig.h` | next to `rules.ini` |
| version resource `ProductName` / `InternalName` / `OriginalFilename` | `VC_PLATFORM_NAME` and `VC_PLATFORM_DLL_NAME` in `src/version.rc` | |
| version resource `CompanyName` / `LegalCopyright` / `FileDescription` | literals in `src/version.rc` | edit by hand |
| log banner name | `VC_PLATFORM_NAME` | `[Platform] <version>` line at startup |
| savegame trailer id | `VC_SAVEGAME_ID` in `src/Core/PlatformConfig.h` | must be unique per platform lineage.  It is part of every save this platform writes: changing it (or the saveable store set) makes older saves' **extension state** incompatible, while the game itself still loads them |

## Checklist

1. **Identity header** - edit `src/Core/PlatformConfig.h`:
   * `VC_PLATFORM_NAME` - human-facing name; it must also be a valid file-name
     fragment (it becomes the INI and log file names),
   * `VC_PLATFORM_DLL_NAME`, `VC_PLATFORM_INI_NAME`, `VC_PLATFORM_LOG_NAME`
     (normally derived automatically; edit only for unusual names),
   * `VC_SAVEGAME_ID` - pick a new unique value for your lineage.
   * Optional: `VC_SECTION_PLATFORM` / `VC_SECTION_FEATURES` if you want different
     INI section names (see the warning below).
2. **Version metadata** - edit `src/Core/PlatformVersion.h` (version numbers,
   prerelease suffix) per `docs/Updating.md`.
3. **Version resource literals** - edit the hand-written strings in
   `src/version.rc`: `CompanyName`, `LegalCopyright`, `FileDescription`.
4. **Output file name** - edit `ProjectName` in `VentureCore.vcxproj` to your
   platform name.  Optionally rename the project files themselves
   (`VentureCore.vcxproj`, `VentureCore.props`, `VentureCore.sln`), but then also
   update the file names used by `scripts/linux/common.sh` and
   `scripts/linux/build_linux.sh` (they read `VentureCore.vcxproj` and generate
   `VentureCore.Git.h`).  Keeping the project file names and only changing
   `ProjectName` is the lower-risk option.
5. **Verification constants** - `scripts/verify_build.py` starts with expected
   values; update them:
   * `PLATFORM_NAME` - your new name,
   * `VERSION_STRINGS` - your version string(s),
   * `YRPP_PIN` (only if you changed the pin),
   * `FEATURE_ID` - the id of your sample feature, or remove that check if you
     delete the Example module.
   The default DLL path in the same file is derived from the repository root and
   the hard-coded file name; update it if you renamed the project output.
6. **Load probe** - `scripts/linux/load_probe.sh` resolves five hook handler names
   by name.  If you renamed the handler functions (or removed the Example module),
   update its `kExports` list and the `resolved exports: N/5` expectation.
7. **Hook handler names** - they are exported symbols (prefix `Venture_`).  You may
   keep the prefix (recommended for derived platforms) or rename all handlers
   consistently.  If you rename them, update `docs/HookCatalogue.md` in the same
   change: `scripts/verify_build.py` takes the expected names from that table.
8. **Documentation** - update `README.md` and the docs that refer to the platform
   name (`docs/MakeYourOwnPlatform.md` is written for the base; your platform
   should have its own copy or an override).  Also update
   `VentureCore.ini.example` if the file name or sections changed.
9. **Build and verify** - build, then:

   ```bash
   python3 scripts/verify_build.py
   scripts/linux/load_probe.sh
   ```

   Install in a test game directory and confirm in `<Platform>.log` that the new
   names are used.

## Do not rename

* the `include/platform/` feature API header names - they are source-level API,
  not a public ABI, but renaming them means touching every feature module;
* `external/YRpp` or its contents - the path is fixed by the build;
* the `.syhks00` / `.syexe00` section names - they are the SyringeEx interface and
  not configurable;
* the feature ids / INI keys of existing features unless you also document the
  migration - users' INI files would silently fall back to defaults.

## Warnings

* Pick a platform name that does not collide with an existing platform (Ares,
  Phobos, Kratos, Kaleidoscope, ...).  Two different DLLs with the same exported
  hook handler names in one game directory are ambiguous for diagnostics, and
  colliding platform names make logs and version resources indistinguishable.
* If you rename the INI section names, every existing user INI stops matching.
  Do it only for a platform that has not shipped yet.
* The version resource and the actual output file name are separate: `ProductName`
  comes from `VC_PLATFORM_DLL_NAME`/`VC_PLATFORM_NAME`, while the file itself is
  named after `ProjectName`.  Set both, or you get a DLL whose properties disagree
  with its file name.
* The check in `verify_build.py` searches for the platform name and the version
  string in the binary; a half-renamed build fails verification (by design).
