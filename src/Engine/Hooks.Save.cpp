#include <Syringe.h>
#include <Helpers/Macro.h>

#include <windows.h>
#include <Objidl.h>

#include "Core/Log.h"
#include "Core/SaveTrailer.h"
#include "Engine/EngineCompat.h"
#include "Ext/ExtSwizzle.h"

// ---------------------------------------------------------------------------
// Savegame hooks: the platform's extension state travels in an anchored tail
// block (see src/Core/SaveTrailer.h and docs/SaveFormat.md).
//
// The save/load sites are the ecosystem's shared ones:
//   * 0x67E42E  - end of the engine's save routine.  Every platform installs its
//                 writer here (Ares size 5, Kratos size 13); our DLL loads after
//                 them, so our handler runs last and our block is the last one in
//                 the stream.  Our window is 5 bytes: `test eax,eax`(2) +
//                 `pop edi`(1) + `pop esi`(1) + `pop ebp`(1) = ends on the next
//                 instruction (0x67E433, `pop ebx`).
//   * 0x67F7C8 - end of the engine's load-objects routine, after every platform's
//                 sequential reader (Ares/Kratos also read here).  We seek to the
//                 end of the stream, validate the trailer, read it and restore the
//                 stream position, so we neither depend on nor disturb the other
//                 platforms' block order.
//   * 0x67E44D/0x67E68A - the engine's load window open/close.  While it is open,
//                 allocations driven by object constructors are deferred, because
//                 the trailer may carry an extension for the same owner.
//   * 0x67E685 - first instruction after the engine's swizzle pass.  The owner
//                 pointers recorded in the trailer have been remapped by now, so
//                 the loaded extensions can be attached.  The window is closed
//                 right here, before the hooked `call 0x685120` re-executes
//                 (A-07): allocations made by that call then take the normal path
//                 instead of being deferred after the drain and lost.
//   * 0x67E4B1/0x67E53A/0x67E595 - the three early exits of LoadGame (0x67E440):
//                 the two StgOpenStorage failures and the COM-method failure.
//                 They return before the engine's close point, so each one closes
//                 the load window itself (A-04); without these hooks the window
//                 stays open for the rest of the game session and every later
//                 extension allocation is silently deferred.  Each window is the
//                 5-byte prefix of an epilogue and ends on the next instruction
//                 (0x67E4B6/0x67E53F/0x67E59A, `pop ebx`).
//
// A scenario clear / return to the main menu additionally resets the window and
// the deferred records (Core/Platform.cpp::OnScenarioClear), so a load aborted
// through a path without a hook cannot poison the next scenario either.
//
// All eight windows are size 5 and every one ends on an instruction boundary (see
// docs/HookCatalogue.md).
// ---------------------------------------------------------------------------

namespace
{
// The extension layer must not include engine headers; these two thunks are the
// bridge to the engine's swizzle manager.
void RegisterForChangeEngine(void** const pointer)
{
	if (pointer)
		SwizzleManagerClass::Instance.Swizzle(pointer);
}

void RegisterIdentityEngine(void* const was, void* const is)
{
	// The native manager keys identity changes by the save-time pointer value,
	// exactly like a port block would; the first parameter is the id.
	SwizzleManagerClass::Instance.Here_I_Am(reinterpret_cast<long>(was), is);
}

void EnsureEngineServices()
{
	static bool installed = false;
	if (installed)
		return;

	installed = true;
	venture::ExtSwizzle::Install(&RegisterForChangeEngine, &RegisterIdentityEngine);
}
}

// The engine starts loading a savegame: open the deferred-allocation window.
DEFINE_HOOK(0x67E44D, Venture_LoadGame_Begin, 0x5)
{
	EnsureEngineServices();
	venture::SaveTrailer::EnterLoadWindow();
	return 0;
}

// A-04: the engine aborts the load at an early exit, before its normal close
// point (0x67E68A).  Close the window here so later allocations take the normal
// path again; the close is idempotent, so the success path is unaffected.
DEFINE_HOOK(0x67E4B1, Venture_LoadGame_Failed1, 0x5)
{
	venture::SaveTrailer::ExitLoadWindow();
	return 0;
}

// A-04: second StgOpenStorage failure of LoadGame.
DEFINE_HOOK(0x67E53A, Venture_LoadGame_Failed2, 0x5)
{
	venture::SaveTrailer::ExitLoadWindow();
	return 0;
}

// A-04: COM-method failure of LoadGame.
DEFINE_HOOK(0x67E595, Venture_LoadGame_Failed3, 0x5)
{
	venture::SaveTrailer::ExitLoadWindow();
	return 0;
}

// End of the engine's save routine: append the platform trailer after every other
// platform's block.  `EAX` carries the engine's HRESULT; do not append on failure.
DEFINE_HOOK(0x67E42E, Venture_SaveGame_Trailer, 0x5)
{
	GET(long, status, EAX);
	if (status < 0)
		return 0;

	GET(IStream*, pStm, ESI);
	EnsureEngineServices();
	venture::SaveTrailer::Write(pStm);
	return 0;
}

// End of the engine's load routine: find and read the tail block.
DEFINE_HOOK(0x67F7C8, Venture_LoadGame_Trailer, 0x5)
{
	GET(IStream*, pStm, ESI);
	EnsureEngineServices();
	venture::SaveTrailer::Read(pStm);
	return 0;
}

// First instruction after the engine's swizzle pass: attach the restored
// extensions to their (now remapped) owners.
DEFINE_HOOK(0x67E685, Venture_LoadGame_PostSwizzle, 0x5)
{
	EnsureEngineServices();
	venture::SaveTrailer::FinishLoad();

	// A-07: close before the hooked `call 0x685120` re-executes.  FinishLoad has
	// already drained the deferred list, so an allocation made by that call while
	// the window were still open would be deferred again and never processed.
	venture::SaveTrailer::ExitLoadWindow();
	return 0;
}

// The engine finished loading: normal allocation resumes.  Kept as the documented
// close point; the post-swizzle hook already closed the window, so this is an
// idempotent second close (A-07).
DEFINE_HOOK(0x67E68A, Venture_LoadGame_End, 0x5)
{
	venture::SaveTrailer::ExitLoadWindow();
	return 0;
}
