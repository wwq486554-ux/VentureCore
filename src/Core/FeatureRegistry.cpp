#include "Core/FeatureRegistry.h"

#include "Core/PlatformConfig.h"

#include "Core/HostRegistry.h"

#include <cstring>

#include <vector>

namespace
{
std::vector<const venture::FeatureDescriptor*>& Registry()
{
	// Function-local static: safe to use from the static initializers of any
	// translation unit, regardless of their order.
	static std::vector<const venture::FeatureDescriptor*> features;
	return features;
}
}

namespace venture
{
void RegisterFeature(const FeatureDescriptor* const descriptor)
{
	if (!descriptor || !descriptor->id || !descriptor->iniKey || !descriptor->Initialize || !descriptor->enabled)
		return;

	for (const FeatureDescriptor* const existing : Registry())
	{
		if (existing == descriptor)
			return;
	}

	Registry().push_back(descriptor);
}

void InitializeFeatures(const FeatureContext& context)
{
	for (const FeatureDescriptor* const descriptor : Registry())
	{
		const bool switchOn = context.IniGetBool
			? context.IniGetBool(VC_SECTION_FEATURES, descriptor->iniKey, VC_DEFAULT_FEATURE_ENABLED)
			: VC_DEFAULT_FEATURE_ENABLED;

		// Host gate (2.0, replacing the removed support tier): requiredHosts is
		// AND, anyOfHosts is OR, and 0 in either field means "no constraint".
		// The descriptor may only reference compile-time known bits; a host
		// discovered at run time is queried by name by the feature itself.
		const bool requiredOk = RequiredHostsPresent(context.hostMask, descriptor->requiredHosts);
		const bool anyOfOk = AnyOfHostsPresent(context.hostMask, descriptor->anyOfHosts);
		const bool enabled = switchOn && requiredOk && anyOfOk;

		*descriptor->enabled = enabled;

		if (context.Log)
		{
			char required[128] = {};
			char anyOf[128] = {};
			char present[256] = {};

			if (enabled)
			{
				context.Log("[Features] %s (%s): enabled", descriptor->id, descriptor->name);
			}
			else if (!switchOn)
			{
				context.Log("[Features] %s (%s): disabled by %s [%s] %s",
					descriptor->id, descriptor->name, VC_PLATFORM_INI_NAME, VC_SECTION_FEATURES, descriptor->iniKey);
			}
			else if (!requiredOk)
			{
				const unsigned missing = descriptor->requiredHosts & ~context.hostMask;
				Hosts().FormatNames(missing, required, sizeof(required));
				Hosts().FormatDetected(present, sizeof(present));
				context.Log("[Features] %s (%s): disabled - requires host%s %s (present: %s)",
					descriptor->id, descriptor->name, IsSingleHostBit(missing) ? "" : "s", required, present);
			}
			else
			{
				Hosts().FormatNames(descriptor->anyOfHosts, anyOf, sizeof(anyOf));
				Hosts().FormatDetected(present, sizeof(present));
				context.Log("[Features] %s (%s): disabled - requires any of [%s] (present: %s)",
					descriptor->id, descriptor->name, anyOf, present);
			}
		}

		descriptor->Initialize(context);
	}
}

int FeatureCount()
{
	return static_cast<int>(Registry().size());
}

const FeatureDescriptor* FeatureAt(const int index)
{
	if (index < 0 || index >= FeatureCount())
		return nullptr;
	return Registry()[static_cast<std::size_t>(index)];
}

const FeatureDescriptor* FindFeature(const char* const id)
{
	if (!id)
		return nullptr;

	for (const FeatureDescriptor* const descriptor : Registry())
	{
		if (std::strcmp(descriptor->id, id) == 0)
			return descriptor;
	}
	return nullptr;
}
}
