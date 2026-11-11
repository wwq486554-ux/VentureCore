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
| `common.sh` | shared settings and helpers, sourced by the others |
| `setup_wineprefix.sh` | creates/validates the Wine prefix (run once) |
| `check_eol.py` | line-ending check (`--fix` to normalise) |
| `check_yrpp.py` | verifies `external/YRpp` against the pin in `src/Core/EnginePin.h` |
| `fetch_yrpp.sh` | copies a local YRpp checkout into `external/YRpp` and stamps its commit (atomic; refuses self-copies) |
| `test_fetch_yrpp.sh` | regression tests for `fetch_yrpp.sh` (self-copy guard, atomic install, git-HEAD pin gate; no Wine needed) |
| `verify_incremental.sh` | regression test for the incremental build: edits `include/`, `src/`, project files and linker flags in a throwaway copy and asserts the build reacts (real toolchain) |
| `abi_probe.sh` | prints the engine ABI values pinned by `src/Engine/EngineCompat.h` |
| `load_probe.sh` | loads the built DLL into a tiny Win32 host under Wine and resolves exports |
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

## Caveats

* `fetch_yrpp.sh` installs through a staging directory and only swaps it in after
  a complete copy, so an interrupted fetch cannot destroy an existing
  `external/YRpp`.  It also refuses to copy the installed tree onto itself:
  with no arguments that is a no-op ("already installed"), an explicit
  `--from external/YRpp` is an error.
* Do not run two builds at the same time: they share one Wine prefix.
* The Wine prefix must live on a **Linux-native, writable** filesystem; NTFS/FUSE
  or read-only mounts fail deep inside `cl.exe` with `error D8037` / `D8050`.
* Build output goes to `Release/` (gitignored). The Linux-side build state lives
  in `Release/IntDir/linux/` and can be dropped with `--clean`.
* The project file is the single source of truth for what is compiled: a
  `src/**/*.cpp` that is missing from `VentureCore.vcxproj` is not compiled, and
  `build_linux.sh` warns about every such file instead of silently skipping it.
* The game directory is not deployed to by default; `--deploy DIR` exists for a
  writable installation. A read-only game directory makes that step fail.
