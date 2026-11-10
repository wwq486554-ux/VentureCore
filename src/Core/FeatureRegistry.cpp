#include "Core/FeatureRegistry.h"

#include "Core/PlatformConfig.h"

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
		const bool tierSupported = static_cast<unsigned>(context.tier) >= static_cast<unsigned>(descriptor->minTier);
		const bool enabled = switchOn && tierSupported;

		*descriptor->enabled = enabled;

		if (context.Log)
		{
			if (enabled)
			{
				context.Log("[Features] %s (%s): enabled (tier %s)",
					descriptor->id, descriptor->name, TierName(descriptor->minTier));
			}
			else if (!tierSupported)
			{
				context.Log("[Features] %s (%s): disabled - requires tier %s, running at %s",
					descriptor->id, descriptor->name, TierName(descriptor->minTier), TierName(context.tier));
			}
			else
			{
				context.Log("[Features] %s (%s): disabled by %s [%s] %s",
					descriptor->id, descriptor->name, VC_PLATFORM_INI_NAME, VC_SECTION_FEATURES, descriptor->iniKey);
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
