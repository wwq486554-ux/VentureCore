// Unit test: malformed-payload safety for the extension savegame layer.
//
// Covers the A-01/A-03 hardening of the savegame payload:
//   * swizzle registrations are staged while a payload is parsed and are only
//     committed after the whole payload validated.  A rejected payload must leave
//     the engine-side fake with ZERO registrations, because the instances created
//     from the partial payload are deleted - registering (or deleting after
//     registering) would make the engine's remap pass touch freed memory;
//   * the byte-stream bounds check rejects truncated reads and sizes that would
//     wrap the 32-bit `offset + size` comparison (0xFFFFFFF8).
//
// The fake swizzle manager below is strict on purpose: like the engine it fails
// the remap pass when a registered slot has no matching identity, and it records
// every registration so the rejection tests can assert "nothing reached it".
//
// TEST_DEPS: src/Ext/AbstractExt.cpp src/Ext/ExtensionIndex.cpp src/Ext/ExtSave.cpp

#include <cstddef>
#include <cstdint>
#include <cstdio>
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

#include "Ext/ExtHierarchy.h"
#include "Ext/ExtensionIndex.h"
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

// --- strict fake swizzle manager ------------------------------------------
// g_commandCount counts every call the fake receives, so a rejection test can
// assert that the engine was not touched at all.  FakeProcess() emulates the
// engine's remap pass and reports a registered slot without a matching identity
// as a failure (the engine's no-match branch is a divide-by-zero trap).
std::vector<void**> g_changedSlots;
std::unordered_map<void*, void*> g_identities;
int g_commandCount = 0;
int g_applyFailures = 0;

void FakeRegisterForChange(void** const pointer)
{
	++g_commandCount;
	g_changedSlots.push_back(pointer);
}

void FakeRegisterIdentity(void* const was, void* const is)
{
	++g_commandCount;
	g_identities[was] = is;
}

void FakeReset()
{
	g_changedSlots.clear();
	g_identities.clear();
	g_commandCount = 0;
	g_applyFailures = 0;
}

bool FakeProcess()
{
	bool applied = true;

	for (void** const slot : g_changedSlots)
	{
		const auto it = g_identities.find(*slot);
		if (it == g_identities.end())
		{
			++g_applyFailures;
			applied = false;
			continue;
		}

		*slot = it->second;
	}

	g_changedSlots.clear();
	g_identities.clear();
	return applied;
}

// --- extension under test -------------------------------------------------
class PayloadUnitExt final : public venture::UnitExt
{
public:
	static constexpr std::uint32_t TypeId = 0x0111u;
	static constexpr std::uint32_t BaseTypeId = venture::UnitExt::TypeId;

	static inline int Constructions = 0;
	static inline int Destructions = 0;

	explicit PayloadUnitExt(AbstractClass* const owner) : UnitExt(owner) { ++Constructions; }

	~PayloadUnitExt() override { ++Destructions; }

	std::uint32_t ExtTypeId() const override { return TypeId; }

	std::uint32_t Serial = 0;
	AbstractClass* Target = nullptr;

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
};

using PayloadUnitStore = venture::ExtensionStore<UnitClass, PayloadUnitExt>;

constexpr std::uint32_t kCanary = 0x56430000u | (PayloadUnitExt::TypeId & 0xFFFFu);
constexpr std::uint32_t kDataVersion = 1;

using Bytes = std::vector<std::uint8_t>;

// --- payload builders -----------------------------------------------------
void AppendU32(Bytes& out, const std::uint32_t value)
{
	out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
	out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
	out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFFu));
	out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFFu));
}

// Appends one store row: [extSaveAddress][ownerSaveAddress][canary][dataSize][data]
void AppendRow(Bytes& rows, const std::uint32_t extAddress, const std::uint32_t ownerAddress,
	const std::uint32_t canary, const Bytes& data)
{
	AppendU32(rows, extAddress);
	AppendU32(rows, ownerAddress);
	AppendU32(rows, canary);
	AppendU32(rows, static_cast<std::uint32_t>(data.size()));
	rows.insert(rows.end(), data.begin(), data.end());
}

// One row's data as the extension's Load() reads it: [serial][target pointer].
Bytes BuildRowData(const std::uint32_t serial, const std::uint32_t targetAddress)
{
	Bytes data;
	AppendU32(data, serial);
	AppendU32(data, targetAddress);
	return data;
}

Bytes BuildBlock(const std::uint32_t rowCount, const Bytes& rows)
{
	venture::ExtByteStreamWriter writer;
	writer.WriteU32(rowCount);
	if (!rows.empty())
		writer.Write(rows.data(), rows.size());
	return writer.Data();
}

