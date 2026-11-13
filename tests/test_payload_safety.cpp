// Unit test: structurally-malformed-payload safety for the extension savegame
// layer.
//
// The savegame is trusted input: this suite does NOT promise that a structurally
// valid, CRC-correct forged payload is harmless (a forged non-zero owner address
// still traps in the engine's remap pass, and the row/byte limits only bound the
// resources one block can claim).  What is covered is the *structural* hardening
// - truncated/short fields, bogus sizes, trailing bytes and an impossible row
// count are rejected before they can corrupt memory or grow without bound (see
// docs/SaveFormat.md for the trust boundary).  Concretely it covers the
// A-01/A-02/A-03/A-05/A-11 hardening plus the SAVE-05 load limits:
//   * swizzle registrations are staged while a payload is parsed and are only
//     committed after the whole payload validated.  A rejected payload must leave
//     the engine-side fake with ZERO registrations, because the instances created
//     from the partial payload are deleted - registering (or deleting after
//     registering) would make the engine's remap pass touch freed memory;
//   * the byte-stream bounds check rejects truncated reads and sizes that would
//     wrap the 32-bit `offset + size` comparison (0xFFFFFFF8);
//   * every size field read from the payload (blockSize / row dataSize) is
//     validated against the bytes that are actually left, the 16 MiB payload cap
//     and the per-row/per-store limits before anything is allocated from it, so
//     a malformed size field cannot make the loader allocate gigabytes; a
//     rowCount that is impossible for the bytes present - or above the
//     documented per-store row cap - is rejected before one instance is created
//     (SAVE-05);
//   * the writer enforces the same 16 MiB cap while it assembles the payload: an
//     oversized payload is rejected instead of being materialized and written;
//   * payload v2 carries a store-set signature (type ids, data versions, order).
//     A payload whose signature does not match the platform's currently
//     registered saveable store set - and every v1 payload - is refused as a
//     whole (`Incompatible`, extension state dropped, nothing registered): a
//     partially restored set used to leave cross-extension pointers unmatched in
//     the engine's swizzle pass, which traps the game;
//   * payloads are consumed exactly (rows, blocks and the payload itself must not
//     leave trailing bytes), the row canary also encodes the store's row layout
//     version, duplicate rows cannot corrupt the store, and a duplicate store
//     type id is refused by ExtensionIndex.
//
// The fake swizzle manager below is strict on purpose: like the engine it fails
// the remap pass when a registered slot has no matching identity, and it records
// every registration so the rejection tests can assert "nothing reached it".
//
// TEST_DEPS: src/Ext/AbstractExt.cpp src/Ext/ExtensionIndex.cpp src/Ext/ExtSave.cpp src/Core/Log.cpp src/Core/PlatformPaths.cpp

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
// assert that the engine was not touched at all.  The fake mirrors the engine's
// remap pass (SAVE-07): RegisterForChange queues the current slot value and
// clears the slot (0x6CF2A8), and FakeProcess() reports a queued non-zero value
// without a matching identity as a failure - the engine's no-match branch is a
// divide-by-zero trap (0x6CF3DD), not a silent skip.  A null slot is not queued
// (0x6CF256).
struct FakeChangedSlot
{
	void* was;
	void** slot;
};

std::vector<FakeChangedSlot> g_changedSlots;
std::unordered_map<void*, void*> g_identities;
int g_commandCount = 0;
int g_applyFailures = 0;

