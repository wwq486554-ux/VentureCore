#pragma once

// ---------------------------------------------------------------------------
// Venture host marker (public, C ABI) - the 2.0 second-phase way for an
// independent platform DLL to be detected by the base.
//
// First phase (source-level): a derived platform lists its private host in
// src/Core/Hosts.Custom.cpp with VENTURE_REGISTER_HOST and an explicit bit.
// Second phase (this marker): a platform that does NOT want to edit the base
// source exports one function and is discovered at run time:
//
//   #include <platform/HostMarker.h>   // the ABI below
//
//   static const VentureHostInfo g_info{
//       VENTURE_HOST_MARKER_MAGIC,
//       VENTURE_HOST_MARKER_ABI_VERSION,
//       "MyPlatform",   // name: static lifetime, NUL-terminated, 1..63 chars
//       "v1.2.0",       // version: may be null, otherwise NUL-terminated, <= 63
//       0u,             // capabilities: reserved (0 in 2.0)
//       0u,             // flags: reserved (0 in 2.0)
//   };
//
//   extern "C" __declspec(dllexport) const VentureHostInfo* __cdecl
//   VentureHost_GetInfo()
//   {
//       return &g_info;
//   }
//
// The base enumerates the process modules at startup, resolves
// VENTURE_HOST_MARKER_EXPORT_GET_INFO, validates the marker and adds the
// platform to the detected host set (see docs/HostMatrix.md).  The export and
// the typedef keep C linkage / __cdecl explicitly: the base builds with /Gz
// (stdcall default), and a decorated name would never be found by
// GetProcAddress.
//
// Contract:
//   * magic and abiVersion must match exactly;
//   * name is a static-lifetime, NUL-terminated string of 1..63 characters;
//     version may be null or a static-lifetime NUL-terminated string of at most
//     63 characters;
//   * the pointer stays valid while the exporting module is loaded; the base
//     copies the strings immediately, so unloading the module later is safe;
//   * capabilities/flags are reserved for a later phase; 2.0 ignores them, and
//     the name/version are recorded for logs only (no version gating);
//   * a DLL of the VentureCore lineage is NOT a host: it exports the lineage
//     marker and is refused by the lineage mutual-exclusion check, so a lineage
//     copy cannot masquerade as a host through this protocol.
//
// Layout: 6 x 4 bytes on the 32-bit game (asserted in src/Core/HostRegistry.cpp).
// ---------------------------------------------------------------------------

#define VENTURE_HOST_MARKER_MAGIC 0x54534856u // 'V' 'H' 'S' 'T' (little endian)
#define VENTURE_HOST_MARKER_ABI_VERSION 1u
#define VENTURE_HOST_MARKER_NAME_MAX 63u
#define VENTURE_HOST_MARKER_EXPORT_GET_INFO "VentureHost_GetInfo"

// Marker returned by VentureHost_GetInfo().  Append new fields only with an
// ABI version bump; the capabilities/flags words are the reserved growth space.
struct VentureHostInfo
{
	unsigned magic;        // VENTURE_HOST_MARKER_MAGIC
	unsigned abiVersion;   // VENTURE_HOST_MARKER_ABI_VERSION
	const char* name;      // platform name, NUL-terminated, 1..63 characters
	const char* version;   // "v1.2.0" or null; NUL-terminated, <= 63 characters
	unsigned capabilities; // reserved for a later phase (0)
	unsigned flags;        // reserved for a later phase (0)
};

// Typed function-pointer alias for GetProcAddress users.
typedef const VentureHostInfo* (__cdecl* VentureHostGetInfoFn)();
