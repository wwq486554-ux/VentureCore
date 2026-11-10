# Logging

The platform writes one append-only log file next to `rules.ini`:
`<Platform>.log` (`VentureCore.log` for the sample build).  This document covers
the format, the configuration keys, the cost model behind the level filter and
sampling, and the automated source assertion that keeps a RELEASE build from
talking to a debugger.

Source of truth: `src/Core/LogPolicy.h` (pure level/filter/sampling rules),
`src/Core/Log.h` / `src/Core/Log.cpp` (the file logger), the configuration reads
in `src/Core/Platform.cpp`, and `scripts/verify_build.py` check 7.

## Line format

```text
[23:59:59.123] [INFO ] hosts   : Ares, Phobos (tier T2)
```

`[local time to the millisecond] [level] message`.  The label width is fixed
(`DEBUG`, `INFO `, `WARN `, `ERROR`) so grep/awk on the column keeps working.
Levels are `debug < info < warn < error`; a level is written when it is at least
the configured minimum.

## The per-process separator header

Every `Log::Open()` writes one header line first, before any of the historic
startup lines:

```text
=== VentureCore v0.2.0.0-experimental (RELEASE) process 1234 started 2026-11-09 23:59:59 ===
```

It carries the platform name, the full version display string, the build type
(`RELEASE` / `NIGHTLY` / `LOCAL`), the process id and the local date/time, so one
log that accumulated several game runs can be split with a single grep:

```bash
grep -n '^===' VentureCore.log
```

The header is written directly to the file, independent of the level filter, and
the file is still opened in append mode - the header is a separator, not a
truncation.  The lines below it keep their historic shape (`VentureCore ...`,
`git     :`, `hosts   :`, `startup complete`, ...), so users can keep the greps
they already have.  The bare `====` line that used to open the startup block is
gone; its closing counterpart is unchanged.

## Levels: `[Platform] LogLevel`

```ini
[Platform]
LogLevel=info
```

| Value | Meaning |
| --- | --- |
| `error` | errors only |
| `warn` | errors and warnings |
| `info` | **default** - errors, warnings and informational lines |
| `debug` | everything, including per-object diagnostics |

The value is case-insensitive, and numeric `0`-`3` (0 = debug, 1 = info,
2 = warn, 3 = error) is accepted as an alternative spelling.  A missing key means
`info`; an unrecognised value keeps `info` and writes a warning line:

```text
[23:59:59.500] [WARN ] log     : unrecognised LogLevel 'verbose' - using info
```

The resolved level is visible in the startup block:

```text
[23:59:59.501] [INFO ] log     : level info
```

The version/git/YRpp/INI lines are written *before* the key can be read (the INI
has to be located first), so they are always present even when `LogLevel=error`;
from the `log     :` line on, the configured filter applies.

`VerboseLogging=yes` is kept as the **legacy alias for `LogLevel=debug`**: it
still works exactly as before, and when both keys are configured
`VerboseLogging=yes` wins.  New configurations should use `LogLevel`.

## Debugger output: `[Platform] DebugOutput`

A RELEASE build **does not** write to an attached debugger by default:

```ini
[Platform]
; Leave no for release; yes only while debugging with a debugger attached.
DebugOutput=no
```

* `DebugOutput=no` (default): every line still goes to the log file; the
  debugger window stays silent.  This is what a shipped DLL runs with.
* `DebugOutput=yes`: every line that passes the level filter is also mirrored to
  the debugger (the Win32 debug-output API) - for debugging with a debugger
  attached, not for normal play.
* A **DEBUG build** always mirrors lines to the debugger; the switch is
  irrelevant there (the call is compiled in).

The two "file logging disabled" fallback messages (the game directory cannot be
resolved / the log file cannot be opened) go through the same one gated helper:
if the log file cannot be opened in a RELEASE build, the platform stays quiet
instead of writing to a debugger nobody asked for.

### Why

In the base's own measurements the unconditional debugger write dominated the
cost of a log line: formatting and the file append were secondary.  Every line
crossed into the debugger/syscall path even when no debugger was attached.  In a
big battle a per-unit log line therefore cost far more than the file write it
ended in.  The RELEASE build no longer calls the debug API at all unless
`DebugOutput=yes` was explicitly configured.

## Cheap logging in hot paths

The cost order in `WriteV()` is deliberate:

1. compare the level against the configured minimum - the only cost of a
   suppressed line;
