// Unit test: savegame trailer format and the extension save/load round trip.
//
// Runs as a plain console program: the savegame layer is engine-free, the test
// provides a minimal stand-in for the owner classes and a fake swizzle manager
// (the engine's pointer-remap pass) so a whole save -> new process -> load cycle
// can be simulated in one process.  The platform writer/reader are also driven
// through a minimal in-memory IStream implementation, so the end-of-stream seek,
// the reported byte counts and the stream-position restore are covered.
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

LARGE_INTEGER MoveTo(const long long offset)
{
	LARGE_INTEGER move{};
	move.QuadPart = offset;
	return move;
}

// --- fake IStream ----------------------------------------------------------
// Minimal in-memory COM stream.  The savegame writer/reader go through the real
// IStream vtable, so the end-of-stream seek, the written/read byte counts and the
// stream-position restore are exercised without the engine.  The failure switches
// model a non-seekable stream, a failing / partial Write and a failing / short
// Read (COM allows both, the reader has to cope).
class FakeStream final : public IStream
{
public:
	explicit FakeStream(std::vector<std::uint8_t> initial = {})
		: m_data(std::move(initial)) {}

	void SetFailWrites(const bool fail) { m_failWrites = fail; }
	void SetShortWrites(const bool shortWrites) { m_shortWrites = shortWrites; }
	void SetFailReads(const bool fail) { m_failReads = fail; }
	void SetShortReads(const bool shortReads) { m_shortReads = shortReads; }
	void SetFailSeeks(const bool fail) { m_failSeeks = fail; }

	const std::vector<std::uint8_t>& Data() const { return m_data; }
	std::uint64_t Position() const { return m_position; }
	int WriteCalls() const { return m_writeCalls; }
	int ReadCalls() const { return m_readCalls; }

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
	bool m_failReads = false;
	bool m_shortReads = false;
	bool m_failSeeks = false;
	int m_writeCalls = 0;
	int m_readCalls = 0;
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

	// ---- 6. IStream writer/reader (0x67D1B4 write site, A-12) -------------
	// The write site appends the trailer to the engine's save stream through the
	// real IStream interface: these tests drive the writer and the reader with an
	// in-memory stream and check the end-of-stream seek, the failure paths and the
	// stream-position restore.
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

		// The engine wrote its save data; another platform left the position at the
		// stream start.  The writer must seek to EOF and append, not overwrite.
		std::vector<std::uint8_t> engineBytes;
		for (std::uint32_t i = 0; i < 24; ++i)
			engineBytes.push_back(static_cast<std::uint8_t>(0x40 + i));

		FakeStream written(engineBytes);
		written.Seek(MoveTo(0), STREAM_SEEK_SET, nullptr);
		venture::SaveTrailer::Write(&written);
		Check(written.Data().size() == engineBytes.size() + savedTrailer.size(),
			"the writer appends exactly one trailer at the end of the stream");
		Check(std::equal(engineBytes.begin(), engineBytes.end(), written.Data().begin()),
			"the writer does not overwrite the engine's save data");
		Check(std::equal(savedTrailer.begin(), savedTrailer.end(),
				written.Data().begin() + static_cast<std::ptrdiff_t>(engineBytes.size())),
			"the appended bytes are the wrapped trailer");
		Check(written.Position() == static_cast<std::uint64_t>(written.Data().size()),
			"the writer leaves the stream position at EOF");

