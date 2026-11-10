#include "Ext/ExtSave.h"

#include "Ext/AbstractExt.h"
#include "Ext/ExtensionIndex.h"
#include "Ext/ExtStream.h"

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
}

namespace venture::ExtSave
{
bool WritePayload(const std::uint32_t savegameId, std::vector<std::uint8_t>& out,
	std::uint32_t* const storeBlocksOut)
{
	const ExtensionIndex& index = ExtensionIndex::Instance();

	struct Block
	{
		const ExtStoreDesc* desc;
		std::vector<std::uint8_t> payload;
	};

	std::vector<Block> blocks;

	for (const ExtStoreDesc* const desc : index.Stores())
	{
		if (!desc->saveable || !desc->save)
			continue;

		ExtByteStreamWriter writer;
		desc->save(writer);
		if (writer.Offset() == 0)
			continue;

		blocks.push_back(Block{ desc, writer.Data() });
	}

	if (blocks.empty())
		return false;

	ExtByteStreamWriter writer;
	writer.WriteU32(savegameId);
	writer.WriteU32(PayloadVersion);
	writer.WriteU32(static_cast<std::uint32_t>(blocks.size()));

	if (storeBlocksOut)
		*storeBlocksOut = static_cast<std::uint32_t>(blocks.size());

	for (const Block& block : blocks)
	{
		writer.WriteU32(block.desc->typeId);
		writer.WriteU32(block.desc->dataVersion);
		writer.WriteU32(static_cast<std::uint32_t>(block.payload.size()));
		if (!block.payload.empty())
			writer.Write(block.payload.data(), block.payload.size());
	}

	out = writer.Data();
	return true;
}

LoadSummary ReadPayload(const std::uint32_t savegameId, const std::uint8_t* const data, const std::size_t size)
{
	LoadSummary summary;
	ExtensionIndex& index = ExtensionIndex::Instance();

	if (!data || size == 0)
		return summary;

	ExtByteStreamReader reader(data, size);

	std::uint32_t payloadSavegameId = 0;
	std::uint32_t payloadVersion = 0;
	std::uint32_t blockCount = 0;

	if (!reader.ReadU32(payloadSavegameId) || !reader.ReadU32(payloadVersion) || !reader.ReadU32(blockCount))
	{
		summary.result = LoadSummary::Result::Invalid;
		return summary;
	}

	if (payloadSavegameId != savegameId || payloadVersion != PayloadVersion)
	{
		// A trailer written by a different platform identity / format generation:
		// ignore it instead of misreading the bytes.
		summary.result = LoadSummary::Result::Invalid;
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
			DiscardLoadedFrom(index, loadedBefore);
			return summary;
		}

		ExtByteStreamReader blockReader(block.data(), block.size());
		desc->load(blockReader);
		if (!blockReader.Ok())
		{
			summary.result = LoadSummary::Result::Invalid;
			DiscardLoadedFrom(index, loadedBefore);
			return summary;
		}
	}

	if (!reader.Ok())
	{
		summary.result = LoadSummary::Result::Invalid;
		DiscardLoadedFrom(index, loadedBefore);
		return summary;
	}

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
