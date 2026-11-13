// Unit test: the savegame load-window lifecycle (A-04 / A-06 / A-07).
//
// Runs as a plain console program: the extension index is engine-free, so the
// whole lifecycle can be driven directly - open the window, defer the allocations
// of object constructors, close it on the success path and on the three failure
// exits, reset it on scenario clear, and drop a deferred entry when the engine
// invalidates the owner pointer.
//
// TEST_DEPS: src/Ext/AbstractExt.cpp src/Ext/ExtensionIndex.cpp src/Ext/ExtSave.cpp src/Core/Log.cpp src/Core/PlatformPaths.cpp

#include <cstdint>
#include <cstdio>

class AbstractClass
{
public:
	virtual ~AbstractClass() = default;
};

class ObjectClass : public AbstractClass {};
class TechnoClass : public ObjectClass {};
class FootClass : public TechnoClass {};
class UnitClass : public FootClass {};

#include "Ext/ExtHierarchy.h"
#include "Ext/ExtensionStore.h"
#include "Ext/ExtSave.h"
#include "Ext/ExtSwizzle.h"

namespace
{
int g_failures = 0;

void Check(const bool ok, const char* const what)
{
	if (ok)
	{
		std::printf("ok    %s\n", what);
	}
	else
	{
		++g_failures;
		std::printf("FAIL  %s\n", what);
	}
}

// Engine-free extension: the load window only cares about allocation and the
// deferred list, not about savegame rows.
class WindowUnitExt final : public venture::UnitExt
{
public:
	static constexpr std::uint32_t TypeId = 0x0131u;
	static constexpr std::uint32_t BaseTypeId = venture::UnitExt::TypeId;

	explicit WindowUnitExt(AbstractClass* const owner) : UnitExt(owner) {}

	std::uint32_t ExtTypeId() const override { return TypeId; }

	int DeferredCalls = 0;

	void OnDeferredAllocation() override
	{
		++DeferredCalls;
	}
};

using WindowStore = venture::ExtensionStore<UnitClass, WindowUnitExt>;
}

