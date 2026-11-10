// Unit test: savegame trailer format and the extension save/load round trip.
//
// Runs as a plain console program: the savegame layer is engine-free, the test
// provides a minimal stand-in for the owner classes and a fake swizzle manager
// (the engine's pointer-remap pass) so a whole save -> new process -> load cycle
// can be simulated in one process.  The platform writer/reader are also driven
// through a minimal in-memory IStream implementation.  The engine's savegame
// stream does not implement Seek, so the IStream cases include fully non-seekable
// streams and require zero Seek dependency: the writer appends sequentially and
// the reader reads the frame from the current position.
//
// The payload is format v2, so it starts with the platform's saveable store-set
// signature; the defensive-parsing cases below cover that gate (unknown store,
// changed store dataVersion, other payload version) as well as frame corruption.
//
// TEST_DEPS: src/Ext/AbstractExt.cpp src/Ext/ExtensionIndex.cpp src/Ext/ExtSave.cpp src/Core/SaveTrailer.cpp src/Core/Log.cpp src/Core/PlatformPaths.cpp

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <utility>
#include <vector>

#include <windows.h>
#include <Objidl.h>

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

// --- fake IStream ----------------------------------------------------------
// Minimal in-memory COM stream.  The savegame writer/reader go through the real
// IStream vtable.  The failure switches model the engine's streaming savegame
// stream (Seek returns E_NOTIMPL), a failing / partial / no-progress Write and a
// failing / short / no-progress Read (COM allows all of these, the code has to
// cope).  `SkipTo` moves the internal position directly, without calling Seek: it
// simulates the engine's other sequential readers/writers having advanced the
// stream, so a test can set up a non-seekable stream exactly where the frame
// starts.
class FakeStream final : public IStream
{
public:
	explicit FakeStream(std::vector<std::uint8_t> initial = {})
		: m_data(std::move(initial)) {}

	void SetFailWrites(const bool fail) { m_failWrites = fail; }
	void SetShortWrites(const bool shortWrites) { m_shortWrites = shortWrites; }
	void SetZeroWrites(const bool zeroWrites) { m_zeroWrites = zeroWrites; }
	void SetFailReads(const bool fail) { m_failReads = fail; }
	void SetShortReads(const bool shortReads) { m_shortReads = shortReads; }
	void SetZeroReads(const bool zeroReads) { m_zeroReads = zeroReads; }
	void SetFailSeeks(const bool fail) { m_failSeeks = fail; }
	void SkipTo(const std::uint64_t position) { m_position = position; }
	void ResetCounters() { m_writeCalls = 0; m_readCalls = 0; m_seekCalls = 0; }

	const std::vector<std::uint8_t>& Data() const { return m_data; }
	std::uint64_t Position() const { return m_position; }
	int WriteCalls() const { return m_writeCalls; }
	int ReadCalls() const { return m_readCalls; }
	int SeekCalls() const { return m_seekCalls; }

