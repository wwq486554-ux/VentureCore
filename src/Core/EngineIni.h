#pragma once

#include <cstddef>

// ---------------------------------------------------------------------------
// Typed reads over an engine INI file.
//
// rules(md).ini and art(md).ini are parsed by the engine itself into YRpp
// CCINIClass objects. A feature must read that parsed view instead of re-opening
// the file: the engine's view is what every other subsystem uses, it already
// contains the engine's own merging/inheritance rules, and it is the only view
// the INI takeover layer (Core/IniTakeover.h) can hide keys from.
//
// This header is the thin typed front end for those reads:
//
//   const bool logUnits = EngineIni::ReadBool(CCINIClass::INI_Rules,
//                                             "Examples", "UnitCounterLog", false);
//
// Every getter returns the supplied default when the INI pointer, the section or
// the key is absent, so feature code never has to probe with Exists() first. The
// header deliberately keeps CCINIClass as an incomplete type; the implementation
// and every call site that needs the INI pointer (CCINIClass::INI_Rules /
// CCINIClass::INI_Art) include Engine/EngineCompat.h, the pinned engine binding
// gate.
// ---------------------------------------------------------------------------

class CCINIClass;

namespace venture
{
class EngineIni
{
public:
	static bool ReadBool(
		CCINIClass* ini,
		const char* section,
		const char* key,
		bool defaultValue);

	static int ReadInt(
		CCINIClass* ini,
		const char* section,
		const char* key,
		int defaultValue);

	static double ReadDouble(
		CCINIClass* ini,
		const char* section,
		const char* key,
		double defaultValue);

	// Writes at most bufferSize bytes (including the terminator) into buffer. A
	// null INI / section / key yields defaultValue, like a missing key would.
	static void ReadString(
		CCINIClass* ini,
		const char* section,
		const char* key,
		const char* defaultValue,
		char* buffer,
		std::size_t bufferSize);
};
}