int __cdecl main()
{
	venture::ExtensionIndex& index = venture::ExtensionIndex::Instance();
	WindowStore& store = WindowStore::Instance();
	store.SetName("UnitClass/WindowUnitExt");

	// ---- 1. outside a load: allocation is immediate ------------------------
	{
		UnitClass unit;
		WindowUnitExt* const ext = store.Allocate(&unit);
		Check(ext != nullptr && store.Fetch(&unit) == ext,
			"a normal allocation creates the extension immediately");
		Check(!index.IsLoadingSaveGame(), "no load window is open outside a savegame load");
		Check(index.DeferredAllocations().empty(), "a normal allocation does not touch the deferred list");
		index.ClearAll();
	}

	// ---- 2. the engine's load window defers constructor allocations --------
	{
		UnitClass objectA;
		UnitClass objectB;

		index.EnterLoadWindow();
		Check(index.IsLoadingSaveGame(), "the load window opens");

		Check(store.Allocate(&objectA) == nullptr,
			"an object constructor allocation is deferred while the window is open");
		Check(store.Allocate(&objectB) == nullptr, "every constructor allocation is deferred");
		Check(store.Size() == 0, "no extension is created during the load window");
		Check(index.DeferredAllocations().size() == 2, "both owners are recorded for after the load");

		// A-04: the engine's failure exits (0x67E4B1 / 0x67E53A / 0x67E595)
		// close the window through this API instead of returning while it is open.
		index.ExitLoadWindow();
		Check(!index.IsLoadingSaveGame(), "the failure-exit handler closes the window");

		UnitClass objectC;
		WindowUnitExt* const ext = store.Allocate(&objectC);
		Check(ext != nullptr, "after a failed load, allocations take the normal path again");
		Check(index.DeferredAllocations().size() == 2, "a normal allocation adds no deferred record");
		index.ResetForScenarioClear();
		index.ClearAll();
	}

	// ---- 3. closing the window is idempotent ------------------------------
	{
		index.ExitLoadWindow();
		index.ExitLoadWindow();
		Check(!index.IsLoadingSaveGame(), "closing an already closed window is a no-op");
	}

	// ---- 4. A-04 fallback: scenario clear resets window and deferred list --
	{
		UnitClass object;
		index.EnterLoadWindow();
		Check(store.Allocate(&object) == nullptr, "allocation is deferred again while the window is open");
		Check(!index.DeferredAllocations().empty(), "the aborted load left a deferred record");

		index.ResetForScenarioClear();
		Check(!index.IsLoadingSaveGame(), "scenario clear closes a window a failure path left open");
		Check(index.DeferredAllocations().empty(),
			"scenario clear drops the deferred records of the aborted load");

		// Idempotent, and safe when no window is open.
		index.ResetForScenarioClear();
		Check(!index.IsLoadingSaveGame() && index.DeferredAllocations().empty(),
			"scenario clear is idempotent");

		UnitClass fresh;
		Check(store.Allocate(&fresh) != nullptr, "the next scenario allocates extensions normally");
		index.ClearAll();
	}

	// ---- 5. A-06: pointer invalidation drops the deferred owner ------------
	{
		UnitClass deadObject;
		UnitClass liveObject;

		index.EnterLoadWindow();
		Check(store.Allocate(&deadObject) == nullptr && store.Allocate(&liveObject) == nullptr,
			"two owners are deferred during the load");
		Check(index.DeferredAllocations().size() == 2, "both deferred owners are recorded");

		index.NotifyInvalidPointer(&deadObject, true);
		Check(index.InvalidPointerNotifications() == 1,
			"the pointer-invalidation notification is counted");
		Check(index.DeferredAllocations().size() == 1
				&& index.DeferredAllocations()[0].owner == &liveObject,
			"the invalidated owner's deferred allocation is dropped");

		// Re-notifying the same pointer is harmless.
		index.NotifyInvalidPointer(&deadObject, true);
		Check(index.DeferredAllocations().size() == 1,
			"re-notifying the same invalid pointer changes nothing");

		index.ResetForScenarioClear();
		index.ClearAll();
	}

	// ---- 6. A-07: FinishLoad, then close, then the hooked call's allocations -
	{
		UnitClass constructedDuringLoad;
		UnitClass constructedAfterClose;

		index.EnterLoadWindow();
		Check(store.Allocate(&constructedDuringLoad) == nullptr,
			"the object the engine created during the load is deferred");

		// Order of the 0x67E685 handler: the real FinishLoad drains the deferred
		// list, then the window is closed before the hooked `call 0x685120`
		// re-executes.  Test the production entry point instead of a hand-copied
		// drain loop, which used to hide a drift between the test and FinishLoad
		// (SAVE-07).
		const venture::ExtSave::LoadSummary finished = venture::ExtSave::FinishLoad();
		Check(finished.deferred == 1 && finished.adopted == 0 && finished.failed == 0,
			"the real FinishLoad drains the deferred allocation");
		index.ExitLoadWindow();
		Check(!index.IsLoadingSaveGame(), "the window is closed right after FinishLoad");

		// The hooked `call 0x685120` now runs: anything it constructs must take the
		// normal path.  With the old order (close at 0x67E68A) this allocation
		// would be deferred after the drain and dropped later without an extension.
		WindowUnitExt* const ext = store.Allocate(&constructedAfterClose);
		Check(ext != nullptr, "an allocation made by the call after the close takes the normal path");
		Check(index.DeferredAllocations().empty(),
			"the post-drain allocation is not lost in the deferred list");

		WindowUnitExt* const deferredExt = store.Fetch(&constructedDuringLoad);
		Check(deferredExt != nullptr && deferredExt->DeferredCalls == 1,
			"the deferred owner got its extension and OnDeferredAllocation during FinishLoad");

		index.ClearAll();
	}

	std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAILED",
		g_failures, g_failures == 1 ? "" : "s");
	return g_failures == 0 ? 0 : 1;
}
