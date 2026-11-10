#include <Syringe.h>
#include <Helpers/Macro.h>

#include <windows.h>
#include <Objidl.h>

#include "Core/Log.h"
#include "Core/SaveTrailer.h"
#include "Engine/EngineCompat.h"
#include "Ext/ExtSwizzle.h"

// ---------------------------------------------------------------------------
// Savegame hooks: the platform's extension state travels in a framed tail block
// that is appended and read strictly sequentially (the engine's savegame IStream
// is a streaming wrapper that does not implement Seek - see
// src/Core/SaveTrailer.h and docs/SaveFormat.md).
//
// The save/load sites are the ecosystem's shared ones:
//   * 0x67D1B4  - inside ScenarioClass::SaveGame, after the engine's object-state
//                 writer (`call 0x67D300` at 0x67D1AF) has returned and before
//                 the finalize/Commit step at 0x67D1CC: the data stream is still
//                 writable there, and writing after the Commit is not guaranteed
//                 to reach the file.  The ecosystem's tail-block sites live
//                 *inside* the routine we just called (Ares and Kratos hook
//                 0x67E42E there); Ares's handler always returns a jump address,
//                 so that address can never host our writer - in a process with
//                 Ares our handler there would never run (or its block would be
//                 written after ours anyway).  Writing at 0x67D1B4 instead puts
//                 our block after every platform's block regardless of handler
//                 order, and no known platform hooks this site (the five
//                 ecosystem `.syhks00` tables are free there).  Window:
//                 `mov bl,al`(2) + `mov eax,[esp+0x14]`(4), next instruction
//                 0x67D1BA.  The save stream is the IStream* the engine loads
//                 into ECX for 0x67D300, i.e. [esp+0x1C] here.  Only AL of EAX
//                 is that writer's BOOL result: every failure epilogue executes
//                 `xor al,al` and leaves the (negative) HRESULT in the upper 24
//                 bits, while the success tail executes `setge al`; the engine
//                 itself consumes only AL (`mov bl,al`).  AL == 0 = the save
//                 failed, do not append.  The append is sequential: the engine's
//                 savegame stream does not support Seek, and this handler runs
//                 after every ecosystem writer, so the current position is the
//                 end of the save data.
//   * 0x67F7C8 - end of the engine's load-objects routine, after every platform's
//                 sequential reader (Ares/Kratos also read here).  We read the
//                 trailer sequentially from the current position - which is exactly
//                 our block, because our handler runs after every other platform's
//                 reader - validate it and hand the payload to the load layer.  No
//                 Seek and no position restore (the stream cannot seek).
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
// All eight windows contain complete instructions and end on the next
// instruction (see docs/HookCatalogue.md).
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

// The engine's object-state writer returned and the save stream is still open
// for writing (before the finalize/Commit): append the platform trailer.
// Only AL of `EAX` is the writer's BOOL result - every failure epilogue runs
// `xor al,al` and leaves the negative HRESULT in the upper 24 bits, so testing
// the full EAX for 0 would never detect a failed save; the success tail runs
// `setge al`.  AL == 0 means the save failed - do not append.
DEFINE_HOOK(0x67D1B4, Venture_SaveGame_Trailer, 0x6)
{
	GET(long, status, EAX);
	if ((status & 0xFFu) == 0)
		return 0;

	GET_STACK(IStream*, pStm, 0x1C);
	if (!pStm)
	{
		venture::Log::Warn("savegame trailer: no save stream at the save site - not appended");
		return 0;
	}

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
