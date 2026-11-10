#include "Core/Lineage.h"

#include "Core/PlatformVersion.h"

#include <windows.h>
#include <tlhelp32.h>

#include <cstring>
#include <utility>

// ---------------------------------------------------------------------------
// Lineage marker and module scan (see docs/Lineage.md).
//
// Two DLLs of the same lineage in one process would each build their own
// extension table and their own savegame trailer, so every copy exports a small
// read-only marker and the running DLL probes the loaded modules for it.  The
// decision itself is pure and lives in Lineage.h; this file provides the marker
// and the Windows-side enumeration.
// ---------------------------------------------------------------------------

// The marker layout is shared between DLLs: a 32-bit process must see 20 bytes.
static_assert(sizeof(VentureLineageInfo) == 20, "VentureLineageInfo layout changed - bump VENTURE_LINEAGE_ABI_VERSION");

namespace
{
// Static storage inside this image: the pointer returned by the export stays
// valid for as long as the module is loaded.
const VentureLineageInfo g_lineageInfo{
	VENTURE_LINEAGE_MAGIC,
	VENTURE_LINEAGE_ABI_VERSION,
	VC_PLATFORM_NAME,
	VC_VERSION_DISPLAY,
	VC_SAVEGAME_ID,
};

// True when the module path is inside the Windows directory.  That covers every
// module the process has loaded for itself (kernel32, user32, the CRT, ...)
// without enumerating a name list, so the rest of the scan is a cheap
// GetProcAddress filter.  A game-directory DLL never matches.
bool IsUnderWindowsDirectory(const char* const path)
{
	if (!path || path[0] == '\0')
		return false;

	char windowsDirectory[MAX_PATH] = {};
	const UINT length = GetWindowsDirectoryA(windowsDirectory, static_cast<UINT>(sizeof(windowsDirectory)));
	if (length == 0 || length >= MAX_PATH)
		return false;

	return _strnicmp(path, windowsDirectory, length) == 0;
}
}

namespace venture
{
std::vector<ForeignLineageModule> ScanForeignLineage()
{
	std::vector<ForeignLineageModule> foreign;

	// Toolhelp from kernel32: no psapi import library is needed.  Under a host
	// where the snapshot is unavailable the scan degrades to "no conflict" - the
	// platform must start even if this diagnostic is not available.
	const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
	if (snapshot == INVALID_HANDLE_VALUE)
		return foreign;

	MODULEENTRY32 entry{};
	entry.dwSize = sizeof(entry);
	if (!Module32First(snapshot, &entry))
	{
		CloseHandle(snapshot);
		return foreign;
	}

	do
	{
		if (!entry.hModule || IsUnderWindowsDirectory(entry.szExePath))
			continue;

		const auto getInfo = reinterpret_cast<VentureLineageGetInfoFn>(
			GetProcAddress(entry.hModule, VENTURE_LINEAGE_EXPORT_GET_INFO));
		if (!getInfo)
			continue;

		// This DLL exports the probe as well: the module whose export is our own
		// function is the running copy, so skip it.  A copy loaded under another
		// file name is a different module and is exactly the conflict to report.
		if (getInfo == &VentureLineage_GetInfo)
			continue;

		const VentureLineageInfo* const info = getInfo();
		if (!info || info->magic != VENTURE_LINEAGE_MAGIC)
			continue;

		ForeignLineageModule module;
		module.moduleName = entry.szModule;
		module.modulePath = entry.szExePath;
		module.magic = info->magic;
		module.abiVersion = info->abiVersion;
		module.platformName = info->name ? info->name : "";
		module.version = info->version ? info->version : "";
		module.savegameId = info->savegameId;
		foreign.push_back(std::move(module));
	} while (Module32Next(snapshot, &entry));

	CloseHandle(snapshot);
	return foreign;
}

unsigned CountForeignLineage()
{
	return static_cast<unsigned>(ScanForeignLineage().size());
}
}

// --- C ABI exports ---------------------------------------------------------
// __cdecl is explicit on purpose: the project builds with /Gz (stdcall default),
// and a decorated name would never be found by GetProcAddress.
extern "C" __declspec(dllexport) const VentureLineageInfo* __cdecl VentureLineage_GetInfo()
{
	return &g_lineageInfo;
}

extern "C" __declspec(dllexport) unsigned __cdecl VentureLineage_CountConflicts()
{
	return venture::CountForeignLineage();
}