// Writes a payload with the store block layout of ExtSave.h, one typeId/dataVersion
// pair per block (all our store).  Deliberately hand-built so a test can produce
// blocks that a correct writer would never emit.
Bytes BuildPayload(const std::vector<Bytes>& blocks)
{
	venture::ExtByteStreamWriter writer;
	writer.WriteU32(kSavegameId);
	writer.WriteU32(venture::ExtSave::PayloadVersion);
	writer.WriteU32(static_cast<std::uint32_t>(blocks.size()));

	for (const Bytes& block : blocks)
	{
		writer.WriteU32(PayloadUnitExt::TypeId);
		writer.WriteU32(kDataVersion);
		writer.WriteU32(static_cast<std::uint32_t>(block.size()));
		if (!block.empty())
			writer.Write(block.data(), block.size());
	}

	return writer.Data();
}

// Runs ReadPayload on a payload that must be rejected and checks the safety
// invariants.  Returns the number of extension instances created on the way.
int ExpectRejected(const Bytes& payload, const char* const what)
{
	venture::ExtensionIndex::Instance().ClearAll();
	FakeReset();

	const int constructedBefore = PayloadUnitExt::Constructions;
	const int destroyedBefore = PayloadUnitExt::Destructions;

	venture::ExtSave::EnterLoadWindow();
	const venture::ExtSave::LoadSummary summary = venture::ExtSave::ReadPayload(
		kSavegameId, payload.data(), payload.size());
	venture::ExtSave::ExitLoadWindow();

	Check(summary.result == venture::ExtSave::LoadSummary::Result::Invalid, what);
	Check(g_commandCount == 0,
		"rejected payload: ZERO registrations reached the engine swizzle manager");
	Check(!venture::ExtSwizzle::IsStaging(), "rejected payload: staging mode is closed again");
	Check(venture::ExtensionIndex::Instance().ExtCount() == 0
			&& PayloadUnitStore::Instance().Size() == 0
			&& venture::ExtensionIndex::Instance().LoadedExtensions().empty(),
		"rejected payload: no extension is attached or left 'loaded'");
	Check(PayloadUnitExt::Constructions - constructedBefore
			== PayloadUnitExt::Destructions - destroyedBefore,
		"rejected payload: every instance created on the way was deleted");

	return PayloadUnitExt::Constructions - constructedBefore;
}

// --- A-03: ExtByteStreamReader bounds check -------------------------------
void TestReadBytesBounds()
{
	std::uint8_t buffer[64] = {};
	std::uint8_t sink[64] = {};

	// A read that ends exactly at the end succeeds; the next byte fails.
	{
		venture::ExtByteStreamReader reader(buffer, 8);
		Check(reader.ReadBytes(sink, 8) && reader.Ok() && reader.Remaining() == 0,
			"ReadBytes accepts a read that ends exactly at the end of the stream");
		Check(!reader.ReadBytes(sink, 1) && !reader.Ok(),
			"ReadBytes rejects a read past the end");
		Check(reader.Offset() == 8 && reader.Remaining() == 0,
			"a rejected read does not move the stream position");
	}

	// size > remaining (no wrap yet).
	{
		venture::ExtByteStreamReader reader(buffer, sizeof(buffer));
		Check(reader.ReadBytes(sink, 8) && reader.Remaining() == 56,
			"ReadBytes consumes an in-bounds read");
		Check(!reader.ReadBytes(sink, 57) && !reader.Ok(),
			"ReadBytes rejects a size larger than the remaining bytes");
		Check(reader.Offset() == 8 && reader.Remaining() == 56,
			"the oversized read leaves offset and remaining untouched");
	}

	// 32-bit wrap: 8 + 0xFFFFFFF8 wraps to 0, so the old `offset + size` guard let
	// the 4 GiB read through and memcpy'd off the end of the buffer.
	{
		venture::ExtByteStreamReader reader(buffer, sizeof(buffer));
		Check(reader.ReadBytes(sink, 8), "stream positioned for the wrap test");
		Check(!reader.ReadBytes(sink, static_cast<std::size_t>(0xFFFFFFF8u)) && !reader.Ok(),
			"ReadBytes rejects a size that would wrap the 32-bit offset+size guard");
		Check(reader.Offset() == 8 && reader.Remaining() == 56,
			"the wrapping read consumes nothing");
	}

	// 0xFFFFFFFF at offset 0 also wraps the old guard.
	{
		venture::ExtByteStreamReader reader(buffer, sizeof(buffer));
		Check(!reader.ReadBytes(sink, static_cast<std::size_t>(0xFFFFFFFFu)) && !reader.Ok(),
			"ReadBytes rejects 0xFFFFFFFF on a fresh stream");
	}

	// Zero-byte reads stay legal at any position, including the end.
	{
		venture::ExtByteStreamReader reader(buffer, sizeof(buffer));
		Check(reader.ReadBytes(sink, 0) && reader.Ok(), "a zero-byte read succeeds");
	}
}

