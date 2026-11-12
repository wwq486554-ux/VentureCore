// Unit test: host registry (registration, bit allocation, detection, formatting).
//
// TEST_DEPS: src/Core/HostRegistry.cpp
//
// The registry is engine-free apart from the Win32 module probe inside Detect(),
// which this test replaces with an injected fake, so registration rules, the
// bit/name conflict checks, the custom-slot limit, detection, deterministic
// ordering and the formatting helpers are all covered without a game process.

#include <cstdio>
#include <cstring>

#include "Core/HostRegistry.h"

namespace
{
int g_failures = 0;

void Check(const bool ok, const char* const what)
{
	if (ok)
	{
		std::printf("ok    %s\n", what);
	}
	else
	{
		++g_failures;
		std::printf("FAIL  %s\n", what);
	}
}

bool Contains(const char* const haystack, const char* const needle)
{
	return haystack && needle && std::strstr(haystack, needle) != nullptr;
}

venture::HostDefinition MakeHost(
	const unsigned bit,
	const char* const moduleName,
	const char* const displayName,
	const venture::HostKind kind)
{
	venture::HostDefinition definition;
	definition.bit = bit;
	definition.moduleName = moduleName;
	definition.displayName = displayName;
	definition.kind = kind;
	return definition;
}

unsigned Bit(const venture::Host host)
{
	return venture::HostBit(host);
}

// Injected module probes (a test has no real host DLL loaded).
bool LoadAres(const char* const moduleName)
{
	return std::strcmp(moduleName, "Ares.dll") == 0;
}

bool LoadAresAndMyPlatform(const char* const moduleName)
{
	return std::strcmp(moduleName, "Ares.dll") == 0
		|| std::strcmp(moduleName, "MyPlatform.dll") == 0;
}

bool LoadPhobos(const char* const moduleName)
{
	return std::strcmp(moduleName, "Phobos.dll") == 0;
}

bool LoadEverything(const char* const moduleName)
{
	(void)moduleName;
	return true;
}

// Static-lifetime names for the generated custom hosts of the capacity tests.
char g_customModules[29][24];
char g_customNames[29][24];

void FillCustomNames()
{
	for (int index = 0; index < 29; ++index)
	{
		_snprintf_s(g_customModules[index], sizeof(g_customModules[index]), _TRUNCATE,
			"CustomHost%02d.dll", index);
		_snprintf_s(g_customNames[index], sizeof(g_customNames[index]), _TRUNCATE,
			"CustomHost%02d", index);
	}
}

void TestBuiltinRegistration()
{
	venture::HostRegistry registry;
	Check(registry.Register(MakeHost(Bit(venture::Host::Ares), "Ares.dll", "Ares", venture::HostKind::Builtin)),
		"built-in Ares registers");
	Check(registry.Register(MakeHost(Bit(venture::Host::Phobos), "Phobos.dll", "Phobos", venture::HostKind::Builtin)),
		"built-in Phobos registers");
	Check(registry.Register(MakeHost(Bit(venture::Host::Kratos), "Kratos.dll", "Kratos", venture::HostKind::Builtin)),
		"built-in Kratos registers");
	Check(registry.DefinitionCount() == 3, "three definitions registered");
	Check(registry.KnownMask() == (Bit(venture::Host::Ares) | Bit(venture::Host::Phobos) | Bit(venture::Host::Kratos)),
		"known mask is the three built-in bits");
	Check(registry.ProblemCount() == 0, "no registration problems so far");

	// -- invalid bits -------------------------------------------------------
	Check(!registry.Register(MakeHost(0, "Zero.dll", "Zero", venture::HostKind::Builtin)),
		"bit 0 is rejected");
	Check(Contains(registry.ProblemAt(registry.ProblemCount() - 1), "not exactly one bit"),
		"bit 0 problem explains the single-bit rule");
	Check(!registry.Register(MakeHost((1u << 0) | (1u << 1), "Two.dll", "Two", venture::HostKind::Builtin)),
		"a two-bit value is rejected");
	Check(!registry.Register(MakeHost(1u << 4, "Builtin4.dll", "Builtin4", venture::HostKind::Builtin)),
		"a built-in registration outside bits 0-2 is rejected");
	Check(Contains(registry.ProblemAt(registry.ProblemCount() - 1), "1<<0..1<<2"),
		"the built-in bit rule is explained");

	// -- custom range and name rules ---------------------------------------
	Check(!registry.Register(MakeHost(1u << 2, "CustomLow.dll", "CustomLow", venture::HostKind::Custom)),
		"a custom host below bit 3 is rejected");
	Check(Contains(registry.ProblemAt(registry.ProblemCount() - 1), "must be >= 1<<3"),
		"the custom bit rule is explained");
	Check(!registry.Register(MakeHost(1u << 3, "", "EmptyModule", venture::HostKind::Custom)),
		"an empty module name is rejected");
	Check(!registry.Register(MakeHost(1u << 3, "HasModule.dll", "", venture::HostKind::Custom)),
		"an empty display name is rejected");
	Check(!registry.Register(MakeHost(1u << 3, "ARES.DLL", "SomeName", venture::HostKind::Custom)),
		"a duplicate module name is rejected case-insensitively");
	Check(!registry.Register(MakeHost(1u << 3, "Some.dll", "ares", venture::HostKind::Custom)),
		"a duplicate display name is rejected case-insensitively");

	// The rejected registrations must not have consumed the bit.
	Check(registry.FindByBit(1u << 3) == nullptr, "the rejected custom bit is still free");
	Check(registry.Register(MakeHost(1u << 3, "Custom.dll", "Custom", venture::HostKind::Custom)),
		"the first valid custom host registers");
	Check(!registry.Register(MakeHost(1u << 3, "Other.dll", "Other", venture::HostKind::Custom)),
		"a duplicate bit is rejected");
	Check(Contains(registry.ProblemAt(registry.ProblemCount() - 1), "already taken"),
		"the duplicate-bit problem names the owner");
}

void TestDetectionAndQueries()
{
	venture::HostRegistry registry;
	registry.Register(MakeHost(Bit(venture::Host::Ares), "Ares.dll", "Ares", venture::HostKind::Builtin));
	registry.Register(MakeHost(Bit(venture::Host::Phobos), "Phobos.dll", "Phobos", venture::HostKind::Builtin));
	registry.Register(MakeHost(Bit(venture::Host::Kratos), "Kratos.dll", "Kratos", venture::HostKind::Builtin));
	registry.Register(MakeHost(1u << 3, "MyPlatform.dll", "MyPlatform", venture::HostKind::Custom));

	const unsigned mask = registry.Detect(&LoadAresAndMyPlatform);
	Check(mask == (Bit(venture::Host::Ares) | (1u << 3)), "detection returns the present mask");
	Check(registry.Mask() == mask, "Mask() returns the same set");
	Check(registry.DetectedCount() == 2, "two hosts detected");

	// Deterministic ordering: ascending bit, built-in first, custom after.
	Check(registry.DetectedAt(0) != nullptr && std::strcmp(registry.DetectedAt(0)->definition->displayName, "Ares") == 0,
		"detected list starts with the lowest bit (Ares)");
	Check(registry.DetectedAt(1) != nullptr && registry.DetectedAt(1)->definition->bit == (1u << 3),
		"the custom host follows in bit order");
	Check(registry.DetectedAt(2) == nullptr, "DetectedAt rejects an index past the end");

	Check(registry.HasBit(Bit(venture::Host::Ares)), "HasBit sees a present host");
	Check(!registry.HasBit(Bit(venture::Host::Phobos)), "HasBit does not see an absent host");
	Check(!registry.HasBit(0), "HasBit(0) is false");

	Check(registry.HasName("ares"), "HasName matches case-insensitively");
	Check(registry.HasName("MYPLATFORM"), "HasName matches a custom host");
	Check(!registry.HasName(nullptr), "HasName(nullptr) is false");
	Check(!registry.HasName(""), "HasName of an empty string is false");
	Check(!registry.HasName("Kratos"), "HasName of an absent host is false");

	char buffer[256] = {};
	Check(std::strcmp(registry.FormatDetected(buffer, sizeof(buffer)), "Ares, MyPlatform (custom)") == 0,
		"FormatDetected annotates custom hosts and separates names");

	Check(std::strcmp(registry.FormatNames(0, buffer, sizeof(buffer)), "<none>") == 0,
		"FormatNames of an empty mask says <none>");
	Check(std::strcmp(registry.FormatNames(Bit(venture::Host::Kratos), buffer, sizeof(buffer)), "Kratos") == 0,
		"FormatNames resolves a single bit");
	Check(std::strcmp(registry.FormatNames(Bit(venture::Host::Phobos) | Bit(venture::Host::Kratos), buffer, sizeof(buffer)),
		"Phobos, Kratos") == 0,
		"FormatNames lists a mask in bit order");

	// Re-detection replaces the set.
	const unsigned phobosOnly = registry.Detect(&LoadPhobos);
	Check(phobosOnly == Bit(venture::Host::Phobos), "a second Detect replaces the detected set");
	Check(registry.DetectedCount() == 1, "the replaced set has one host again");

	// Registration after detection is a lifecycle error.
	Check(!registry.Register(MakeHost(1u << 4, "Late.dll", "Late", venture::HostKind::Custom)),
		"registration after detection is rejected");
	Check(Contains(registry.ProblemAt(registry.ProblemCount() - 1), "after host detection"),
		"the late-registration problem explains the lifecycle");

	Check(std::strcmp(registry.FormatDetected(buffer, sizeof(buffer)), "Phobos") == 0,
		"FormatDetected follows the replaced set");
}

void TestCapacityLimits()
{
	FillCustomNames();

	// The 32-bit table is split into 3 built-in bits and the 29-bit custom
	// range, so the custom range is exhausted by definition after 29 hosts.
	venture::HostRegistry registry;
	registry.Register(MakeHost(Bit(venture::Host::Ares), "Ares.dll", "Ares", venture::HostKind::Builtin));
	int registered = 0;
	for (int index = 0; index < 29; ++index)
	{
		if (registry.Register(MakeHost(1u << (index + 3), g_customModules[index], g_customNames[index], venture::HostKind::Custom)))
			++registered;
	}
	Check(registered == 29, "all 29 custom host bits register");
	Check(registry.DefinitionCount() == 30, "the registry holds the built-in host plus 29 custom hosts");
	Check((registry.KnownMask() & 0xFFFFFFF8u) == 0xFFFFFFF8u,
		"the known mask covers the whole custom range");
	Check(!registry.Register(MakeHost(1u << 3, "Extra.dll", "Extra", venture::HostKind::Custom)),
		"there is no 30th custom host slot (the custom bit range is full)");
	Check(Contains(registry.ProblemAt(registry.ProblemCount() - 1), "already taken"),
		"the rejected 30th custom host collides with an allocated bit");

	// Built-in bits stay available for their own definitions.
	Check(registry.Register(MakeHost(Bit(venture::Host::Phobos), "Phobos.dll", "Phobos", venture::HostKind::Builtin)),
		"a built-in host still registers while two built-in bits are free");
}

void TestFormattingEdgeCases()
{
	venture::HostRegistry registry;
	registry.Register(MakeHost(Bit(venture::Host::Ares), "Ares.dll", "Ares", venture::HostKind::Builtin));

	char buffer[256] = {};
	Check(std::strcmp(registry.FormatDetected(buffer, sizeof(buffer)), "<none>") == 0,
		"FormatDetected says <none> before detection runs");

	registry.Detect(&LoadEverything);
	Check(std::strcmp(registry.FormatNames(Bit(venture::Host::Ares), buffer, sizeof(buffer)), "Ares") == 0,
		"FormatNames resolves the only registered host");
	Check(std::strcmp(registry.FormatNames(1u << 3, buffer, sizeof(buffer)), "0x00000008") == 0,
		"FormatNames prints an undeclared bit as hexadecimal so a bad descriptor is visible");
	Check(std::strcmp(registry.FormatNames(Bit(venture::Host::Ares) | (1u << 3), buffer, sizeof(buffer)),
		"Ares, 0x00000008") == 0,
		"FormatNames mixes names and undeclared bits in bit order");
}
}

int __cdecl main()
{
	TestBuiltinRegistration();
	TestDetectionAndQueries();
	TestCapacityLimits();
	TestFormattingEdgeCases();

	std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAILED",
		g_failures, g_failures == 1 ? "" : "s");
	return g_failures == 0 ? 0 : 1;
}
