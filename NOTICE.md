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
| YRpp (engine wrapper headers) | no | no license declaration | external build dependency, fetched locally, pinned by commit |
| SyringeEx (`Syringe.exe`) | no | not covered; interface format only | used at run time; only the documented section format is used |
| Ares | no | per its own project | optional host, detected at run time only |
| Phobos | no | per its own project | optional host, detected at run time only |
| Kratos | no | per its own project | optional host, detected at run time only |
| Kaleidoscope | no | per its own project | optional host, detected at run time only |

## YRpp

`external/YRpp/` is the YRpp engine wrapper that provides the C++ view of
`gamemd.exe`.  YRpp carries **no license declaration**; the community uses it in
practice without a formal grant.  For that reason:

* this repository does **not** redistribute YRpp - `external/YRpp/` and
  `external/YRpp.commit` are excluded by `.gitignore`;
* the local checkout is fetched with `scripts/linux/fetch_yrpp.sh` and pinned by
  commit in `src/Core/EnginePin.h` (`VC_YRPP_PIN`).  The build refuses to run
  against a different revision (`scripts/linux/check_yrpp.py`, plus
  `static_assert`s in `src/Engine/EngineCompat.h`);
* only public headers and `StaticInits.cpp` from that external checkout are
  compiled - no YRpp source is copied into this repository.

The MIT grant above therefore covers VentureCore's own code, not YRpp.  A derived
platform must make the same separation and must not vendor YRpp either.

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

## Ares, Phobos, Kratos, Kaleidoscope

These are optional host platforms.  VentureCore detects whether their modules
are loaded in the game process and logs the result, and then resolves a support
tier from that (see `docs/HostMatrix.md`).  In v1.0.0 there is:

* no code copied or vendored from any of them;
* no link dependency on any of them;
* no functional dependency - they can be absent, present, or in any combination,
  and the base still runs (tier T0).

The extension-storage layer deliberately does **not** use the engine's unused
`AbstractClass` extension slot, because DLLs in the Phobos lineage own that slot.
Features that interoperate with a host platform would have to be opt-in and are
not part of the base.

## No Phobos code

All source code in this repository was written for VentureCore.  v1.0.0 contains
no Phobos source code and does not call Phobos extensions.

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
