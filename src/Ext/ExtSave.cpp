#include "Ext/ExtSave.h"

#include "Ext/AbstractExt.h"
#include "Ext/ExtensionIndex.h"
#include "Ext/ExtStream.h"
#include "Ext/ExtSwizzle.h"

#include <unordered_map>

namespace
{
// Deletes every extension instance that ReadPayload created but did not hand to
// a store (only on the error paths).
void DiscardLoadedFrom(venture::ExtensionIndex& index, const std::size_t firstIndex)
{
	auto loaded = index.LoadedExtensions();

	for (std::size_t i = firstIndex; i < loaded.size(); ++i)
	{
		delete loaded[i].ext;
	}

	index.ClearLoadedExtensions();

	for (std::size_t i = 0; i < firstIndex && i < loaded.size(); ++i)
		index.RegisterLoadedExtension(loaded[i].desc, loaded[i].ext);
}

// Reads and drops a block payload of `size` bytes from the stream.
bool SkipBytes(venture::ExtStreamReader& stream, const std::uint32_t size)
{
	std::uint8_t scratch[64] = {};
	std::uint32_t left = size;
	while (left != 0)
	{
		const std::uint32_t step = left < sizeof(scratch) ? left : static_cast<std::uint32_t>(sizeof(scratch));
		if (!stream.ReadBytes(scratch, step))
			return false;
		left -= step;
	}
	return true;
}

// --- payload assembly ------------------------------------------------------
constexpr std::size_t PayloadHeaderSize = 3u * sizeof(std::uint32_t);

// Writes `value` into the already-sized `out` at `offset` (byte order matches
// ExtStreamWriter::WriteU32).
void StoreU32(std::vector<std::uint8_t>& out, const std::size_t offset, const std::uint32_t value)
{
	out[offset + 0] = static_cast<std::uint8_t>(value & 0xFFu);
	out[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
	out[offset + 2] = static_cast<std::uint8_t>((value >> 16) & 0xFFu);
	out[offset + 3] = static_cast<std::uint8_t>((value >> 24) & 0xFFu);
}

// Appends `size` bytes only when the payload still stays within MaxPayloadSize.
// This is checked while the payload is built (A-02/A-11), so an oversized payload
// fails immediately instead of being materialized and rejected afterwards.
bool AppendChecked(std::vector<std::uint8_t>& out, const void* const data, const std::size_t size)
{
	if (size == 0)
		return true;

	const std::size_t limit = venture::ExtSave::MaxPayloadSize;
	if (out.size() > limit || size > limit - out.size())
		return false;

	const auto* const bytes = static_cast<const std::uint8_t*>(data);
	out.insert(out.end(), bytes, bytes + size);
	return true;
}

bool AppendU32Checked(std::vector<std::uint8_t>& out, const std::uint32_t value)
{
	return AppendChecked(out, &value, sizeof(value));
}
}

namespace venture::ExtSave
{
bool WritePayload(const std::uint32_t savegameId, std::vector<std::uint8_t>& out,
	std::uint32_t* const storeBlocksOut, bool* const sizeExceededOut)
{
	if (sizeExceededOut)
		*sizeExceededOut = false;

	out.clear();

	const ExtensionIndex& index = ExtensionIndex::Instance();

	// Fixed header: the block count is only known after the walk, so reserve the
	// three u32 values and patch them in at the end.
	out.resize(PayloadHeaderSize, 0);

	std::uint32_t blockCount = 0;

	for (const ExtStoreDesc* const desc : index.Stores())
	{
		if (!desc->saveable || !desc->save)
			continue;

		// One store's block is materialized at a time (its size has to be known
		// before the block header can be written); the payload itself is assembled
		// directly in `out` and checked against the limit on every append.
		ExtByteStreamWriter block;
		desc->save(block);
		if (block.Offset() == 0)
			continue;

		const std::vector<std::uint8_t>& blockData = block.Data();

		if (!AppendU32Checked(out, desc->typeId)
			|| !AppendU32Checked(out, desc->dataVersion)
			|| !AppendU32Checked(out, static_cast<std::uint32_t>(blockData.size()))
			|| !AppendChecked(out, blockData.data(), blockData.size()))
		{
			out.clear();
			if (sizeExceededOut)
				*sizeExceededOut = true;
			return false;
		}

		++blockCount;
	}

	if (blockCount == 0)
	{
		out.clear();
		return false;
	}

	StoreU32(out, 0, savegameId);
	StoreU32(out, sizeof(std::uint32_t), PayloadVersion);
	StoreU32(out, 2u * sizeof(std::uint32_t), blockCount);

	if (storeBlocksOut)
		*storeBlocksOut = blockCount;

	return true;
}

LoadSummary ReadPayload(const std::uint32_t savegameId, const std::uint8_t* const data, const std::size_t size)
{
	LoadSummary summary;
	ExtensionIndex& index = ExtensionIndex::Instance();

	if (!data || size == 0)
		return summary;

	// A-01: no registration may reach the engine's swizzle manager until the whole
	// payload validated.  A late failure deletes the instances created so far, and
	// the engine must not keep a registration pointing into a deleted extension.
	// Every exit path below either DiscardStaged()s or (on success) CommitStaged()s.
	ExtSwizzle::BeginStaging();

	ExtByteStreamReader reader(data, size);

	std::uint32_t payloadSavegameId = 0;
	std::uint32_t payloadVersion = 0;
	std::uint32_t blockCount = 0;

	if (!reader.ReadU32(payloadSavegameId) || !reader.ReadU32(payloadVersion) || !reader.ReadU32(blockCount))
	{
		summary.result = LoadSummary::Result::Invalid;
		ExtSwizzle::DiscardStaged();
		return summary;
	}

	if (payloadSavegameId != savegameId || payloadVersion != PayloadVersion)
	{
		// A trailer written by a different platform identity / format generation:
		// ignore it instead of misreading the bytes.
		summary.result = LoadSummary::Result::Invalid;
		ExtSwizzle::DiscardStaged();
		return summary;
	}

	summary.storeBlocks = blockCount;
	const std::size_t loadedBefore = index.LoadedExtensions().size();

	for (std::uint32_t i = 0; i < blockCount; ++i)
	{
		std::uint32_t typeId = 0;
		std::uint32_t dataVersion = 0;
		std::uint32_t blockSize = 0;

		if (!reader.ReadU32(typeId) || !reader.ReadU32(dataVersion) || !reader.ReadU32(blockSize))
		{
			summary.result = LoadSummary::Result::Invalid;
			ExtSwizzle::DiscardStaged();
			DiscardLoadedFrom(index, loadedBefore);
			return summary;
		}

		// A-02/W5b: the size fields are savegame-controlled.  Validate the block
		// size before anything is allocated from it: it must fit in the bytes the
		// payload still carries and in the payload size cap.
		if (blockSize > MaxPayloadSize || blockSize > reader.Remaining())
		{
			summary.result = LoadSummary::Result::Invalid;
			ExtSwizzle::DiscardStaged();
			DiscardLoadedFrom(index, loadedBefore);
			return summary;
		}

		const ExtStoreDesc* const desc = index.FindStore(typeId);

		if (!desc || !desc->saveable || !desc->load || desc->dataVersion != dataVersion)
		{
			// Unknown store (module removed from the platform) or an older row
			// layout: the block is self-describing, so it can be skipped exactly.
			if (!SkipBytes(reader, blockSize))
			{
				summary.result = LoadSummary::Result::Invalid;
				ExtSwizzle::DiscardStaged();
				DiscardLoadedFrom(index, loadedBefore);
				return summary;
			}
			++summary.skippedBlocks;
			continue;
		}

		std::vector<std::uint8_t> block(blockSize);
		if (blockSize != 0 && !reader.ReadBytes(block.data(), blockSize))
		{
			summary.result = LoadSummary::Result::Invalid;
			ExtSwizzle::DiscardStaged();
			DiscardLoadedFrom(index, loadedBefore);
			return summary;
		}

		ExtByteStreamReader blockReader(block.data(), block.size());
		desc->load(blockReader);
		if (!blockReader.Ok())
		{
			summary.result = LoadSummary::Result::Invalid;
			ExtSwizzle::DiscardStaged();
			DiscardLoadedFrom(index, loadedBefore);
			return summary;
		}
	}

	if (!reader.Ok())
	{
		summary.result = LoadSummary::Result::Invalid;
		ExtSwizzle::DiscardStaged();
		DiscardLoadedFrom(index, loadedBefore);
		return summary;
	}

	// Every block validated: hand the staged swizzle registrations to the engine.
	ExtSwizzle::CommitStaged();

	summary.created = static_cast<std::uint32_t>(index.LoadedExtensions().size() - loadedBefore);
	summary.result = LoadSummary::Result::Loaded;
	return summary;
}

LoadSummary FinishLoad()
{
	LoadSummary summary;
	ExtensionIndex& index = ExtensionIndex::Instance();

	// 1. Adopt the extensions restored from the trailer: the engine's swizzle pass
	//    has rewritten their owner pointers to the loaded objects by now.
	for (const ExtensionIndex::LoadedExtension& loaded : index.LoadedExtensions())
	{
		AbstractExt* const ext = loaded.ext;
		if (!ext->Owner())
		{
			++summary.failed;
			delete ext;
			continue;
		}

		if (loaded.desc->adopt && loaded.desc->adopt(ext))
		{
			ext->PostLoad();
			++summary.adopted;
		}
		else
		{
			// The owner already carries an extension (or the store refused it):
			// dropping the loaded instance keeps the index consistent.
			++summary.failed;
			delete ext;
		}
	}
	index.ClearLoadedExtensions();

	// 2. Owners the engine constructed itself while loading carry no saved data;
	//    they get a fresh extension now.
	for (const ExtensionIndex::DeferredAllocation& deferred : index.DeferredAllocations())
	{
		if (index.Fetch(deferred.owner) != nullptr)
			continue; // restored from the trailer a moment ago

		if (deferred.desc->allocateDeferred)
		{
			deferred.desc->allocateDeferred(deferred.owner);
			++summary.deferred;
		}
	}
	index.ClearDeferredAllocations();

	summary.result = LoadSummary::Result::Loaded;
	return summary;
}

void EnterLoadWindow()
{
	ExtensionIndex::Instance().EnterLoadWindow();
}

void ExitLoadWindow()
{
	ExtensionIndex::Instance().ExitLoadWindow();
}

bool IsLoadingSaveGame()
{
	return ExtensionIndex::Instance().IsLoadingSaveGame();
}
}
