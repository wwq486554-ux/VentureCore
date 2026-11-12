#include "Core/IniTakeover.h"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
struct OwnedKey
{
	std::string section;
	std::string key;
};

std::vector<OwnedKey>& OwnedKeys()
{
	// Function-local statics: safe from any static initializer order, like the
	// feature registry.
	static std::vector<OwnedKey> keys;
	return keys;
}

std::vector<venture::IniTakeover::RulesIniObserver>& Observers()
{
	static std::vector<venture::IniTakeover::RulesIniObserver> observers;
	return observers;
}

bool g_enabled = true;
std::size_t g_refusedKeys = 0;
venture::IniTakeover::WarnSink g_warnSink = nullptr;

void Warn(const char* const message)
{
	const venture::IniTakeover::WarnSink sink = g_warnSink;
	if (sink)
		sink(message);
}

// Sections the engine parses and reads itself (rules(md).ini).  Owning a key in
// one of them would silently remove it before the engine reads it, so the
// takeover refuses these (C-09).  Case-insensitive; keep the list to sections
// the engine/host platforms are known to consume.
//
// Two kinds of engine core section are listed:
//   * the scalar/global rules sections (log, balance, colours, ...);
//   * the engine's type-list sections.  RulesClass::Init reads those itself right
//     after our purge hook (the type-name lists it registers/validates:
//     InfantryTypes ... OverlayTypes), so owning a type name there would delete
//     the type itself before the engine sees it (START-02).  [Projectiles] is
//     not an engine type-list section in this build, and [Weapons] has no
//     standalone section name - do not add guessed names: a name that the engine
//     does not read would refuse a legitimate platform-namespaced section.
const char* const kReservedEngineSections[] = {
	"General",
	"Basic",
	"AudioVisual",
	"MultiPlayer",
	"Maximums",
	"AI",
	"Colors",
	"Countries",
	"Sides",
	"CrateRules",
	"CombatDamage",
	"Radiation",
	"ElevationModel",
	"WallModel",
	"SpecialFlags",
	"Powerups",
	"LandCharacteristics",
	"MouseCursors",
	// Engine type-list sections read by RulesClass::Init (START-02).
	"InfantryTypes",
	"VehicleTypes",
	"AircraftTypes",
	"BuildingTypes",
	"SuperWeaponTypes",
	"Warheads",
	"Teams",
	"TerrainTypes",
	"SmudgeTypes",
	"OverlayTypes",
};

// ASCII-only case folding. The engine's INI names are ASCII, and a local
// implementation keeps this translation unit free of <cctype>/locale behaviour.
char LowerAscii(const char value)
{
	if (value >= 'A' && value <= 'Z')
		return static_cast<char>(value - 'A' + 'a');
	return value;
}

// Returns the lower-cased copy used for comparisons; an empty string for null.
std::string Folded(const char* const text)
{
	std::string folded;
	if (!text)
		return folded;

	const std::size_t length = std::strlen(text);
	folded.reserve(length);
	for (std::size_t index = 0; index < length; ++index)
		folded.push_back(LowerAscii(text[index]));

	return folded;
}

bool IsEmpty(const std::string& text)
{
	return text.empty();
}
}

namespace venture
{
void IniTakeover::SetWarnSink(const WarnSink sink)
{
	g_warnSink = sink;
}

bool IniTakeover::IsReservedEngineSection(const char* const section)
{
	const std::string foldedSection = Folded(section);
	if (IsEmpty(foldedSection))
		return false;

	for (const char* const reserved : kReservedEngineSections)
	{
		if (foldedSection == Folded(reserved))
			return true;
	}

	return false;
}

std::size_t IniTakeover::RefusedKeyCount()
{
	return g_refusedKeys;
}

void IniTakeover::RegisterOwnedKey(const char* const section, const char* const key)
{
	const std::string foldedSection = Folded(section);
	const std::string foldedKey = Folded(key);

	// Both parts are mandatory: a half-empty pair can never be removed reliably.
	if (IsEmpty(foldedSection) || IsEmpty(foldedKey))
		return;

	// A platform-owned key in an engine core section would be deleted before the
	// engine reads it - refuse and report instead of silently breaking the game
	// or a host platform (C-09).
	if (IsReservedEngineSection(section))
	{
		++g_refusedKeys;
		char message[320] = {};
		std::snprintf(
			message,
			sizeof(message),
			"INI takeover: [%s] %s refused as platform-owned - '%s' is an engine core section; "
			"put platform keys in a platform-namespaced section (see docs/Ini.md)",
			section,
			key,
			section);
		Warn(message);
		return;
	}

	for (const OwnedKey& existing : OwnedKeys())
	{
		if (existing.section == foldedSection && existing.key == foldedKey)
			return;
	}

	OwnedKeys().push_back(OwnedKey{ foldedSection, foldedKey });
}

std::size_t IniTakeover::OwnedKeyCount()
{
	return OwnedKeys().size();
}

void IniTakeover::RegisterRulesIniObserver(const RulesIniObserver observer)
{
	if (!observer)
		return;

	for (const RulesIniObserver existing : Observers())
	{
		if (existing == observer)
			return;
	}

	Observers().push_back(observer);
}

IniTakeover::ApplyResult IniTakeover::Apply(IReader* const reader, IPurgeTarget& target)
{
	ApplyResult result{ 0, 0, OwnedKeys().size() };

	// Readers first: a feature can only see its own key while the takeover has
	// not hidden it yet. This runs in both switch states - turning the interop
	// off must not break the read side.
	if (reader)
	{
		for (const RulesIniObserver observer : Observers())
		{
			if (observer)
			{
				observer(*reader);
				++result.observed;
			}
		}
	}

	if (!g_enabled)
		return result;

	// The target is asked to remove exactly the registered pairs, so a
	// non-owned key is never touched. Removal is case-insensitive on both sides.
	for (const OwnedKey& owned : OwnedKeys())
	{
		if (target.RemoveKey(owned.section.c_str(), owned.key.c_str()))
			++result.purged;
	}

	return result;
}

void IniTakeover::SetEnabled(const bool enabled)
{
	g_enabled = enabled;
}

bool IniTakeover::Enabled()
{
	return g_enabled;
}
}
