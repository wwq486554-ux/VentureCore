#pragma once

#include <windows.h>

#include <platform/Feature.h>

namespace venture
{
// Optional host platforms that can be installed next to VentureCore. They are
// never hard dependencies: the base only detects what is present, logs it, and
// resolves the support tier (see docs/HostMatrix.md).
enum class Host : unsigned
{
	None = 0,
	Ares = 1u << 0,
	Phobos = 1u << 1,
	Kratos = 1u << 2,
	Kaleidoscope = 1u << 3,
};

unsigned DetectHostMask();

// "Ares, Phobos" / "<none>"; returns the buffer for convenience.
const char* HostMaskToString(unsigned hostMask, char* buffer, DWORD bufferSize);

// T0 = SyringeEx only, T1 = +Ares, T2 = +Phobos, T3 = +Kratos/Kaleidoscope.
Tier ResolveTier(unsigned hostMask);
}
