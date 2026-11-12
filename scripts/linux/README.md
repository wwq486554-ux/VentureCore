# scripts/linux - building VentureCore on Linux with the real MSVC toolchain

These scripts drive the **real MSVC compiler / linker / resource compiler**
installed on the mounted Windows volume, running them under Wine. Nothing is
downloaded and no substitute compiler is introduced, so the resulting
`VentureCore.dll` is built by the same toolset as the Windows build.

They were adapted from an earlier build setup, which is why the `VCS_*`
variable names are unusual for a template.

## Scripts

| Script | Purpose |
| --- | --- |
| `build_linux.sh` | main build entry point (see the header of the script for all options) |
| `common.sh` | shared settings and helpers, sourced by the others; holds the single compiler-flag definition (`vcs_cl_flags`) |
| `setup_wineprefix.sh` | creates/validates the Wine prefix (run once) |
| `check_eol.py` | line-ending check (`--fix` to normalise) |
| `check_yrpp.py` | verifies `external/YRpp` against the pin in `src/Core/EnginePin.h` **and** the committed content manifest `external/YRpp.sha256` (rejects a dirty checkout) |
| `fetch_yrpp.sh` | copies a local YRpp checkout into `external/YRpp`, verifies the staged copy against the content manifest, then stamps its commit (atomic; refuses self-copies, dirty sources and unknown content; `--write-manifest` accepts a new content set) |
| `test_fetch_yrpp.sh` | regression tests for `fetch_yrpp.sh` / `check_yrpp.py` (pin + content gates, atomic install, dirty/self-copy guards; no Wine needed) |
| `verify_incremental.sh` | regression test for the incremental build: edits `include/`, `src/`, project files and linker flags in a throwaway copy and asserts the build reacts (real toolchain) |
| `abi_probe.sh` | prints the engine ABI values pinned by `src/Engine/EngineCompat.h` |
| `load_probe.sh` | loads the built DLL into a tiny Win32 host under Wine and resolves exports |
| `probe_hosts.sh` | builds the stub modules for both host phases (source-level registration and the `VentureHost_GetInfo` marker, incl. malformed markers) and verifies detection, formatting, discovery and feature gating under Wine |
| `run_tests.sh` | compiles and runs every `tests/test_*.cpp` (a missing/empty `tests/` is a failure; each test runs with a time limit) |
| `bench_store.sh` | builds and runs the extension-store microbenchmark (`tests/bench_store.cpp`) |

## Build

```bash
cd VentureCore
scripts/linux/setup_wineprefix.sh     # once
scripts/linux/fetch_yrpp.sh           # once per checkout (or use a submodule); no-op when installed
scripts/linux/build_linux.sh Release --build-type RELEASE
```

Success is: exit code 0, a line containing `[linux-build] output:`, and
`file Release/VentureCore.dll` reporting `PE32 ... Intel i386`.

The build prints a machine-readable diagnostics summary
(`[linux-build] compiler diagnostics: 0 warning(s) in N compiled translation unit(s)`)
and writes `Release/IntDir/linux/log/build-summary.log`, a transcript with the
configuration, flag hash, toolchain, optional-source count and warning totals.
`VCS_WARNINGS_AS_ERRORS=1` additionally turns any compiler warning into a build
failure (`/WX` plus a final assertion).  This is what makes the "0 warnings"
release criterion a machine check instead of an observation (TOOL-07).

## Build configuration (one definition, two build systems)

All compiler options for **every** Linux-side cl.exe invocation - the DLL build,
`run_tests.sh`, `bench_store.sh` and the ABI/load/host probes - come from one
function: `vcs_cl_flags` in `common.sh`.  The unit tests and the probes are
therefore compiled with the *release* code-generation flags of the shipped DLL
(`/O2 /Oy /GS- /GR- /Zp8 /permissive-` ...), not with a separate debug profile.
`vcs_assert_flag_parity` fails a run if any profile ever diverges from the build
profile in anything but the documented debug-info format (`/Z7` vs `/Zi`), and
every script prints the resulting flag hash.  A build-only change can no longer
make the test evidence describe a different binary than the one that ships
(TOOL-01).  `scripts/linux/build_linux.sh Release --build-type RELEASE --check`
runs the static cross-checks only (project files vs. the Linux configuration,
flag parity) without Wine.

C++ exceptions are **disabled** for the DLL and for everything else that
compiles platform code: no `/EH` option is passed (MSBuild's
`ExceptionHandling=false` also emits no switch), `_HAS_EXCEPTIONS=0` is defined,
and the force-included `include/platform/BuildGuards.h` fails the compile if
`_CPPUNWIND` ever becomes defined or `_HAS_EXCEPTIONS` is not 0.  Release builds
also define `NDEBUG` (both build systems), so live `assert()`s are compiled out
of the shipped DLL (TOOL-02/TOOL-03).

### Intentional differences from the MSBuild build

The Linux command line is *not* a verbatim copy of the MSBuild invocation.  The
remaining differences are deliberate and listed here; everything else is meant
to be identical and is checked by `--check` where a static comparison is
possible (TOOL-08):

