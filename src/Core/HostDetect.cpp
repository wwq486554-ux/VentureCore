#include "Core/HostDetect.h"

#include <windows.h>

namespace
{
struct HostEntry
{
	const char* moduleName;
	const char* displayName;
	venture::Host host;
};

// Detection is by loaded module, not by file presence: a DLL sitting in the game
// directory is not necessarily injected (e.g. an old Syringe that only injects
// one platform). SyringeEx loads every DLL that carries a .syhks00 section, so
// for the supported hosts a loaded module means an active host.
const HostEntry g_hosts[] = {
	{ "Ares.dll", "Ares", venture::Host::Ares },
	{ "Phobos.dll", "Phobos", venture::Host::Phobos },
	{ "Kratos.dll", "Kratos", venture::Host::Kratos },
	{ "Kaleidoscope.dll", "Kaleidoscope", venture::Host::Kaleidoscope },
};
}

namespace venture
{
unsigned DetectHostMask()
{
	unsigned mask = static_cast<unsigned>(Host::None);
	for (const auto& entry : g_hosts)
	{
		if (GetModuleHandleA(entry.moduleName) != nullptr)
			mask |= static_cast<unsigned>(entry.host);
	}
	return mask;
}

const char* HostMaskToString(const unsigned hostMask, char* const buffer, const DWORD bufferSize)
{
	if (!buffer || bufferSize == 0)
		return "";

	buffer[0] = '\0';
	bool first = true;
	for (const auto& entry : g_hosts)
	{
		if ((hostMask & static_cast<unsigned>(entry.host)) == 0)
			continue;

		if (!first)
			strcat_s(buffer, bufferSize, ", ");
		strcat_s(buffer, bufferSize, entry.displayName);
		first = false;
	}

	if (first)
		strcat_s(buffer, bufferSize, "<none>");

	return buffer;
}

Tier ResolveTier(const unsigned hostMask)
{
	const unsigned kratosOrKaleidoscope =
		static_cast<unsigned>(Host::Kratos) | static_cast<unsigned>(Host::Kaleidoscope);

	if ((hostMask & kratosOrKaleidoscope) != 0)
		return Tier::T3;
	if ((hostMask & static_cast<unsigned>(Host::Phobos)) != 0)
		return Tier::T2;
	if ((hostMask & static_cast<unsigned>(Host::Ares)) != 0)
		return Tier::T1;
	return Tier::T0;
}
}
