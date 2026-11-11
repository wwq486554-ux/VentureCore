#pragma once

// ---------------------------------------------------------------------------
// Process-wide startup gate.
//
// The platform's engine hooks are installed by SyringeEx at DLL load time, long
// before Startup() runs from the ExeRun hook, and the command/event registries
// are populated by static initializers - also before Startup().  The gate is the
// single process-wide answer to "is the platform allowed to act?":
//
//   Pending  - startup has not run yet.  Static registrations collect normally
//              (they are invisible to the engine until the registration hooks
//              consume them); engine-facing handlers stay inert.
//   Running  - Startup() completed successfully; every handler may act.
//   Refused  - Startup() refused to start (a lineage conflict under
//              LineagePolicy=refuse).  The registries reject new entries and
//              every engine-facing handler returns immediately, so the refused
//              DLL does not touch the engine's state (C-06).
//
// This is a header-only module on purpose: the engine-free registry layers
// (src/Command/Commands.cpp, src/Event/EventChannel.cpp) and their unit tests
// must be able to read it without linking the engine-facing Platform.cpp.
//
// Threading: written once on the game's main thread during startup and then
// only read, like the rest of the platform's startup state.
// ---------------------------------------------------------------------------

namespace venture
{
class PlatformGate
{
public:
	enum class State
	{
		Pending,
		Running,
		Refused,
	};

	// Current state; Pending until Startup() finishes.
	static State Current() { return StateRef(); }

	static bool IsPending() { return StateRef() == State::Pending; }
	static bool IsRunning() { return StateRef() == State::Running; }
	static bool IsRefused() { return StateRef() == State::Refused; }

	// Called by Platform::Startup() (MarkRunning/MarkRefused) and
	// Platform::Shutdown() (MarkPending); the Mark* functions are also what the
	// registry unit tests use to enter a state.
	static void MarkRunning() { StateRef() = State::Running; }
	static void MarkRefused() { StateRef() = State::Refused; }
	static void MarkPending() { StateRef() = State::Pending; }

private:
	static State& StateRef()
	{
		// Function-local static: safe from any static-initializer order, never
		// destroyed at unload (the game process owns the DLL).
		static State state = State::Pending;
		return state;
	}
};
}
