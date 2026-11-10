#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace venture::ExtSave
{
// ---------------------------------------------------------------------------
// Savegame trailer payload builder/parser for extension state.
//
// Payload layout (all integers little endian, u32 unless stated):
//
//   [savegameId][payloadVersion][storeBlockCount]
//   storeBlock*:
//     [storeTypeId][storeDataVersion][blockSize]
//     payload[blockSize]:
//       [rowCount]
//       row*: [extSaveAddress][ownerSaveAddress][canary][dataSize][data...]
//
// `savegameId` is VC_SAVEGAME_ID from PlatformConfig.h (passed in by the caller
// so this layer stays independent of the platform identity), `storeTypeId` is the
// extension class type id of the store.  Rows are written in allocation order, so
// the on-disk order is deterministic and a load recreates the allocation order.
//
// The parser is deliberately defensive: an unknown store type, an unknown store
// data version or a malformed row skips the block (or rejects the trailer with a
// warning) instead of failing the game's load.
// ---------------------------------------------------------------------------
constexpr std::uint32_t PayloadVersion = 1;

struct LoadSummary
{
	enum class Result
	{
		NoData,   // no trailer in the stream (old save, or an empty platform)
		Invalid,  // trailer found but rejected (bad id/version/size/crc)
		Loaded,   // extension data restored
	};

	Result result = Result::NoData;
	std::uint32_t storeBlocks = 0;
	std::uint32_t created = 0;
	std::uint32_t adopted = 0;
	std::uint32_t deferred = 0;
	std::uint32_t skippedBlocks = 0;
	std::uint32_t failed = 0;
};

// Serializes every saveable store into `out` (payload only, no trailer suffix).
// Returns false when there is nothing to save (no saveable store is instantiated,
// i.e. the platform never started or has no stateful feature).
bool WritePayload(std::uint32_t savegameId, std::vector<std::uint8_t>& out,
	std::uint32_t* storeBlocks = nullptr);

// Parses a payload and creates the extension instances.  They are not attached to
// their stores yet: their owner pointers are still save-time addresses and are
// remapped by the engine's swizzle pass; FinishLoad() adopts them.
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
