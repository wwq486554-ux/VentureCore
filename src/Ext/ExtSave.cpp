#include "Ext/ExtSave.h"

#include "Ext/AbstractExt.h"
#include "Ext/ExtensionIndex.h"
#include "Ext/ExtStream.h"
#include "Ext/ExtSwizzle.h"

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
	{
		index.RegisterLoadedExtension(loaded[i].desc, loaded[i].ext);
	}
}

// A store takes part in the savegame payload when it is saveable and owns both
// serializers.  The v2 signature and the block list are built from exactly this
// set, in ExtensionIndex::Stores() registration order; the loader compares the
// signature with the same predicate, so a platform whose store set changed
// (module removed/added, dataVersion bumped, order changed) rejects the payload as
// a whole instead of partially restoring it (A-05).
bool IsPayloadStore(const venture::ExtStoreDesc& desc)
{
	return desc.saveable && desc.save != nullptr && desc.load != nullptr;
}

std::vector<const venture::ExtStoreDesc*> PayloadStores(const venture::ExtensionIndex& index)
{
	std::vector<const venture::ExtStoreDesc*> stores;

	for (const venture::ExtStoreDesc* const desc : index.Stores())
	{
		if (desc && IsPayloadStore(*desc))
			stores.push_back(desc);
	}

	return stores;
}

// --- payload assembly ------------------------------------------------------
// [savegameId][payloadVersion][storeBlockCount][signatureCount]
constexpr std::size_t PayloadHeaderSize = 4u * sizeof(std::uint32_t);

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
	const std::vector<const ExtStoreDesc*> stores = PayloadStores(index);

	// Nothing to save: no saveable store exists (the platform refused to start, or
	// it has no stateful feature).
	if (stores.empty())
		return false;

	// Fixed header: the block/signature counts are known now, but the header is
	// patched in at the end so the assembly below stays a single forward pass.
	out.resize(PayloadHeaderSize, 0);

	// Signature first: the loader validates it before it reads a single block, so
	// the store set (ids, row versions and order) is an all-or-nothing gate.
	std::uint32_t signatureCount = 0;

	for (const ExtStoreDesc* const desc : stores)
	{
		if (!AppendU32Checked(out, desc->typeId) || !AppendU32Checked(out, desc->dataVersion))
		{
			out.clear();
			if (sizeExceededOut)
				*sizeExceededOut = true;
			return false;
		}

		++signatureCount;
	}

	// One store's block is materialized at a time (its size has to be known before
	// the block header can be written); the payload itself is assembled directly in
	// `out` and checked against the limit on every append.
	std::uint32_t blockCount = 0;

	for (const ExtStoreDesc* const desc : stores)
	{
		ExtByteStreamWriter block;
		desc->save(block);

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

	StoreU32(out, 0, savegameId);
	StoreU32(out, sizeof(std::uint32_t), PayloadVersion);
	StoreU32(out, 2u * sizeof(std::uint32_t), blockCount);
	StoreU32(out, 3u * sizeof(std::uint32_t), signatureCount);

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

	if (!reader.ReadU32(payloadSavegameId) || !reader.ReadU32(payloadVersion))
	{
		summary.result = LoadSummary::Result::Invalid;
		ExtSwizzle::DiscardStaged();
		return summary;
	}

	summary.payloadVersion = payloadVersion;

	if (payloadSavegameId != savegameId)
	{
		// A trailer written by a different platform identity: never misread it.
		summary.result = LoadSummary::Result::Invalid;
		ExtSwizzle::DiscardStaged();
		return summary;
	}

	if (payloadVersion != PayloadVersion)
	{
		// v1 (no store-set signature) and any future payload generation are
		// rejected as a whole: extension state starts empty, the game's own objects
		// still load, and there is no migration (decision D19).
		summary.result = LoadSummary::Result::Incompatible;
		summary.incompatibility = LoadSummary::Incompatibility::PayloadVersion;
		ExtSwizzle::DiscardStaged();
		return summary;
	}

	std::uint32_t blockCount = 0;
	std::uint32_t signatureCount = 0;

	if (!reader.ReadU32(blockCount) || !reader.ReadU32(signatureCount))
	{
		summary.result = LoadSummary::Result::Invalid;
		ExtSwizzle::DiscardStaged();
		return summary;
	}

	// The signature is the platform's currently registered saveable store set, in
	// registration order.  Compare it before reading any block: a store that was
	// removed/added, whose row version changed or whose position changed makes the
	// whole payload incompatible.  Unknown type ids are caught here - they never
	// reach a block (A-05).
	const std::vector<const ExtStoreDesc*> stores = PayloadStores(index);

	if (signatureCount != static_cast<std::uint32_t>(stores.size()))
	{
		summary.result = LoadSummary::Result::Incompatible;
		summary.incompatibility = LoadSummary::Incompatibility::StoreSet;
		ExtSwizzle::DiscardStaged();
		return summary;
	}

	for (std::uint32_t i = 0; i < signatureCount; ++i)
	{
		std::uint32_t typeId = 0;
		std::uint32_t dataVersion = 0;

		if (!reader.ReadU32(typeId) || !reader.ReadU32(dataVersion))
		{
			summary.result = LoadSummary::Result::Invalid;
			ExtSwizzle::DiscardStaged();
			return summary;
		}

		if (typeId != stores[i]->typeId || dataVersion != stores[i]->dataVersion)
		{
			summary.result = LoadSummary::Result::Incompatible;
			summary.incompatibility = LoadSummary::Incompatibility::StoreSet;
			ExtSwizzle::DiscardStaged();
			return summary;
		}
	}

	// v2 writes exactly one block per signature entry.
	if (blockCount != signatureCount)
	{
		summary.result = LoadSummary::Result::Invalid;
		ExtSwizzle::DiscardStaged();
		return summary;
	}

	summary.storeBlocks = blockCount;
	const std::size_t loadedBefore = index.LoadedExtensions().size();

	for (std::uint32_t i = 0; i < blockCount; ++i)
	{
		const ExtStoreDesc* const desc = stores[i];

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

		if (typeId != desc->typeId || dataVersion != desc->dataVersion)
		{
			// The block header must agree with the signature entry it follows.
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
		// A-09: a block must be consumed exactly.  Trailing bytes after the last
		// row mean the payload is malformed or was written by a drifted store
		// layout - reject it instead of restoring part of it.
		if (!blockReader.Ok() || blockReader.Remaining() != 0)
		{
			summary.result = LoadSummary::Result::Invalid;
			ExtSwizzle::DiscardStaged();
			DiscardLoadedFrom(index, loadedBefore);
			return summary;
		}
	}

	// A-09: the payload must be consumed exactly too (no trailing bytes after the
	// last block).
	if (!reader.Ok() || reader.Remaining() != 0)
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
