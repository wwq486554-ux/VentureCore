# NOTICE

VentureCore is a MIT-licensed starter base for building a SyringeEx platform DLL
for *Command & Conquer: Yuri's Revenge*.  The MIT license in `LICENSE.md` applies
to the original source code in this repository only.  This file records the
third-party components the build and the runtime touch, and what is (and is not)
distributed here.

## Summary

| Component | Distributed here | License / terms | Notes |
| --- | --- | --- | --- |
| VentureCore base source | yes | MIT (`LICENSE.md`) | Copyright (c) 2026 The VentureCore Contributors |
| `gamemd.exe` and game assets | no | proprietary (EA / Westwood) | the user must own a copy of Yuri's Revenge 1.001 |
| YRpp (engine wrapper headers) | no | no repository-level license; 1 of 337 files (`QueueClass.h`) is GPL-3.0-or-later, and that file is never instantiated | external build dependency, fetched locally, pinned by commit |
| SyringeEx (`Syringe.exe`) | no | not covered; interface format only | used at run time; only the documented section format is used |
| Ares | no | per its own project | optional host, detected at run time only |
| Phobos | no | per its own project | optional host, detected at run time only |
| Kratos | no | per its own project | optional host, detected at run time only |

## YRpp

`external/YRpp/` is the YRpp engine wrapper that provides the C++ view of
`gamemd.exe`.  **Repository level: no license.**  YRpp ships no `LICENSE` /
`COPYING` file and its README states no terms; the community uses it in practice
without a formal grant.  **File level: not uniform.**  In the pinned revision
`QueueClass.h` carries an explicit **GPL-3.0-or-later** notice (the file comes
from TS++, credited there to CCHyper and ZivDero) and defines every queue member
function **inline in the header**, so any call to one of those members compiles a
GPL implementation into the calling binary.  An audit of the pinned tree
(`scripts/check_copyleft.py`) finds exactly that one file carrying a copyleft
notice among 337 source files; no other file states any terms.

This platform therefore **instantiates nothing from `QueueClass.h`**.  The one
place that needs the engine's event out queue - `EventClass::OutList`, in
`src/Engine/Hooks.Events.cpp` - appends to the queue with its own code, written
against the queue layout measured in `gamemd.exe` (`Count` at `0x00A802C8`,
timestamps at `0x00A83A54`, i.e. `+0x378C`); `QueueClass.h` is used only for its
declaration, as the type of the engine object.  The release gate fails when a
copyleft-marked YRpp header is used through one of its members, and when the set
of copyleft-marked files in the pinned tree changes (`scripts/check_copyleft.py`,
wired into `scripts/verify_build.py`).  The distributed DLL is consequently built
from MIT-licensed code only, and is offered under the MIT grant in `LICENSE.md`.

The rest of the dependency is handled as before:

* this repository does **not** redistribute YRpp - `external/YRpp/` and
  `external/YRpp.commit` are excluded by `.gitignore`.  The one committed file
  in that directory, `external/YRpp.sha256`, is a manifest of file hashes (no
  YRpp source) used to verify the local checkout, so it is not a redistribution
  of YRpp either;
* the local checkout is fetched with `scripts/linux/fetch_yrpp.sh` and pinned by
  commit in `src/Core/EnginePin.h` (`VC_YRPP_PIN`).  The build refuses to run
  against a different revision (`scripts/linux/check_yrpp.py`, plus
  `static_assert`s in `src/Engine/EngineCompat.h`);
* only public headers and `StaticInits.cpp` from that external checkout are
  compiled - no YRpp source is copied into this repository.

The MIT grant above therefore covers VentureCore's own code, not YRpp.  A derived
platform must make the same separation, must not vendor YRpp, and must not
instantiate the copyleft-marked header either; run `scripts/check_copyleft.py`
after a pin update or before adding a new engine call.

## SyringeEx

VentureCore is loaded by SyringeEx (the Syringe fork that scans the two sections
below) through `.syhks00` for hooks and `.syexe00` for host declarations.  The
section layout is used as a documented interface fact:

* no SyringeEx headers are copied into this repository;
* the `DEFINE_HOOK` / `DEFINE_HOOK_AGAIN` / `REGISTERS` macros and the
  `SyringeFeatures` flags come from the external YRpp `Syringe.h`, i.e. from the
  external build dependency, not from a vendored SyringeEx source tree;
* the platform refuses to start when it detects an older Syringe (the
  `SyringeFeatures` flags are false) instead of risking a corrupted process.

## Ares, Phobos, Kratos

These are optional host platforms.  VentureCore detects whether their modules
are loaded in the game process and logs the detected set (see
`docs/HostMatrix.md`).  A derived platform can register its own private hosts
the same way (source-level, explicit bit; no code is copied from a host).  The
base has:

* no code copied or vendored from any of them;
* no link dependency on any of them;
* no functional dependency - they can be absent, present, or in any combination,
  and the base still runs.

The extension-storage layer deliberately does **not** use the engine's unused
`AbstractClass` extension slot, because DLLs in the Phobos lineage own that slot.
Features that interoperate with a host platform would have to be opt-in and are
not part of the base.

## No Phobos code

All source code in this repository was written for VentureCore.  The base
contains no Phobos source code and does not call Phobos extensions.

## Game and trademarks

`gamemd.exe`, Yuri's Revenge, Command & Conquer and related names and assets are
the property of their respective rights holders (Electronic Arts / Westwood
Studios).  Nothing in this repository grants any rights to them.  VentureCore
requires a legitimate installation of the game and does not distribute or
reproduce any game data.

## Derived platforms

If you publish a platform built from this base, keep this NOTICE (and
`LICENSE.md`) with your distribution and add your own third-party notices.  If
you copy GPL-3.0-licensed feature modules into your platform, the combined work
must be GPL-3.0 - see `docs/Licensing.md`.
