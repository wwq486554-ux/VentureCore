#pragma once

#include <cstdint>

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
	if (const RegisterForChangeFn fn = ForChangeHook())
		fn(pointer);
}

inline void RegisterIdentity(void* const was, void* const is)
{
	if (const RegisterIdentityFn fn = IdentityHook())
		fn(was, is);
}
}
