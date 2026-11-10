#pragma once

#include <cstddef>

// ---------------------------------------------------------------------------
// INI takeover (D33): hide the keys the platform owns from the other platform
// DLLs loaded next to it, so a key defined for a VentureCore feature is not also
// consumed by a host platform (official Phobos and friends) and counted twice.
//
// How it works
// ------------
// * A feature registers every (section, key) pair it owns while the feature
//   switch is on, from its Initialize() callback - that runs from the platform
//   startup hook, before the engine parses rules(md).ini.
// * The engine hook (src/Engine/Hooks.Ini.cpp) fires when the engine has parsed
//   rules(md).ini and is about to read it (RulesClass::Init entry). It calls
//   Apply(): the rules-INI observers get a read-only view of the real parsed INI
//   first, then every registered key is removed from it through the engine
//   adapter. Hosts that read the same INIClass afterwards only see their own
//   keys.
// * [Platform] IniTakeover=yes|no (default yes) gates only the removal: with the
//   switch off the read APIs and the observers still work and nothing is
//   purged, so a user can deliberately opt out of the interop.
//
// The core below is pure C++ (no engine headers): the removal back end and the
// read view are small interfaces the engine adapter implements over CCINIClass
// and the unit test implements over an in-memory INI.
//
// Threading: the registry is populated during startup and consumed when the
// engine parses the rules INI, both on the game's main thread. There are no
// locks; do not register keys from a worker thread.
// ---------------------------------------------------------------------------

namespace venture
{
class IniTakeover
{
public:
	// Removal back end. The engine adapter implements it over the game's parsed
	// INIClass; the unit test implements it over a std::map-like store.
	class IPurgeTarget
	{
	public:
		virtual ~IPurgeTarget() = default;

		// Removes (section, key) case-insensitively. Returns false when the
		// section or the key is absent. Must be safe to call repeatedly.
		virtual bool RemoveKey(const char* section, const char* key) = 0;
	};

	// Read-only typed view of the parsed INI handed to the rules-INI observers
	// before the purge runs. Same contract as EngineIni.h, but abstract so the
	// core does not depend on the engine.
	class IReader
	{
	public:
		virtual ~IReader() = default;

		virtual bool ReadBool(
			const char* section,
			const char* key,
			bool defaultValue) = 0;

		virtual int ReadInt(
			const char* section,
			const char* key,
			int defaultValue) = 0;

		virtual double ReadDouble(
			const char* section,
			const char* key,
			double defaultValue) = 0;

		virtual void ReadString(
			const char* section,
			const char* key,
			const char* defaultValue,
			char* buffer,
			std::size_t bufferSize) = 0;
	};

	// Called once per rules-INI load, before the purge, with the real parsed INI.
	using RulesIniObserver = void (*)(IReader& ini);

	struct ApplyResult
	{
		std::size_t observed;   // rules-INI observers notified
		std::size_t purged;     // owned keys actually removed from the INI
		std::size_t registered; // keys in the platform registry
	};

	// Registers a (section, key) pair the platform owns. Case-insensitive and
	// de-duplicating; call it from the owning feature's Initialize() while the
	// feature switch is on. Empty or null arguments are ignored.
	static void RegisterOwnedKey(const char* section, const char* key);

	// Number of distinct owned keys in the registry.
	static std::size_t OwnedKeyCount();

	// Registers a callback that reads from the parsed rules INI before the purge.
	// Used by features that need their own keys' real values; duplicate
	// registrations are ignored and null is ignored.
	static void RegisterRulesIniObserver(RulesIniObserver observer);

	// Runs the rules-INI stage: notifies every observer with the pre-purge view,
	// then removes every registered key when takeover is enabled. The reader is
	// optional (pass nullptr to only purge). Idempotent: a second call on an INI
	// the keys were already removed from removes nothing and reports purged = 0.
	static ApplyResult Apply(IReader* reader, IPurgeTarget& target);

	// [Platform] IniTakeover=yes|no, resolved by the engine hook when the rules
	// INI is first loaded. Default: enabled (the interop is the point).
	static void SetEnabled(bool enabled);
	static bool Enabled();
};
}
