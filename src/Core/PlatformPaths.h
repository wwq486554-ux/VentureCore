#pragma once

#include <windows.h>

namespace venture
{
// Fills `buffer` with the full path of the game directory (the directory that
// contains the running gamemd.exe), including the trailing backslash.
// Returns false when the path cannot be determined (it always can in practice -
// this only guards against a truncated buffer).
bool GetGameDirectory(char* buffer, DWORD bufferSize);

// Fills `buffer` with `<game directory>\<fileName>`.
bool GetGameFilePath(const char* fileName, char* buffer, DWORD bufferSize);
}
