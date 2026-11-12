#include "Core/HostRegistry.h"

// ---------------------------------------------------------------------------
// Custom (private) hosts of a derived platform.
//
// The base ships no custom host.  A platform built from this base registers its
// own optional hosts in this file, with an explicit bit in the custom range
// (>= 1u<<3; bits 0-2 belong to the built-in Ares / Phobos / Kratos):
//
//   VENTURE_REGISTER_HOST(MyPlatform, 1u << 3, "MyPlatform.dll", "MyPlatform");
//
// The bit is a process-wide, explicit resource: the first definition wins, and
// a duplicate bit or name is rejected at load time and reported once in
// <Platform>.log at the host line (see docs/HostMatrix.md).  Keep the strings
// static (literals) - the registry stores pointers to them for the DLL's
// lifetime.
//
// A feature module declares its dependency like for a built-in host:
//   requiredHosts = 1u << 3;   // custom hosts are compile-time known here
// Hosts discovered at run time (a later phase) are queried by name instead -
// see FeatureContext::HasHostName in include/platform/Feature.h.
// ---------------------------------------------------------------------------

// (none: the base has no private hosts)
