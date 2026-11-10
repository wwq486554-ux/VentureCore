#pragma once

#include <windows.h>

namespace venture
{
// Thin wrapper around the game's <Platform>.ini (see PlatformConfig.h). The file
// lives next to rules.ini in the game directory. A missing file is not an error:
// every getter falls back to the supplied default, and feature switches default
// to "on" (see VC_DEFAULT_FEATURE_ENABLED).
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
};
}
