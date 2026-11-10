// ---------------------------------------------------------------------------
// Example feature: UnitClass counter
//
// The smallest complete feature module, meant to be copied when adding a real
// feature. It demonstrates the whole contract:
//   * one FeatureDescriptor registered at load time (VENTURE_REGISTER_FEATURE);
//   * one extension class derived from the platform's extension hierarchy
//     (UnitExt) plus a typed ExtensionStore;
//   * its own engine hooks, gated on the feature switch;
//   * a switch in <Platform>.ini: [Features] Example=yes|no (default: on) -
//     the INI key is descriptor.iniKey, the internal id stays example_unit_counter;
//   * lifecycle handling: allocated on ctor, destroyed on dtor, wiped on
//     scenario clear.
//
// It has no gameplay effect: it only counts UnitClass objects and logs the
// transitions, which makes it a good smoke test for the base.
// ---------------------------------------------------------------------------

#include "Features/Example/ExampleFeature.h"

#include "Core/Log.h"
#include "Core/LogPolicy.h"
#include "Engine/EngineCompat.h"
#include "Ext/ExtHierarchy.h"
#include "Ext/ExtensionIndex.h"
#include "Ext/ExtStream.h"
#include "Ext/ExtensionStore.h"

#include <platform/Feature.h>

namespace venture::features::example
{
namespace
{
// Written by the platform before Initialize() runs (see FeatureContext).
bool g_enabled = false;

int g_created = 0;
int g_destroyed = 0;
int g_baseViews = 0;

// Per-unit transitions are DEBUG lines; every kProgressLogEvery-th unit also
// gets one INFO progress line, so a default (info) log still shows the smoke
// test result without two lines per unit in a big battle.  See docs/Logging.md;
// ShouldLogSample() treats 0 as "no sampling" (log every unit).
constexpr unsigned kProgressLogEvery = 100;

// The example extension derives from the base hierarchy, so feature code can be
// written against `UnitExt::Fetch(unit)` / `TechnoExt::Fetch(techno)` exactly like
// the code the platform is meant to host.  The type-id pair is what makes the
// base-class views validate (there is no RTTI in this build).
class ExampleUnitExt final : public UnitExt
{
public:
	static constexpr std::uint32_t TypeId = 0x0101u;
	static constexpr std::uint32_t BaseTypeId = UnitExt::TypeId;

	explicit ExampleUnitExt(AbstractClass* const owner) : UnitExt(owner) {}

	std::uint32_t ExtTypeId() const override { return TypeId; }

	// A tiny piece of state, so a save/load round trip is observable in the log
	// ("restored N extensions").  A real feature stores its gameplay data here.
	std::uint32_t Serial = 0;

	void Save(ExtStreamWriter& stream) const override
	{
		stream.WriteU32(Serial);
	}

	void Load(ExtStreamReader& stream) override
	{
		stream.ReadU32(Serial);
	}

	static ExampleUnitExt* Fetch(const UnitClass* const pUnit)
	{
		return FetchExtAs<ExampleUnitExt>(pUnit);
	}
};

using ExampleStore = ExtensionStore<UnitClass, ExampleUnitExt>;

ExampleStore& Store()
{
	return ExampleStore::Instance();
}

void Initialize(const FeatureContext& context)
{
	(void)context;
	Store().SetName("UnitClass/ExampleUnitExt");
	// Persist the extension across a save/load cycle (the savegame
	// trailer).  The store's row layout is version 1.
	Store().SetSaveable(true, 1);
	Log::Info("[Example] initialized (enabled=%d, store=%zu)", g_enabled ? 1 : 0, Store().Size());
}
}

void OnUnitCreated(UnitClass* const pUnit)
{
	if (!g_enabled || !pUnit)
		return;

	if (Store().Fetch(pUnit) != nullptr)
		return;

	ExampleUnitExt* const pExt = Store().Allocate(pUnit);
	if (!pExt)
	{
		// While a savegame is loading the allocation is deferred: the trailer may
		// restore this unit's extension, and the post-load step fills in the rest.
		if (ExtensionIndex::Instance().IsLoadingSaveGame())
			return;

		// Another store already claimed this owner: composition error, see
		// ExtensionIndex::Attach.
		Log::Warn("[Example] UnitClass 0x%08X already has an extension of another store",
			static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(pUnit)));
		return;
	}

	pExt->Serial = static_cast<std::uint32_t>(++g_created);

	// Prove the base-class view reaches the very same instance.
	if (UnitExt::Fetch(pUnit) == pExt && FootExt::Fetch(pUnit) == pExt && TechnoExt::Fetch(pUnit) == pExt)
		++g_baseViews;

	// The per-unit transition is a debug detail; the sampled INFO line keeps the
	// smoke test result visible at the default level.
	Log::Debug("[Example] UnitClass 0x%08X created (live=%zu, created=%d, serial=%u)",
		static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(pUnit)), Store().Size(), g_created,
		static_cast<unsigned>(pExt->Serial));

	if (ShouldLogSample(static_cast<unsigned>(g_created), kProgressLogEvery))
		Log::Info("[Example] progress: %d created (live=%zu)", g_created, Store().Size());
}

void OnUnitDestroyed(UnitClass* const pUnit)
{
	if (!pUnit)
		return;

	// Remove even when the feature is currently switched off: the unit may have
	// been created while the feature was on.
	if (Store().Fetch(pUnit) == nullptr)
		return;

	Store().Remove(pUnit);
	++g_destroyed;

	Log::Debug("[Example] UnitClass 0x%08X destroyed (live=%zu, destroyed=%d)",
		static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(pUnit)), Store().Size(), g_destroyed);

	if (ShouldLogSample(static_cast<unsigned>(g_destroyed), kProgressLogEvery))
		Log::Info("[Example] progress: %d destroyed (live=%zu)", g_destroyed, Store().Size());

	// Scenario teardown destroys the live units; when the last tracked unit is
	// gone the totals are worth one INFO line even below the sampling interval.
	if (Store().Size() == 0)
		Log::Info("[Example] no units left (created=%d, destroyed=%d)", g_created, g_destroyed);
}
}

// --- feature registration --------------------------------------------------
namespace venture::features::example
{
namespace
{
const FeatureDescriptor kDescriptor{
	"example_unit_counter", // id: internal identifier
	"Example",              // iniKey: switch in [Features]
	"Example: UnitClass counter",
	Tier::T0,
	&g_enabled,
	&Initialize,
};
}

VENTURE_REGISTER_FEATURE(kDescriptor);
}

// --- hooks -----------------------------------------------------------------
// UnitClass ctor: allocate the example extension.
DEFINE_HOOK(0x7353D3, Venture_Example_UnitClass_CTOR, 0x7)
{
	GET(UnitClass*, pItem, ESI);
	venture::features::example::OnUnitCreated(pItem);
	return 0;
}

// UnitClass dtor (two call sites): remove the example extension.
DEFINE_HOOK_AGAIN(0x7359DA, Venture_Example_UnitClass_DTOR, 0x9)
DEFINE_HOOK(0x735967, Venture_Example_UnitClass_DTOR, 0x9)
{
	GET(UnitClass*, pItem, ESI);
	venture::features::example::OnUnitDestroyed(pItem);
	return 0;
}
