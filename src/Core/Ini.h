#pragma once

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
};
}
