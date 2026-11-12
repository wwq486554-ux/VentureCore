#pragma once

#include <platform/Feature.h>

class AbstractClass;

namespace venture
{
// Starts the platform from the ExeRun hook: verifies SyringeEx, opens the log,
// loads <Platform>.ini, detects the optional hosts, runs the lineage
// mutual-exclusion check and initializes every registered feature.
//
// Returns false when the platform refused to start.  An unsupported Syringe
// terminates the game (see Platform.cpp); a lineage conflict under
// [Platform] LineagePolicy=refuse only leaves the platform disabled - the game
// keeps running without the platform's features, and every lifecycle entry point
// below stays a no-op (the startup gate stays Refused, see Core/PlatformGate.h).
bool Startup();

// Tears the platform down and closes the log.  This is an entry point for a
// derived platform's own teardown path - the base itself never calls it (no
// engine hook triggers it).  Shutdown() is final for the process: feature
// initialization is once-only (`InitializeFeatures` is documented to be safe to
// call once, from Startup), so the startup gate becomes
// `PlatformGate::State::Finished` and a later Startup() returns false instead
// of initializing every feature a second time.  Calling it before a successful
// Startup() is a no-op.  See src/Core/PlatformGate.h and START-04.
void Shutdown();

// True once Startup() completed successfully.  The engine-facing hook handlers
// (Engine/Hooks.*.cpp) return immediately when it is false, so a platform that
// was refused (lineage conflict under LineagePolicy=refuse) does not touch the
// engine's command list, event stream, rules INI or savegame stream even though
// SyringeEx keeps its hooks installed (C-06).  See src/Core/PlatformGate.h.
bool IsStarted();

// Lifecycle entry points called by the engine hooks in Engine/Hooks.Lifecycle.cpp.
void OnScenarioClear();
void OnInvalidatePointer(AbstractClass* pInvalid, bool removed);

// Detected optional hosts as a bit set (platform/HostIds.h + custom hosts).
unsigned HostMask();
}
