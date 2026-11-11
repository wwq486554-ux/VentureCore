#include <Syringe.h>
#include <Helpers/Macro.h>

#include "Core/EngineIni.h"
#include "Core/Ini.h"
#include "Core/IniTakeover.h"
#include "Core/Log.h"
#include "Core/Platform.h"
#include "Core/PlatformConfig.h"
#include "Engine/EngineCompat.h"

// ---------------------------------------------------------------------------
// Rules INI takeover (D33).
//
// The engine parses rules(md).ini into CCINIClass::INI_Rules and then calls
// RulesClass::Init(CCINIClass*) to read it. The first instruction of that
// function is sub esp,0xd0 (6 bytes, next instruction at 0x6686C6), so hooking
// its entry lets this module run after the file has been parsed but before any
// rules key is read - both by the engine and by a host platform such as official
// Phobos, whose own hook at 0x668BF0 (RulesClass_Addition) is called from inside
// this same function at 0x668A27 (and twice more, later).  "RulesClass_Addition"
// is Phobos's name for its hook at 0x668BF0; the pinned YRpp names the engine
// function there RulesClass::Read_File - same address, different names, so do
// not read the Phobos name as a typo for RulesClass::Init.
//
// The handler gives the registered rules-INI observers a last look at the parsed
// INI, then removes the platform-owned (section, key) pairs through the engine's
// native key removal. See docs/Ini.md for the design, the verification evidence
// and the limitations of this route.
// ---------------------------------------------------------------------------

namespace
{
char LowerAscii(const char value)
{
	if (value >= 'A' && value <= 'Z')
		return static_cast<char>(value - 'A' + 'a');
	return value;
}

bool EqualsIgnoreCase(const char* const left, const char* const right)
{
	if (!left || !right)
		return false;

	const char* a = left;
	const char* b = right;
	while (*a && *b)
	{
		if (LowerAscii(*a) != LowerAscii(*b))
			return false;

		++a;
		++b;
	}

	return *a == *b;
}

// Adapter between the pure takeover core (Core/IniTakeover.h) and the game's
// parsed rules INI. It implements both interfaces: the read view handed to the
// rules-INI observers and the removal back end used by the purge.
class EngineRulesIni final
	: public venture::IniTakeover::IReader
	, public venture::IniTakeover::IPurgeTarget
{
public:
	explicit EngineRulesIni(CCINIClass* const ini) : m_ini(ini) {}

	EngineRulesIni(const EngineRulesIni&) = delete;
	EngineRulesIni& operator=(const EngineRulesIni&) = delete;
	~EngineRulesIni() override = default;

	// --- IniTakeover::IReader ------------------------------------------------
	bool ReadBool(const char* const section, const char* const key, const bool defaultValue) override
	{
		return venture::EngineIni::ReadBool(m_ini, section, key, defaultValue);
	}

	int ReadInt(const char* const section, const char* const key, const int defaultValue) override
	{
		return venture::EngineIni::ReadInt(m_ini, section, key, defaultValue);
	}

	double ReadDouble(const char* const section, const char* const key, const double defaultValue) override
	{
		return venture::EngineIni::ReadDouble(m_ini, section, key, defaultValue);
	}

	void ReadString(
		const char* const section,
		const char* const key,
		const char* const defaultValue,
		char* const buffer,
		const std::size_t bufferSize) override
	{
		venture::EngineIni::ReadString(m_ini, section, key, defaultValue, buffer, bufferSize);
	}

	// --- IniTakeover::IPurgeTarget -------------------------------------------
	bool RemoveKey(const char* const section, const char* const key) override
	{
		if (!m_ini || !section || !key)
			return false;

		bool removed = false;

		for (;;)
		{
			INIClass::INISection* const pSection = FindSection(section);
			if (!pSection)
				return removed;

			INIClass::INIEntry* const pEntry = FindEntry(pSection, key);
			if (!pEntry)
				return removed;

			// The engine's own single-entry removal (YRpp INIClass::Clear(section,
			// key), gamemd 0x5257C0): it drops the entry from the section's
			// EntryIndex, unlinks it from the entry list and deletes it with the
			// game's deleting destructor, exactly like the engine's own resets do.
			// Passing the stored section/key text avoids any engine-side hashing or
			// case assumptions; the case-insensitive match happened just above.
			m_ini->Clear(pSection->Name, pEntry->Key);
			removed = true;

			// Defensive: if the engine ever leaves the entry behind, stop instead of
			// spinning. A nil result here means the key is gone.
			if (FindEntry(pSection, key) == pEntry)
				return removed;
		}
	}

private:
	INIClass::INISection* FindSection(const char* const name) const
	{
		if (!m_ini || !name)
			return nullptr;

		// Walk the section index instead of calling GetSection(): the lookup has to
		// be case-insensitive regardless of what the INI parser stored, and the
		// index already gives the exact stored spelling that the removal needs.
		for (const auto& node : m_ini->SectionIndex)
		{
			if (node.Data && node.Data->Name && EqualsIgnoreCase(node.Data->Name, name))
				return node.Data;
		}

		return nullptr;
	}

	static INIClass::INIEntry* FindEntry(INIClass::INISection* const pSection, const char* const key)
	{
		if (!pSection || !key)
			return nullptr;

		for (const auto& node : pSection->EntryIndex)
		{
			if (node.Data && node.Data->Key && EqualsIgnoreCase(node.Data->Key, key))
				return node.Data;
		}

		return nullptr;
	}

	CCINIClass* const m_ini;
};

bool g_takeoverSwitchResolved = false;

// Resolves [Platform] IniTakeover once, when the rules INI is first loaded.
// Platform::Startup() has already loaded <Platform>.ini by then (it runs from the
// earlier ExeRun hook); if it has not, the INI getter falls back to the default.
void ResolveTakeoverSwitch()
{
	if (g_takeoverSwitchResolved)
		return;

	g_takeoverSwitchResolved = true;

	const bool enabled = venture::Ini::GetBool(VC_SECTION_PLATFORM, "IniTakeover", true);
	venture::IniTakeover::SetEnabled(enabled);
	venture::Log::Info(
		"[IniTakeover] [%s] IniTakeover=%s (%zu owned key(s) registered)",
		VC_SECTION_PLATFORM,
		enabled ? "yes" : "no",
		venture::IniTakeover::OwnedKeyCount());
}
}

// RulesClass::Init(CCINIClass*): thiscall, one 4-byte stack argument, the already
// parsed rules(md).ini (both call sites pass CCINIClass::INI_Rules). The hook
// window is the function's first instruction, so the stack still holds the
// return address at ESP and the INI pointer at ESP+4.
DEFINE_HOOK(0x6686C0, Venture_RulesClass_Init, 0x6)
{
	// C-06: a refused platform (lineage conflict) keeps its hooks installed but
	// must not act on the engine.
	if (!venture::IsStarted())
		return 0;

	GET_STACK(CCINIClass*, pINI, 0x4);
	if (!pINI)
		return 0;

	ResolveTakeoverSwitch();

	EngineRulesIni rulesIni(pINI);
	const venture::IniTakeover::ApplyResult result =
		venture::IniTakeover::Apply(&rulesIni, rulesIni);

	if (result.purged > 0)
	{
		venture::Log::Debug(
			"[IniTakeover] rules INI purge: %zu owned key(s) removed (%zu registered, %zu observer(s))",
			result.purged,
			result.registered,
			result.observed);
	}

	return 0;
}
