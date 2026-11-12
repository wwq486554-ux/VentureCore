#pragma once

#include <cstdint>

// ---------------------------------------------------------------------------
// Public host bits of the VentureCore base (2.0 host model).
//
// A host is an optional platform DLL loaded next to the base.  The base
// detects a *set* of hosts and never resolves it to a support tier (the tier
// scalar was removed in 2.0; see docs/HostMatrix.md).
//
// The bits below are compile-time known and stable: use them in a feature
// descriptor (`requiredHosts` / `anyOfHosts`) and to mask `FeatureContext::
// hostMask`.  A derived platform's own private hosts use the custom range
// (bit 3 and up): either an explicit bit registered in
// src/Core/Hosts.Custom.cpp, or the run-time marker protocol in
// platform/HostMarker.h, where the base allocates the lowest free bit.  Neither
// is part of this header because the base does not know the names at compile
// time; a discovered host is queried by name - see FeatureContext::HasHostName.
//
// The values must match the registrations in src/Core/Hosts.Builtin.cpp.
// ---------------------------------------------------------------------------

namespace venture
{
enum class Host : std::uint32_t
{
	Ares = 1u << 0, // Ares.dll - the usual ecosystem base
	Phobos = 1u << 1, // Phobos.dll - usually layered on top of Ares
	Kratos = 1u << 2, // Kratos.dll - usually layered on top of Ares
};

// Bit value of a built-in host.  Use it to build a mask:
//
//   requiredHosts = HostBit(Host::Ares);                                  // AND
//   anyOfHosts    = HostBit(Host::Phobos) | HostBit(Host::Kratos);        // OR
constexpr unsigned HostBit(const Host host)
{
	return static_cast<unsigned>(host);
}
}
