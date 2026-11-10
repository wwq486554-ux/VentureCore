#pragma once

#include <cassert>
#include <cstdint>
#include <utility>
#include <vector>

namespace venture::ExtSwizzle
{
// ---------------------------------------------------------------------------
// Engine pointer-swizzle adapter.
//
// The savegame trailer writes save-time object addresses; after the engine
// loaded its own objects it remaps (swizzles) every registered pointer.  The Ext
// layer must not include engine headers (it is unit-tested without the game), so
// the engine integration installs this adapter from src/Engine/Hooks.Save.cpp at
// startup:
//
//   * RegisterForChange(&field)  - "the pointer stored in `field` is a save-time
//     address; rewrite it once the load completes";
//   * RegisterIdentity(was, is)  - "what was at address `was` is now at `is`".
//
// Without an installed adapter both calls are no-ops, which is exactly what an
// unit test wants until it installs its own fake implementation.
// ---------------------------------------------------------------------------
using RegisterForChangeFn = void (*)(void** pointer);
using RegisterIdentityFn = void (*)(void* was, void* is);

inline RegisterForChangeFn& ForChangeHook()
{
	static RegisterForChangeFn fn = nullptr;
	return fn;
}

inline RegisterIdentityFn& IdentityHook()
{
	static RegisterIdentityFn fn = nullptr;
	return fn;
}

// --- payload-read staging --------------------------------------------------
// A savegame payload is validated row by row, and every row registers pointer
// slots and identities with the engine.  If a later row/block turns out to be
// malformed, the instances created so far are deleted - but the engine would
// still hold their registrations and its remap pass would write through pointers
// into freed memory (A-01).  While staging is active the registrations are
// collected here instead; the savegame layer commits them only after the whole
// payload validated and discards them on every failure path *before* it deletes
// anything.
//
// The staging window is not nested/reentrant: BeginStaging() is followed by
// exactly one CommitStaged() or DiscardStaged().
inline std::vector<void**>& StagedChanges()
{
	static std::vector<void**> staged;
	return staged;
}

inline std::vector<std::pair<void*, void*>>& StagedIdentities()
{
	static std::vector<std::pair<void*, void*>> staged;
	return staged;
}

inline bool& StagingActive()
{
	static bool active = false;
	return active;
}

inline bool IsStaging()
{
	return StagingActive();
}

inline void BeginStaging()
{
	assert(!StagingActive() && "ExtSwizzle staging is already active");
	StagingActive() = true;
}

// Hands every staged registration to the installed engine adapter.  No-op when
// staging is not active.
inline void CommitStaged()
{
	if (!StagingActive())
		return;

	StagingActive() = false;

	if (const RegisterForChangeFn forChange = ForChangeHook())
	{
		for (void** const pointer : StagedChanges())
			forChange(pointer);
	}

	if (const RegisterIdentityFn identity = IdentityHook())
	{
		for (const std::pair<void*, void*>& entry : StagedIdentities())
			identity(entry.first, entry.second);
	}

	StagedChanges().clear();
	StagedIdentities().clear();
}

// Drops every staged registration without touching the engine adapter.  Must run
// before the instances whose slots/identities were staged are deleted.
inline void DiscardStaged()
{
	StagingActive() = false;
	StagedChanges().clear();
	StagedIdentities().clear();
}

inline void Install(const RegisterForChangeFn forChange, const RegisterIdentityFn identity)
{
	ForChangeHook() = forChange;
	IdentityHook() = identity;
}

inline bool Installed()
{
	return ForChangeHook() != nullptr;
}

inline void RegisterForChange(void** const pointer)
{
	if (StagingActive())
	{
		StagedChanges().push_back(pointer);
		return;
	}

	if (const RegisterForChangeFn fn = ForChangeHook())
		fn(pointer);
}

inline void RegisterIdentity(void* const was, void* const is)
{
	if (StagingActive())
	{
		StagedIdentities().emplace_back(was, is);
		return;
	}

	if (const RegisterIdentityFn fn = IdentityHook())
		fn(was, is);
}
}
