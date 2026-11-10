# Savegame Format

This document describes the platform's **savegame tail block**: how extension
data travels through the engine's save/load stream, why it is anchored at the end
of the stream, and what a platform/feature author has to know about it.

Source of truth: `src/Core/SaveTrailer.h` / `.cpp` (frame), `src/Ext/ExtSave.h` /
`.cpp` (payload), `src/Ext/ExtensionStore.h` (per-store rows), and the eight hooks
in `src/Engine/Hooks.Save.cpp` (listed in `docs/HookCatalogue.md`).

## Why a tail block

The engine does not know about platform extensions, so every platform appends an
opaque block of its own to the same `IStream` while the game saves, and reads its
block back while the game loads. The ecosystem rule is: **the set of platforms
that wrote a save must be the set that reads it**, in the same DLL order;
otherwise the sequential readers consume each other's bytes and the load fails.

This platform is designed to be safe next to that convention:

* it **writes at an engine site no other platform hooks** - after the engine's
  object-state writer has returned and before the engine finalizes/commits the
  storage - so its block is appended after every other platform's block
  regardless of handler order.  The original assumption ("the DLL that loads last
  also writes last") was wrong: Ares hooks the historical tail site `0x67E42E`
  and its handler always jumps away, so a writer installed there never runs in a
  process with Ares, and even if it ran, its block would not be the last one;
* it **reads by scanning backwards from the end of the stream** (`Seek(END)`,
  validate, read, restore the stream position) instead of reading at the current
  sequential position. A save that contains no trailer simply has no extension
  data - an old save, or a save made without this platform, loads normally;
* the trailer is self-describing (magic + version + size + CRC), so a truncated,
  foreign or damaged tail is rejected with a log warning instead of corrupting
  the game's load.

Known limitation: a future platform that appends its block after our write site
would hide our trailer from the backwards scan. The log line reports that case
and extension state starts empty; it is a platform-ordering concern, not a
savegame format break.

## Stream layout

```
[payload][magic "VNC1"][formatVersion][payloadSize][crc32][magic "VNC1"]
```

| Field | Size | Meaning |
| --- | --- | --- |
| `payload` | `payloadSize` | extension payload (below) |
| `magic` | 4 | `0x31434E56` = ASCII `VNC1`, written before and after the fixed fields |
| `formatVersion` | 4 | trailer format version (`1`); an unknown version is rejected |
| `payloadSize` | 4 | payload length in bytes; capped at 16 MiB |
| `crc32` | 4 | standard IEEE 802.3 CRC-32 over the payload |

The suffix is 20 bytes, so the reader reads a single backwards window of at most
`payloadSize + 20` bytes (legal COM short reads are retried until the window is
full).

## Payload layout

All integers are little-endian `u32`.

```
[savegameId][payloadVersion = 2][storeBlockCount][signatureCount]
signature*: [storeTypeId][storeDataVersion]        (signatureCount entries)
storeBlock*:                                      (storeBlockCount entries)
  [storeTypeId][storeDataVersion][blockSize]
  block[blockSize]:
    [rowCount]
    row*: [extSaveAddress][ownerSaveAddress][canary][dataSize][data...]
```

* `savegameId` is `VC_SAVEGAME_ID` from `src/Core/PlatformConfig.h`. **Give every
  derived platform its own value** (see `docs/Renaming.md`); a trailer written by
  another platform identity is rejected as a whole.
* `payloadVersion` is `2`. Payload version 1 (the previous, signature-less
  format) and any future generation are **rejected as a whole**, with no
  migration: old saves load the game normally, extension state starts empty.
* the **signature** is the platform's saveable store set in
  `ExtensionIndex::Stores()` registration order - exactly the stores that carry a
  block of their own. The loader compares it item by item (`storeTypeId`,
  `storeDataVersion`, order) with the currently registered set **before it reads
  any block**. Any difference (a feature module removed or added, a bumped
  `dataVersion`, reordered stores) rejects the whole payload with
  `Log::Warn("store set changed - extension state dropped")` - partial restore is
  never attempted. This is the A-05 protection: a skipped store used to leave
  pointers registered by other extensions unmatched in the engine's swizzle pass,
  which traps the game.
* `storeTypeId` is the extension class type id of the store (see
  `docs/EngineBindings.md`), `storeDataVersion` is the store's row format version.
  A block is present for every signature entry, in the same order, with a
  matching `storeTypeId`/`storeDataVersion` header (the numbers are repeated so a
  corrupted block cannot be read against the wrong store).
* `extSaveAddress` / `ownerSaveAddress` are the instance's and the owner's
  save-time addresses. They are not dereferenced on load: the loader registers the
  owner slot with the engine's swizzle manager, and the engine rewrites it to the
  address of the loaded object before `PostLoad()` runs.
* `canary` identifies the extension class **and** the store's row layout version:
  `canary = 0x56430000 ^ (storeTypeId & 0xFFFF) ^ ((storeDataVersion & 0xFFFF) << 16)`
  (low 16 bits = type id, high 16 bits = row version with the `VC` magic folded
  in; injective for ids/versions that fit 16 bits, which the platform enforces
  when the store is declared). A row written for a different class or a drifted
  `Save()`/`Load()` pair is rejected instead of misread.
* rows, blocks and the payload are **consumed exactly**: the extension's `Load()`
  must read its row data to the last byte, a block must contain nothing after its
  last row, and the payload must contain nothing after the last block.  Trailing
  bytes (or a short read in a row) mean a drifted layout or a modified save and
  reject the whole payload as malformed (`Invalid`).
* `data` is the bytes written by the extension's `Save(ExtStreamWriter&)`. Pointers
  inside it are written with `WritePointer` and read with
  `ReadPointerForChange`, which registers the field for the swizzle pass.
* every `blockSize` / `dataSize` field is savegame-controlled and is validated
  **before** the bytes it claims are allocated or read: it must not exceed the
  bytes the enclosing payload/block still carries and must stay within the 16 MiB
  payload cap.  A violation rejects the whole payload (`Invalid`, extension state
  starts empty) instead of attempting the claimed allocation (A-02).
* the writer enforces the same 16 MiB cap **while it builds the payload**: a
  payload that would exceed it is not appended to the savegame (`Log::Error`) -
  that save is written without extension state, which the next load reports as
  empty, rather than aborting the engine's save (A-11).

Rows are written in **allocation order** (`ExtensionStore` keeps instances in a
vector, and stores are visited in registration order), so a load recreates a
deterministic order.

## Save / load sequence

| Step | Engine site | What the platform does |
| --- | --- | --- |
| save | `0x67D1B4` (after the engine's object-state writer, before the finalize/Commit) | `ExtSave::WritePayload` collects every saveable store, wraps it in the trailer, seeks to the end of the stream and appends it after the last platform block. Nothing is appended when the writer's BOOL result (`AL` of `EAX` - the engine itself consumes only `AL`) is 0 (the engine's save failed) or when no store carries state (e.g. the platform refused to start). Logs `savegame trailer: offset=… size=…`. |
| load start | `0x67E44D` | Opens the load window in `ExtensionIndex`; allocations requested by object constructors are deferred (the trailer may carry an extension for the same owner). |
| load failure | `0x67E4B1` / `0x67E53A` / `0x67E595` | The engine aborts the load at one of its three early exits and returns **before** the normal close point: the handler at that site closes the load window immediately, so later allocations in the game session take the normal path again (see `Core/Platform.cpp::OnScenarioClear` for the unconditional fallback). |
| load objects | (engine) | Object constructors run while the window is open → `ExtensionStore::Allocate` records the owner as a deferred allocation and returns `nullptr`. |
| load end | `0x67F7C8` (end of the engine's load routine) | `ExtSave::ReadPayload`: backwards scan, magic/version/size/CRC check, then payload version + store-set signature check (a mismatch drops the extension state, see *Compatibility notes*), then per store block: create the extension instances with their save-time owner pointer and extension address and load their data. Their swizzle registrations are staged and handed to the engine only once the whole payload validated (see *Failure handling*). The stream position is restored. |
| swizzle | (engine's swizzle pass) | Rewrites every registered owner slot to the loaded object address. |
| post-swizzle | `0x67E685` | `ExtSave::FinishLoad`: adopt the restored instances into their stores (the owner is valid now), call `PostLoad()`, then allocate fresh extensions (with `OnDeferredAllocation()`) for owners the game created during the load. The load window is closed right after this, before the hooked `call 0x685120` re-executes, so allocations made by that call take the normal path instead of being deferred after the drain and lost. |
| load end | `0x67E68A` | Second, idempotent close of the load window (kept as the documented success-path close point); normal allocation is already active. |

## Failure handling

The parser is strict by design: a payload is either applied in full or rejected
as a whole - it never restores "what it could read".

* The whole payload is validated **before the engine's swizzle manager is
  touched**.  Row registrations are staged in the platform
  (`ExtSwizzle::BeginStaging`) and handed to the engine only after every block
  validated (`CommitStaged`); on any error the staged registrations are discarded
  (`DiscardStaged`) *before* the instances created from the partial payload are
  deleted, so the engine's remap pass never sees a pointer into a deleted
  extension (A-01).
* Malformed input is a hard `Invalid`, never a best-effort skip: a truncated
  header/signature/block/row, an unreadable or oversized size field, a bad canary,
  a wrong payload version or a changed store set (see *Compatibility notes*), and
  trailing bytes in a row/block/payload all leave extension state empty with a
  `Log::Warn`.  The engine's own objects still load.
* Size fields are validated against the bytes that are actually present and the
  16 MiB cap **before** anything is allocated from them (A-02); the writer
  enforces the same cap while it assembles the payload (A-11).
* The frame reader tolerates legal COM short reads by retrying until the window is
  full (A-13), restores the stream position afterwards and warns when the stream
  cannot be restored (A-14).  A tail with exactly one of our two magic values is
  reported as a corrupt trailer, not as "no trailer" (A-14).

## Feature author rules

1. A stateful extension class overrides `Save(ExtStreamWriter&)` and
   `Load(ExtStreamReader&)` and reads every field it wrote, in the same order.
   Use the `Write*`/`Read*` helpers; write game-object pointers with
   `WritePointer` / `ReadPointerForChange` so they survive the load.
2. Its store calls `SetSaveable(true, dataVersion)`; **bump `dataVersion` whenever
   the row layout changes** so older saves are rejected as incompatible instead
   of being misread (the whole payload is dropped, not just this store's block).
3. `PostLoad()` runs after the owner pointer is valid: resolve derived values
   that depend on the owner or on other engine state there, not in `Load()`.
   `Load()` must not dereference `Owner()` (it is still the save-time address at
   that point).
4. Only explicitly saveable stores are serialized. A store for an owner class the
   engine does **not** save (engine-internal objects) must stay non-saveable.
   Every store must use a **unique extension type id**: a store registered with
   an id that is already taken is refused with `Log::Error` (the first store keeps
   the id), because the type id keys the payload signature and every lookup.
5. A load never runs the INI pass again. If a feature derives state from type INI
   data and caches it, recompute after load (`PostLoad` / `Load`) instead of
   relying on the constructor default.

## Compatibility notes

* **The saveable store set is part of the format.** A save is only accepted when
  the platform's currently registered saveable stores match the payload's
  signature in type id, `dataVersion` and registration order. Removing or adding
  a saveable feature module, bumping a store's `dataVersion` or changing the
  store registration order makes old saves **incompatible**: the game loads and
  its own state is restored, but the extension state starts empty (the engine log
  reports `store set changed - extension state dropped`). There is deliberately
  no per-store fallback/migration (decision D19).
* Payload version 1 (`v0.2.0-experimental` saves) has no signature and is
  rejected the same way (`extension payload version 1 is not supported ... no
  migration`). Old saves keep working for the game itself; only extension state
  is lost once.
* Extension state is preserved across a save/load in the same game process and
  across a game restart **as long as the platform's saveable store set is
  unchanged**; a changed `VC_SAVEGAME_ID`, a changed format version or a damaged
  trailer also leaves the extension state empty.
* Multiplayer saves are not a supported scenario (the engine does not save during
  a network game).
* The `Example` module persists a per-unit serial number, so a load is visible in
  the log (`savegame: extension state restored (N adopted, M created during
  load)`).
