# Licensing

VentureCore is a MIT-licensed *starter base*.  Feature packs built on top of it
are GPL-3.0, and a platform you build from the base may choose its own license.  This document explains what that means in practice and which
components are **not** covered by the MIT grant.

## Summary

| Component | License | Distributed here? |
| --- | --- | --- |
| VentureCore base source (this repository) | MIT - see `LICENSE.md` | yes |
| Venture feature packs (a separate repository) | GPL-3.0 (project policy) | no |
| A platform you build from this base ("derived platform") | your choice, see below | your repository |
| YRpp (engine wrapper headers) | no repository-level license; 1 of 337 files (`QueueClass.h`) is GPL-3.0-or-later and is never instantiated | no |
| SyringeEx (`Syringe.exe`) | not covered; not redistributed | no |
| Ares / Phobos / Kratos | their own licenses; not redistributed | no |
| `gamemd.exe` and game assets | proprietary (EA / Westwood) | no |

## The base: MIT

`LICENSE.md` is the standard MIT license, copyright (c) 2026 The VentureCore
Contributors.  It covers **only** the original source code in this repository.

If you distribute a derived platform, keep `LICENSE.md` and `NOTICE.md` and add
your own copyright line if you changed the code.  MIT does not require you to
open-source your derived platform.

## Our feature packs: GPL-3.0

Feature packs published under the Venture umbrella are licensed **GPL-3.0**,
because they are the product line that actually adds gameplay behaviour and we
want those modules to stay open.

A module copied from such a feature pack into a derived platform brings GPL-3.0
obligations with it.

## Derived platforms: your call

You may license your own platform however you like - MIT, GPL, proprietary -
**as long as**:

* you keep the MIT notice for the base code you copied (`LICENSE.md`, and the
  relevant part of `NOTICE.md`), and
* if you incorporate GPL-3.0-licensed modules (for example from a Venture feature
  pack), the combined work must be distributed under GPL-3.0.  GPL-3.0 is
  viral in that direction: MIT-licensed base code can go into a GPL-3.0 work, but
  GPL-3.0 code cannot be relicensed as MIT/proprietary.

If in doubt, the safe combination for a closed-source platform is: the base code
plus only MIT code you wrote yourself, no GPL-3.0 modules.  The base is safe
because it compiles no third-party copyleft implementation: `QueueClass.h` is
GPL-3.0-or-later and the base appends to the engine event queue with its own code
instead of instantiating that header (see the YRpp section).

## YRpp is not covered by the MIT license

YRpp is the C++ wrapper around `gamemd.exe` that the platform compiles against.
**At the repository level it has no license file**; the community uses it in
practice but there is no formal grant.  **At the file level one header is
copyleft:** `QueueClass.h` - derived from TS++, credited there to CCHyper and
ZivDero - carries a GPL-3.0-or-later notice and defines every queue member
function inline, so calling one of them would compile that implementation into
the DLL and make the distributed binary a GPL-3.0 work.  The platform therefore
does not instantiate that header: the engine event queue is appended to by
`src/Engine/Hooks.Events.cpp` with its own code, written against the layout
measured in `gamemd.exe`, and `QueueClass.h` is used only as a declaration (the
type of the engine object).  `scripts/check_copyleft.py` enforces both halves -
no member of a copyleft-marked header is instantiated, and the set of
copyleft-marked files in the pinned tree is exactly the one audited file.

The rest stays as it was:

* YRpp is **not** distributed in this repository (`external/YRpp/` is gitignored);
* it is an external build dependency, fetched locally with
  `scripts/linux/fetch_yrpp.sh` and pinned by commit in `src/Core/EnginePin.h`;
* the MIT grant above covers VentureCore's own code only - it does not make YRpp
  MIT, and it does not grant anything on YRpp's behalf;
* a derived platform must keep the same separation, must not vendor YRpp, and
  must not instantiate the copyleft-marked header either: replace the call with
  your own code against the engine layout, or license the result GPL-3.0.

See `NOTICE.md` for the full statement.

## SyringeEx and the optional hosts

* **SyringeEx** is used at run time and as an interface definition (the
  `.syhks00` / `.syexe00` sections).  No SyringeEx headers or source are copied;
  the hook macros come from the external YRpp `Syringe.h`.
* **Ares / Phobos / Kratos** are optional hosts.  The base only detects their
  loaded modules and logs the result; nothing is copied, linked or vendored.
  Their own licenses apply to them.  A derived platform can register its own
  private hosts the same way (see `docs/HostMatrix.md`); that does not change the
  licensing of the host.

## The game

`gamemd.exe`, Yuri's Revenge and its assets are proprietary property of their
respective rights holders.  VentureCore requires a legitimate installation and
distributes no game data.  Nothing here grants rights to the game.

## Inbound contributions

Contributions to this repository are accepted under the MIT license (inbound =
outbound), unless a contribution file says otherwise.

## Questions this document answers

* *Can I use the base in a closed-source platform?* - Yes; keep the MIT notice.
  The base compiles no third-party copyleft implementation (see the YRpp section),
  so the built DLL is MIT-licensed code only.
* *Can I sell a platform built on the base?* - The base allows it; the game,
  YRpp and optional host terms are separate and may forbid aspects of that.
* *Can I copy GPL-3.0 modules into my MIT platform?* - No; the combined work
  becomes GPL-3.0.
* *Can I call `EventClass::OutList.Add()` / other `QueueClass` members?* - No:
  that header is GPL-3.0-or-later, and the call compiles it into your DLL.
  `scripts/check_copyleft.py` fails the release gate when you do.
* *Can I redistribute YRpp?* - No; the repository does not distribute it and the
  project makes no claim that it may be redistributed.