		// Round trip: a "new load" reads the written stream and restores the state;
		// the position another platform left in the stream is restored afterwards.
		venture::ExtensionIndex::Instance().ClearAll();
		const std::size_t enginePosition = 9;
		written.Seek(MoveTo(static_cast<long long>(enginePosition)), STREAM_SEEK_SET, nullptr);
		venture::ExtSave::EnterLoadWindow();
		venture::SaveTrailer::Read(&written);
		Check(written.Position() == enginePosition, "the reader restores the stream position");
		Check(venture::ExtensionIndex::Instance().LoadedExtensions().size() == 1,
			"the reader creates the stored extension instance");
		const venture::ExtSave::LoadSummary adopted = venture::ExtSave::FinishLoad();
		venture::ExtSave::ExitLoadWindow();
		Check(adopted.adopted == 1 && adopted.failed == 0,
			"the extension read through the IStream is adopted");
		SavedUnitExt* const restored = store.Fetch(&writeUnit);
		Check(restored != nullptr && restored->Serial == 4242,
			"the extension state round-trips through the IStream");
		venture::ExtensionIndex::Instance().ClearAll();

		// A non-seekable stream is not written to (and the engine data stays intact).
		{
			const std::vector<std::uint8_t> base(16, 0x11);
			FakeStream stream(base);
			stream.SetFailSeeks(true);
			venture::SaveTrailer::Write(&stream);
			Check(stream.Data() == base && stream.WriteCalls() == 0,
				"a non-seekable stream is left untouched by the writer");
		}

		// A failed write is detected; a short write is detected through the
		// reported byte count instead of being treated as success.
		{
			const std::vector<std::uint8_t> base(16, 0x22);

			FakeStream failed(base);
			failed.SetFailWrites(true);
			venture::SaveTrailer::Write(&failed);
			Check(failed.Data() == base, "a failed stream write leaves the engine data untouched");

			FakeStream shortWrite(base);
			shortWrite.SetShortWrites(true);
			venture::SaveTrailer::Write(&shortWrite);
			Check(shortWrite.Data().size() > base.size()
					&& shortWrite.Data().size() < base.size() + savedTrailer.size(),
				"a short write is detected from the reported byte count");
		}

		// Read failures (short read / error / non-seekable) restore the position and
		// leave the extension state empty instead of aborting the game's load.
		{
			std::vector<std::uint8_t> bytes(engineBytes);
			bytes.insert(bytes.end(), savedTrailer.begin(), savedTrailer.end());
			const std::size_t positionBefore = 5;

			FakeStream shortRead(bytes);
			shortRead.SetShortReads(true);
			shortRead.Seek(MoveTo(static_cast<long long>(positionBefore)), STREAM_SEEK_SET, nullptr);
			venture::ExtensionIndex::Instance().EnterLoadWindow();
			venture::SaveTrailer::Read(&shortRead);
			venture::ExtensionIndex::Instance().ExitLoadWindow();
			Check(shortRead.Position() == positionBefore, "a short read restores the stream position");
			Check(venture::ExtensionIndex::Instance().LoadedExtensions().empty(),
				"a short read restores no extension state");

			FakeStream failedRead(bytes);
			failedRead.SetFailReads(true);
			failedRead.Seek(MoveTo(static_cast<long long>(positionBefore)), STREAM_SEEK_SET, nullptr);
			venture::ExtensionIndex::Instance().EnterLoadWindow();
			venture::SaveTrailer::Read(&failedRead);
			venture::ExtensionIndex::Instance().ExitLoadWindow();
			Check(failedRead.Position() == positionBefore, "a read error restores the stream position");
			Check(venture::ExtensionIndex::Instance().LoadedExtensions().empty(),
				"a read error restores no extension state");

			FakeStream noSeek(bytes);
			noSeek.SetFailSeeks(true);
			venture::ExtensionIndex::Instance().EnterLoadWindow();
			venture::SaveTrailer::Read(&noSeek);
			venture::ExtensionIndex::Instance().ExitLoadWindow();
			Check(noSeek.Position() == 0 && noSeek.ReadCalls() == 0,
				"the reader leaves a non-seekable stream alone");
			Check(venture::ExtensionIndex::Instance().LoadedExtensions().empty(),
				"a non-seekable stream restores no extension state");
		}

		venture::ExtensionIndex::Instance().ClearAll();
	}

	std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAILED",
		g_failures, g_failures == 1 ? "" : "s");
	return g_failures == 0 ? 0 : 1;
}