| Difference | Why |
| --- | --- |
| `/Z7` instead of MSBuild's `/Zi` (`DebugInformationFormat=ProgramDatabase`) | one self-contained object file per TU is robust under Wine; selectable with `VCS_DEBUG_INFO=zi` |
| link `/DEBUG:FULL` always | makes a PDB available for local profiling; `VentureCore.props` does not set `GenerateDebugInformation`, and whether MSBuild emits a PDB in Release cannot be verified on this host |
| `/D_WINDLL` | not emitted by MSBuild; kept because the DLL already builds with it and it only affects SDK headers, not the platform's own code |
| `/diagnostics:column`, `/FS`, `/external:W4`, `/analyze-` | compiler-driver/diagnostic settings with no code-generation effect |
| no `/FC` | `/FC` made `__FILE__` and the PDB absolute (`Z:\...` build-machine paths); without it the repo-relative source path is recorded, and the build passes repo-relative paths for that reason (TOOL-03) |
| no `/openmp` | no source uses `_OPENMP` and the shipped DLL never imported vcomp/libomp |

Warning level (`/W4`), `/MT`, `/Zp8`, `/GS-`, `/GR-`, `/Gz`, `/O2 /Oy`, the
definitions and the libraries are identical on both build systems.

## Environment

All settings can be overridden from the environment; the defaults are right for
this machine.

| Variable | Meaning | Default |
| --- | --- | --- |
| `VCS_WINEPREFIX` | Wine prefix used for the MSVC tools | `$HOME/.cache/venturecore-wine`, or `<repo>/../.wine-venturecore` when `$HOME` is not writable |
| `VCS_MSVC_VER` | MSVC toolset version | `14.44.35207` |
| `VCS_SDK_VER` | Windows SDK version | newest installed |
| `VCS_WINDOWS_ROOT` | root of the mounted Windows volume | autodetected |
| `VCS_JOBS` | parallel compiler invocations | `nproc` |
| `VCS_DEBUG_INFO` | `z7` (robust under Wine) or `zi` (MSBuild default) | `z7` |
| `VCS_MODULES` | `1` also passes `/experimental:module` | `0` |
| `VCS_DEPLOY_DIR` | default `--deploy` target | unset |
| `VCS_TEST_TIMEOUT` | time limit per test binary in `run_tests.sh`, seconds | `120` |
| `VCS_COMPILE_TIMEOUT` | time limit for one cl.exe compile / smoke test, seconds | `600` |
| `VCS_LINK_TIMEOUT` | time limit for one link.exe / rc.exe run, seconds | `600` |
| `VCS_PROBE_TIMEOUT` | time limit for one probe binary under Wine, seconds | `300` |
| `VCS_SETUP_TIMEOUT` | time limit for wineboot in `setup_wineprefix.sh`, seconds | `1200` |
| `VCS_WARNINGS_AS_ERRORS` | `1` makes cl.exe warnings fatal (`/WX`) | `0` |

## Caveats

* `fetch_yrpp.sh` installs through a staging directory and only swaps it in after
  a complete copy **that matches the committed content manifest**
  `external/YRpp.sha256`, so an interrupted fetch cannot destroy an existing
  `external/YRpp` and a non-git source tree is no longer installed on trust.  It
  also refuses to copy the installed tree onto itself: with no arguments that is
  a no-op ("already installed"), an explicit `--from external/YRpp` is an error.
  When the pin is intentionally updated, `--write-manifest` rewrites the
  manifest as the explicit acceptance step (commit it together with the pin).
* Do not run two builds at the same time: they share one Wine prefix.  The
  build, the test runner, the benchmark, the probes *and* `setup_wineprefix.sh`
  take one lock file inside the prefix (fixed path, `VCS_WINEPREFIX`), and every
  Wine invocation has a wall-clock limit (`VCS_*_TIMEOUT`), so a hung tool
  cannot block the next run for an hour.
* `run_tests.sh` only accepts a test as passed when its output contains the
  suite's completion marker (`PASS (0 failures)` / `test_<name>: all checks
  passed`) **and** at least one `ok ` check line, in addition to exit code 0.
  A test whose `Check()` calls were deleted is reported as
  `FAIL (no ok check lines)`, and tests that print no marker fail as well -
  keep the existing `ok    <what>` / `PASS (0 failures)` convention when
  adding a test.
* The Wine prefix must live on a **Linux-native, writable** filesystem; NTFS/FUSE
  or read-only mounts fail deep inside `cl.exe` with `error D8037` / `D8050`.
* Build output goes to `Release/` (gitignored). The Linux-side build state lives
  in `Release/IntDir/linux/` and can be dropped with `--clean`.
* The project file is the single source of truth for what is compiled: a
  `src/**/*.cpp` that is missing from `VentureCore.vcxproj` is not compiled, and
  `build_linux.sh` warns about every such file instead of silently skipping it.
* The game directory is not deployed to by default; `--deploy DIR` exists for a
  writable installation. A read-only game directory makes that step fail.