	// --- IUnknown ---
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** const ppvObject) override
	{
		if (ppvObject)
			*ppvObject = nullptr;
		return E_NOINTERFACE;
	}

	ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
	ULONG STDMETHODCALLTYPE Release() override { return 1; }

	// --- ISequentialStream ---
	HRESULT STDMETHODCALLTYPE Read(void* const pv, const ULONG cb, ULONG* const pcbRead) override
	{
		++m_readCalls;

		if (m_failReads)
		{
			if (pcbRead)
				*pcbRead = 0;
			return E_FAIL;
		}

		if (m_zeroReads)
		{
			// A read that succeeds but makes no progress: legal for COM, but a
			// consumer that keeps looping would spin forever.
			if (pcbRead)
				*pcbRead = 0;
			return S_OK;
		}

		const std::uint64_t size = static_cast<std::uint64_t>(m_data.size());
		const std::uint64_t offset = m_position < size ? m_position : size;
		std::uint64_t count = static_cast<std::uint64_t>(cb);
		const std::uint64_t remaining = size - offset;
		if (count > remaining)
			count = remaining;
		if (m_shortReads && count > 1)
			count /= 2;

		if (count != 0)
			std::memcpy(pv, m_data.data() + static_cast<std::size_t>(offset), static_cast<std::size_t>(count));
		m_position = offset + count;
		if (pcbRead)
			*pcbRead = static_cast<ULONG>(count);
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE Write(const void* const pv, const ULONG cb, ULONG* const pcbWritten) override
	{
		++m_writeCalls;

		if (m_failWrites)
		{
			if (pcbWritten)
				*pcbWritten = 0;
			return E_FAIL;
		}

		if (m_zeroWrites)
		{
			// A write that succeeds but makes no progress: a consumer that keeps
			// looping would spin forever, so the writer has to abort.
			if (pcbWritten)
				*pcbWritten = 0;
			return S_OK;
		}

		std::uint64_t count = static_cast<std::uint64_t>(cb);
		if (m_shortWrites && count > 1)
			count /= 2;

		const std::size_t end = static_cast<std::size_t>(m_position + count);
		if (m_data.size() < end)
			m_data.resize(end, 0);

		if (count != 0)
			std::memcpy(m_data.data() + static_cast<std::size_t>(m_position), pv, static_cast<std::size_t>(count));
		m_position += count;
		if (pcbWritten)
			*pcbWritten = static_cast<ULONG>(count);
		return S_OK;
	}

	// --- IStream ---
	HRESULT STDMETHODCALLTYPE Seek(const LARGE_INTEGER dlibMove, const DWORD dwOrigin,
		ULARGE_INTEGER* const plibNewPosition) override
	{
		++m_seekCalls;

		if (m_failSeeks)
			return E_NOTIMPL;

		std::int64_t base = 0;
		switch (dwOrigin)
		{
		case STREAM_SEEK_SET: base = 0; break;
		case STREAM_SEEK_CUR: base = static_cast<std::int64_t>(m_position); break;
		case STREAM_SEEK_END: base = static_cast<std::int64_t>(m_data.size()); break;
		default: return STG_E_INVALIDFUNCTION;
		}

		const std::int64_t target = base + dlibMove.QuadPart;
		if (target < 0)
			return STG_E_INVALIDFUNCTION;

		m_position = static_cast<std::uint64_t>(target);
		if (plibNewPosition)
			plibNewPosition->QuadPart = static_cast<ULONGLONG>(m_position);
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE CopyTo(IStream*, ULARGE_INTEGER, ULARGE_INTEGER*, ULARGE_INTEGER*) override
	{
		return E_NOTIMPL;
	}
	HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return S_OK; }
	HRESULT STDMETHODCALLTYPE Revert() override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE Stat(STATSTG*, DWORD) override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE Clone(IStream**) override { return E_NOTIMPL; }

private:
	std::vector<std::uint8_t> m_data;
	std::uint64_t m_position = 0;
	bool m_failWrites = false;
	bool m_shortWrites = false;
	bool m_zeroWrites = false;
	bool m_failReads = false;
	bool m_shortReads = false;
	bool m_zeroReads = false;
	bool m_failSeeks = false;
	int m_writeCalls = 0;
	int m_readCalls = 0;
	int m_seekCalls = 0;
};

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

