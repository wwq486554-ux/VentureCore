// Unit test: extension hierarchy, polymorphic index and store lifecycle.
//
// Runs as a plain console program on this machine - the Ext layer only needs
// "the owner is an AbstractClass", so the test provides a minimal stand-in for
// the engine class hierarchy instead of including YRpp / gamemd.exe.
//
// TEST_DEPS: src/Ext/AbstractExt.cpp src/Ext/ExtensionIndex.cpp src/Core/Log.cpp src/Core/PlatformPaths.cpp

#include <cstddef>
#include <cstdint>
#include <cstdio>

class AbstractClass
{
public:
	virtual ~AbstractClass() = default;
};

class ObjectClass : public AbstractClass {};
class TechnoClass : public ObjectClass {};
class FootClass : public TechnoClass {};
class UnitClass : public FootClass {};
class BuildingClass : public TechnoClass {};

#include "Ext/ExtHierarchy.h"
#include "Ext/ExtensionStore.h"

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

// The platform's most derived extension class for UnitClass.
class TestUnitExt final : public venture::UnitExt
{
public:
	static constexpr std::uint32_t TypeId = 0x0101u;
	static constexpr std::uint32_t BaseTypeId = venture::UnitExt::TypeId;

	explicit TestUnitExt(AbstractClass* const owner) : UnitExt(owner) {}

	std::uint32_t ExtTypeId() const override { return TypeId; }
};

using TestStore = venture::ExtensionStore<UnitClass, TestUnitExt>;

// A second store for the same owner class: used to prove that an owner accepts
// exactly one extension and the second claim fails cleanly.
class OtherUnitExt final : public venture::UnitExt
{
public:
	static constexpr std::uint32_t TypeId = 0x0102u;
	static constexpr std::uint32_t BaseTypeId = venture::UnitExt::TypeId;

	explicit OtherUnitExt(AbstractClass* const owner) : UnitExt(owner) {}

	std::uint32_t ExtTypeId() const override { return TypeId; }
};

using OtherUnitStore = venture::ExtensionStore<UnitClass, OtherUnitExt>;

// A different owner class, to check that base views do not cross classes.
class BuildingExt final : public venture::TechnoExt
{
public:
	static constexpr std::uint32_t TypeId = 0x0103u;
	static constexpr std::uint32_t BaseTypeId = venture::TechnoExt::TypeId;

	explicit BuildingExt(AbstractClass* const owner) : TechnoExt(owner) {}

	std::uint32_t ExtTypeId() const override { return TypeId; }
};

using BuildingStore = venture::ExtensionStore<BuildingClass, BuildingExt>;

// SAVE-03: an extension whose destructor re-enters its store.  Clear() detaches
// the whole store before deleting, so a destructor must observe an empty store
// instead of a half-torn-down list (formerly a use-after-free / double free).
class ReentrantExt final : public venture::TechnoExt
{
public:
	static constexpr std::uint32_t TypeId = 0x0104u;
	static constexpr std::uint32_t BaseTypeId = venture::TechnoExt::TypeId;

	explicit ReentrantExt(AbstractClass* const owner) : TechnoExt(owner) {}

	std::uint32_t ExtTypeId() const override { return TypeId; }

	// A plain function pointer: the store alias below needs a complete extension
	// class, so the probe cannot be a member that names its own store.
	static inline void (*OnDestroyed)() = nullptr;

	~ReentrantExt() override
	{
		if (OnDestroyed)
			OnDestroyed();
	}
};

using ReentrantStore = venture::ExtensionStore<BuildingClass, ReentrantExt>;

// D-22: capture what RegisterExtType reports through the problem sink.  A plain
// function (not a lambda) because the sink is a function pointer.
int g_problemMessages = 0;

void CaptureExtTypeProblem(const char* const message)
{
	if (message && *message)
		++g_problemMessages;
}

// SAVE-03: what the re-entrant destructor probe observed while its store was
// being cleared.
BuildingClass* g_reentrantProbeOwner = nullptr;
venture::AbstractExt* g_reentrantFetched = nullptr;
int g_reentrantDestructions = 0;
int g_reentrantNestedClears = 0;

