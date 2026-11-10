#include "Core/EngineIni.h"

#include "Engine/EngineCompat.h"

#include <cstdio>

namespace
{
// Fills the caller's buffer with the default text when there is no INI to read
// from, matching what a missing key in an existing INI would return.
void CopyDefault(const char* const defaultValue, char* const buffer, const std::size_t bufferSize)
{
	if (!buffer || bufferSize == 0)
		return;

	_snprintf_s(buffer, bufferSize, _TRUNCATE, "%s", defaultValue ? defaultValue : "");
}
}

namespace venture
{
bool EngineIni::ReadBool(
	CCINIClass* const ini,
	const char* const section,
	const char* const key,
	const bool defaultValue)
{
	if (!ini || !section || !key)
		return defaultValue;

	return ini->ReadBool(section, key, defaultValue);
}

int EngineIni::ReadInt(
	CCINIClass* const ini,
	const char* const section,
	const char* const key,
	const int defaultValue)
{
	if (!ini || !section || !key)
		return defaultValue;

	return ini->ReadInteger(section, key, defaultValue);
}

double EngineIni::ReadDouble(
	CCINIClass* const ini,
	const char* const section,
	const char* const key,
	const double defaultValue)
{
	if (!ini || !section || !key)
		return defaultValue;

	return ini->ReadDouble(section, key, defaultValue);
}

void EngineIni::ReadString(
	CCINIClass* const ini,
	const char* const section,
	const char* const key,
	const char* const defaultValue,
	char* const buffer,
	const std::size_t bufferSize)
{
	if (!buffer || bufferSize == 0)
		return;

	if (!ini || !section || !key)
	{
		CopyDefault(defaultValue, buffer, bufferSize);
		return;
	}

	ini->ReadString(section, key, defaultValue ? defaultValue : "", buffer, bufferSize);
}
}
