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

* it **writes after every other platform's block**, because the DLL name
  (`VentureCore.dll`, and any derivative name that keeps the same prefix) sorts
  after the known platforms (`Ares.dll`, `Kratos.dll`, `Kaleidoscope.dll`,
  `Mix.dll`, `Phobos.dll`, `RA2YRLWResourceReader.dll`) and SyringeEx chains
  same-address handlers in DLL order;
* it **reads by scanning backwards from the end of the stream** (`Seek(END)`,
  validate, read, restore the stream position) instead of reading at the current
  sequential position. A save that contains no trailer simply has no extension
  data - an old save, or a save made without this platform, loads normally;
* the trailer is self-describing (magic + version + size + CRC), so a truncated,
  foreign or damaged tail is rejected with a log warning instead of corrupting
  the game's load.

Known limitation: a future platform whose DLL name sorts **after** this one would
write its block after our trailer, and our backwards scan would then reject the
tail. The log line reports that case and extension state starts empty; it is a
platform-ordering concern, not a savegame format break.

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

The suffix is 20 bytes, so the reader needs a single backwards read of at most
`payloadSize + 20` bytes.

## Payload layout

All integers are little-endian `u32`.

```
[savegameId][payloadVersion][storeBlockCount]
storeBlock*:
  [storeTypeId][storeDataVersion][blockSize]
  block[blockSize]:
    [rowCount]
    row*: [extSaveAddress][ownerSaveAddress][canary][dataSize][data...]
```

* `savegameId` is `VC_SAVEGAME_ID` from `src/Core/PlatformConfig.h`. **Give every
  derived platform its own value** (see `docs/Renaming.md`); a trailer written by
  another platform identity is rejected as a whole.
* `storeTypeId` is the extension class type id of the store (see
  `docs/EngineBindings.md`), `storeDataVersion` is the store's row format
  version. A block for an unknown store, or with an unknown data version, is
  **skipped exactly** using `blockSize` - this is what makes removing a feature
  module from a platform a forward-compatible change.
* `extSaveAddress` / `ownerSaveAddress` are the instance's and the owner's
  save-time addresses. They are not dereferenced on load: the loader registers the
  owner slot with the engine's swizzle manager, and the engine rewrites it to the
  address of the loaded object before `PostLoad()` runs.
* `canary` is derived from the extension class type id and rejects a block whose
  rows belong to a different extension class layout.
* `data` is the bytes written by the extension's `Save(ExtStreamWriter&)`. Pointers
  inside it are written with `WritePointer` and read with
  `ReadPointerForChange`, which registers the field for the swizzle pass.

Rows are written in **allocation order** (`ExtensionStore` keeps instances in a
vector, and stores are visited in registration order), so a load recreates a
deterministic order.

## Save / load sequence

| Step | Engine site | What the platform does |
| --- | --- | --- |
| save | `0x67E42E` (end of the engine's save routine) | `ExtSave::WritePayload` collects every saveable store, wraps it in the trailer and appends it after the last platform block. Nothing is appended when no store carries state (e.g. the platform refused to start). Logs `savegame trailer: offset=… size=…`. |
| load start | `0x67E44D` | Opens the load window in `ExtensionIndex`; allocations requested by object constructors are deferred (the trailer may carry an extension for the same owner). |
| load failure | `0x67E4B1` / `0x67E53A` / `0x67E595` | The engine aborts the load at one of its three early exits and returns **before** the normal close point: the handler at that site closes the load window immediately, so later allocations in the game session take the normal path again (see `Core/Platform.cpp::OnScenarioClear` for the unconditional fallback). |
| load objects | (engine) | Object constructors run while the window is open → `ExtensionStore::Allocate` records the owner as a deferred allocation and returns `nullptr`. |
| load end | `0x67F7C8` (end of the engine's load routine) | `ExtSave::ReadPayload`: backwards scan, magic/version/size/CRC check, then per store block: create the extension instances with their save-time owner pointer and extension address, register both, load their data. The stream position is restored. |
| swizzle | (engine's swizzle pass) | Rewrites every registered owner slot to the loaded object address. |
| post-swizzle | `0x67E685` | `ExtSave::FinishLoad`: adopt the restored instances into their stores (the owner is valid now), call `PostLoad()`, then allocate fresh extensions (with `OnDeferredAllocation()`) for owners the game created during the load. The load window is closed right after this, before the hooked `call 0x685120` re-executes, so allocations made by that call take the normal path instead of being deferred after the drain and lost. |
| load end | `0x67E68A` | Second, idempotent close of the load window (kept as the documented success-path close point); normal allocation is already active. |

## Feature author rules

1. A stateful extension class overrides `Save(ExtStreamWriter&)` and
   `Load(ExtStreamReader&)` and reads every field it wrote, in the same order.
   Use the `Write*`/`Read*` helpers; write game-object pointers with
   `WritePointer` / `ReadPointerForChange` so they survive the load.
2. Its store calls `SetSaveable(true, dataVersion)`; **bump `dataVersion` whenever
   the row layout changes** so older saves skip the block instead of misreading
   it.
3. `PostLoad()` runs after the owner pointer is valid: resolve derived values
   that depend on the owner or on other engine state there, not in `Load()`.
   `Load()` must not dereference `Owner()` (it is still the save-time address at
   that point).
4. Only explicitly saveable stores are serialized. A store for an owner class the
   engine does **not** save (engine-internal objects) must stay non-saveable.
5. A load never runs the INI pass again. If a feature derives state from type INI
   data and caches it, recompute after load (`PostLoad` / `Load`) instead of
   relying on the constructor default.

## Compatibility notes

* Extension state is preserved across a save/load in the same game process and
  across a game restart; the format is forward-compatible for *removed* stores
  and *rejected* (state starts empty) for a bumped `dataVersion`, changed
  `VC_SAVEGAME_ID` or a damaged trailer.
* Multiplayer saves are not a supported scenario (the engine does not save during
  a network game).
* The `Example` module persists a per-unit serial number, so a load is visible in
  the log (`savegame: extension state restored (N adopted, M created during
  load)`).
