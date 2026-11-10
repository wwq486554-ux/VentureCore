#include "Ext/AbstractExt.h"

#include "Ext/ExtHierarchy.h"

#include <unordered_map>

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
}

namespace venture
{
void RegisterExtType(const std::uint32_t typeId, const std::uint32_t baseTypeId)
{
	if (typeId == 0)
		return;

	// First registration wins: a class' (type, base) pair is a compile-time
	// constant, so a second call can only repeat it.
	TypeParents().emplace(typeId, baseTypeId);
}

bool IsExtTypeDerivedFrom(const std::uint32_t actualTypeId, const std::uint32_t wantedTypeId)
{
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
}
