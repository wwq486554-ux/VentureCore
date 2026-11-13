# Logging

The platform writes one append-only log file next to `rules.ini`:
`<Platform>.log` (`VentureCore.log` for this base).  This document covers
the format, the configuration keys, the cost model behind the level filter and
sampling, and the automated source assertion that keeps a RELEASE build from
talking to a debugger.

Source of truth: `src/Core/LogPolicy.h` (pure level/filter/sampling rules),
`src/Core/Log.h` / `src/Core/Log.cpp` (the file logger), the configuration reads
in `src/Core/Platform.cpp`, and `scripts/verify_build.py` check 7.  The pure
rules are tested by `tests/test_log_levels.cpp`; the file I/O described below is
tested by `tests/test_logging.cpp`.  Neither test replaces an in-game run.

## Line format

```text
[23:59:59.123] [INFO ] hosts   : Ares, Phobos (2 host(s))
```

`[local time to the millisecond] [level] message`.  The label width is fixed
(`DEBUG`, `INFO `, `WARN `, `ERROR`) so grep/awk on the column keeps working.
Levels are `debug < info < warn < error`; a level is written when it is at least
the configured minimum.

## The per-process separator header

`Log::Open()` resolves `<game directory>\<Platform>.log` and calls
`Log::OpenAt(path)`, which is public so a derived platform - or a test - can
route the log explicitly.  Opening while the logger is already open is a no-op,
so a repeated `Open`/`OpenAt` does not add a second separator.

Every successful open writes one header line first, before any of the historic
startup lines:

```text
=== VentureCore v2.1.0.0 (RELEASE) process 1234 started 2026-11-12 23:59:59 ===
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

## Startup problems before the log is open

SyringeEx installs the DLL and runs its static initializers before the engine
reaches `ExeRun`, so a registration that fails at that point - for example a
command or an event type rejected by the static registrars in
`src/Engine/Hooks.Commands.cpp` - cannot be written to the log file yet:
`Log::Open()` has not run, and a RELEASE build keeps the debugger silent.
**Warning and Error** lines written while the file is closed are not lost: the
logger keeps a bounded, allocation-free startup queue (8 lines; a further line is
counted, not stored) and flushes it right after `Log::Open()` has written the
per-process separator header.  Informational/Debug lines are not queued - they
only describe the normal path - and formatting/filtering happen when the line is
written, so a queued line keeps the level in force at that moment.  If the queue
overflows, one line at flush time reports how many diagnostics were dropped.
`Log::PendingCount()` returns the number of queued lines and
`Log::DiscardPending()` drops them: both are for a test or a derived platform that
wants to inspect/discard its startup diagnostics, while the base itself only calls
`Open()`.

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

The "file logging disabled" fallback messages (the game directory cannot be
resolved / the log file cannot be opened) and the one-time "cannot write" report
go through the same one gated helper: if the log file cannot be opened in a
RELEASE build, the platform stays quiet instead of writing to a debugger nobody
asked for.

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

`python3 scripts/verify_build.py` runs the check `Log policy (source)`, which
scans every file under `src/` **and** `include/` that can reach the compiler
(`.c` / `.cc` / `.cpp` / `.cxx` / `.h` / `.hh` / `.hpp` / `.hxx` / `.inl`) and
recognizes every `OutputDebugString*` spelling (`...A`, `...W`, the bare macro),
and fails when either:

* a call to the debugger API (`OutputDebugString` in **every** spelling -
  `...A`, `...W` and the bare macro) appears **outside** a `#ifdef DEBUG` /
  `#if defined(DEBUG)` region, or
* the call appears in **any file other than `src/Core/Log.cpp`** - the base owns
  exactly one gated debugger helper, so a second copy is rejected even inside a
  DEBUG region.

It reports `file:line: <source line>` for each violation, for example:

```text
[FAIL] Log policy (source)
       1 call(s) to OutputDebugString* compiled in without a DEBUG gate or
       the g_debugOutput run-time guard (95 file(s) scanned)
       src/Core/Foo.cpp:42: 	OutputDebugStringA("cheap diagnostic\n");
       Move the call into the DEBUG-gated helper in src/Core/Log.cpp (or under
       #ifdef DEBUG); the helper's release branch must keep the
       `if (g_debugOutput)` guard.
       See docs/Logging.md - this is a lexical hygiene check.
```

The check is line-based and documented on purpose - it tracks the preprocessor
conditional nesting, no C++ parsing.  Three consequences:

* it inspects the **source tree**, not the DLL, so it also catches a violation
  when the DLL in `Release/` was built from an older state (the check is printed
  even when the DLL cannot be found);