// --- ExtSwizzle staging API -----------------------------------------------
void TestStagingApi()
{
	FakeReset();

	int dummyA = 0;
	int dummyB = 0;
	void* slot = &dummyA;
	void** const slotAddress = &slot;

	venture::ExtSwizzle::BeginStaging();
	Check(venture::ExtSwizzle::IsStaging(), "BeginStaging opens a staging window");
	venture::ExtSwizzle::RegisterForChange(slotAddress);
	venture::ExtSwizzle::RegisterIdentity(&dummyA, &dummyB);
	Check(g_commandCount == 0 && g_changedSlots.empty() && g_identities.empty(),
		"staged registrations are held back from the engine adapter");

	venture::ExtSwizzle::DiscardStaged();
	Check(!venture::ExtSwizzle::IsStaging() && g_commandCount == 0 && g_changedSlots.empty()
			&& g_identities.empty(),
		"DiscardStaged drops the staged registrations without touching the engine");

	venture::ExtSwizzle::BeginStaging();
	venture::ExtSwizzle::RegisterForChange(slotAddress);
	venture::ExtSwizzle::RegisterIdentity(&dummyA, &dummyB);
	venture::ExtSwizzle::CommitStaged();
	Check(!venture::ExtSwizzle::IsStaging() && g_commandCount == 2
			&& g_changedSlots.size() == 1 && g_identities.size() == 1,
		"CommitStaged hands exactly the staged registrations to the engine");

	// Outside a staging window the calls go straight to the adapter (old behavior).
	FakeReset();
	venture::ExtSwizzle::RegisterForChange(slotAddress);
	venture::ExtSwizzle::RegisterIdentity(&dummyA, &dummyB);
	Check(g_commandCount == 2 && g_changedSlots.size() == 1 && g_identities.size() == 1,
		"outside staging the registrations reach the engine immediately");
	FakeReset();
}

// --- malformed payloads ---------------------------------------------------
void TestRejectedPayloads()
{
	// 1. Payload cut inside its fixed header.
	{
		Bytes payload;
		AppendU32(payload, kSavegameId);
		payload.push_back(0x01); // only 1 of the 4 version bytes
		ExpectRejected(payload, "a payload truncated in its fixed header is rejected");
	}

	// 2. Payload cut inside a block header.
	{
		Bytes payload;
		AppendU32(payload, kSavegameId);
		AppendU32(payload, venture::ExtSave::PayloadVersion);
		AppendU32(payload, 1);
		AppendU32(payload, PayloadUnitExt::TypeId);
		AppendU32(payload, kDataVersion); // blockSize missing
		ExpectRejected(payload, "a payload truncated inside a block header is rejected");
	}

	// 3. blockSize promises more bytes than the payload still carries.
	{
		venture::ExtByteStreamWriter writer;
		writer.WriteU32(kSavegameId);
		writer.WriteU32(venture::ExtSave::PayloadVersion);
		writer.WriteU32(1);
		writer.WriteU32(PayloadUnitExt::TypeId);
		writer.WriteU32(kDataVersion);
		writer.WriteU32(64); // blockSize
		writer.WriteU32(1);  // ...but only 4 block bytes follow
		ExpectRejected(writer.Data(), "a block whose size exceeds the remaining payload is rejected");
	}

	// 4. Row cut inside its header (rowCount says two rows, one is present).
	{
		Bytes rows;
		AppendRow(rows, 0x1000, 0x2000, kCanary, BuildRowData(7, 0x3000));
		const Bytes payload = BuildPayload({ BuildBlock(2, rows) });
		Check(ExpectRejected(payload, "a row truncated in its header is rejected") == 1,
			"the truncated-row-header case created (then deleted) exactly one instance");
	}

	// 5. Bad canary: rejected before any instance is constructed.
	{
		Bytes rows;
		AppendRow(rows, 0x1000, 0x2000, kCanary ^ 0xFFFFu, BuildRowData(7, 0x3000));
		const Bytes payload = BuildPayload({ BuildBlock(1, rows) });
		Check(ExpectRejected(payload, "a row with a bad canary is rejected") == 0,
			"the bad-canary case is rejected before constructing an instance");
	}

	// 6. Row whose Load() runs past the row data: the instance exists and its
	//    registrations are staged when the row fails - the row-level delete point
	//    from A-01.  Deleting it must not leave anything registered in the engine.
	{
		Bytes data(4, 0u); // Load() reads a u32 serial + a u32 pointer = 8 bytes
		Bytes rows;
		AppendRow(rows, 0x1000, 0x2000, kCanary, data);
		const Bytes payload = BuildPayload({ BuildBlock(1, rows) });
		Check(ExpectRejected(payload, "a row whose Load runs past its data is rejected") == 1,
			"the truncated-row-data case exercised create-then-delete with zero registrations");
	}

	// 7. First block valid, second block corrupt: the successfully loaded instance
	//    from block one must be deleted (before deletion its registrations are
	//    discarded), and nothing may be registered.
	{
		Bytes validRows;
		AppendRow(validRows, 0x1000, 0x2000, kCanary, BuildRowData(42, 0x3000));
		Bytes badRows;
		AppendRow(badRows, 0x4000, 0x5000, kCanary ^ 0x01u, BuildRowData(9, 0x6000));
		const Bytes payload = BuildPayload({ BuildBlock(1, validRows), BuildBlock(1, badRows) });
		Check(ExpectRejected(payload, "a corrupt later block rejects the whole payload") == 1,
			"the partially loaded instance is deleted and never registered");
	}
}

