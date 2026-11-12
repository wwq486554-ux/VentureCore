#pragma once

#include <platform/Feature.h>

namespace venture
{
// Resolves the feature switches from <Platform>.ini, stores the result in each
// descriptor, and calls the feature's Initialize(). Safe to call once, from
// Startup().
//
// The registry is engine-free and registration happens from static
// initializers, before the logger is open, so registration problems are kept in
// the registry and drained by the startup path after InitializeFeatures() (see
// FeatureRegistrationProblemCount below).
void InitializeFeatures(const FeatureContext& context);

int FeatureCount();
const FeatureDescriptor* FeatureAt(int index);
const FeatureDescriptor* FindFeature(const char* id);

// -- registration diagnostics (API-01 / API-02 / HOST-07) -------------------
// Descriptors rejected at load time (malformed: null/empty id, iniKey or name,
// null Initialize/enabled; duplicate id or iniKey, ASCII case-insensitive) and
// host requirements naming a bit no registered host defines are recorded here,
// in registration order, and reported by Platform::Startup() through the
// logger.  RegisterFeature() is engine-free, so it cannot log directly.
int FeatureRegistrationProblemCount();
const char* FeatureRegistrationProblemAt(int index);
}