void FakeRegisterForChange(void** const pointer)
{
	++g_commandCount;
	if (!pointer || *pointer == nullptr)
		return;

	g_changedSlots.push_back(FakeChangedSlot{ *pointer, pointer });
	*pointer = nullptr;
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

	for (const FakeChangedSlot& changed : g_changedSlots)
	{
		const auto it = g_identities.find(changed.was);
		if (it == g_identities.end())
		{
			++g_applyFailures;
			applied = false;
			continue;
		}

		if (changed.slot)
			*changed.slot = it->second;
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

// Write-side cap test (A-11): a store whose Save() emits a configurable amount of
// bytes, so a payload can be pushed towards (and past) the 16 MiB limit without
// allocating millions of instances.
class BulkUnitExt final : public venture::UnitExt
{
public:
	static constexpr std::uint32_t TypeId = 0x0131u;
	static constexpr std::uint32_t BaseTypeId = venture::UnitExt::TypeId;

	static inline std::size_t BulkBytes = 0;

	explicit BulkUnitExt(AbstractClass* const owner) : UnitExt(owner) {}

	std::uint32_t ExtTypeId() const override { return TypeId; }

	void Save(venture::ExtStreamWriter& stream) const override
	{
		stream.WriteU32(static_cast<std::uint32_t>(BulkBytes));

		const std::uint8_t chunk[512] = {};
		for (std::size_t left = BulkBytes; left != 0;)
		{
			const std::size_t step = left < sizeof(chunk) ? left : sizeof(chunk);
			stream.Write(chunk, step);
			left -= step;
		}
	}
};

using BulkUnitStore = venture::ExtensionStore<UnitClass, BulkUnitExt>;

constexpr std::uint32_t kDataVersion = 1;

// Mirrors ExtensionStore's row canary: extension class type id in the low 16 bits,
// row layout version in the high 16 bits, with the "VC" magic folded in.
constexpr std::uint32_t RowCanary(const std::uint32_t dataVersion)
{
	return 0x56430000u ^ (PayloadUnitExt::TypeId & 0xFFFFu)
		^ ((dataVersion & 0xFFFFu) << 16);
}

constexpr std::uint32_t kCanary = RowCanary(kDataVersion);

using Bytes = std::vector<std::uint8_t>;

// One entry of the payload v2 store-set signature.
struct SignatureEntry
{
	std::uint32_t typeId;
	std::uint32_t dataVersion;
};

// Signature of the test platform while only PayloadUnitStore participates.
constexpr SignatureEntry kPayloadSignature{ PayloadUnitExt::TypeId, kDataVersion };

// Little-endian read-back used to inspect a writer-produced payload.
std::uint32_t ReadU32At(const Bytes& bytes, const std::size_t offset)
{
	return static_cast<std::uint32_t>(bytes[offset])
		| (static_cast<std::uint32_t>(bytes[offset + 1]) << 8)
		| (static_cast<std::uint32_t>(bytes[offset + 2]) << 16)
		| (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

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

// Builds a payload v2 in the layout of ExtSave.h:
//   [savegameId][payloadVersion][blockCount][signatureCount]
//   signature*: [typeId][dataVersion]
//   block*:     [typeId][dataVersion][blockSize][data...]
// Deliberately hand-built so a test can produce payloads that a correct writer
// would never emit (wrong store set, bogus sizes, truncated blocks, ...).
Bytes BuildPayloadV2(const std::vector<SignatureEntry>& signature,
	const std::vector<SignatureEntry>& blockHeaders, const std::vector<Bytes>& blockData)
{
	venture::ExtByteStreamWriter writer;
	writer.WriteU32(kSavegameId);
	writer.WriteU32(venture::ExtSave::PayloadVersion);
	writer.WriteU32(static_cast<std::uint32_t>(blockData.size()));
	writer.WriteU32(static_cast<std::uint32_t>(signature.size()));

	for (const SignatureEntry& entry : signature)
	{
		writer.WriteU32(entry.typeId);
		writer.WriteU32(entry.dataVersion);
	}

	for (std::size_t i = 0; i < blockData.size(); ++i)
	{
		writer.WriteU32(blockHeaders[i].typeId);
		writer.WriteU32(blockHeaders[i].dataVersion);
		writer.WriteU32(static_cast<std::uint32_t>(blockData[i].size()));
		if (!blockData[i].empty())
			writer.Write(blockData[i].data(), blockData[i].size());
	}

	return writer.Data();
}

// Payload whose block headers repeat the signature (the well-formed case).
Bytes BuildPayloadV2(const std::vector<SignatureEntry>& signature, const std::vector<Bytes>& blocks)
{
	return BuildPayloadV2(signature, signature, blocks);
}

// Default payload for this test platform: one signature entry and one block
// header of the test store per raw block.
Bytes BuildPayload(const std::vector<Bytes>& blocks)
{
	const std::vector<SignatureEntry> signature(blocks.size(), kPayloadSignature);
	return BuildPayloadV2(signature, blocks);
}

// One-block payload whose blockSize field is independent of the bytes actually
// appended (a modified save can claim any size).
Bytes BuildPayloadWithBlockSize(const std::uint32_t typeId, const std::uint32_t dataVersion,
	const std::uint32_t blockSize, const Bytes& blockBytes)
{
	venture::ExtByteStreamWriter writer;
	writer.WriteU32(kSavegameId);
	writer.WriteU32(venture::ExtSave::PayloadVersion);
	writer.WriteU32(1); // blockCount
	writer.WriteU32(1); // signatureCount
	writer.WriteU32(kPayloadSignature.typeId);
	writer.WriteU32(kPayloadSignature.dataVersion);
	writer.WriteU32(typeId);
	writer.WriteU32(dataVersion);
	writer.WriteU32(blockSize);
	if (!blockBytes.empty())
		writer.Write(blockBytes.data(), blockBytes.size());
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

// Runs ReadPayload on a payload the platform must refuse as incompatible (v1
// format or a changed store set) and checks the same safety invariants.
void ExpectIncompatible(const Bytes& payload,
	const venture::ExtSave::LoadSummary::Incompatibility expectedKind, const char* const what)
{
	venture::ExtensionIndex::Instance().ClearAll();
	FakeReset();

	const int constructedBefore = PayloadUnitExt::Constructions;
	const int destroyedBefore = PayloadUnitExt::Destructions;

	venture::ExtSave::EnterLoadWindow();
	const venture::ExtSave::LoadSummary summary = venture::ExtSave::ReadPayload(
		kSavegameId, payload.data(), payload.size());
	venture::ExtSave::ExitLoadWindow();

	Check(summary.result == venture::ExtSave::LoadSummary::Result::Incompatible, what);
	Check(summary.incompatibility == expectedKind,
		"incompatible payload reports the expected reason");
	Check(g_commandCount == 0,
		"incompatible payload: ZERO registrations reached the engine swizzle manager");
	Check(!venture::ExtSwizzle::IsStaging(), "incompatible payload: staging mode is closed again");
	Check(venture::ExtensionIndex::Instance().ExtCount() == 0
			&& PayloadUnitStore::Instance().Size() == 0
			&& venture::ExtensionIndex::Instance().LoadedExtensions().empty(),
		"incompatible payload: no extension is attached or left 'loaded'");
	Check(PayloadUnitExt::Constructions - constructedBefore
			== PayloadUnitExt::Destructions - destroyedBefore,
		"incompatible payload: nothing was created");
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
	// The commit above cleared the slot (0x6CF2A8), so re-arm it first.
	FakeReset();
	slot = &dummyA;
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
		AppendU32(payload, 1); // blockCount
		AppendU32(payload, 1); // signatureCount
		AppendU32(payload, PayloadUnitExt::TypeId);
		AppendU32(payload, kDataVersion); // signature entry complete
		AppendU32(payload, PayloadUnitExt::TypeId);
		AppendU32(payload, kDataVersion); // blockSize missing
		ExpectRejected(payload, "a payload truncated inside a block header is rejected");
	}

	// 3. blockSize promises more bytes than the payload still carries.
	{
		const Bytes payload = BuildPayloadWithBlockSize(PayloadUnitExt::TypeId, kDataVersion,
			64, Bytes(4, 0xAB));
		ExpectRejected(payload, "a block whose size exceeds the remaining payload is rejected");
	}

	// 4. Row cut inside its header (rowCount says two rows, one is present).  The
	//    count is already impossible for the bytes present (two minimal rows need
	//    32 bytes, the block carries 24), so SAVE-05 rejects it before one instance
	//    is created - a truncated row is therefore caught by the count limit.
	{
		Bytes rows;
		AppendRow(rows, 0x1000, 0x2000, kCanary, BuildRowData(7, 0x3000));
		const Bytes payload = BuildPayload({ BuildBlock(2, rows) });
		Check(ExpectRejected(payload, "an infeasible rowCount is rejected before any row is read") == 0,
			"the truncated-row-header case is caught by the rowCount limit, before construction");
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

	// 7. Two registered saveable stores; the first block is valid, the second is
	//    corrupt: the instance loaded from block one must be deleted (before
	//    deletion its registrations are discarded) and nothing may be registered.
	{
		BulkUnitStore& bulkStore = BulkUnitStore::Instance();
		bulkStore.SetName("UnitClass/BulkUnitExt");
		bulkStore.SetSaveable(true, 1);

		Bytes validRows;
		AppendRow(validRows, 0x1000, 0x2000, kCanary, BuildRowData(42, 0x3000));

		const std::vector<SignatureEntry> signature{ kPayloadSignature,
			SignatureEntry{ BulkUnitExt::TypeId, 1 } };
		const Bytes payload = BuildPayloadV2(signature,
			{ BuildBlock(1, validRows), Bytes(8, 0xAB) });

		Check(ExpectRejected(payload, "a corrupt later block rejects the whole payload") == 1,
			"the partially loaded instance is deleted and never registered");

		bulkStore.SetSaveable(false, 1);
		venture::ExtensionIndex::Instance().ClearAll();
	}

	// 8. blockSize is savegame-controlled: 0xFFFFFFFF / 0x7FFFFFFF must be rejected
	//    BEFORE a vector of that size is allocated (A-02).
	{
		const Bytes payload = BuildPayloadWithBlockSize(PayloadUnitExt::TypeId, kDataVersion,
			0xFFFFFFFFu, Bytes{});
		ExpectRejected(payload, "a blockSize of 0xFFFFFFFF is rejected before allocation");
	}
	{
		const Bytes payload = BuildPayloadWithBlockSize(PayloadUnitExt::TypeId, kDataVersion,
			0x7FFFFFFFu, Bytes{});
		ExpectRejected(payload, "a blockSize of 0x7FFFFFFF is rejected before allocation");
	}

	// 9. blockSize only slightly larger than the bytes the payload still carries.
	{
		const Bytes payload = BuildPayloadWithBlockSize(PayloadUnitExt::TypeId, kDataVersion,
			9, Bytes(8, 0xAB));
		ExpectRejected(payload, "a blockSize larger than the remaining payload is rejected");
	}

	// 10. Row dataSize is savegame-controlled too: 0xFFFFFFFF / 0x7FFFFFFF must be
	//     rejected before the row buffer is allocated (A-02).
	{
		Bytes rows;
		AppendU32(rows, 0x1000);
		AppendU32(rows, 0x2000);
		AppendU32(rows, kCanary);
		AppendU32(rows, 0xFFFFFFFFu); // dataSize
		const Bytes payload = BuildPayload({ BuildBlock(1, rows) });
		ExpectRejected(payload, "a row dataSize of 0xFFFFFFFF is rejected before allocation");
	}
	{
		Bytes rows;
		AppendU32(rows, 0x1000);
		AppendU32(rows, 0x2000);
		AppendU32(rows, kCanary);
		AppendU32(rows, 0x7FFFFFFFu); // dataSize
		const Bytes payload = BuildPayload({ BuildBlock(1, rows) });
		ExpectRejected(payload, "a row dataSize of 0x7FFFFFFF is rejected before allocation");
	}

	// 11. Row dataSize only slightly larger than the bytes left in its block.
	{
		Bytes rows;
		AppendU32(rows, 0x1000);
		AppendU32(rows, 0x2000);
		AppendU32(rows, kCanary);
		AppendU32(rows, 5); // dataSize claims 5 bytes...
		AppendU32(rows, 7); // ...but only 4 data bytes follow
		const Bytes payload = BuildPayload({ BuildBlock(1, rows) });
		ExpectRejected(payload, "a row dataSize larger than the remaining block is rejected");
	}

	// 12. A-09: a block with trailing bytes after its rows is rejected (a block
	//     must be consumed exactly by the store's Load).
	{
		Bytes rows;
		AppendRow(rows, 0x1000, 0x2000, kCanary, BuildRowData(7, 0x3000));
		Bytes block = BuildBlock(1, rows);
		AppendU32(block, 0xDEADBEEFu); // trailing garbage inside the block
		const Bytes payload = BuildPayload({ block });
		Check(ExpectRejected(payload, "a block with trailing bytes is rejected") == 1,
			"the trailing-block-bytes case created (then deleted) the row's instance");
	}

	// 13. A-09: a row whose Load() leaves bytes unread is a layout drift - reject.
	{
		Bytes rowData = BuildRowData(7, 0x3000);
		AppendU32(rowData, 0xBEEFu); // 4 bytes the extension's Load never reads
		Bytes rows;
		AppendRow(rows, 0x1000, 0x2000, kCanary, rowData);
		const Bytes payload = BuildPayload({ BuildBlock(1, rows) });
		Check(ExpectRejected(payload, "a row with trailing bytes is rejected") == 1,
			"the trailing-row-bytes case deleted the partially loaded instance");
	}

	// 14. A-09: trailing bytes after the last block are rejected too.
	{
		Bytes rows;
		AppendRow(rows, 0x1000, 0x2000, kCanary, BuildRowData(7, 0x3000));
		Bytes payload = BuildPayload({ BuildBlock(1, rows) });
		payload.push_back(0u); // one stray byte after the last block
		Check(ExpectRejected(payload, "trailing bytes after the last block are rejected") == 1,
			"the trailing-payload-bytes case deleted the loaded instance");
	}

	// 15. A-09: the row canary encodes the store's row layout version too; a row
	//     written for another dataVersion is rejected even if the class id matches.
	{
		Bytes rows;
		AppendRow(rows, 0x1000, 0x2000, RowCanary(kDataVersion + 1), BuildRowData(7, 0x3000));
		const Bytes payload = BuildPayload({ BuildBlock(1, rows) });
		ExpectRejected(payload, "a row canary with a different dataVersion is rejected");
	}

	// 16. SAVE-05: a rowCount above the per-store cap is rejected before any
	//     instance is created, even though the block would fit in the payload cap
	//     (the payload cap alone would allow ~1M minimal rows).
	{
		const Bytes payload = BuildPayload(
			{ BuildBlock(venture::ExtSave::MaxRowsPerStore + 1, Bytes{}) });
		Check(ExpectRejected(payload, "a rowCount above the per-store cap is rejected") == 0,
			"the rowCount cap is enforced before construction");
	}

	// 17. SAVE-05: a rowCount the remaining bytes cannot possibly hold (four
	//     minimal rows need 64 bytes, the block carries one 24-byte row) is
	//     rejected before the first row is parsed.
	{
		Bytes rows;
		AppendRow(rows, 0x1000, 0x2000, kCanary, BuildRowData(7, 0x3000));
		const Bytes payload = BuildPayload({ BuildBlock(4, rows) });
		Check(ExpectRejected(payload, "an impossible rowCount is rejected before construction") == 0,
			"the count-versus-remaining check runs before any row is parsed");
	}

	// 18. SAVE-05: a row dataSize above the per-row cap is rejected before the
	//     row buffer is allocated, even when that many bytes really follow.
	{
		const std::size_t dataSize = venture::ExtSave::MaxRowDataSize + 1;
		Bytes rows;
		AppendU32(rows, 0x1000);
		AppendU32(rows, 0x2000);
		AppendU32(rows, kCanary);
		AppendU32(rows, static_cast<std::uint32_t>(dataSize));
		rows.insert(rows.end(), dataSize, 0xEE);
		const Bytes payload = BuildPayload({ BuildBlock(1, rows) });
		Check(ExpectRejected(payload, "a row dataSize above the 1 MiB row cap is rejected") == 0,
			"the row data cap is enforced before the row buffer is allocated");
	}
}

// --- A-05: payload v2 store-set signature ---------------------------------
void TestStoreSetSignature()
{
	// A v1 payload (no signature) is refused with the version reason, no migration.
	{
		venture::ExtByteStreamWriter writer;
		writer.WriteU32(kSavegameId);
		writer.WriteU32(1); // payload format 1
		writer.WriteU32(0); // block count
		const Bytes payload = writer.Data();

		ExpectIncompatible(payload, venture::ExtSave::LoadSummary::Incompatibility::PayloadVersion,
			"a v1 payload is rejected as incompatible (no migration)");
	}

	// An unknown typeId in the signature.
	{
		const std::vector<SignatureEntry> signature{ SignatureEntry{ 0x7777u, 1 } };
		const Bytes payload = BuildPayloadV2(signature, std::vector<Bytes>(1));
		ExpectIncompatible(payload, venture::ExtSave::LoadSummary::Incompatibility::StoreSet,
			"an unknown typeId in the signature is rejected before any block is read");
	}

	// A missing store: the payload carries no signature entry at all.
	{
		const Bytes payload = BuildPayloadV2({}, std::vector<Bytes>());
		ExpectIncompatible(payload, venture::ExtSave::LoadSummary::Incompatibility::StoreSet,
			"a payload missing a registered store is rejected");
	}

	// A changed store dataVersion.
	{
		const std::vector<SignatureEntry> signature{
			SignatureEntry{ PayloadUnitExt::TypeId, kDataVersion + 1 } };
		const Bytes payload = BuildPayloadV2(signature, std::vector<Bytes>(1));
		ExpectIncompatible(payload, venture::ExtSave::LoadSummary::Incompatibility::StoreSet,
			"a changed store dataVersion is rejected");
	}

	// An extra store in the signature (count larger than the registered set).
	{
		const std::vector<SignatureEntry> signature{ kPayloadSignature,
			SignatureEntry{ 0x7777u, 1 } };
		const Bytes payload = BuildPayloadV2(signature, std::vector<Bytes>(2));
		ExpectIncompatible(payload, venture::ExtSave::LoadSummary::Incompatibility::StoreSet,
			"an extra store in the signature is rejected");
	}

	// The same two stores in the wrong order: both sides count two entries, but
	// the first entry already differs.
	{
		BulkUnitStore& bulkStore = BulkUnitStore::Instance();
		bulkStore.SetSaveable(true, 1);

		const std::vector<SignatureEntry> signature{ SignatureEntry{ BulkUnitExt::TypeId, 1 },
			kPayloadSignature };
		const Bytes payload = BuildPayloadV2(signature, std::vector<Bytes>(2));
		ExpectIncompatible(payload, venture::ExtSave::LoadSummary::Incompatibility::StoreSet,
			"a reordered store set is rejected");

		bulkStore.SetSaveable(false, 1);
		venture::ExtensionIndex::Instance().ClearAll();
	}
}

// --- A-15/A-16: duplicate rows --------------------------------------------
void TestDuplicateRows()
{
	venture::ExtensionIndex::Instance().ClearAll();
	FakeReset();

	PayloadUnitStore& store = PayloadUnitStore::Instance();

	// Two rows that resolve to the same owner but different save-time extension
	// addresses: structurally valid, but only one extension may be attached to an
	// owner.  The loader must not crash and must keep one, drop the other.
	Bytes rows;
	const Bytes rowData = BuildRowData(7, 0x3000);
	AppendRow(rows, 0x1000, 0x2000, kCanary, rowData);
	AppendRow(rows, 0x1008, 0x2000, kCanary, rowData);

	const Bytes payload = BuildPayload({ BuildBlock(2, rows) });

	UnitClass loadedOwner;
	UnitClass loadedTarget;

	venture::ExtSave::EnterLoadWindow();
	const venture::ExtSave::LoadSummary summary = venture::ExtSave::ReadPayload(
		kSavegameId, payload.data(), payload.size());
	Check(summary.result == venture::ExtSave::LoadSummary::Result::Loaded && summary.created == 2,
		"duplicate rows: both rows are read as separate extension instances");

	// The engine loaded the owner and the saved target object at new addresses.
	FakeRegisterIdentity(reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x2000)), &loadedOwner);
	FakeRegisterIdentity(reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x3000)), &loadedTarget);
	Check(FakeProcess(), "duplicate rows: the strict swizzle resolves both owner slots");

	const venture::ExtSave::LoadSummary finished = venture::ExtSave::FinishLoad();
	venture::ExtSave::ExitLoadWindow();
	Check(finished.adopted == 1 && finished.failed == 1,
		"duplicate rows: only one extension is adopted for the shared owner");
	Check(store.Fetch(&loadedOwner) != nullptr,
		"duplicate rows: the adopted instance belongs to the owner");

	venture::ExtensionIndex::Instance().ClearAll();
}

// --- A-10: duplicate / invalid store registration -------------------------
void DuplicateStoreClear() {}

void TestStoreRegistration()
{
	venture::ExtensionIndex& index = venture::ExtensionIndex::Instance();
	const std::size_t before = index.StoreCount();
	const venture::ExtStoreDesc* const original = index.FindStore(PayloadUnitExt::TypeId);
	Check(original != nullptr, "store registration: the test store is registered");

	// A hand-made descriptor with a colliding type id: the index must keep the
	// original store and refuse the duplicate (A-10).
	static venture::ExtStoreDesc duplicate{};
	duplicate.typeId = PayloadUnitExt::TypeId;
	duplicate.dataVersion = 1;
	duplicate.name = "UnitClass/DuplicateUnitExt";
	duplicate.clear = &DuplicateStoreClear;
	index.RegisterStore(&duplicate);
	Check(index.StoreCount() == before,
		"store registration: a duplicate type id is rejected");
	Check(index.FindStore(PayloadUnitExt::TypeId) == original,
		"store registration: the original store still answers FindStore");

	// A descriptor without a clear callback cannot be torn down on scenario end;
	// it must be refused as well.
	static venture::ExtStoreDesc incomplete{};
	incomplete.typeId = 0x0777u;
	incomplete.name = "UnitClass/IncompleteStore";
	index.RegisterStore(&incomplete);
	Check(index.StoreCount() == before,
		"store registration: a descriptor without a clear callback is rejected");
	Check(index.FindStore(0x0777u) == nullptr,
		"store registration: the rejected store is not visible to FindStore");
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

	// The writer emits the v2 header and the store-set signature.
	Check(payload.size() >= 4u * sizeof(std::uint32_t) + 2u * sizeof(std::uint32_t)
			&& ReadU32At(payload, sizeof(std::uint32_t)) == venture::ExtSave::PayloadVersion
			&& ReadU32At(payload, 2u * sizeof(std::uint32_t)) == 1
			&& ReadU32At(payload, 3u * sizeof(std::uint32_t)) == 1
			&& ReadU32At(payload, 4u * sizeof(std::uint32_t)) == PayloadUnitExt::TypeId
			&& ReadU32At(payload, 5u * sizeof(std::uint32_t)) == kDataVersion,
		"success path: payload carries the v2 header and the store-set signature");

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

// --- write-side 16 MiB cap (A-11) -----------------------------------------
void TestWriteSizeCap()
{
	venture::ExtensionIndex::Instance().ClearAll();

	BulkUnitStore& bulkStore = BulkUnitStore::Instance();
	bulkStore.SetName("UnitClass/BulkUnitExt");
	bulkStore.SetSaveable(true, 1);

	UnitClass bulkOwner;
	Check(bulkStore.Allocate(&bulkOwner) != nullptr,
		"write cap: a bulk-data extension exists");

	// Baseline: no bulk bytes.  The payload size then grows exactly one-for-one
	// with BulkBytes, so the cap boundary can be hit to the byte.
	BulkUnitExt::BulkBytes = 0;
	Bytes payload;
	std::uint32_t blocks = 0;
	bool exceeded = false;
	Check(venture::ExtSave::WritePayload(kSavegameId, payload, &blocks, &exceeded)
			&& !exceeded && !payload.empty() && blocks == 2,
		"write cap: a payload below the limit is written");

	const std::size_t baseSize = payload.size();
	const std::size_t maxSize = venture::ExtSave::MaxPayloadSize;

	// Exactly at the limit is accepted (the cap is "more than", not "at least").
	BulkUnitExt::BulkBytes = maxSize - baseSize;
	exceeded = true;
	Check(venture::ExtSave::WritePayload(kSavegameId, payload, &blocks, &exceeded)
			&& !exceeded && payload.size() == maxSize && blocks == 2,
		"write cap: a payload exactly at the 16 MiB limit is written");

	// One byte over is rejected while the payload is built, and nothing is kept.
	BulkUnitExt::BulkBytes = maxSize - baseSize + 1;
	exceeded = false;
	Check(!venture::ExtSave::WritePayload(kSavegameId, payload, &blocks, &exceeded)
			&& exceeded && payload.empty(),
		"write cap: a payload over the 16 MiB limit is rejected while building");

	BulkUnitExt::BulkBytes = 0;
	bulkStore.SetSaveable(false, 1);
	venture::ExtensionIndex::Instance().ClearAll();
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
	TestStoreSetSignature();
	TestDuplicateRows();
	TestStoreRegistration();
	TestSuccessPath();
	TestWriteSizeCap();

	std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAILED",
		g_failures, g_failures == 1 ? "" : "s");
	return g_failures == 0 ? 0 : 1;
}
