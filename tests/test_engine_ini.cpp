// Unit test: the INI takeover core (owned-key registry, purge, rules-INI
// observers, [Platform] IniTakeover gating) and its typed-read contract.
//
// Runs as a plain console program on this machine: the takeover core only needs
// the small interfaces it declares, so the test implements them over an
// in-memory INI instead of including YRpp / gamemd.exe. The engine adapter
// (src/Engine/Hooks.Ini.cpp) implements the same two interfaces over the game's
// parsed CCINIClass.
//
// TEST_DEPS: src/Core/IniTakeover.cpp

#include "Core/IniTakeover.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

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

char LowerAscii(const char value)
{
	if (value >= 'A' && value <= 'Z')
		return static_cast<char>(value - 'A' + 'a');
	return value;
}

bool EqualsIgnoreCase(const char* left, const char* right)
{
	if (!left || !right)
		return false;

	while (*left && *right)
	{
		if (LowerAscii(*left) != LowerAscii(*right))
			return false;

		++left;
		++right;
	}

	return *left == *right;
}

// In-memory stand-in for the engine's parsed INI. Like the engine adapter it
// implements both sides of the takeover core: case-insensitive lookup and
// case-insensitive, idempotent removal.
class FakeIni final
	: public venture::IniTakeover::IPurgeTarget
	, public venture::IniTakeover::IReader
{
public:
	void Set(const char* const section, const char* const key, const char* const value)
	{
		for (Entry& entry : m_entries)
		{
			if (EqualsIgnoreCase(entry.section.c_str(), section)
				&& EqualsIgnoreCase(entry.key.c_str(), key))
			{
				entry.value = value;
				return;
			}
		}

		m_entries.push_back(Entry{ section, key, value });
	}

	bool Has(const char* const section, const char* const key) const
	{
		return Find(section, key) != nullptr;
	}

	std::size_t Count() const
	{
		return m_entries.size();
	}

	// --- IniTakeover::IPurgeTarget -------------------------------------------
	bool RemoveKey(const char* const section, const char* const key) override
	{
		for (std::size_t index = 0; index < m_entries.size(); ++index)
		{
			if (EqualsIgnoreCase(m_entries[index].section.c_str(), section)
				&& EqualsIgnoreCase(m_entries[index].key.c_str(), key))
			{
				m_entries.erase(m_entries.begin() + static_cast<std::ptrdiff_t>(index));
				return true;
			}
		}

		return false;
	}

	// --- IniTakeover::IReader -------------------------------------------------
	bool ReadBool(const char* const section, const char* const key, const bool defaultValue) override
	{
		const Entry* const pEntry = Find(section, key);
		if (!pEntry)
			return defaultValue;

		const char* const value = pEntry->value.c_str();
		if (EqualsIgnoreCase(value, "yes") || EqualsIgnoreCase(value, "true")
			|| EqualsIgnoreCase(value, "on") || std::strcmp(value, "1") == 0)
		{
			return true;
		}
		if (EqualsIgnoreCase(value, "no") || EqualsIgnoreCase(value, "false")
			|| EqualsIgnoreCase(value, "off") || std::strcmp(value, "0") == 0)
		{
			return false;
		}

		return defaultValue;
	}

	int ReadInt(const char* const section, const char* const key, const int defaultValue) override
	{
		const Entry* const pEntry = Find(section, key);
		if (!pEntry)
			return defaultValue;

		char* end = nullptr;
		const long parsed = std::strtol(pEntry->value.c_str(), &end, 10);
		if (end == pEntry->value.c_str())
			return defaultValue;

		return static_cast<int>(parsed);
	}

	double ReadDouble(const char* const section, const char* const key, const double defaultValue) override
	{
		const Entry* const pEntry = Find(section, key);
		if (!pEntry)
			return defaultValue;

		char* end = nullptr;
		const double parsed = std::strtod(pEntry->value.c_str(), &end);
		if (end == pEntry->value.c_str())
			return defaultValue;

		return parsed;
	}

	void ReadString(
		const char* const section,
		const char* const key,
		const char* const defaultValue,
		char* const buffer,
		const std::size_t bufferSize) override
	{
		if (!buffer || bufferSize == 0)
			return;

		const Entry* const pEntry = Find(section, key);
		_snprintf_s(
			buffer,
			bufferSize,
			_TRUNCATE,
			"%s",
			pEntry ? pEntry->value.c_str() : (defaultValue ? defaultValue : ""));
	}

private:
	struct Entry
	{
		std::string section;
		std::string key;
		std::string value;
	};

	const Entry* Find(const char* const section, const char* const key) const
	{
		for (const Entry& entry : m_entries)
		{
			if (EqualsIgnoreCase(entry.section.c_str(), section)
				&& EqualsIgnoreCase(entry.key.c_str(), key))
			{
				return &entry;
			}
		}

		return nullptr;
	}

	std::vector<Entry> m_entries;
};

