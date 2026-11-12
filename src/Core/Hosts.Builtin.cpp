#include "Core/HostRegistry.h"

// ---------------------------------------------------------------------------
// Built-in optional hosts of the base.
//
// The 2.0 host model registers three hosts (bits 0-2).  A feature declares what
// it needs with requiredHosts / anyOfHosts; the base logs the detected set as a
// set, not as a support tier (see docs/HostMatrix.md).
//
// Detection is by loaded module, not by file presence: a DLL sitting in the game
// directory is not necessarily injected, and SyringeEx only loads DLLs that
// carry a .syhks00 section.
//
// A derived platform that adds a private host does not edit this file - it uses
// VENTURE_REGISTER_HOST in Hosts.Custom.cpp with a custom bit (>= 1<<3), so a
// base sync does not overwrite it.
// ---------------------------------------------------------------------------

namespace
{
const venture::HostDefinition g_ventureHostAres{
	venture::HostBit(venture::Host::Ares), "Ares.dll", "Ares", venture::HostKind::Builtin };
const venture::HostRegistrar g_ventureHostRegistrarAres(g_ventureHostAres);

const venture::HostDefinition g_ventureHostPhobos{
	venture::HostBit(venture::Host::Phobos), "Phobos.dll", "Phobos", venture::HostKind::Builtin };
const venture::HostRegistrar g_ventureHostRegistrarPhobos(g_ventureHostPhobos);

const venture::HostDefinition g_ventureHostKratos{
	venture::HostBit(venture::Host::Kratos), "Kratos.dll", "Kratos", venture::HostKind::Builtin };
const venture::HostRegistrar g_ventureHostRegistrarKratos(g_ventureHostKratos);
}
