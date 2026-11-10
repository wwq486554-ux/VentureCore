// Unit test: savegame trailer format and the extension save/load round trip.
//
// Runs as a plain console program: the savegame layer is engine-free, the test
// provides a minimal stand-in for the owner classes and a fake swizzle manager
// (the engine's pointer-remap pass) so a whole save -> new process -> load cycle
// can be simulated in one process.
//
// TEST_DEPS: src/Ext/AbstractExt.cpp src/Ext/ExtensionIndex.cpp src/Ext/ExtSave.cpp src/Core/SaveTrailer.cpp src/Core/Log.cpp src/Core/PlatformPaths.cpp

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

class AbstractClass
{
public:
	virtual ~AbstractClass() = default;
};

class ObjectClass : public AbstractClass {};
class TechnoClass : public ObjectClass {};
class FootClass : public TechnoClass {};
class UnitClass : public FootClass {};

#include "Core/SaveTrailer.h"
#include "Ext/ExtHierarchy.h"
#include "Ext/ExtensionStore.h"
#include "Ext/ExtSave.h"
#include "Ext/ExtStream.h"
#include "Ext/ExtSwizzle.h"

namespace
{
int g_failures = 0;

void Check(const bool ok, const char* const what)
{
	if (ok)
	{
		std::printf("ok    %s\n", what);
	}
	else
	{
		++g_failures;
		std::printf("FAIL  %s\n", what);
	}
}

constexpr std::uint32_t kSavegameId = 0x56433031u;

// --- fake swizzle manager --------------------------------------------------
// Records the pointer fields registered for change and the save-time -> loaded
// address identities; Process() emulates the engine's conversion pass.
std::vector<void**> g_changedSlots;
std::unordered_map<void*, void*> g_identities;

void FakeRegisterForChange(void** const pointer)
{
	if (pointer)
		g_changedSlots.push_back(pointer);
}

void FakeRegisterIdentity(void* const was, void* const is)
{
	g_identities[was] = is;
}

void FakeProcess()
{
	for (void** const slot : g_changedSlots)
	{
		const auto it = g_identities.find(*slot);
		if (it != g_identities.end())
			*slot = it->second;
	}
	g_changedSlots.clear();
	g_identities.clear();
}

// --- extension under test --------------------------------------------------
class SavedUnitExt final : public venture::UnitExt
{
public:
	static constexpr std::uint32_t TypeId = 0x0111u;
	static constexpr std::uint32_t BaseTypeId = venture::UnitExt::TypeId;

	explicit SavedUnitExt(AbstractClass* const owner) : UnitExt(owner) {}

	std::uint32_t ExtTypeId() const override { return TypeId; }

	std::uint32_t Serial = 0;
	AbstractClass* Target = nullptr;
	bool PostLoaded = false;
	int DeferredCalls = 0;

	void Save(venture::ExtStreamWriter& stream) const override
	{
		stream.WriteU32(Serial);
		stream.WritePointer(Target);
	}

	void Load(venture::ExtStreamReader& stream) override
	{
		stream.ReadU32(Serial);
		stream.ReadPointerForChange(reinterpret_cast<void**>(&Target));
	}

	void PostLoad() override
	{
		PostLoaded = true;
	}

	void OnDeferredAllocation() override
	{
		++DeferredCalls;
	}
};

using SavedStore = venture::ExtensionStore<UnitClass, SavedUnitExt>;

void SetUp(bool saveable)
{
	SavedStore::Instance().SetName("UnitClass/SavedUnitExt");
	SavedStore::Instance().SetSaveable(saveable, 1);
}

// Builds a payload by hand for the "unknown store / bad header" cases.
std::vector<std::uint8_t> BuildRawPayload(
	const std::uint32_t savegameId, const std::uint32_t payloadVersion,
	const std::uint32_t typeId, const std::uint32_t dataVersion,
	const std::vector<std::uint8_t>& block)
{
	venture::ExtByteStreamWriter writer;
	writer.WriteU32(savegameId);
	writer.WriteU32(payloadVersion);
	writer.WriteU32(1);
	writer.WriteU32(typeId);
	writer.WriteU32(dataVersion);
	writer.WriteU32(static_cast<std::uint32_t>(block.size()));
	if (!block.empty())
		writer.Write(block.data(), block.size());
	return writer.Data();
}
}

