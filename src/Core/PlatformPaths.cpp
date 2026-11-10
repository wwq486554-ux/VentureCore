#include "Core/PlatformPaths.h"

#include <cstring>

namespace
{
// Path of the running executable. SyringeEx injects the platform DLL into
// gamemd.exe before the game starts, so this is always the game directory.
bool GetExecutablePath(char* buffer, DWORD bufferSize)
{
	const DWORD length = GetModuleFileNameA(nullptr, buffer, bufferSize);
	if (length == 0 || length >= bufferSize)
		return false;
	return true;
}
}

namespace venture
{
bool GetGameDirectory(char* const buffer, const DWORD bufferSize)
{
	if (!buffer || bufferSize < 4)
		return false;

	if (!GetExecutablePath(buffer, bufferSize))
		return false;

	// Trim the executable name, keeping the trailing separator.
	char* lastSeparator = std::strrchr(buffer, '\\');
	if (!lastSeparator)
		lastSeparator = std::strrchr(buffer, '/');
	if (!lastSeparator)
		return false;

	lastSeparator[1] = '\0';
	return true;
}

bool GetGameFilePath(const char* const fileName, char* const buffer, const DWORD bufferSize)
{
	if (!fileName || !buffer || bufferSize == 0)
		return false;

	char directory[MAX_PATH * 2] = {};
	if (!GetGameDirectory(directory, static_cast<DWORD>(sizeof(directory))))
		return false;

	buffer[0] = '\0';
	const errno_t result = strcat_s(buffer, bufferSize, directory);
	if (result != 0)
		return false;

	const errno_t appendResult = strcat_s(buffer, bufferSize, fileName);
	return appendResult == 0;
}
}