void ProbeStoreReentrancy()
{
	++g_reentrantDestructions;
	ReentrantStore& store = ReentrantStore::Instance();
	// Every call here used to touch a node that had already been deleted, or a
	// node the teardown loop was about to delete again.
	g_reentrantFetched = store.Fetch(g_reentrantProbeOwner);
	store.Remove(g_reentrantProbeOwner);
	store.Clear();
	++g_reentrantNestedClears;
}
}

int __cdecl main()
{
	using venture::AbstractExt;
	using venture::ExtensionIndex;
	using venture::FootExt;
	using venture::TechnoExt;
	using venture::UnitExt;

	TestStore& store = TestStore::Instance();
	store.SetName("UnitClass/TestUnitExt");
	OtherUnitStore& otherStore = OtherUnitStore::Instance();
	BuildingStore& buildingStore = BuildingStore::Instance();

	Check(store.Descriptor().typeId == TestUnitExt::TypeId, "store descriptor carries the extension type id");
	Check(ExtensionIndex::Instance().FindStore(TestUnitExt::TypeId) == &store.Descriptor(),
		"store is registered with the global index");
	Check(ExtensionIndex::Instance().StoreCount() >= 3, "all stores are registered");

	// Relative counts: the numbers below must describe what this test allocated,
	// not depend on "no other store has allocated anything yet" (A-17).
	const std::size_t extCountAtStart = ExtensionIndex::Instance().ExtCount();

	// -- allocation and fetch ------------------------------------------------
	UnitClass unitA;
	UnitClass unitB;
	BuildingClass building;

	TestUnitExt* const extA = store.Allocate(&unitA);
	TestUnitExt* const extB = store.Allocate(&unitB);
	Check(extA != nullptr && extB != nullptr && extA != extB, "two owners get two instances");
	Check(store.Allocate(&unitA) == extA, "Allocate is idempotent for the same owner");
	Check(store.Size() == 2, "store keeps every instance");
	Check(store.Fetch(&unitA) == extA && store.Fetch(&unitB) == extB, "typed Fetch finds the instance");

	// -- base views ----------------------------------------------------------
	Check(UnitExt::Fetch(&unitA) == extA, "UnitExt::Fetch returns the derived instance");
	Check(FootExt::Fetch(&unitA) == extA, "FootExt::Fetch returns the derived instance");
	Check(TechnoExt::Fetch(&unitA) == extA, "TechnoExt::Fetch returns the derived instance");
	Check(venture::FetchExtAs<AbstractExt>(&unitA) == extA, "root view returns the derived instance");
	Check(TestUnitExt::Fetch(&unitB) == extB, "feature Fetch helper returns the instance");
	Check(ExtensionIndex::Instance().Fetch(&unitA) == extA, "global index sees the same instance");
	Check(ExtensionIndex::Instance().ExtCount() == extCountAtStart + 2,
		"global index counts the two instances this test allocated");

	// -- type validation -----------------------------------------------------
	Check(venture::IsExtTypeDerivedFrom(TestUnitExt::TypeId, UnitExt::TypeId), "type chain: user -> UnitExt");
	Check(venture::IsExtTypeDerivedFrom(TestUnitExt::TypeId, AbstractExt::TypeId), "type chain: user -> AbstractExt");
	Check(!venture::IsExtTypeDerivedFrom(TestUnitExt::TypeId, otherStore.Descriptor().typeId),
		"type chain: sibling extension class is not a base of the instance");

	// -- registration validation (D-22) --------------------------------------
	// A rejected pair must poison the type id: the chain cannot be trusted any
	// more, so every base view returns nullptr instead of risking a wrong
	// static_cast (there is no RTTI in this build).
	venture::SetExtTypeProblemSink(&CaptureExtTypeProblem);

	const std::size_t firstProblemCount = venture::ExtTypeProblemCount();
	Check(!venture::RegisterExtType(0u, venture::AbstractExt::TypeId),
		"registration rejects type id 0 (root sentinel)");
	Check(!venture::RegisterExtType(0x0099u, venture::UnitExt::TypeId),
		"registration rejects a type id below FirstUserTypeId");
	Check(!venture::RegisterExtType(0x1F1u, 0x1F1u), "registration rejects a self-referencing pair");

	Check(venture::RegisterExtType(0x1F2u, venture::UnitExt::TypeId), "a well-formed pair registers");
	Check(venture::RegisterExtType(0x1F2u, venture::UnitExt::TypeId),
		"the identical pair is idempotent");
	Check(!venture::RegisterExtType(0x1F2u, venture::TechnoExt::TypeId),
		"the same type id with a different base is rejected");
	Check(!venture::IsExtTypeDerivedFrom(0x1F2u, venture::UnitExt::TypeId),
		"the poisoned type id no longer reports a base view");
	Check(!venture::IsExtTypeDerivedFrom(0x1F2u, 0u),
		"the poisoned type id does not even match the 'any extension' query");

	Check(venture::RegisterExtType(0x1F3u, 0x1F4u),
		"a forward reference to a not-yet-registered base is accepted");
	Check(!venture::RegisterExtType(0x1F4u, 0x1F3u),
		"registering the reverse pair makes the chain cyclic and is rejected");
	Check(!venture::IsExtTypeDerivedFrom(0x1F3u, venture::AbstractExt::TypeId),
		"the cyclic pair reports no base view");

	Check(venture::ExtTypeProblemCount() == firstProblemCount + 5,
		"every rejected registration is counted once");
	Check(g_problemMessages == 5, "every rejected registration is reported through the problem sink");

	// -- SAVE-04: a poisoned intermediate type invalidates the chain ----------
	// The lookup used to validate only the two endpoints, so a chain through a
	// poisoned middle node still passed and FetchExtAs performed a static_cast
	// through a class relation that had been rejected.  (Checked before the
	// built-in conflict below, which would poison its own endpoint.)
	Check(venture::RegisterExtType(0x1A1u, UnitExt::TypeId), "SAVE-04: the chain base registers");
	Check(venture::RegisterExtType(0x1A0u, 0x1A1u), "SAVE-04: the derived chain type registers");
	Check(venture::IsExtTypeDerivedFrom(0x1A0u, UnitExt::TypeId),
		"SAVE-04: the healthy chain reports the base view");
	Check(!venture::RegisterExtType(0x1A1u, TechnoExt::TypeId),
		"SAVE-04: re-registering the mid-chain type with another base is rejected");
	Check(venture::ExtTypeProblemCount() == firstProblemCount + 6,
		"SAVE-04: the mid-chain rejection is counted");
	Check(!venture::IsExtTypeDerivedFrom(0x1A0u, UnitExt::TypeId),
		"SAVE-04: a base view through a poisoned intermediate type is refused");
	Check(!venture::IsExtTypeDerivedFrom(0x1A0u, AbstractExt::TypeId),
		"SAVE-04: a root view through a poisoned intermediate type is refused too");
	Check(g_problemMessages == 6, "SAVE-04: the mid-chain rejection is logged");

	// -- SAVE-04: a built-in id conflict is rejected without poisoning --------
	// The seeded built-in ids are registered before any feature runs.  A feature
	// that re-declares one of them is rejected, but poisoning it used to make
	// every base view of every real instance fail (one bad feature turned
	// UnitExt::Fetch into a universal nullptr).
	Check(!venture::RegisterExtType(UnitExt::TypeId, TechnoExt::TypeId),
		"SAVE-04: re-registering a built-in type with another base is rejected");
	Check(venture::ExtTypeProblemCount() == firstProblemCount + 7,
		"SAVE-04: the rejected built-in re-registration is counted");
	Check(g_problemMessages == 7, "SAVE-04: the rejected built-in re-registration is logged");
	Check(venture::IsExtTypeDerivedFrom(TestUnitExt::TypeId, UnitExt::TypeId),
		"SAVE-04: the rejected re-registration does not poison the built-in hierarchy");
	Check(venture::IsExtTypeDerivedFrom(UnitExt::TypeId, AbstractExt::TypeId),
		"SAVE-04: the built-in chain still reaches the root");
	Check(UnitExt::Fetch(&unitA) == extA,
		"SAVE-04: a healthy instance keeps its base view after the rejected re-registration");
	Check(venture::RegisterExtType(UnitExt::TypeId, venture::FootExt::TypeId),
		"SAVE-04: the correct built-in pair is still idempotent");

	// -- parent-chain depth guard (A-18) -------------------------------------
	// Build a forward-referenced chain deeper than any real hierarchy (each pair
	// is accepted because its base is not registered yet).  The lookup must stop
	// at the depth limit, report the chain once and poison the type instead of
	// walking forever.
	bool chainRegistered = true;
	for (std::uint32_t i = 0; i < 35; ++i)
		chainRegistered = venture::RegisterExtType(0x200u + i, 0x201u + i) && chainRegistered;
	Check(chainRegistered, "a deep forward-referenced chain can be registered");

	const std::size_t beforeDepthGuard = venture::ExtTypeProblemCount();
	const int messagesBeforeDepthGuard = g_problemMessages;
	Check(!venture::IsExtTypeDerivedFrom(0x200u, venture::AbstractExt::TypeId),
		"the lookup stops at the parent-chain depth limit instead of walking forever");
	Check(venture::ExtTypeProblemCount() == beforeDepthGuard + 1,
		"the depth limit is reported once");
	Check(g_problemMessages == messagesBeforeDepthGuard + 1,
		"the depth-limit problem reaches the sink");
	Check(!venture::IsExtTypeDerivedFrom(0x200u, venture::AbstractExt::TypeId),
		"the poisoned deep type stays unusable");
	Check(venture::ExtTypeProblemCount() == beforeDepthGuard + 1,
		"a repeated lookup does not report the same chain again");

	venture::SetExtTypeProblemSink(nullptr);

	// -- START-07: bounded static-phase problem queue -------------------------
	// A real feature can register its type during static initialization, i.e.
	// before Platform::Startup() installs the sink.  Those problems are queued
	// (bounded) and the platform drains them right after installing the sink.
	{
		Check(venture::PendingExtTypeProblemCount() == 0,
			"START-07: nothing is queued while the sink had been installed");
		g_problemMessages = 0;
		const std::size_t problemsBefore = venture::ExtTypeProblemCount();

		Check(!venture::RegisterExtType(0x2A0u, 0x2A0u),
			"START-07: the rejected self-base registration is rejected");
		Check(venture::PendingExtTypeProblemCount() == 1,
			"START-07: a problem reported without a sink is queued, not dropped");
		Check(g_problemMessages == 0, "START-07: no sink was called while it is not installed");
		Check(venture::ExtTypeProblemCount() == problemsBefore + 1,
			"START-07: the rejected registration is still counted");

		Check(venture::ExtTypeProblemDrain(nullptr) == 0,
			"START-07: draining without a sink reports nothing");
		Check(venture::PendingExtTypeProblemCount() == 1,
			"START-07: a null drain keeps the queued problem");

		Check(venture::ExtTypeProblemDrain(&CaptureExtTypeProblem) == 1,
			"START-07: the drain reports the queued problem");
		Check(g_problemMessages == 1, "START-07: the queued problem reached the sink");
		Check(venture::PendingExtTypeProblemCount() == 0,
			"START-07: the queue is empty after the drain");
		Check(venture::ExtTypeProblemDrain(&CaptureExtTypeProblem) == 0,
			"START-07: a second drain reports nothing");
		Check(venture::ExtTypeProblemCount() == problemsBefore + 1,
			"START-07: draining does not change the rejected-registration total");
	}
	{
		// Overflow: the queue must stay bounded even when feature code raises many
		// problems before the logger exists.
		g_problemMessages = 0;
		bool allRejected = true;
		for (std::uint32_t i = 0; i < 20; ++i)
			allRejected = !venture::RegisterExtType(0x2B0u + i, 0x2B0u + i) && allRejected;
		Check(allRejected, "START-07: twenty self-base registrations are all rejected");
		Check(venture::PendingExtTypeProblemCount() == venture::MaxPendingExtTypeProblems,
			"START-07: the startup queue is bounded");
		Check(venture::DroppedExtTypeProblemCount()
				== static_cast<std::size_t>(20) - venture::MaxPendingExtTypeProblems,
			"START-07: problems beyond the queue capacity are counted as dropped");
		Check(venture::ExtTypeProblemDrain(&CaptureExtTypeProblem)
				== venture::MaxPendingExtTypeProblems + 1,
			"START-07: the drain reports the queued problems plus one overflow summary");
		Check(g_problemMessages == static_cast<int>(venture::MaxPendingExtTypeProblems) + 1,
			"START-07: the overflow summary reached the sink");
		Check(venture::PendingExtTypeProblemCount() == 0
				&& venture::DroppedExtTypeProblemCount() == 0,
			"START-07: a drain resets the queue and the drop counter");
	}

	BuildingExt* const buildingExt = buildingStore.Allocate(&building);
	Check(buildingExt != nullptr, "a second owner class has its own store");
	Check(TechnoExt::Fetch(&building) == buildingExt, "TechnoExt view reaches the building extension");
	Check(ExtensionIndex::Instance().ExtCount() == extCountAtStart + 3,
		"global index counts instances of every owner class (relative to the start)");
	Check(UnitExt::Fetch(&building) == nullptr, "UnitExt view rejects an extension of another class");
	Check(TestUnitExt::Fetch(&unitA) == extA && TestUnitExt::Fetch(&building) == nullptr,
		"typed Fetch is class-exact across owners");

	// -- one extension per owner --------------------------------------------
	Check(otherStore.Allocate(&unitA) == nullptr, "a second store cannot claim an owner that already has one");
	Check(store.Fetch(&unitA) == extA, "the failed claim leaves the existing extension intact");

	// -- removal -------------------------------------------------------------
	store.Remove(&unitA);
	Check(store.Fetch(&unitA) == nullptr, "Remove drops the typed entry");
	Check(UnitExt::Fetch(&unitA) == nullptr, "Remove drops the base view");
	Check(ExtensionIndex::Instance().Fetch(&unitA) == nullptr, "Remove drops the global index entry");
	Check(store.Size() == 1, "Remove keeps the other instance");
	store.Remove(&unitA);
	Check(store.Size() == 1, "Remove is a no-op for an owner without an extension");

	// -- scenario clear ------------------------------------------------------
	ExtensionIndex::Instance().ClearAll();
	Check(store.Size() == 0 && buildingStore.Size() == 0, "scenario clear drops every store");
	Check(ExtensionIndex::Instance().ExtCount() == 0, "scenario clear drops the global index");
	ExtensionIndex::Instance().ClearAll();
	Check(store.Size() == 0, "scenario clear is idempotent");

	// -- SAVE-03: store teardown is re-entrancy safe -------------------------
	// A feature extension's destructor may call back into the store.  Clear()
	// detaches the list and the map before the first delete, so the re-entrant
	// Fetch/Remove/Clear calls must see an empty store instead of dangling nodes
	// (formerly a use-after-free / double free).
	{
		ReentrantStore& reentrant = ReentrantStore::Instance();
		reentrant.SetName("BuildingClass/ReentrantExt");

		BuildingClass firstOwner;
		BuildingClass secondOwner;
		Check(reentrant.Allocate(&firstOwner) != nullptr
				&& reentrant.Allocate(&secondOwner) != nullptr,
			"SAVE-03: the re-entrancy store allocated two extensions");
		Check(reentrant.Size() == 2, "SAVE-03: both extensions are in the store");

		g_reentrantProbeOwner = &firstOwner;
		g_reentrantFetched = nullptr;
		g_reentrantDestructions = 0;
		g_reentrantNestedClears = 0;
		ReentrantExt::OnDestroyed = &ProbeStoreReentrancy;

		ExtensionIndex::Instance().ClearAll();
		ReentrantExt::OnDestroyed = nullptr;

		Check(g_reentrantDestructions == 2,
			"SAVE-03: scenario clear destroys every instance exactly once");
		Check(reentrant.Size() == 0, "SAVE-03: the store is empty after the clear");
		Check(g_reentrantFetched == nullptr,
			"SAVE-03: Fetch from a destructor sees the detached map, not a dangling node");
		Check(g_reentrantNestedClears == 2,
			"SAVE-03: a nested Clear from a destructor runs once per instance and is a no-op");
		Check(ExtensionIndex::Instance().Fetch(&firstOwner) == nullptr
				&& ExtensionIndex::Instance().Fetch(&secondOwner) == nullptr,
			"SAVE-03: the global index holds no entry for the cleared owners");
	}

	// -- allocation order after a clear -------------------------------------
	TestUnitExt* const again = store.Allocate(&unitA);
	Check(again != nullptr && store.Size() == 1, "a store is reusable after a scenario clear");
	ExtensionIndex::Instance().ClearAll();

	std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAILED",
		g_failures, g_failures == 1 ? "" : "s");
	return g_failures == 0 ? 0 : 1;
}
