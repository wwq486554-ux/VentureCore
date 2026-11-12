#include "Core/Ini.h"

#include "Core/PlatformConfig.h"
#include "Core/PlatformPaths.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
char g_iniPath[MAX_PATH * 2] = {};

// INI-06: hard-limit events of the most recent EnumerateEntries() call (see
// Ini.h).  The Win32 profile API truncates when its buffers are full; counting
// the events lets the startup path report an incomplete scan.
std::size_t g_enumerationLimits = 0;

bool EqualsIgnoreCase(const char* const left, const char* const right)
{
	return _stricmp(left, right) == 0;
}
}

namespace venture
{
bool Ini::Load()
{
	g_iniPath[0] = '\0';
	return GetGameFilePath(VC_PLATFORM_INI_NAME, g_iniPath, static_cast<DWORD>(sizeof(g_iniPath)));
}

const char* Ini::Path()
{
	return g_iniPath;
}

bool Ini::Exists()
{
	if (g_iniPath[0] == '\0')
		return false;

	const DWORD attributes = GetFileAttributesA(g_iniPath);
	return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::size_t Ini::EnumerationLimitCount()
{
	return g_enumerationLimits;
}

bool Ini::GetBool(const char* const section, const char* const key, const bool defaultValue)
{
	if (g_iniPath[0] == '\0')
		return defaultValue;

	char value[32] = {};
	GetPrivateProfileStringA(
		section,
		key,
		defaultValue ? "yes" : "no",
		value,
		static_cast<DWORD>(sizeof(value)),
		g_iniPath);

	if (EqualsIgnoreCase(value, "yes") || EqualsIgnoreCase(value, "true") || EqualsIgnoreCase(value, "on")
		|| std::strcmp(value, "1") == 0)
	{
		return true;
	}
	if (EqualsIgnoreCase(value, "no") || EqualsIgnoreCase(value, "false") || EqualsIgnoreCase(value, "off")
		|| std::strcmp(value, "0") == 0)
	{
		return false;
	}

	return defaultValue;
}

int Ini::GetInt(const char* const section, const char* const key, const int defaultValue)
{
	if (g_iniPath[0] == '\0')
		return defaultValue;

	char defaultText[32] = {};
	_snprintf_s(defaultText, sizeof(defaultText), _TRUNCATE, "%d", defaultValue);

	char value[64] = {};
	GetPrivateProfileStringA(
		section,
		key,
		defaultText,
		value,
		static_cast<DWORD>(sizeof(value)),
		g_iniPath);

	char* end = nullptr;
	const long parsed = std::strtol(value, &end, 10);
	if (end == value)
		return defaultValue;

	return static_cast<int>(parsed);
}

void Ini::GetString(
	const char* const section,
	const char* const key,
	const char* const defaultValue,
	char* const buffer,
	const DWORD bufferSize)
{
	if (!buffer || bufferSize == 0)
		return;

	if (g_iniPath[0] == '\0')
	{
		_snprintf_s(buffer, bufferSize, _TRUNCATE, "%s", defaultValue ? defaultValue : "");
		return;
	}

	GetPrivateProfileStringA(section, key, defaultValue ? defaultValue : "", buffer, bufferSize, g_iniPath);
}

void Ini::EnumerateEntries(
	void (*visitor)(const char* section, const char* key, void* userData),
	void* const userData)
{
	g_enumerationLimits = 0;
	if (!visitor || g_iniPath[0] == '\0' || !Exists())
		return;

	char sectionNames[16384] = {};
	const DWORD sectionBytes = GetPrivateProfileSectionNamesA(
		sectionNames, static_cast<DWORD>(sizeof(sectionNames)), g_iniPath);
	if (sectionBytes == 0)
		return;

	// GetPrivateProfileSectionNamesA returns nSize-1 when the double-NUL list
	// does not fit; a full buffer means section names were cut (INI-06).
	if (sectionBytes >= sizeof(sectionNames) - 2)
		++g_enumerationLimits;

	for (const char* section = sectionNames; *section != '\0'; section += std::strlen(section) + 1)
	{
		char entries[32768] = {};
		const DWORD entryBytes = GetPrivateProfileSectionA(
			section, entries, static_cast<DWORD>(sizeof(entries)), g_iniPath);
		if (entryBytes == 0)
			continue;

		// A full entry buffer means keys past the 32 KiB cap were cut (INI-06).
		if (entryBytes >= sizeof(entries) - 2)
			++g_enumerationLimits;

		for (const char* entry = entries; *entry != '\0'; entry += std::strlen(entry) + 1)
		{
			const char* const equals = std::strchr(entry, '=');
			const char* const keyEnd = equals ? equals : entry + std::strlen(entry);

			// Trim the key; the registry compares trimmed keys case-insensitively.
			const char* keyBegin = entry;
			while (keyBegin < keyEnd && (*keyBegin == ' ' || *keyBegin == '\t'))
				++keyBegin;
			const char* trimmedEnd = keyEnd;
			while (trimmedEnd > keyBegin && (trimmedEnd[-1] == ' ' || trimmedEnd[-1] == '\t'))
				--trimmedEnd;

			char key[256] = {};
			const std::size_t keyLength = static_cast<std::size_t>(trimmedEnd - keyBegin);
			if (keyLength == 0)
				continue;
			if (keyLength >= sizeof(key))
			{
				// Longer than the registry's per-key buffer: skip and count (INI-06).
				++g_enumerationLimits;
				continue;
			}
			std::memcpy(key, keyBegin, keyLength);
			key[keyLength] = '\0';
			visitor(section, key, userData);
		}
	}
}
}
