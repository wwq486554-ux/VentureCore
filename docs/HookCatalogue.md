# Hook Catalogue

This file is the **single source of truth** for every engine hook the VentureCore
base installs.  `scripts/verify_build.py` parses the table below at run time and
requires the hook records embedded in the built DLL to match it exactly - there
is no second copy of the list in the verify script, and adding a hook to the
code without adding its row here (or the other way round) makes verification
fail.

## Format rules

The table is machine-read.  Keep these rules when editing it:

* Only this one table may start with two `0x`-prefixed hexadecimal cells - the
  parser treats every such row as a hook record.  Use a bullet list or a table
  with a different first column for anything else.
* Column order is fixed: **Address | Size | Name | Purpose | Notes**.  The
  header row is `| 地址 | size | 名称 | 用途 | 备注 |` (kept in that form because
  the task specification and the parser both key on this column order); the
  rows below it are English.
* Address and size are hexadecimal with a `0x` prefix; sizes are the byte count
  that the hook window overwrites.
* The name cell must contain the hook handler identifier (a C identifier).
* Addresses are game-executable virtual addresses for the supported
  `gamemd.exe` build (Yuri's Revenge 1.001, as pinned by the build target).  A
  different executable build will not match these addresses.

## v0.2.0-experimental hook table

| 地址 | size | 名称 | 用途 | 备注 |
| --- | --- | --- | --- | --- |
| 0x7CD810 | 0x9 | Venture_ExeRun | Base initialization: SyringeEx gate, read the INI, build the host detection matrix, initialize feature modules, write the startup log | Core; runs once per process |
| 0x685659 | 0xa | Venture_ScenarioClear | Scenario cleanup: clear every extension store and the global extension index | Core; scenario teardown. NB: the engine also calls `ResetScenario` (this function) **inside** a savegame load (`0x67E730` -> `0x67E739`), after the load window opened at `0x67E44D`; the handler keeps the load window open in that case (resetting it would break the trailer adoption) |
| 0x7258D0 | 0x6 | Venture_AnnounceInvalidPointer | Pointer-invalidation notice: the engine announces that an owner pointer became invalid; the extension index records the notification and forwards it to pointer-keyed subscribers | Core; the object's destructor hook still owns extension removal - v0 only counts and debug-logs the notification |
| 0x7353D3 | 0x7 | Venture_Example_UnitClass_CTOR | Example module: allocate the `UnitClass` extension on construction | Example module; demonstrates the extension store |
| 0x735967 | 0x9 | Venture_Example_UnitClass_DTOR | Example module: remove the `UnitClass` extension on destruction | Example module; first destructor site |
| 0x7359DA | 0x9 | Venture_Example_UnitClass_DTOR | Example module: same handler at the second destructor site | Example module; declared with `DEFINE_HOOK_AGAIN` |
| 0x6686C0 | 0x6 | Venture_RulesClass_Init | INI takeover: the parsed rules(md).ini is available and not yet read - notify the rules-INI observers, then remove the platform-owned keys so a host platform such as Phobos (its `0x668BF0` hook is called from inside this function) cannot read them too | Core; objdump from `0x6686C0`: first instruction `sub esp,0xd0` (6 B) and `0x6686C6` is the next instruction - free site, no platform hook table uses it; `[Platform] IniTakeover=no` keeps the keys visible |
| 0x67E44D | 0x5 | Venture_LoadGame_Begin | Savegame load: open the extension load window, so allocations driven by object constructors are deferred until the trailer has been read | Save tail; first instruction `push 0x83d620` (5 B), `address + 5` is the next instruction; same site as the ecosystem's LoadGame_SetFlag hooks (all size 5) |
| 0x67D1B4 | 0x6 | Venture_SaveGame_Trailer | Savegame save: append the platform's framed tail block after the engine's object-state writer returned and before the engine finalizes/commits the storage | Save tail; window `mov bl,al`(2) + `mov eax,[esp+0x14]`(4) ends at 0x67D1BA (`cmp eax,ebp`); free site - none of the five ecosystem platform hook tables (`Ares` / `Kratos` / `Kaleidoscope` / `Mix` / `Phobos`) uses it. The historical same-address tail site 0x67E42E is owned by Ares: its handler always returns a jump address and the site lies *inside* the 0x67D300 object-state writer, i.e. it runs before this site, so it cannot host our writer. The save stream `IStream*` is at [esp+0x1C] (loaded into ECX for `call 0x67D300` at 0x67D1AF); AL of EAX is 0x67D300's BOOL result (every failure epilogue runs `xor al,al` and leaves the negative HRESULT in the upper 24 bits, the success tail runs `setge al`; the engine itself consumes only AL) - AL == 0 means the save failed, do not append. The append is strictly sequential: the engine's savegame stream does not implement `Seek` (streaming wrapper around the OLE CONTENTS stream), and every ecosystem writer runs inside the `0x67D300` call (or at 0x67E42E inside it), so the current position is the end of the save data and our block is the last one |
| 0x67F7C8 | 0x5 | Venture_LoadGame_Trailer | Savegame load: read the framed tail block sequentially from the current stream position (our handler runs after every ecosystem sequential reader, so that position is exactly our block) and validate it | Same site as the Ares / Kratos / Kaleidoscope / Mix load-end hooks (all size 5); window `pop edi`(1) + `pop esi`(1) + `pop ebp`(1) + `mov al,1`(2) ends at 0x67F7CD. No `Seek`/position restore: the engine's savegame stream is a non-seekable streaming wrapper and every platform reads its block sequentially; legal COM short reads are retried by the header/payload read loops |
| 0x67E685 | 0x5 | Venture_LoadGame_PostSwizzle | Savegame load: first instruction after the engine's swizzle pass - attach the restored extensions to their remapped owners | Window `call 0x685120` (5 B) ends on the next instruction; same site as the fork lineage's post-swizzle hook (size 5) |
| 0x67E68A | 0x5 | Venture_LoadGame_End | Savegame load: close the extension load window and resume normal allocation | Window `mov ecx,0x87f7e8` (5 B); same site as the Kaleidoscope / Mix load-window-close hooks (size 5); idempotent second close - the post-swizzle handler already closes the window on the success path, this site remains the documented close point |
| 0x67E4B1 | 0x5 | Venture_LoadGame_Failed1 | Savegame load: close the extension load window when the engine aborts the load (first `StgOpenStorage` failure) | Save tail; window `xor al,al`(2) + `pop edi`(1) + `pop esi`(1) + `pop ebp`(1) ends at 0x67E4B6 (`pop ebx`); free site - none of the five ecosystem platform hook tables uses it; the engine's normal close point (0x67E68A) is not reached on this path, so this is the only place the window is closed |
| 0x67E53A | 0x5 | Venture_LoadGame_Failed2 | Savegame load: same, for the second `StgOpenStorage` failure of the engine's load routine | Save tail; window `xor al,al`(2) + `pop edi`(1) + `pop esi`(1) + `pop ebp`(1) ends at 0x67E53F (`pop ebx`); free site; same failure-close purpose as Failed1 |
| 0x67E595 | 0x5 | Venture_LoadGame_Failed3 | Savegame load: same, for a failed COM method in the engine's load routine | Save tail; window `pop edi`(1) + `pop esi`(1) + `pop ebp`(1) + `xor al,al`(2) ends at 0x67E59A (`pop ebx`); free site; same failure-close purpose as Failed1 |
| 0x4C6CC8 | 0x5 | Venture_Event_Dispatch | Event channel: route the platform's own event types 0x70-0x7F to their registered responders; the engine's own event switch still runs afterwards | Event channel; same site and semantics as the reference ecosystem declares (Phobos/Kratos/Mix `.syhks00` tables: 0x4C6CC8, size 0x5, handler returns 0). objdump from 0x4C6CC8: `mov al,[esi]` (2 bytes) + `mov edi,[ecx+ebx*4]` (3 bytes), next instruction 0x4C6CCD = address+5; the size must stay 5 because 0x4C6CCD is another platform's patch window |
| 0x64B6FE | 0x6 | Venture_Event_GetSize1 | Event channel: return the registered payload size for our event types at the first GetEventSize site (type in EDI), then resume at 0x64B71D with EDX/EBP = size | Event channel; same site and semantics as the reference ecosystem declares (Phobos/Kratos/Mix: size 0x6). objdump from 0x64B6FE: `and edi,0xff` (6 bytes), next instruction 0x64B704 = address+6; the vanilla path reads the engine's 0x2F-entry size table, which our numbers are outside of |
| 0x64BE7D | 0x6 | Venture_Event_GetSize2 | Event channel: same as GetSize1 but also writes the size into the caller frame slot [esp+0x20] (STACK_OFFSET(0xAC, -0x8C)) and resumes at 0x64BE97 | Event channel; same site and semantics as the reference ecosystem declares (Phobos/Kratos/Mix: size 0x6, stack slot [esp+0x20]). objdump from 0x64BE7D: `and edi,0xff` (6 bytes), next instruction 0x64BE83 = address+6 |
| 0x64C30E | 0x6 | Venture_Event_GetSize3 | Event channel: same as GetSize1 with the type taken from ESI and resumes at 0x64C321 | Event channel; same site and semantics as the reference ecosystem declares (Phobos/Kratos/Mix: size 0x6). objdump from 0x64C30E: `and esi,0xff` (6 bytes), next instruction 0x64C314 = address+6 |
| 0x533066 | 0x6 | Venture_Commands_Register | Commands: create one engine command object per enabled platform command descriptor and append it to CommandClass::Array; the command then appears in the game's keyboard dialog where the player binds a key | Commands; same site and semantics as the reference ecosystem declares (Phobos/Kratos/Kaleido/Mix `.syhks00` tables: 0x533066, size 0x6, handler returns 0). objdump from 0x533066: `mov DWORD PTR [eax],0x7eb89c` (6 bytes), next instruction 0x53306C = address+6; returning 0 lets the trampoline re-execute that store, so the engine's own command object is still initialised |

Twenty hook records, nineteen distinct hook handler names (`Venture_Example_UnitClass_DTOR`
is installed at two addresses with `DEFINE_HOOK_AGAIN`).

## What "size" means

A SyringeEx hook does not patch the whole function - it overwrites the first
`size` bytes of it with a jump and restores those bytes when the handler
returns.  The size therefore has to be chosen so that:

* the window contains **complete instructions only** (no instruction may be cut
  in half), and
* `address + size` is the start of the **next** instruction.

If two hooks share the same address, SyringeEx uses the largest declared size
for the window (`max(size)`).  Every size in this catalogue comes from the
instruction listing at the hook site; when the site is shared with the wider
Phobos / YRpp ecosystem the declared size also matches the convention used there.
Do not invent a new size when adding a hook to an existing site - derive it from
the instruction listing and check it against the ecosystem conventions.

## Adding a hook

1. Derive the address and verify the instruction window as described above.
2. Add `DEFINE_HOOK` / `DEFINE_HOOK_AGAIN` in the appropriate source file
   (`src/Engine/Hooks.Lifecycle.cpp` for base lifecycle hooks, the feature
   module for feature-specific hooks).
3. Add one row to the table above with address, size, handler name and a short
   purpose.  The parser matches address, size and name exactly, so a typo in
   either place is a verification failure.
4. Add feature-specific hooks only in feature modules; keep the base hook
   surface small so derived platforms can replace modules without dragging
   unrelated patches along.
5. Rebuild, then run `python3 scripts/verify_build.py`.

## Planned hook sites (not implemented in v0.2.0-experimental)

The following are roadmap items only - they are **not** in the table above and
**not** in the DLL yet.  When one is implemented it moves into the table.

* Host-specific (T1-T3) feature hooks for Ares / Phobos / Kratos environments:
  planned for a later increment, after the host matrix grows beyond
  detect-and-log.

## See also

* `docs/EngineBindings.md` - the binding layers this catalogue belongs to.
* `docs/MakeYourOwnPlatform.md` - how a derived platform adds its own features
  (and hooks) without touching the base.
* `scripts/verify_build.py` - the checker that enforces this table.
