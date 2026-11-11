#include "Ext/AbstractExt.h"

#include "Ext/ExtHierarchy.h"

#include <cstddef>
#include <cstdio>
#include <unordered_map>
#include <unordered_set>

namespace
{
// (typeId -> direct base typeId).  The built-in hierarchy is seeded here so that
// base-class views work even before a feature store is instantiated; feature
// classes register themselves through EnsureExtTypeRegistered<>.
std::unordered_map<std::uint32_t, std::uint32_t>& TypeParents()
{
	static std::unordered_map<std::uint32_t, std::uint32_t> parents = {
		{ venture::AbstractExt::TypeId, venture::AbstractExt::BaseTypeId },
		{ venture::TechnoExt::TypeId, venture::TechnoExt::BaseTypeId },
		{ venture::FootExt::TypeId, venture::FootExt::BaseTypeId },
		{ venture::UnitExt::TypeId, venture::UnitExt::BaseTypeId },
	};
	return parents;
}

// Type ids whose registration was rejected.  Their instances can no longer be
// viewed through any base class (the type chain cannot be trusted any more), so
// a broken composition fails at the lookup instead of producing a wrong
// static_cast - the DLL is built with /GR-, there is no RTTI to catch it (D-22).
std::unordered_set<std::uint32_t>& PoisonedTypes()
{
	static std::unordered_set<std::uint32_t> poisoned;
	return poisoned;
}

venture::ExtTypeProblemSink& ProblemSink()
{
	static venture::ExtTypeProblemSink sink = nullptr;
	return sink;
}

std::size_t& ProblemCount()
{
	static std::size_t count = 0;
	return count;
}

// A parent chain deeper than this cannot be a real extension hierarchy; it means
// the registrations form a cycle (A-18).
constexpr std::size_t kMaxParentChain = 32;

void ReportProblem(
	const std::uint32_t typeId,
	const std::uint32_t baseTypeId,
	const char* const reason)
{
	++ProblemCount();
	// Type id 0 is the root sentinel, not an extension type: poisoning it would
	// turn the documented "wantedTypeId == 0 matches every extension" rule off.
	if (typeId != 0)
		PoisonedTypes().insert(typeId);

	const venture::ExtTypeProblemSink sink = ProblemSink();
	if (!sink)
		return;

	char message[256] = {};
	std::snprintf(
		message,
		sizeof(message),
		"ext type 0x%08X (base 0x%08X): registration rejected - %s; extensions of this type "
		"report no base view (FetchExtAs returns nullptr) instead of risking a wrong cast",
		static_cast<unsigned>(typeId),
		static_cast<unsigned>(baseTypeId),
		reason);
	sink(message);
}

// Walks the already known part of the parent chain.  Returns false when the
// chain revisits typeId (a registration cycle) or grows past the depth limit.
bool ChainReachesWellKnownRoot(const std::uint32_t baseTypeId, const std::uint32_t typeId)
{
	std::uint32_t current = baseTypeId;
	for (std::size_t depth = 0; depth < kMaxParentChain; ++depth)
	{
		if (current == 0)
			return true;
		if (current == typeId)
			return false;

		const auto it = TypeParents().find(current);
		if (it == TypeParents().end())
		{
			// The base is not registered yet (a forward reference between two
			// feature modules): the relation is valid for a direct view, only the
			// walk to the root cannot be verified yet.  Accept it.
			return true;
		}

		current = it->second;
	}

	// Too deep to be a real hierarchy: treat it as a cycle.
	return false;
}
}

namespace venture
{
bool RegisterExtType(const std::uint32_t typeId, const std::uint32_t baseTypeId)
{
	if (typeId == 0)
	{
		ReportProblem(typeId, baseTypeId, "type id 0 is the root sentinel, not a class");
		return false;
	}

	// The built-in hierarchy is seeded before any feature registers, and
	// FetchExtAs<> re-runs EnsureExtTypeRegistered for the skeleton classes too:
	// an identical pair is always idempotent, including the seeded 0x0001-0x0100
	// entries.
	const auto existing = TypeParents().find(typeId);
	if (existing != TypeParents().end())
	{
		if (existing->second == baseTypeId)
			return true;

		ReportProblem(
			typeId,
			baseTypeId,
			"the type id is already registered with a different base (copy-pasted TypeId in another extension class)");
		return false;
	}

	// The base hierarchy owns 0x0001-0x00FF (ExtHierarchy.h); a new feature type
	// id there would silently capture the built-in base mapping and make both
	// views validate wrongly (D-22).  ExtensionStore also rejects it at compile
	// time.
	if (typeId < AbstractExt::FirstUserTypeId)
	{
		ReportProblem(typeId, baseTypeId, "feature type ids must start at AbstractExt::FirstUserTypeId (0x100)");
		return false;
	}

	if (typeId == baseTypeId)
	{
		ReportProblem(typeId, baseTypeId, "a type cannot be its own base");
		return false;
	}

	if (!ChainReachesWellKnownRoot(baseTypeId, typeId))
	{
		ReportProblem(typeId, baseTypeId, "the base chain is cyclic or deeper than any real hierarchy");
		return false;
	}

	TypeParents().emplace(typeId, baseTypeId);
	return true;
}

bool IsExtTypeDerivedFrom(const std::uint32_t actualTypeId, const std::uint32_t wantedTypeId)
{
	if (PoisonedTypes().count(actualTypeId) != 0 || PoisonedTypes().count(wantedTypeId) != 0)
		return false;

	if (wantedTypeId == 0)
		return true;

	std::uint32_t current = actualTypeId;
	while (current != 0)
	{
		if (current == wantedTypeId)
			return true;

		const auto& parents = TypeParents();
		const auto it = parents.find(current);
		if (it == parents.end())
			return false;
		current = it->second;
	}

	return false;
}

void SetExtTypeProblemSink(const ExtTypeProblemSink sink)
{
	ProblemSink() = sink;
}

std::size_t ExtTypeProblemCount()
{
	return ProblemCount();
}
}
