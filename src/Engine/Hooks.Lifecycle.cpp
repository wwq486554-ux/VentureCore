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

// At CRT startup, before WinMain and before the game main loop - this is not
// "about to enter the main loop" (HOOK-07).  0x7CD80F is the PE
// AddressOfEntryPoint and starts with the one-byte `push ebp`; the hook starts at
// 0x7CD810, the `mov ebp,esp; push -1; push 0x7f9798` prologue of the standard
// MSVC CRT startup frame (the SEH chain is set up right after it), and the
// 9-byte window ends exactly on the next instruction (0x7CD819, `push 0x7cc1d4`).
// Running Startup() here is the safe point to read the INI, detect hosts,
// initialize features and open the log, because the engine has not built its own
// state yet.
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