// Builds a payload v2 by hand for the "store set / bad header" cases: one
// signature entry and one block with the same type id / data version.
std::vector<std::uint8_t> BuildRawPayload(
	const std::uint32_t savegameId, const std::uint32_t payloadVersion,
	const std::uint32_t typeId, const std::uint32_t dataVersion,
	const std::vector<std::uint8_t>& block)
{
	venture::ExtByteStreamWriter writer;
	writer.WriteU32(savegameId);
	writer.WriteU32(payloadVersion);
	writer.WriteU32(1); // block count
	writer.WriteU32(1); // signature count
	writer.WriteU32(typeId);       // signature entry
	writer.WriteU32(dataVersion);
	writer.WriteU32(typeId);       // block header
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
	Check(trailer.size() == payload.size() + venture::SaveTrailer::HeaderSize,
		"WrapPayload prepends the fixed 16-byte header");

	// ---- 2. frame parsing --------------------------------------------------
	// Header-first frame: 16-byte header [magic][formatVersion][payloadSize][crc32]
	// followed by exactly payloadSize payload bytes.
	{
		std::size_t offset = 0;
		std::size_t size = 0;
		Check(venture::SaveTrailer::Parse(trailer.data(), trailer.size(), offset, size)
				== venture::SaveTrailer::ParseResult::Ok,
			"Parse accepts a well-formed header-first trailer");
		Check(offset == venture::SaveTrailer::HeaderSize && size == payload.size(),
			"Parse reports the payload range after the 16-byte header");
	}
	{
		std::vector<std::uint8_t> corrupt = trailer;
		corrupt[venture::SaveTrailer::HeaderSize + 1] ^= 0xFF; // inside the payload: CRC must catch it
		std::size_t offset = 0;
		std::size_t size = 0;
		Check(venture::SaveTrailer::Parse(corrupt.data(), corrupt.size(), offset, size)
				== venture::SaveTrailer::ParseResult::Corrupt,
			"Parse rejects a payload hit by a bit error (CRC)");
	}
	{
		std::size_t offset = 0;
		std::size_t size = 0;
		Check(venture::SaveTrailer::Parse(trailer.data(), venture::SaveTrailer::HeaderSize - 1, offset, size)
				== venture::SaveTrailer::ParseResult::NoTrailer,
			"Parse reports NoTrailer for a buffer shorter than the header");
		Check(venture::SaveTrailer::Parse(nullptr, 0, offset, size)
				== venture::SaveTrailer::ParseResult::NoTrailer,
			"Parse reports NoTrailer for a null/empty buffer");

		std::vector<std::uint8_t> notOurs(64, 0x5A);
		Check(venture::SaveTrailer::Parse(notOurs.data(), notOurs.size(), offset, size)
				== venture::SaveTrailer::ParseResult::Corrupt,
			"Parse reports Corrupt (not NoTrailer) for a full foreign block");
	}
	{
		std::vector<std::uint8_t> magicBumped = trailer;
		magicBumped[0] ^= 0xFF;
		std::size_t offset = 0;
		std::size_t size = 0;
		Check(venture::SaveTrailer::Parse(magicBumped.data(), magicBumped.size(), offset, size)
				== venture::SaveTrailer::ParseResult::Corrupt,
			"Parse rejects a damaged magic");
	}
	{
		std::vector<std::uint8_t> versionBumped = trailer;
		const std::uint32_t future = venture::SaveTrailer::FormatVersion + 1;
		std::memcpy(versionBumped.data() + 4, &future, sizeof(future));
		std::size_t offset = 0;
		std::size_t size = 0;
		Check(venture::SaveTrailer::Parse(versionBumped.data(), versionBumped.size(), offset, size)
				== venture::SaveTrailer::ParseResult::Corrupt,
			"Parse rejects a future trailer format version");
	}
	{
		// The declared payload size must match the bytes that follow the header
		// exactly: no trailing bytes, no truncation, no oversized claim.
		std::size_t offset = 0;
		std::size_t size = 0;

		std::vector<std::uint8_t> trailing = trailer;
		trailing.push_back(0x5A);
		Check(venture::SaveTrailer::Parse(trailing.data(), trailing.size(), offset, size)
				== venture::SaveTrailer::ParseResult::Corrupt,
			"Parse rejects a frame with trailing bytes");

		std::vector<std::uint8_t> truncated = trailer;
		truncated.pop_back();
		Check(venture::SaveTrailer::Parse(truncated.data(), truncated.size(), offset, size)
				== venture::SaveTrailer::ParseResult::Corrupt,
			"Parse rejects a frame whose payload is shorter than declared");

		std::vector<std::uint8_t> oversized = trailer;
		const std::uint32_t tooLarge = venture::SaveTrailer::MaxPayloadSize + 1;
		std::memcpy(oversized.data() + 8, &tooLarge, sizeof(tooLarge));
		Check(venture::SaveTrailer::Parse(oversized.data(), oversized.size(), offset, size)
				== venture::SaveTrailer::ParseResult::Corrupt,
			"Parse rejects a payloadSize above the 16 MiB cap");
	}
	{
		// An empty payload is a valid frame: header only, zero payload bytes.
		const std::vector<std::uint8_t> emptyTrailer = venture::SaveTrailer::WrapPayload({});
		Check(emptyTrailer.size() == venture::SaveTrailer::HeaderSize,
			"WrapPayload of an empty payload is exactly the header");
		std::size_t offset = 0;
		std::size_t size = 1;
		Check(venture::SaveTrailer::Parse(emptyTrailer.data(), emptyTrailer.size(), offset, size)
				== venture::SaveTrailer::ParseResult::Ok
				&& offset == venture::SaveTrailer::HeaderSize && size == 0,
			"Parse accepts an empty payload frame");
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
		// A store this platform does not know (module removed): the payload v2
		// signature is compared before any block is read, so the whole payload is
		// incompatible instead of being partially applied.
		const std::vector<std::uint8_t> block(8, 0xAB);
		const std::vector<std::uint8_t> unknown = BuildRawPayload(kSavegameId,
			venture::ExtSave::PayloadVersion, 0x7777u, 1, block);
		venture::ExtensionIndex::Instance().ClearAll();
		venture::ExtSave::EnterLoadWindow();
		const venture::ExtSave::LoadSummary unknownStore =
			venture::ExtSave::ReadPayload(kSavegameId, unknown.data(), unknown.size());
		venture::ExtSave::ExitLoadWindow();
		Check(unknownStore.result == venture::ExtSave::LoadSummary::Result::Incompatible
				&& unknownStore.incompatibility
					== venture::ExtSave::LoadSummary::Incompatibility::StoreSet
				&& unknownStore.created == 0,
			"an unknown store is rejected by the payload v2 signature before any block is read");
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
		Check(summary.result == venture::ExtSave::LoadSummary::Result::Incompatible
				&& summary.incompatibility
					== venture::ExtSave::LoadSummary::Incompatibility::PayloadVersion
				&& summary.payloadVersion == venture::ExtSave::PayloadVersion + 1,
			"a payload with another payload version is incompatible (no migration)");

		std::vector<std::uint8_t> wrongStoreVersion = BuildRawPayload(kSavegameId,
			venture::ExtSave::PayloadVersion, SavedUnitExt::TypeId, 99, block);
		summary = venture::ExtSave::ReadPayload(kSavegameId, wrongStoreVersion.data(),
			wrongStoreVersion.size());
		Check(summary.result == venture::ExtSave::LoadSummary::Result::Incompatible
				&& summary.incompatibility == venture::ExtSave::LoadSummary::Incompatibility::StoreSet
				&& summary.created == 0,
			"a store with a different dataVersion is rejected by the signature");

		// A v0.2.0 (payload version 1) payload is refused as a whole: no migration.
		venture::ExtByteStreamWriter v1Writer;
		v1Writer.WriteU32(kSavegameId);
		v1Writer.WriteU32(1);
		const std::vector<std::uint8_t>& v1 = v1Writer.Data();
		summary = venture::ExtSave::ReadPayload(kSavegameId, v1.data(), v1.size());
		Check(summary.result == venture::ExtSave::LoadSummary::Result::Incompatible
				&& summary.incompatibility
					== venture::ExtSave::LoadSummary::Incompatibility::PayloadVersion
				&& summary.payloadVersion == 1,
			"a v1 payload is rejected as incompatible (no migration)");
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

	// ---- 6. IStream writer/reader (0x67D1B4 / 0x67F7C8, sequential only) ---
	// The engine's savegame stream is a non-seekable streaming wrapper, so the writer
	// appends at the current position and the reader consumes the frame sequentially:
	// neither may depend on Seek.  These cases drive both through the real IStream
	// vtable with a fully non-seekable fake stream.
	{
		venture::ExtensionIndex::Instance().ClearAll();
		UnitClass writeUnit;
		SavedUnitExt* const writeExt = store.Allocate(&writeUnit);
		Check(writeExt != nullptr, "the stream test has a saveable extension");
		writeExt->Serial = 4242;

		std::vector<std::uint8_t> savedPayload;
		const bool savedOk = venture::ExtSave::WritePayload(kSavegameId, savedPayload, nullptr);
		const std::vector<std::uint8_t> savedTrailer = venture::SaveTrailer::WrapPayload(savedPayload);
		Check(savedOk && !savedTrailer.empty(), "the stream test builds a non-empty trailer");

		// The engine wrote its save data; the stream is a non-seekable wrapper and its
		// position is at the end of that data when our save handler runs.
		std::vector<std::uint8_t> engineBytes;
		for (std::uint32_t i = 0; i < 24; ++i)
			engineBytes.push_back(static_cast<std::uint8_t>(0x40 + i));

		FakeStream written(engineBytes);
		written.SetFailSeeks(true);
		written.SkipTo(engineBytes.size());
		venture::SaveTrailer::Write(&written);
		Check(written.SeekCalls() == 1,
			"the writer only probes the stream position once (for the log)");
		Check(written.Data().size() == engineBytes.size() + savedTrailer.size(),
			"a non-seekable stream is appended to sequentially (no Seek dependency)");
		Check(std::equal(engineBytes.begin(), engineBytes.end(), written.Data().begin()),
			"the writer does not disturb the engine's save data");
		Check(std::equal(savedTrailer.begin(), savedTrailer.end(),
				written.Data().begin() + static_cast<std::ptrdiff_t>(engineBytes.size())),
			"the appended bytes are the wrapped header-first trailer");
		Check(written.Position() == static_cast<std::uint64_t>(written.Data().size()),
			"the writer leaves the stream position at the end of the appended frame");

		// Round trip through the same non-seekable stream: a "new load" finds the
		// stream exactly at the start of our frame (the other platforms' sequential
		// readers already consumed their blocks), so the reader must not Seek either.
		venture::ExtensionIndex::Instance().ClearAll();
		FakeStream loaded(written.Data());
		loaded.SetFailSeeks(true);
		loaded.SkipTo(engineBytes.size());
		venture::ExtSave::EnterLoadWindow();
		venture::SaveTrailer::Read(&loaded);
		Check(loaded.SeekCalls() == 0, "the reader does not call Seek at all");
		Check(loaded.Position() == static_cast<std::uint64_t>(loaded.Data().size()),
			"the reader consumes exactly the frame (the stream ends after it)");
		Check(venture::ExtensionIndex::Instance().LoadedExtensions().size() == 1,
			"a non-seekable stream restores the stored extension instance");
		const venture::ExtSave::LoadSummary adopted = venture::ExtSave::FinishLoad();
		venture::ExtSave::ExitLoadWindow();
		Check(adopted.adopted == 1 && adopted.failed == 0,
			"the extension read from a non-seekable stream is adopted");
		SavedUnitExt* const restored = store.Fetch(&writeUnit);
		Check(restored != nullptr && restored->Serial == 4242,
			"the extension state round-trips without any Seek");

		// Short writes (legal for COM) are retried until the whole frame is out.
		{
			const std::vector<std::uint8_t> base(16, 0x22);
			FakeStream shortWrite(base);
			shortWrite.SkipTo(base.size());
			shortWrite.SetShortWrites(true);
			venture::SaveTrailer::Write(&shortWrite);
			std::size_t offset = 0;
			std::size_t payloadSize = 0;
			Check(shortWrite.WriteCalls() > 1
					&& venture::SaveTrailer::Parse(shortWrite.Data().data() + base.size(),
						shortWrite.Data().size() - base.size(), offset, payloadSize)
						== venture::SaveTrailer::ParseResult::Ok,
				"short writes are retried until the complete frame is out");
		}

		// A failed write or a write that reports no progress fails cleanly.
		{
			const std::vector<std::uint8_t> base(16, 0x22);
			FakeStream failed(base);
			failed.SkipTo(base.size());
			failed.SetFailWrites(true);
			venture::SaveTrailer::Write(&failed);
			Check(failed.Data() == base, "a failed write leaves the stream unchanged");

			FakeStream noProgress(base);
			noProgress.SkipTo(base.size());
			noProgress.SetZeroWrites(true);
			venture::SaveTrailer::Write(&noProgress);
			Check(noProgress.Data() == base && noProgress.WriteCalls() == 1,
				"a write that makes no progress aborts instead of spinning");
		}

		// No frame at the read position: nothing is restored.  This is the normal
		// case for a save written without this platform.
		{
			FakeStream noBlock(engineBytes);
			noBlock.SkipTo(engineBytes.size());
			venture::ExtensionIndex::Instance().EnterLoadWindow();
			venture::SaveTrailer::Read(&noBlock);
			venture::ExtensionIndex::Instance().ExitLoadWindow();
			Check(venture::ExtensionIndex::Instance().LoadedExtensions().empty(),
				"no bytes after the ecosystem blocks: no extension state restored");

			FakeStream empty;
			venture::ExtensionIndex::Instance().EnterLoadWindow();
			venture::SaveTrailer::Read(&empty);
			venture::ExtensionIndex::Instance().ExitLoadWindow();
			Check(venture::ExtensionIndex::Instance().LoadedExtensions().empty(),
				"an empty stream: no extension state restored");

			FakeStream foreign(std::vector<std::uint8_t>(32, 0x5A));
			venture::ExtensionIndex::Instance().EnterLoadWindow();
			venture::SaveTrailer::Read(&foreign);
			venture::ExtensionIndex::Instance().ExitLoadWindow();
			Check(foreign.Position() == venture::SaveTrailer::HeaderSize,
				"a foreign block is consumed up to its header, nothing more");
			Check(venture::ExtensionIndex::Instance().LoadedExtensions().empty(),
				"a foreign block (magic mismatch) restores no extension state");
		}

		// Corrupted frame fields: every case drops the extension state, never the
		// game, and never reads beyond what the header claims.
		{
			std::vector<std::uint8_t> bytes(engineBytes);
			bytes.insert(bytes.end(), savedTrailer.begin(), savedTrailer.end());
			const std::size_t frameStart = engineBytes.size();

			std::vector<std::uint8_t> badVersion = bytes;
			const std::uint32_t future = venture::SaveTrailer::FormatVersion + 1;
			std::memcpy(badVersion.data() + frameStart + 4, &future, sizeof(future));

			std::vector<std::uint8_t> badSize = bytes;
			const std::uint32_t tooLarge = venture::SaveTrailer::MaxPayloadSize + 1;
			std::memcpy(badSize.data() + frameStart + 8, &tooLarge, sizeof(tooLarge));

			std::vector<std::uint8_t> badCrc = bytes;
			badCrc[frameStart + venture::SaveTrailer::HeaderSize + 1] ^= 0xFF;

			std::vector<std::uint8_t> truncatedPayload = bytes;
			truncatedPayload.pop_back();

			std::vector<std::uint8_t> truncatedHeader = bytes;
			truncatedHeader.resize(frameStart + 8);

			const std::vector<std::uint8_t>* const damaged[] = {
				&badVersion, &badSize, &badCrc, &truncatedPayload, &truncatedHeader,
			};
			for (const std::vector<std::uint8_t>* const frame : damaged)
			{
				FakeStream stream(*frame);
				stream.SkipTo(frameStart);
				venture::ExtensionIndex::Instance().EnterLoadWindow();
				venture::SaveTrailer::Read(&stream);
				venture::ExtensionIndex::Instance().ExitLoadWindow();
				Check(venture::ExtensionIndex::Instance().LoadedExtensions().empty(),
					"a damaged frame restores no extension state");
			}
		}

		// Legal short reads (and no-progress / failed reads) in the sequential reader.
		{
			std::vector<std::uint8_t> bytes(engineBytes);
			bytes.insert(bytes.end(), savedTrailer.begin(), savedTrailer.end());
			const std::size_t frameStart = engineBytes.size();

			FakeStream shortRead(bytes);
			shortRead.SetFailSeeks(true);
			shortRead.SetShortReads(true);
			shortRead.SkipTo(frameStart);
			venture::ExtensionIndex::Instance().EnterLoadWindow();
			venture::SaveTrailer::Read(&shortRead);
			Check(shortRead.SeekCalls() == 0
					&& venture::ExtensionIndex::Instance().LoadedExtensions().size() == 1,
				"a short-reading non-seekable stream restores the extension state");
			(void)venture::ExtSave::FinishLoad();
			venture::ExtensionIndex::Instance().ExitLoadWindow();
			venture::ExtensionIndex::Instance().ClearAll();

			FakeStream zeroRead(bytes);
			zeroRead.SkipTo(frameStart);
			zeroRead.SetZeroReads(true);
			venture::ExtensionIndex::Instance().EnterLoadWindow();
			venture::SaveTrailer::Read(&zeroRead);
			venture::ExtensionIndex::Instance().ExitLoadWindow();
			Check(venture::ExtensionIndex::Instance().LoadedExtensions().empty(),
				"a no-progress read drops the extension state instead of spinning");

			FakeStream failedRead(bytes);
			failedRead.SkipTo(frameStart);
			failedRead.SetFailReads(true);
			venture::ExtensionIndex::Instance().EnterLoadWindow();
			venture::SaveTrailer::Read(&failedRead);
			venture::ExtensionIndex::Instance().ExitLoadWindow();
			Check(venture::ExtensionIndex::Instance().LoadedExtensions().empty(),
				"a failed read drops the extension state");
		}

		venture::ExtensionIndex::Instance().ClearAll();
	}

	// ---- 7. an old save without a trailer (A-16) --------------------------
	// The engine loads its objects and their constructors allocate extensions while
	// the load window is open.  A save written without this platform has no
	// trailer, so nothing is restored - but the deferred allocations must still get
	// fresh extensions in FinishLoad.
	{
		venture::ExtensionIndex::Instance().ClearAll();
		FakeStream noTrailer(std::vector<std::uint8_t>(32, 0x5A)); // another platform's data only
		// The other platforms' sequential readers consumed their blocks: our read
		// position is the end of the stream, where no frame starts.
		noTrailer.SkipTo(noTrailer.Data().size());
		venture::ExtSave::EnterLoadWindow();
		UnitClass deferredUnit;
		Check(store.Allocate(&deferredUnit) == nullptr && store.Size() == 0,
			"no trailer: allocations during the load window are deferred");
		venture::SaveTrailer::Read(&noTrailer);
		Check(venture::ExtensionIndex::Instance().LoadedExtensions().empty(),
			"no trailer: no extension state is restored");
		const venture::ExtSave::LoadSummary finished = venture::ExtSave::FinishLoad();
		venture::ExtSave::ExitLoadWindow();
		Check(finished.adopted == 0 && finished.deferred == 1,
			"no trailer: the deferred owner gets a fresh extension");
		SavedUnitExt* const fresh = store.Fetch(&deferredUnit);
		Check(fresh != nullptr && fresh->Serial == 0 && fresh->DeferredCalls == 1,
			"no trailer: the deferred extension is fresh and initialised exactly once");
		venture::ExtensionIndex::Instance().ClearAll();
	}

	std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAILED",
		g_failures, g_failures == 1 ? "" : "s");
	return g_failures == 0 ? 0 : 1;
}