// What the rules-INI observer sees. It runs before the purge, so the owned key
// must still be readable there.
struct Observation
{
	int calls = 0;
	bool unitCounterLog = false;
	char text[64] = {};
};

Observation g_observation;

void ObserveRulesIni(venture::IniTakeover::IReader& ini)
{
	++g_observation.calls;
	g_observation.unitCounterLog = ini.ReadBool("Examples", "UnitCounterLog", false);
	ini.ReadString(
		"Examples",
		"UnitCounterLog",
		"<absent>",
		g_observation.text,
		sizeof(g_observation.text));
}
}

int __cdecl main()
{
	using venture::IniTakeover;

	// -- 1. registry defaults ------------------------------------------------
	Check(IniTakeover::OwnedKeyCount() == 0, "the owned-key registry starts empty");
	Check(IniTakeover::Enabled(), "[Platform] IniTakeover defaults to enabled");

	// -- 2. typed reads over the in-memory INI (a) ---------------------------
	FakeIni ini;
	ini.Set("Examples", "UnitCounterLog", "yes");
	ini.Set("Examples", "UnitCounterCap", "42");
	ini.Set("Examples", "UnitCounterScale", "1.5");
	ini.Set("General", "UnitCounterLog", "keep-me-not-owned");

	Check(ini.ReadBool("Examples", "UnitCounterLog", false), "ReadBool sees a value before the purge");
	Check(ini.ReadInt("Examples", "UnitCounterCap", 7) == 42, "ReadInt sees a value before the purge");
	Check(ini.ReadDouble("Examples", "UnitCounterScale", 0.0) > 1.49
		&& ini.ReadDouble("Examples", "UnitCounterScale", 0.0) < 1.51,
		"ReadDouble sees a value before the purge");

	char text[64] = {};
	ini.ReadString("Examples", "UnitCounterLog", "<absent>", text, sizeof(text));
	Check(std::strcmp(text, "yes") == 0, "ReadString sees a value before the purge");

	Check(ini.ReadBool("Examples", "Missing", true), "ReadBool falls back to its default");
	Check(ini.ReadInt("Examples", "Missing", 7) == 7, "ReadInt falls back to its default");
	Check(ini.ReadDouble("Examples", "Missing", 2.5) == 2.5, "ReadDouble falls back to its default");
	text[0] = '\0';
	ini.ReadString("Examples", "Missing", "<default>", text, sizeof(text));
	Check(std::strcmp(text, "<default>") == 0, "ReadString falls back to its default");

	// -- 3. registry is case-insensitive and de-duplicating ------------------
	IniTakeover::RegisterOwnedKey("Examples", "UnitCounterLog");
	IniTakeover::RegisterOwnedKey("examples", "unitcounterlog");
	Check(IniTakeover::OwnedKeyCount() == 1, "the registry de-duplicates case-variant registrations");

	IniTakeover::RegisterOwnedKey(nullptr, "SomeKey");
	IniTakeover::RegisterOwnedKey("Examples", "");
	Check(IniTakeover::OwnedKeyCount() == 1, "null or empty registrations are ignored");

	IniTakeover::RegisterRulesIniObserver(&ObserveRulesIni);
	IniTakeover::RegisterRulesIniObserver(&ObserveRulesIni);
	IniTakeover::RegisterRulesIniObserver(nullptr);

	// -- 4. Apply: observers first, then the purge of owned keys only --------
	const IniTakeover::ApplyResult first = IniTakeover::Apply(&ini, ini);
	Check(g_observation.calls == 1, "the rules-INI observer runs once per Apply");
	Check(g_observation.unitCounterLog, "the observer reads the owned value before the purge");
	Check(std::strcmp(g_observation.text, "yes") == 0, "the observer sees the raw owned value before the purge");
	Check(first.observed == 1, "Apply reports the number of observers it notified");
	Check(first.registered == 1, "Apply reports the registered owned keys");
	Check(first.purged == 1, "Apply removes the owned key");

	// -- 5. (b) after the purge the key is gone and reads return defaults ----
	Check(!ini.Has("Examples", "UnitCounterLog"), "the owned key is gone after the purge");
	Check(!ini.ReadBool("Examples", "UnitCounterLog", false), "a read after the purge returns the default");
	text[0] = '\0';
	ini.ReadString("Examples", "UnitCounterLog", "<absent>", text, sizeof(text));
	Check(std::strcmp(text, "<absent>") == 0, "a string read after the purge returns the default");

	// -- 6. (c) only owned keys are removed ----------------------------------
	Check(ini.Has("Examples", "UnitCounterCap"), "an unowned key in the same section survives the purge");
	Check(ini.Has("General", "UnitCounterLog"), "the same key name in another section survives the purge");

	// -- 7. (f) purge is idempotent ------------------------------------------
	const IniTakeover::ApplyResult second = IniTakeover::Apply(&ini, ini);
	Check(second.purged == 0, "a second purge removes nothing");
	Check(second.observed == 1, "the observer still runs on the second rules-INI load");

	// -- 8. (d) case-insensitive matching against the INI --------------------
	FakeIni mixedCase;
	mixedCase.Set("EXAMPLES", "unitcounterlog", "no");
	const IniTakeover::ApplyResult mixed = IniTakeover::Apply(&mixedCase, mixedCase);
	Check(mixed.purged == 1, "the purge matches the section and key case-insensitively");
	Check(!mixedCase.Has("EXAMPLES", "unitcounterlog"), "the case-variant owned key is gone");

	// -- 9. Apply can purge without a reader (engine/feature split) ----------
	FakeIni purgeOnly;
	purgeOnly.Set("Examples", "UnitCounterLog", "yes");
	const IniTakeover::ApplyResult noReader = IniTakeover::Apply(nullptr, purgeOnly);
	Check(noReader.observed == 0, "Apply without a reader notifies no observer");
	Check(noReader.purged == 1, "Apply without a reader still purges the owned key");

	// -- 10. (e) [Platform] IniTakeover=no: reads work, nothing is purged ----
	IniTakeover::SetEnabled(false);
	Check(!IniTakeover::Enabled(), "SetEnabled(false) turns the takeover off");

	FakeIni gateOff;
	gateOff.Set("Examples", "UnitCounterLog", "yes");
	const IniTakeover::ApplyResult off = IniTakeover::Apply(&gateOff, gateOff);
	Check(off.purged == 0, "IniTakeover=no: nothing is purged");
	Check(gateOff.Has("Examples", "UnitCounterLog"), "IniTakeover=no: the owned key stays visible");
	Check(gateOff.ReadBool("Examples", "UnitCounterLog", false), "IniTakeover=no: the read APIs still work");
	Check(off.observed == 1, "IniTakeover=no: the rules-INI observer still runs");

	IniTakeover::SetEnabled(true);
	Check(IniTakeover::Enabled(), "the takeover can be re-enabled");

	std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAILED",
		g_failures, g_failures == 1 ? "" : "s");
	return g_failures == 0 ? 0 : 1;
}