// --- happy path: the staged registrations are committed and used ----------
void TestSuccessPath()
{
	PayloadUnitStore& store = PayloadUnitStore::Instance();

	venture::ExtensionIndex::Instance().ClearAll();
	FakeReset();

	// First process: one live extension holding a pointer to another object.
	UnitClass oldOwner;
	UnitClass oldTarget;
	PayloadUnitExt* const oldExt = store.Allocate(&oldOwner);
	Check(oldExt != nullptr, "success path: the first process allocates an extension");
	if (!oldExt)
		return;

	oldExt->Serial = 42;
	oldExt->Target = &oldTarget;

	Bytes payload;
	std::uint32_t storeBlocks = 0;
	Check(venture::ExtSave::WritePayload(kSavegameId, payload, &storeBlocks) && storeBlocks == 1,
		"success path: WritePayload produces one store block");

	// Second process: the engine loaded the objects at new addresses.
	venture::ExtensionIndex::Instance().ClearAll();
	FakeReset();

	UnitClass newOwner;
	UnitClass newTarget;

	venture::ExtSave::EnterLoadWindow();
	const venture::ExtSave::LoadSummary summary = venture::ExtSave::ReadPayload(
		kSavegameId, payload.data(), payload.size());
	Check(summary.result == venture::ExtSave::LoadSummary::Result::Loaded && summary.created == 1,
		"success path: the payload recreates the stored extension");
	Check(!venture::ExtSwizzle::IsStaging(), "success path: staging is committed and closed");
	Check(g_commandCount == 3 && g_changedSlots.size() == 2 && g_identities.size() == 1,
		"success path: identity + owner slot + saved pointer registrations are committed");

	// The engine registers its loaded object identities; the strict fake then runs
	// the remap pass, which must match every registration.
	FakeRegisterIdentity(&oldOwner, &newOwner);
	FakeRegisterIdentity(&oldTarget, &newTarget);
	Check(FakeProcess() && g_applyFailures == 0,
		"success path: every committed registration matches an identity in the strict fake");

	const venture::ExtSave::LoadSummary finished = venture::ExtSave::FinishLoad();
	venture::ExtSave::ExitLoadWindow();
	Check(finished.adopted == 1 && finished.failed == 0 && finished.deferred == 0,
		"success path: the restored extension is adopted exactly once");

	PayloadUnitExt* const newExt = store.Fetch(&newOwner);
	Check(newExt != nullptr && newExt->Serial == 42,
		"success path: the extension state is restored");
	Check(newExt != nullptr && newExt->Target == &newTarget,
		"success path: the saved pointer is swizzled to the loaded object");
}
}

int __cdecl main()
{
	venture::ExtSwizzle::Install(&FakeRegisterForChange, &FakeRegisterIdentity);

	PayloadUnitStore& store = PayloadUnitStore::Instance();
	store.SetName("UnitClass/PayloadUnitExt");
	store.SetSaveable(true, kDataVersion);

	TestReadBytesBounds();
	TestStagingApi();
	TestRejectedPayloads();
	TestSuccessPath();

	std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAILED",
		g_failures, g_failures == 1 ? "" : "s");
	return g_failures == 0 ? 0 : 1;
}
