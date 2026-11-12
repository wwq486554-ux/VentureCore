#pragma once

#include <cstddef>

#include <windows.h>

namespace venture
{
// Thin wrapper around the game's <Platform>.ini (see PlatformConfig.h). The file
// lives next to rules.ini in the game directory. A missing file is not an error:
// every getter falls back to the supplied default.  The typed reads the platform
// and the features use live in Core/IniRegistry.h (2.0); this wrapper is the raw
// file access (getters + entry enumeration for the unknown-key scan).
class Ini
{
public:
	static bool Load();
	static const char* Path();
	static bool Exists();

	static bool GetBool(const char* section, const char* key, bool defaultValue);
	static int GetInt(const char* section, const char* key, int defaultValue);
	static void GetString(
		const char* section,
		const char* key,
		const char* defaultValue,
		char* buffer,
		DWORD bufferSize);

	// Enumerates every (section, key) pair of the file, in file order.  Keys are
	// trimmed of surrounding whitespace.  Used by the INI key registry for the
	// startup unknown-key scan; a missing file or path is a silent no-op.
	static void EnumerateEntries(
		void (*visitor)(const char* section, const char* key, void* userData),
		void* userData);

	// INI-06 diagnostics: hard-limit events of the most recent EnumerateEntries()
	// call - a truncated section-name list (16 KiB buffer), a truncated section
	// body (32 KiB buffer) or a key name too long for the per-key buffer (256
	// bytes).  These are Win32 profile API limits; the startup path reports a
	// nonzero value at Debug level after the unknown-key scan.
	static std::size_t EnumerationLimitCount();
};
}
