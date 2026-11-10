#pragma once

// ---------------------------------------------------------------------------
// VentureCore lineage marker (public, C ABI).
//
// Two DLLs of the same lineage would each build their own extension table and
// their own savegame trailer inside one process, corrupting the state they are
// supposed to share.  Every DLL built from this base therefore exports a small
// read-only marker, and the running DLL scans the loaded modules for it at
// startup (see src/Core/Lineage.h and docs/Lineage.md).
//
// The marker is resolved with GetProcAddress, so both exports keep C linkage and
// the __cdecl convention explicitly: the project is compiled with /Gz (stdcall
// is the default), and a calling convention inherited from that default would
// decorate the export name so the probe could never find it.
// ---------------------------------------------------------------------------

#define VENTURE_LINEAGE_MAGIC 0x564C4E31u  // 'V' 'L' 'N' '1'
#define VENTURE_LINEAGE_ABI_VERSION 1u

// Names of the exports the scanner resolves on every candidate module.
#define VENTURE_LINEAGE_EXPORT_GET_INFO "VentureLineage_GetInfo"
#define VENTURE_LINEAGE_EXPORT_COUNT_CONFLICTS "VentureLineage_CountConflicts"

// Marker returned by VentureLineage_GetInfo().  The layout is part of the C ABI
// between lineage DLLs: append new fields only with an ABI version bump.
struct VentureLineageInfo
{
	unsigned magic;      // VENTURE_LINEAGE_MAGIC
	unsigned abiVersion; // VENTURE_LINEAGE_ABI_VERSION
	const char* name;    // platform name (VC_PLATFORM_NAME), NUL-terminated
	const char* version; // platform version (VC_VERSION_DISPLAY), NUL-terminated
	unsigned savegameId; // VC_SAVEGAME_ID of the savegame trailer format
};

extern "C"
{
	// Returns this DLL's marker.  The returned pointer is static storage inside
	// the exporting module and stays valid while the module is loaded.
	__declspec(dllexport) const VentureLineageInfo* __cdecl VentureLineage_GetInfo();

	// Diagnostic export: number of *other* loaded modules that answer the lineage
	// probe with a matching marker (0 = this DLL is the only one).  Used by
	// scripts/linux/load_probe.sh to exercise the scan without starting the game.
	__declspec(dllexport) unsigned __cdecl VentureLineage_CountConflicts();
}

// Typed function-pointer aliases for GetProcAddress users (the load probe).
typedef const VentureLineageInfo* (__cdecl* VentureLineageGetInfoFn)();
typedef unsigned (__cdecl* VentureLineageCountConflictsFn)();