int __cdecl main()
{
	venture::ExtSwizzle::Install(&FakeRegisterForChange, &FakeRegisterIdentity);
	SetUp(true);
	SavedStore& store = SavedStore::Instance();

	// ---- 1. first process: create state and save ---------------------------
	UnitClass oldUnitA;
	UnitClass oldUnitB;
	UnitClass oldUnitC;   // deliberately without an extension
	SavedUnitExt* const oldA = store.Allocate(&oldUnitA);
	SavedUnitExt* const oldB = store.Allocate(&oldUnitB);
	Check(oldA != nullptr && oldB != nullptr && store.Size() == 2, "first process allocates two extensions");
	oldA->Serial = 11;
	oldA->Target = &oldUnitC;
	oldB->Serial = 22;
	oldB->Target = &oldUnitA;

	std::vector<std::uint8_t> payload;
	std::uint32_t blockCount = 0;
	const bool saved = venture::ExtSave::WritePayload(kSavegameId, payload, &blockCount);
	Check(saved && !payload.empty() && blockCount == 1, "WritePayload produces one store block");

	const std::vector<std::uint8_t> trailer = venture::SaveTrailer::WrapPayload(payload);
	Check(trailer.size() == payload.size() + venture::SaveTrailer::SuffixSize,
		"WrapPayload appends the fixed 20-byte suffix");

	// ---- 2. frame parsing --------------------------------------------------
	{
		std::size_t offset = 0;
		std::size_t size = 0;
		Check(venture::SaveTrailer::Parse(trailer.data(), trailer.size(), offset, size)
				== venture::SaveTrailer::ParseResult::Ok,
			"Parse accepts a well-formed trailer");
		Check(offset == 0 && size == payload.size(), "Parse reports the payload range");
	}
	{
		std::vector<std::uint8_t> corrupt = trailer;
		corrupt[1] ^= 0xFF;   // inside the payload: CRC must catch it
		std::size_t offset = 0;
		std::size_t size = 0;
		Check(venture::SaveTrailer::Parse(corrupt.data(), corrupt.size(), offset, size)
				== venture::SaveTrailer::ParseResult::Corrupt,
			"Parse rejects a payload hit by a bit error (CRC)");
	}
	{
		std::size_t offset = 0;
		std::size_t size = 0;
		Check(venture::SaveTrailer::Parse(trailer.data(), 8, offset, size)
				== venture::SaveTrailer::ParseResult::NoTrailer,
			"Parse reports NoTrailer for a stream shorter than the suffix");
		std::vector<std::uint8_t> notOurs(64, 0x5A);
		Check(venture::SaveTrailer::Parse(notOurs.data(), notOurs.size(), offset, size)
				== venture::SaveTrailer::ParseResult::NoTrailer,
			"Parse reports NoTrailer when our magic is absent");
	}
	{
		std::vector<std::uint8_t> versionBumped = trailer;
		const std::uint32_t future = venture::SaveTrailer::FormatVersion + 1;
		std::memcpy(versionBumped.data() + versionBumped.size() - 16, &future, sizeof(future));
		std::size_t offset = 0;
		std::size_t size = 0;
		Check(venture::SaveTrailer::Parse(versionBumped.data(), versionBumped.size(), offset, size)
				== venture::SaveTrailer::ParseResult::Corrupt,
			"Parse rejects a future trailer format version");
	}

	// ---- 3. second process: load into new objects -------------------------
	// The engine cleared the previous scenario and constructed the stored objects
	// at new addresses.
	venture::ExtensionIndex::Instance().ClearAll();
	UnitClass newUnitA;
	UnitClass newUnitB;
	UnitClass newUnitC;

	g_identities.clear();
	FakeRegisterIdentity(&oldUnitA, &newUnitA);
	FakeRegisterIdentity(&oldUnitB, &newUnitB);
	FakeRegisterIdentity(&oldUnitC, &newUnitC);

	venture::ExtSave::EnterLoadWindow();
	Check(venture::ExtSave::IsLoadingSaveGame(), "load window is open");

	// Object constructors run while loading: the store defers every allocation.
	Check(store.Allocate(&newUnitA) == nullptr && store.Allocate(&newUnitB) == nullptr
			&& store.Allocate(&newUnitC) == nullptr,
		"allocations are deferred while a savegame loads");
	Check(store.Size() == 0, "no extension exists before the trailer is read");

	// Platform block read.
	const venture::ExtSave::LoadSummary read = venture::ExtSave::ReadPayload(
		kSavegameId, payload.data(), payload.size());
	Check(read.result == venture::ExtSave::LoadSummary::Result::Loaded && read.created == 2,
		"the payload recreates the two stored extensions");

	// Engine swizzle pass.
	FakeProcess();

	// Post-swizzle step.
	const venture::ExtSave::LoadSummary finished = venture::ExtSave::FinishLoad();
	Check(finished.adopted == 2 && finished.failed == 0, "both extensions are adopted");
	Check(finished.deferred == 1, "the owner created during the load gets a fresh extension");
	venture::ExtSave::ExitLoadWindow();
	Check(!venture::ExtSave::IsLoadingSaveGame(), "load window is closed");

	SavedUnitExt* const newA = store.Fetch(&newUnitA);
	SavedUnitExt* const newB = store.Fetch(&newUnitB);
	SavedUnitExt* const newC = store.Fetch(&newUnitC);
	Check(newA != nullptr && newB != nullptr && newC != nullptr, "every loaded owner has an extension");
	Check(store.Size() == 3 && venture::ExtensionIndex::Instance().ExtCount() == 3,
		"stores and the global index agree after the load");

	Check(newA->Serial == 11 && newB->Serial == 22, "extension state is restored (serials)");
	Check(newA->Target == &newUnitC, "a saved game-object pointer is swizzled to the loaded object");
	Check(newB->Target == &newUnitA, "a second saved pointer is swizzled independently");
	Check(newA->PostLoaded && newB->PostLoaded, "PostLoad runs for restored extensions");
	Check(newC->Serial == 0 && newC->DeferredCalls == 1,
		"the deferred extension is fresh and gets OnDeferredAllocation exactly once");

	Check(venture::UnitExt::Fetch(&newUnitA) == newA, "the base-class view reaches the restored instance");

	// ---- 4. defensive parsing ---------------------------------------------
	{
		// A block of a store this platform does not know (module removed): skipped.
		const std::vector<std::uint8_t> block(8, 0xAB);
		const std::vector<std::uint8_t> unknown = BuildRawPayload(kSavegameId,
			venture::ExtSave::PayloadVersion, 0x7777u, 1, block);
		venture::ExtensionIndex::Instance().ClearAll();
		venture::ExtSave::EnterLoadWindow();
		const venture::ExtSave::LoadSummary skipped =
			venture::ExtSave::ReadPayload(kSavegameId, unknown.data(), unknown.size());
		venture::ExtSave::ExitLoadWindow();
		Check(skipped.result == venture::ExtSave::LoadSummary::Result::Loaded
				&& skipped.skippedBlocks == 1 && skipped.created == 0,
			"an unknown store block is skipped exactly");
	}
	{
		const std::vector<std::uint8_t> block;
		std::vector<std::uint8_t> wrongId = BuildRawPayload(kSavegameId + 1,
			venture::ExtSave::PayloadVersion, SavedUnitExt::TypeId, 1, block);
		venture::ExtSave::LoadSummary summary =
			venture::ExtSave::ReadPayload(kSavegameId, wrongId.data(), wrongId.size());
		Check(summary.result == venture::ExtSave::LoadSummary::Result::Invalid,
			"a payload with another platform id is rejected");

		std::vector<std::uint8_t> wrongVersion = BuildRawPayload(kSavegameId,
			venture::ExtSave::PayloadVersion + 1, SavedUnitExt::TypeId, 1, block);
		summary = venture::ExtSave::ReadPayload(kSavegameId, wrongVersion.data(), wrongVersion.size());
		Check(summary.result == venture::ExtSave::LoadSummary::Result::Invalid,
			"a payload with a future payload version is rejected");

		std::vector<std::uint8_t> wrongStoreVersion = BuildRawPayload(kSavegameId,
			venture::ExtSave::PayloadVersion, SavedUnitExt::TypeId, 99, block);
		summary = venture::ExtSave::ReadPayload(kSavegameId, wrongStoreVersion.data(), wrongStoreVersion.size());
		Check(summary.result == venture::ExtSave::LoadSummary::Result::Loaded
				&& summary.skippedBlocks == 1,
			"a store block with an older/newer row layout is skipped, not misread");
	}
	{
		venture::ExtSave::LoadSummary summary = venture::ExtSave::ReadPayload(kSavegameId, nullptr, 0);
		Check(summary.result == venture::ExtSave::LoadSummary::Result::NoData,
			"an empty payload reads as NoData");
	}
	{
		// A restored extension whose owner slot ends up unresolved after the swizzle
		// pass must not corrupt the store: the instance is dropped and counted as
		// failed instead of being attached to a stale pointer.
		venture::ExtensionIndex::Instance().ClearAll();
		UnitClass orphanSource;
		SavedUnitExt* const orphan = store.Allocate(&orphanSource);
		Check(orphan != nullptr, "an extension can be saved for the failure-path scenario");
		orphan->Serial = 77;

		std::vector<std::uint8_t> orphanPayload;
		(void)venture::ExtSave::WritePayload(kSavegameId, orphanPayload, nullptr);

		venture::ExtensionIndex::Instance().ClearAll();
		venture::ExtSave::EnterLoadWindow();
		(void)venture::ExtSave::ReadPayload(kSavegameId, orphanPayload.data(), orphanPayload.size());
		// Simulate the engine resolving this owner to "no object" (a null slot).
		FakeRegisterIdentity(&orphanSource, nullptr);
		FakeProcess();
		const venture::ExtSave::LoadSummary missing = venture::ExtSave::FinishLoad();
		venture::ExtSave::ExitLoadWindow();
		Check(missing.failed == 1 && missing.adopted == 0 && store.Size() == 0,
			"an extension with an unresolved owner is dropped, not attached");
	}

	// ---- 5. write with no stateful store ----------------------------------
	{
		// After a scenario clear a saveable store still writes its (empty) block, so
		// the trailer exists; only a platform that never started has no stores.
		venture::ExtensionIndex::Instance().ClearAll();
		std::vector<std::uint8_t> empty;
		const bool wrote = venture::ExtSave::WritePayload(kSavegameId, empty, nullptr);
		Check(wrote && !empty.empty(), "a registered saveable store writes an empty block");
	}

	std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAILED",
		g_failures, g_failures == 1 ? "" : "s");
	return g_failures == 0 ? 0 : 1;
}