2. only then format the message (`_vsnprintf_s`) and build the line;
3. append the line to the file (one `WriteFile`);
4. mirror to the debugger only in a DEBUG build or with `DebugOutput=yes`.

Feature code should therefore log freely at `debug` for per-object detail: with
the default `info` level those calls are a level comparison, nothing more, and
they become available for diagnosis by setting `LogLevel=debug`.

For diagnostics that are worth seeing at the default level but would flood a
large battle, use the sampling helper from `src/Core/LogPolicy.h`:

```cpp
#include "Core/LogPolicy.h"

if (ShouldLogSample(counter, kEvery))
    Log::Info("[Feature] progress: %u ...", counter);

// true for counter = 0, every, 2*every, ...  (a counter of 0 means a wrapped
// unsigned counter did not lose its slot)
// every == 0 disables throttling: every call reports (logging without sampling)
```

### The Example module

`src/Features/Example/ExampleFeature.cpp` demonstrates the split:

* the per-unit `UnitClass` created/destroyed transitions are `Log::Debug`
  lines - **the default `info` log shows none of them**;
* every 100th unit (`kProgressLogEvery = 100`) also writes one `Info` progress
  line, for creations and destructions separately:

  ```text
  [01:02:03.004] [INFO ] [Example] progress: 100 created (live=100)
  ```

* when the last tracked unit is gone (scenario teardown, or the end of a small
  battle) one `Info` summary line records the totals:

  ```text
  [01:02:03.005] [INFO ] [Example] no units left (created=83, destroyed=83)
  ```

That is at most a few lines per scenario instead of two lines per unit, while a
`LogLevel=debug` run still has the full per-object trail.  `scripts/linux/load_probe.sh` and
`scripts/verify_build.py` verify the build side; the smoke-test result is proven
by simply playing a battle and grepping the log for `[Example]`.

## Release-source assertion (`verify_build.py` check 7)

`python3 scripts/verify_build.py` runs a seventh check, `Log policy (source)`,
which scans every `src/**/*.cpp` and `src/**/*.h` file and fails if a call to the
debugger API appears outside a `#ifdef DEBUG` / `#if defined(DEBUG)` region.
It reports `file:line: <source line>` for each violation, for example:

```text
[FAIL] Log policy (source)
       1 call(s) to OutputDebugStringA outside a #ifdef DEBUG region (14 file(s) scanned)
       src/Core/Foo.cpp:42: 	OutputDebugStringA("cheap diagnostic\n");
       Move the call into the DEBUG-gated helper in src/Core/Log.cpp
       (or under #ifdef DEBUG); see docs/Logging.md.
```

The check is line-based and documented on purpose - it tracks the preprocessor
conditional nesting, no C++ parsing.  Two consequences:

* it inspects the **source tree**, not the DLL, so it also catches a violation
  when the DLL in `Release/` was built from an older state (the check is printed
  even when the DLL cannot be found);
* an `#else`/`#elif` branch of an `#ifdef DEBUG` region counts as gated.  That
  is deliberate: the single helper in `src/Core/Log.cpp` uses exactly that shape
  for its `#else` branch, which is additionally gated on the run-time
  `DebugOutput` switch.  Do not add another debugger call outside that helper.

## The logger cannot crash the game

Every write is best-effort: a failed `WriteFile` returns a value that is ignored,
file I/O failure never throws and never exits, and a missing/undetectable game
directory only disables file logging (the gated debugger helper is the fallback).
The DLL is compiled with `/EHsc-`, so there are no exceptions to escape from the
logger in the first place.

## Verifying a build

```bash
scripts/linux/build_linux.sh Release --build-type RELEASE --clean   # 0 warnings
python3 scripts/verify_build.py                                     # 7/7 checks
scripts/linux/run_tests.sh log_levels                               # pure policy test
```

In-game, check the first line of `<Platform>.log` for the separator header, the
`log     : level ...` line, and that a battle at the default `info` level keeps
the `[Example] progress:` lines sparse.

## See also

* `docs/Lineage.md` - the startup-time lineage check logs its result in the same
  startup block.
* `docs/HookCatalogue.md` - logging introduces no engine hooks.
* `src/Core/LogPolicy.h`, `src/Core/Log.h`, `src/Core/Log.cpp`,
  `src/Features/Example/ExampleFeature.cpp`, `scripts/verify_build.py`.
