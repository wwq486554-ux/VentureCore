#include <Syringe.h>
#include <Helpers/Macro.h>

#include "Core/Platform.h"

// ---------------------------------------------------------------------------
// Base lifecycle hooks.
//
// These belong to the base, not to a feature: every platform built from
// VentureCore needs them. Feature-owned hooks live next to their feature under
// src/Features/<Name>/. Hook addresses, sizes and their audit notes are in
// docs/HookCatalogue.md.
// ---------------------------------------------------------------------------

// Engine is about to run the game main loop: safe point to read the INI, detect
// hosts, initialize features and open the log.
DEFINE_HOOK(0x7CD810, Venture_ExeRun, 0x9)
{
	venture::Startup();
	return 0;
}

// Scenario unload: drop all extension state. Extensions are never carried across
// scenarios.
DEFINE_HOOK(0x685659, Venture_ScenarioClear, 0xa)
{
	venture::OnScenarioClear();
	return 0;
}

// The engine announces that an AbstractClass pointer became invalid. The base
// keeps the extension instances with their object's lifetime; this hook records a
// diagnostic count and drops stale deferred allocations (see ExtensionIndex).  It
// is not a feature-facing subscription point yet.
DEFINE_HOOK(0x7258D0, Venture_AnnounceInvalidPointer, 0x6)
{
	GET(AbstractClass* const, pInvalid, ECX);
	GET(bool const, removed, EDX);

	venture::OnInvalidatePointer(pInvalid, removed);
	return 0;
}
