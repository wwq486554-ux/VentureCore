# Savegame Format

This document describes the platform's **savegame tail block**: how extension
data travels through the engine's save/load stream, why it is appended at the end
of the stream and read back strictly sequentially, and what a platform/feature
author has to know about it.

Source of truth: `src/Core/SaveTrailer.h` / `.cpp` (frame), `src/Ext/ExtSave.h` /
`.cpp` (payload), `src/Ext/ExtensionStore.h` (per-store rows), and the eight hooks
in `src/Engine/Hooks.Save.cpp` (listed in `docs/HookCatalogue.md`).

## Why a tail block, and why it is sequential

The engine does not know about platform extensions, so every platform appends an
opaque block of its own to the same `IStream` while the game saves, and reads its
block back while the game loads. The ecosystem rule is: **the platforms write
their blocks in DLL order and read them back in the same order**; a reader that
is out of order consumes another platform's bytes and the load fails.

The engine's savegame `IStream` is a **streaming wrapper** around the OLE
`CONTENTS` stream (the game wraps it in a `CoCreateInstance`'d object); it does
**not implement `Seek`**. Ares and Kratos therefore only use sequential
`Read`/`Write` on it, and so does this platform. There is no backwards scan and no
position restore:

* it **writes at an engine site that runs after every other writer**: the
  `0x67D1B4` hook executes after the engine's object-state writer
  (`call 0x67D300`) has returned, while the ecosystem's historical tail handlers
  (Ares/Kratos `0x67E42E`) run *inside* that call. A sequential append is
  therefore the last block in the stream regardless of handler order. (The
  previous assumption "the DLL that loads last also writes last" was wrong: Ares
  owns `0x67E42E` and its handler always jumps away, so a writer installed there
  never runs next to Ares.)
* it **reads sequentially where the other readers left the stream**: the load
  hook at `0x67F7C8` runs after every ecosystem reader (our DLL name sorts after
  `Ares.dll`, `Kratos.dll`, `Kaleidoscope.dll`, `Mix.dll`, `Phobos.dll`), so when
  it runs the stream is positioned exactly at the start of our frame. A save that
  was written without this platform has no frame there (the stream simply ends, or
  the next platform's block starts) - that is reported as "no extension trailer"
  and the game loads normally;
* the frame is self-describing (**header first**: magic + version + size + CRC),
  so a truncated, foreign or damaged tail is rejected with a log warning instead
  of corrupting the game's load. The header is fixed size, so the reader validates
  it before allocating or reading the payload.

Known limitation: the protocol needs the same order on both sides. A platform
whose DLL name sorts **after** `VentureCore.dll` would read at `0x67F7C8` after
us, while its block was still written before ours (all ecosystem writers run
inside `0x67D300`) - our sequential read would then see that platform's block
instead of our frame and drop the extension state. The current known platform set
(Ares/Kratos/Kaleidoscope/Mix/Phobos) sorts before us, so the order holds; a
platform that writes after `0x67D1B4` or reads after `0x67F7C8` is outside the
contract. The log line reports the mismatch and extension state starts empty; it
is a platform-ordering concern, not a savegame format break.

## Stream layout

Header first, then the payload (the header size is fixed, so the reader validates
the frame before it reads or allocates the payload):

```
[magic "VNC1"][formatVersion][payloadSize][crc32][payload]
```

| Field | Size | Meaning |
| --- | --- | --- |
| `magic` | 4 | `0x31434E56` = ASCII `VNC1` |
| `formatVersion` | 4 | trailer format version (`2`, the header-first sequential frame); an unknown version is rejected with a warning |
| `payloadSize` | 4 | payload length in bytes; capped at 16 MiB |
| `crc32` | 4 | standard IEEE 802.3 CRC-32 over the payload |
| `payload` | `payloadSize` | extension payload (below) |

The header is 16 bytes. The reader reads it with a loop that tolerates legal COM
short reads, checks magic/version/size, then reads exactly `payloadSize` bytes and
verifies the CRC. A stream that ends before the header is complete (a save written
without this platform) is reported as "no extension trailer"; a frame that fails a
check is dropped with a warning and the extension state starts empty.

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
| save | `0x67D1B4` (after the engine's object-state writer, before the finalize/Commit) | `ExtSave::WritePayload` collects every saveable store, wraps it in the header-first frame and appends it **sequentially** at the current stream position - which is the end of the save data, because this site runs after every ecosystem writer. No `Seek` is needed (the savegame stream cannot seek); short writes are retried. Nothing is appended when the writer's BOOL result (`AL` of `EAX` - the engine itself consumes only `AL`) is 0 (the engine's save failed) or when no store carries state (e.g. the platform refused to start). Logs `savegame trailer: offset=… size=…`; `offset=unknown` when the optional position probe fails (normal: the stream is not seekable). |
| load start | `0x67E44D` | Opens the load window in `ExtensionIndex`; allocations requested by object constructors are deferred (the trailer may carry an extension for the same owner). |
| load failure | `0x67E4B1` / `0x67E53A` / `0x67E595` | The engine aborts the load at one of its three early exits and returns **before** the normal close point: the handler at that site closes the load window immediately, so later allocations in the game session take the normal path again (see `Core/Platform.cpp::OnScenarioClear` for the unconditional fallback). |
| load objects | (engine) | Object constructors run while the window is open → `ExtensionStore::Allocate` records the owner as a deferred allocation and returns `nullptr`. |
| load end | `0x67F7C8` (end of the engine's load routine) | Reads the frame **sequentially** from the current position - exactly our block, because this handler runs after every ecosystem reader - with loops that tolerate legal COM short reads: 16-byte header, magic/version/size validation, then the payload and its CRC. No `Seek` and no position restore (the stream cannot seek and we consume exactly our own frame at the end). Then `ExtSave::ReadPayload`: payload version + store-set signature check (a mismatch drops the extension state, see *Compatibility notes*), then per store block: create the extension instances with their save-time owner pointer and extension address and load their data. Their swizzle registrations are staged and handed to the engine only once the whole payload validated (see *Failure handling*). |
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
* The frame reader is sequential and tolerates legal COM short reads by retrying
  until the header/payload is complete (A-13).  A failed read or a read that makes
  no progress drops the frame with a warning instead of spinning.  A stream that
  ends before the header is complete is reported as "no extension trailer" (a save
  written without this platform); a foreign block at the read position (magic
  mismatch) is the same non-error case.  A frame that fails the version, size or
  CRC check is dropped with a warning and the extension state starts empty.  No
  position restore is needed: the reader consumes exactly its own frame and is the
  last sequential reader of the stream.

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
  unchanged**; a changed `VC_SAVEGAME_ID`, a changed frame/payload format version
  or a damaged frame also leaves the extension state empty.
* Multiplayer saves are not a supported scenario (the engine does not save during
  a network game).
* The `Example` module persists a per-unit serial number, so a load is visible in
  the log (`savegame: extension state restored (N adopted, M created during
  load)`).
