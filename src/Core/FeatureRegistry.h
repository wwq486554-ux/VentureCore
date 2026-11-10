#pragma once

#include <platform/Feature.h>

namespace venture
{
// Resolves the feature switches from <Platform>.ini, stores the result in each
// descriptor, and calls the feature's Initialize(). Safe to call once, from
// Startup().
void InitializeFeatures(const FeatureContext& context);

int FeatureCount();
const FeatureDescriptor* FeatureAt(int index);
const FeatureDescriptor* FindFeature(const char* id);
}