* the branch gates follow the C preprocessor: `#elif` recomputes its branch from
  its own condition (`#elif defined(DEBUG)` opens a DEBUG region even when the
  preceding `#if` was not a DEBUG gate), and `#else` is the **negation** of its
  conditional - entering it invalidates the DEBUG gate of the region it
  continues.  The single helper in `src/Core/Log.cpp` uses exactly that shape:
  its `#else` branch is the run-time `[Platform] DebugOutput=yes` switch, so the
  helper's release branch must match an explicit whitelist shape.  Three shapes
  are accepted: the guard on the same line
  (`if (g_debugOutput) OutputDebugStringA(x);`), the guard on the immediately
  preceding line, and the early-exit variant (`if (!g_debugOutput) return;`).
  Removing that run-time guard fails the check; it is the regression this
  whitelist protects;
* it proves a **lexical hygiene** property only, not that the release branch is
  unreachable: every literal occurrence of the API name must sit inside a gated
  region of the one helper (and that helper's release path behind the run-time
  `DebugOutput` switch).  It does not see calls hidden behind macros or outside
  the scanned tree, and reachability remains the run-time switch's job.
  Do not add another debugger call anywhere.

## Failure behaviour

The logger is best-effort and must not take the game down with it:

* **A failed write** drops the line.  `WriteFile`'s result is checked (a short
  write counts as a failure); the first failure is reported once through the
  gated debugger helper - not once per line, which a full disk would turn into a
  flood - and logging continues.  A write failure cannot be repaired from here,
  so the line is not retried.
* **A failed open** (the game directory cannot be resolved, or the file cannot be
  opened) disables file logging for the process; the same gated helper carries
  the one-line explanation, and `Log::OpenAt(path)` reports it as `false`.
* **A NULL format string** never reaches the C runtime: a line passed without a
  format is dropped.  The variadic API documents `format` as required
  (`src/Core/Log.h`), but the guard keeps the "cannot crash" promise even if a
  feature passes NULL.
* **C++ exceptions are disabled**, so there are no exceptions to escape from the
  logger in the first place.  Neither build system passes any `/EH` option
  (`ExceptionHandling=false` in `VentureCore.props` emits no switch;
  `scripts/linux/common.sh`'s `vcs_cl_flags` passes none), `_HAS_EXCEPTIONS=0`
  is defined by both, and `include/platform/BuildGuards.h` - force-included in
  every translation unit by both build systems - fails the compile if
  `_CPPUNWIND` becomes defined (`/EH` came back) or `_HAS_EXCEPTIONS` is not 0.
  This is stricter than the old `/EHsc-` claim: `/EHsc-` only disabled the
  `c` half of `/EHsc` and left synchronous C++ exception handling *enabled*.

The normal open/write path and the NULL-format guard are covered by
`tests/test_logging.cpp`.  The write-failure/debugger-report branch cannot be
provoked reliably in the automated checks (it needs a full disk) and is verified
by inspection only.

### Durability

Each line is one synchronous `WriteFile` call: it reaches the OS file cache
immediately (there is no user-mode stdio buffer to lose), and a normal process
exit flushes the cache.  There is **no** `FlushFileBuffers` call, so a power loss
or blue screen can still drop the last lines - deliberately, because flushing
every line would cost a disk round trip in the game loop.

## Verifying a build

```bash
scripts/linux/build_linux.sh Release --build-type RELEASE --clean   # 0 warnings (machine-checked)
python3 scripts/verify_build.py                                     # every check PASS
scripts/linux/run_tests.sh                                          # every test
scripts/linux/run_tests.sh test_logging                             # log file I/O
scripts/linux/run_tests.sh log_levels                               # pure policy only
```

The automated coverage is split: `tests/test_log_levels.cpp` covers the pure
policy in `src/Core/LogPolicy.h` (parsing, filtering, sampling) and links no
logger source; `tests/test_logging.cpp` links `src/Core/Log.cpp` and drives the
compiled logger - a real file under `%TEMP%`, the separator header, level
filtering, the `[HH:MM:SS.mmm] [LEVEL] message` line format, a repeated `Open`,
`Close` idempotence, a failed open and NULL/`%` format strings.  `verify_build.py`
check 7 only asserts *where* the debugger API may be called.  The in-game
integration (SyringeEx injection, the real game directory, a long session) stays
a human check:

Check the first line of `<Platform>.log` for the separator header, the
`log     : level ...` line, and that a battle at the default `info` level keeps
the `[Example] progress:` lines sparse.

## See also

* `docs/Lineage.md` - the startup-time lineage check logs its result in the same
  startup block.
* `docs/HookCatalogue.md` - logging introduces no engine hooks.
* `src/Core/LogPolicy.h`, `src/Core/Log.h`, `src/Core/Log.cpp`,
  `src/Features/Example/ExampleFeature.cpp`, `scripts/verify_build.py`.
