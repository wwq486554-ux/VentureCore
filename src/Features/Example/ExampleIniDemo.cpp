// ---------------------------------------------------------------------------
// Example feature: rules(md).ini read + owned-key takeover
//
// The smallest module that exercises the INI layer of the platform:
//   * reads a typed key from the engine's parsed rules(md).ini through
//     Core/EngineIni.h (the header for feature authors to copy);
//   * registers the same (section, key) pair as platform-owned, so the INI
//     takeover (Core/IniTakeover.h, hook in src/Engine/Hooks.Ini.cpp) removes it
//     from the parsed rules INI before a host platform such as official Phobos
//     can read it - the anti-double-count contract (D33);
//   * demonstrates the rules-INI notification: the engine parses rules(md).ini
//     after the platform starts, so a feature that needs the value reads it in
//     the notification, before the takeover purge hides the key.
//
// Its switch is [Features] ExampleIniKey=yes|no (default: on). With the switch
// off nothing is registered and nothing is read. It has no gameplay effect; the
// example key is
//
//   [Examples]
//   UnitCounterLog=yes
//
// and it is only useful as a smoke test for the INI path.
// ---------------------------------------------------------------------------

#include "Core/EngineIni.h"
#include "Core/IniTakeover.h"
#include "Core/Log.h"
#include "Engine/EngineCompat.h"

#include <platform/Feature.h>

namespace venture::features::example_ini
{
namespace
{
// Written by the platform before Initialize() runs (see FeatureContext).
bool g_enabled = false;

const char* const kSection = "Examples";
const char* const kKey = "UnitCounterLog";

// Delivered by the takeover hook once the engine has parsed rules(md).ini and is
// about to initialize RulesClass - before the takeover removes this key from the
// same parsed INI, so this is the real value the game will use.
void OnRulesIniReady(IniTakeover::IReader& rulesIni)
{
	char raw[64] = {};
	rulesIni.ReadString(kSection, kKey, "<absent>", raw, sizeof(raw));
	const bool enabled = rulesIni.ReadBool(kSection, kKey, false);

	Log::Info(
		"[ExampleIniKey] rules(md).ini [%s] %s=%s (bool=%s)",
		kSection,
		kKey,
		raw,
		enabled ? "yes" : "no");
}

void __cdecl Initialize(const FeatureContext& context)
{
	(void)context;

	if (!g_enabled)
	{
		Log::Info("[ExampleIniKey] disabled - the example key is not read or registered as platform-owned");
		return;
	}

	// Register before the engine parses rules(md).ini: Startup() runs from the
	// ExeRun hook, which fires before the rules INI is loaded.
	IniTakeover::RegisterOwnedKey(kSection, kKey);
	IniTakeover::RegisterRulesIniObserver(&OnRulesIniReady);

	// A host may already have loaded rules(md).ini before this module starts
	// (and a derived platform can call Startup() later than VentureCore does); if
	// so, read the value here as well. Normally the INI is not parsed yet at this
	// point and the notification above carries the value.
	if (CCINIClass* const pRules = CCINIClass::INI_Rules)
	{
		char raw[64] = {};
		EngineIni::ReadString(pRules, kSection, kKey, "<absent>", raw, sizeof(raw));
		const bool enabled = EngineIni::ReadBool(pRules, kSection, kKey, false);

		Log::Info(
			"[ExampleIniKey] startup read [%s] %s=%s (bool=%s)",
			kSection,
			kKey,
			raw,
			enabled ? "yes" : "no");
	}
	else
	{
		Log::Info(
			"[ExampleIniKey] armed: [%s] %s will be read and hidden from host platforms when the engine parses rules(md).ini",
			kSection,
			kKey);
	}
}
}

const FeatureDescriptor kDescriptor{
	"example_ini_key",       // id: internal identifier
	"ExampleIniKey",         // iniKey: switch in [Features]
	"Example: rules INI read + owned-key takeover",
	0,                       // requiredHosts (AND): no host dependency
	0,                       // anyOfHosts (OR): no host dependency
	&g_enabled,
	&Initialize,
};

VENTURE_REGISTER_FEATURE(kDescriptor);
}
