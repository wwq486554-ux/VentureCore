// Unit test: extension hierarchy, polymorphic index and store lifecycle.
//
// Runs as a plain console program on this machine - the Ext layer only needs
// "the owner is an AbstractClass", so the test provides a minimal stand-in for
// the engine class hierarchy instead of including YRpp / gamemd.exe.
//
// TEST_DEPS: src/Ext/AbstractExt.cpp src/Ext/ExtensionIndex.cpp

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
	Check(ExtensionIndex::Instance().ExtCount() == 2, "global index counts every instance");

	// -- type validation -----------------------------------------------------
	Check(venture::IsExtTypeDerivedFrom(TestUnitExt::TypeId, UnitExt::TypeId), "type chain: user -> UnitExt");
	Check(venture::IsExtTypeDerivedFrom(TestUnitExt::TypeId, AbstractExt::TypeId), "type chain: user -> AbstractExt");
	Check(!venture::IsExtTypeDerivedFrom(TestUnitExt::TypeId, otherStore.Descriptor().typeId),
		"type chain: sibling extension class is not a base of the instance");

	BuildingExt* const buildingExt = buildingStore.Allocate(&building);
	Check(buildingExt != nullptr, "a second owner class has its own store");
	Check(TechnoExt::Fetch(&building) == buildingExt, "TechnoExt view reaches the building extension");
	Check(ExtensionIndex::Instance().ExtCount() == 3, "global index counts instances of every owner class");
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

	// -- allocation order after a clear -------------------------------------
	TestUnitExt* const again = store.Allocate(&unitA);
	Check(again != nullptr && store.Size() == 1, "a store is reusable after a scenario clear");
	ExtensionIndex::Instance().ClearAll();

	std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAILED",
		g_failures, g_failures == 1 ? "" : "s");
	return g_failures == 0 ? 0 : 1;
}
