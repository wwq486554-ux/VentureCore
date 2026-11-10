#pragma once

#include <platform/Feature.h>

class AbstractClass;

namespace venture
{
// Starts the platform from the ExeRun hook: verifies SyringeEx, opens the log,
// loads <Platform>.ini, detects optional hosts, resolves the support tier, runs
// the lineage mutual-exclusion check and initializes every registered feature.
//
// Returns false when the platform refused to start.  An unsupported Syringe
// terminates the game (see Platform.cpp); a lineage conflict under
// [Platform] LineagePolicy=refuse only leaves the platform disabled - the game
// keeps running without the platform's features, and every lifecycle entry point
// below stays a no-op (g_started is never set).
bool Startup();

void Shutdown();

// Lifecycle entry points called by the engine hooks in Engine/Hooks.Lifecycle.cpp.
void OnScenarioClear();
void OnInvalidatePointer(AbstractClass* pInvalid, bool removed);

unsigned HostMask();
Tier CurrentTier();
}
