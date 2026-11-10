#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace venture::ExtSave
{
// ---------------------------------------------------------------------------
// Savegame trailer payload builder/parser for extension state.
//
// Payload layout, format version 2 (all integers little endian, u32 unless
// stated):
//
//   [savegameId][payloadVersion = 2][storeBlockCount][signatureCount]
//   signature*: [storeTypeId][storeDataVersion]              (signatureCount entries)
//   storeBlock*:                                            (storeBlockCount entries)
//     [storeTypeId][storeDataVersion][blockSize]
//     payload[blockSize]:
//       [rowCount]
//       row*: [extSaveAddress][ownerSaveAddress][canary][dataSize][data...]
//         canary = 0x56430000 ^ (storeTypeId & 0xFFFF) ^ ((storeDataVersion & 0xFFFF) << 16)
//         (extension class + row layout version, see ExtensionStore.h)
//
// The payload is consumed exactly: every row must be read to its last byte by the
// extension's Load(), every block and the payload itself must have no trailing
// bytes, otherwise the payload is rejected as malformed.
//
// `savegameId` is VC_SAVEGAME_ID from PlatformConfig.h (passed in by the caller
// so this layer stays independent of the platform identity), `storeTypeId` is the
// extension class type id of the store.  Rows are written in allocation order, so
// the on-disk order is deterministic and a load recreates the allocation order.
//
// The **signature** is the platform's saveable store set in
// ExtensionIndex::Stores() registration order (a store participates when it is
// saveable and has both serializers).  The loader compares it item by item
// (type id, data version and order) with the currently registered set *before* it
// reads anything else: a payload whose store set differs is not partially
// applied.  That protects pointer swizzling from references to extensions that
// would never be recreated (a skipped block used to leave such references
// unmatched in the engine's remap pass, which traps the game).
//
// Payload version 1 has no signature and is rejected (no migration).  Both the
// store set and the format version are therefore compatibility gates, not
// best-effort skips: a mismatch drops the whole extension state and the game's
// own objects still load normally.
//
// The parser is deliberately defensive: size fields read from the payload are
// validated against the remaining bytes and MaxPayloadSize before anything is
// allocated, and any malformed header/signature/block rejects the payload with a
// warning instead of failing the game's load.
// ---------------------------------------------------------------------------
constexpr std::uint32_t PayloadVersion = 2;

// Upper bound of one complete extension payload (all store blocks) and of every
// individual block/row size inside it.  The trailer frame publishes the same
// limit as SaveTrailer::MaxPayloadSize (a static_assert in SaveTrailer.cpp keeps
// the two in sync).  Both sides enforce it: a payload from a modified save is
// rejected before anything is allocated from an oversized size field, and a save
// whose payload would exceed the limit is not written.
constexpr std::uint32_t MaxPayloadSize = 16u * 1024u * 1024u;

struct LoadSummary
{
	enum class Result
	{
		NoData,        // no payload at all (old save, or an empty platform)
		Invalid,       // a payload is present but malformed (bad size/CRC/header/block)
		Incompatible,  // well-formed payload this platform cannot apply (see below)
		Loaded,        // extension data restored
	};

	// Why a well-formed payload was refused.  The engine still loads its own
	// objects; only the extension state is dropped (no migration, decision D19).
	enum class Incompatibility
	{
		None,
		PayloadVersion,  // written by another payload format generation (e.g. v1)
		StoreSet,        // saveable store set differs (type ids/data versions/order)
	};

	Result result = Result::NoData;
	Incompatibility incompatibility = Incompatibility::None;
	std::uint32_t payloadVersion = 0; // version found in the payload (0 = not read)
	std::uint32_t storeBlocks = 0;
	std::uint32_t created = 0;
	std::uint32_t adopted = 0;
	std::uint32_t deferred = 0;
	std::uint32_t failed = 0;
};

// Serializes every saveable store into `out` (payload only, no trailer suffix).
// The payload starts with the store-set signature (the same stores, in the same
// registration order as their blocks), so the loader can validate the set before
// reading any block.
// The payload is assembled directly into `out` and its size is checked while it is
// built: a payload that would exceed MaxPayloadSize is rejected immediately (`out`
// cleared, *sizeExceeded set when provided) instead of being materialized and
// checked afterwards.
// Returns false when there is nothing to save (no saveable store is instantiated,
// i.e. the platform never started or has no stateful feature) or when the payload
// limit was exceeded.
bool WritePayload(std::uint32_t savegameId, std::vector<std::uint8_t>& out,
	std::uint32_t* storeBlocks = nullptr, bool* sizeExceeded = nullptr);

// Parses a payload and creates the extension instances.  They are not attached to
// their stores yet: their owner pointers are still save-time addresses and are
// remapped by the engine's swizzle pass; FinishLoad() adopts them.
//
// The store-set signature is compared with the platform's currently registered
// saveable stores before any block is read; any mismatch (and any v1 payload)
// returns Result::Incompatible and creates nothing.
//
// The parser stages every swizzle registration (ExtSwizzle::BeginStaging) and
// hands them to the engine only after the whole payload validated; on any failure
// it discards them *before* deleting the instances created so far, so a rejected
// payload never leaves the engine holding a pointer into a deleted extension.
LoadSummary ReadPayload(std::uint32_t savegameId, const std::uint8_t* data, std::size_t size);

// After the swizzle pass: adopt the loaded instances and allocate extensions for
// objects the engine created itself while loading.
LoadSummary FinishLoad();

// The engine's savegame load window.  While it is open, allocation from object
// constructors is deferred.
void EnterLoadWindow();
void ExitLoadWindow();
bool IsLoadingSaveGame();
}
