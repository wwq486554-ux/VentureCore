#pragma once

#include "Ext/AbstractExt.h"
#include "Ext/ExtensionIndex.h"

namespace venture
{
// ---------------------------------------------------------------------------
// Extension hierarchy skeleton.
//
// These classes exist so feature modules can be written against the same call
// shape as the platform's own code and as the code being migrated to it:
//
//     UnitExt* pUnitExt = UnitExt::Fetch(pUnit);        // base view
//     TechnoExt* pTechnoExt = TechnoExt::Fetch(pTechno);
//
// An object has exactly one *most derived* extension instance (declared by the
// platform under src/Features/<Name>/); every Fetch in the chain returns that
// same instance, validated by the runtime type-id chain (no RTTI: the DLL is
// built with /GR-).  Feature extension classes derive from one of these and
// declare `TypeId >= AbstractExt::FirstUserTypeId` plus `BaseTypeId` = their
// direct base's TypeId; see docs/EngineBindings.md.
//
// The classes deliberately carry no engine layout assumptions beyond "the owner
// is an AbstractClass" - they are a skeleton, not a copy of another platform's
// container/pointer-slot design.
// ---------------------------------------------------------------------------
class TechnoExt : public AbstractExt
{
public:
	static constexpr std::uint32_t TypeId = 0x0002u;
	static constexpr std::uint32_t BaseTypeId = AbstractExt::TypeId;

	explicit TechnoExt(AbstractClass* const owner) : AbstractExt(owner) {}
	std::uint32_t ExtTypeId() const override { return TypeId; }

	static TechnoExt* Fetch(const AbstractClass* const owner) { return FetchExtAs<TechnoExt>(owner); }
};

class FootExt : public TechnoExt
{
public:
	static constexpr std::uint32_t TypeId = 0x0003u;
	static constexpr std::uint32_t BaseTypeId = TechnoExt::TypeId;

	explicit FootExt(AbstractClass* const owner) : TechnoExt(owner) {}
	std::uint32_t ExtTypeId() const override { return TypeId; }

	static FootExt* Fetch(const AbstractClass* const owner) { return FetchExtAs<FootExt>(owner); }
};

class UnitExt : public FootExt
{
public:
	static constexpr std::uint32_t TypeId = 0x0004u;
	static constexpr std::uint32_t BaseTypeId = FootExt::TypeId;

	explicit UnitExt(AbstractClass* const owner) : FootExt(owner) {}
	std::uint32_t ExtTypeId() const override { return TypeId; }

	static UnitExt* Fetch(const AbstractClass* const owner) { return FetchExtAs<UnitExt>(owner); }
};
}
