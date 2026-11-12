// Unit test: host registry (registration, bit allocation, detection, formatting).
//
// TEST_DEPS: src/Core/HostRegistry.cpp
//
// The registry is engine-free apart from the Win32 module probe inside Detect(),
// which this test replaces with an injected fake, so registration rules, the
// bit/name conflict checks, the custom-slot limit, detection, deterministic
// ordering, the marker reader (including its SEH-protected untrusted-pointer
// path), the truncation-safe formatters and the problem drain are all covered
// without a game process.

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "Core/HostRegistry.h"

#include <platform/HostMarker.h>

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

bool LoadStaticAndSelf(const char* const moduleName)
{
	return std::strcmp(moduleName, "StaticHost.dll") == 0
		|| std::strcmp(moduleName, "SelfHost.dll") == 0;
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

void TestMarkerValidation()
{
	VentureHostInfo valid{};
	valid.magic = VENTURE_HOST_MARKER_MAGIC;
	valid.abiVersion = VENTURE_HOST_MARKER_ABI_VERSION;
	valid.name = "MyPlatform";
	valid.version = "v1.2.0";
	Check(venture::IsValidHostMarker(&valid), "a well-formed host marker is accepted");

	VentureHostInfo badMagic = valid;
	badMagic.magic = 0;
	Check(!venture::IsValidHostMarker(&badMagic), "a wrong marker magic is rejected");

	VentureHostInfo badAbi = valid;
	badAbi.abiVersion = VENTURE_HOST_MARKER_ABI_VERSION + 1u;
	Check(!venture::IsValidHostMarker(&badAbi), "a different marker ABI version is rejected");

	Check(!venture::IsValidHostMarker(nullptr), "a null marker is rejected");

	VentureHostInfo nullName = valid;
	nullName.name = nullptr;
	Check(!venture::IsValidHostMarker(&nullName), "a null name is rejected");

	VentureHostInfo emptyName = valid;
	emptyName.name = "";
	Check(!venture::IsValidHostMarker(&emptyName), "an empty name is rejected");

	char maxName[64] = {};
	std::memset(maxName, 'n', sizeof(maxName) - 1);
	VentureHostInfo maxInfo = valid;
	maxInfo.name = maxName;
	Check(venture::IsValidHostMarker(&maxInfo), "a 63-character name is accepted");

	char longName[128] = {};
	std::memset(longName, 'n', sizeof(longName) - 1);
	VentureHostInfo tooLong = valid;
	tooLong.name = longName;
	Check(!venture::IsValidHostMarker(&tooLong), "a name longer than 63 characters is rejected");

	VentureHostInfo noVersion = valid;
	noVersion.version = nullptr;
	Check(venture::IsValidHostMarker(&noVersion), "a null version is accepted");

	char longVersion[128] = {};
	std::memset(longVersion, 'v', sizeof(longVersion) - 1);
	VentureHostInfo badVersion = valid;
	badVersion.version = longVersion;
	Check(!venture::IsValidHostMarker(&badVersion), "a version longer than 63 characters is rejected");
}

void TestDynamicRegistration()
{
	venture::HostRegistry registry;
	registry.Register(MakeHost(Bit(venture::Host::Ares), "Ares.dll", "Ares", venture::HostKind::Builtin));
	registry.Register(MakeHost(Bit(venture::Host::Phobos), "Phobos.dll", "Phobos", venture::HostKind::Builtin));
	registry.Register(MakeHost(Bit(venture::Host::Kratos), "Kratos.dll", "Kratos", venture::HostKind::Builtin));
	Check(registry.Register(MakeHost(1u << 3, "StaticHost.dll", "StaticHost", venture::HostKind::Custom)),
		"the source-level custom host registers");

	// A discovered host is registered before detection runs; the public entry
	// point is closed after Detect() (HOST-04), so the marker scan uses the
	// internal path while detection is in progress and the test drives the public
	// path in its documented pre-detection window.
	const unsigned dynamicBit = registry.RegisterDynamicHost(
		"SelfHost.dll", "SelfHost", "v1.2.3", "Z:\\game\\SelfHost.dll");
	Check(dynamicBit == (1u << 4), "a discovered host takes the lowest free custom bit");
	Check(registry.Mask() == (1u << 4), "the discovered host is part of the detected set");
	Check(registry.HasBit(dynamicBit), "HasBit sees the discovered host");
	Check(registry.HasName("SELFHOST"), "HasHostName matches the discovered host case-insensitively");

	char buffer[256] = {};
	Check(std::strcmp(registry.FormatDetected(buffer, sizeof(buffer)), "SelfHost (custom)") == 0,
		"the discovered host is formatted with the custom suffix");

	const venture::DetectedHost* const discovered = registry.DetectedAt(0);
	Check(discovered && discovered->definition->bit == dynamicBit,
		"the discovered host is the first detected entry");
	Check(discovered && discovered->definition->version
		&& std::strcmp(discovered->definition->version, "v1.2.3") == 0,
		"the marker version is recorded");
	Check(discovered && std::strstr(discovered->modulePath, "SelfHost.dll") != nullptr,
		"the discovered module path is recorded");

	const int problemsBefore = registry.ProblemCount();
	Check(registry.RegisterDynamicHost("OtherHost.dll", "selfhost", "v1.0", "Z:\\game\\OtherHost.dll") == 0,
		"a duplicate display name is rejected");
	Check(registry.ProblemCount() == problemsBefore + 1
		&& Contains(registry.ProblemAt(registry.ProblemCount() - 1), "already registered"),
		"the duplicate-name problem is recorded");

	Check(registry.RegisterDynamicHost("SelfHost.dll", "SelfHost", "v1.2.3", "Z:\\game\\SelfHost.dll") == dynamicBit,
		"the same module is not registered twice");
	Check(registry.DefinitionCount() == 5, "the rejected duplicates did not add a definition");

	// A later Detect() re-probes the dynamic definition by module name and keeps
	// the discovered set.
	const unsigned mask = registry.Detect(&LoadStaticAndSelf);
	Check(mask == ((1u << 3) | (1u << 4)), "Detect re-probes the discovered host by module name");
	Check(registry.DetectedCount() == 2, "the re-probe finds the source-level and the dynamic host");
	Check(std::strcmp(registry.FormatDetected(buffer, sizeof(buffer)),
		"StaticHost (custom), SelfHost (custom)") == 0,
		"the detected set lists both custom hosts in bit order");

	// HOST-04: detection closes registration.  This used to be accepted, which
	// could extend the detected set after the feature gate had snapshotted it.
	const unsigned secondBit = registry.RegisterDynamicHost(
		"AfterDetect.dll", "AfterDetect", "v2.0", "Z:\\game\\AfterDetect.dll");
	Check(secondBit == 0, "HOST-04: a discovered host registered after detection is rejected");
	Check(registry.Mask() == mask, "the rejected registration did not change the detected set");
	Check(registry.DefinitionCount() == 5, "the rejected registration did not add a definition");
	Check(Contains(registry.ProblemAt(registry.ProblemCount() - 1), "after host detection"),
		"the frozen-set rejection is recorded as a problem");

	// The 29-slot limit is shared: fill a fresh registry with dynamically
	// discovered hosts and verify the 30th is rejected (all before detection).
	FillCustomNames();
	venture::HostRegistry full;
	full.Register(MakeHost(Bit(venture::Host::Ares), "Ares.dll", "Ares", venture::HostKind::Builtin));
	full.Register(MakeHost(Bit(venture::Host::Phobos), "Phobos.dll", "Phobos", venture::HostKind::Builtin));
	full.Register(MakeHost(Bit(venture::Host::Kratos), "Kratos.dll", "Kratos", venture::HostKind::Builtin));
	int registered = 0;
	for (int index = 0; index < 29; ++index)
	{
		if (full.RegisterDynamicHost(g_customModules[index], g_customNames[index], "", "") != 0)
			++registered;
	}
	Check(registered == 29, "all 29 custom host slots can be filled dynamically");
	Check(full.RegisterDynamicHost("ExtraHost.dll", "ExtraHost", "", "") == 0,
		"the 30th dynamically discovered host is rejected");
	Check(Contains(full.ProblemAt(full.ProblemCount() - 1), "all 29 custom host slots"),
		"the exhausted-slot problem is recorded");
}

// -- HOST-01: untrusted marker pointers are read inside the SEH region -------
//
// Synthetic getters with the same shape a hostile/stale marker DLL can produce.
// MarkerGetterBadName is the HOST-01 regression: a well-formed marker struct
// whose `name` is a wild pointer.  Before the reader copied everything under
// __try, the first character read faulted outside any SEH region and took the
// process down; the test binary surviving is the red/green criterion.
const VentureHostInfo* __cdecl MarkerGetterGood()
{
	static const VentureHostInfo info{
		VENTURE_HOST_MARKER_MAGIC,
		VENTURE_HOST_MARKER_ABI_VERSION,
		"MyPlatform",
		"v1.2.0",
		0u,
		0u,
	};
	return &info;
}

const VentureHostInfo* __cdecl MarkerGetterBadName()
{
	static VentureHostInfo info{};
	info.magic = VENTURE_HOST_MARKER_MAGIC;
	info.abiVersion = VENTURE_HOST_MARKER_ABI_VERSION;
	info.name = reinterpret_cast<const char*>(static_cast<std::uintptr_t>(1));
	info.version = nullptr;
	return &info;
}

const VentureHostInfo* __cdecl MarkerGetterBadStruct()
{
	return reinterpret_cast<const VentureHostInfo*>(static_cast<std::uintptr_t>(1));
}

char g_overlongMarkerText[128];

const VentureHostInfo* __cdecl MarkerGetterOverlongName()
{
	static VentureHostInfo info{};
	info.magic = VENTURE_HOST_MARKER_MAGIC;
	info.abiVersion = VENTURE_HOST_MARKER_ABI_VERSION;
	info.name = g_overlongMarkerText; // no NUL within the 64-byte copy window
	info.version = nullptr;
	return &info;
}

const VentureHostInfo* __cdecl MarkerGetterOverlongVersion()
{
	static VentureHostInfo info{};
	info.magic = VENTURE_HOST_MARKER_MAGIC;
	info.abiVersion = VENTURE_HOST_MARKER_ABI_VERSION;
	info.name = "MyPlatform";
	info.version = g_overlongMarkerText; // no NUL within the 64-byte copy window
	return &info;
}

void TestHostMarkerReader()
{
	std::memset(g_overlongMarkerText, 'x', sizeof(g_overlongMarkerText) - 1u);
	g_overlongMarkerText[sizeof(g_overlongMarkerText) - 1u] = '\0';

	char name[VENTURE_HOST_MARKER_NAME_MAX + 1u] = {};
	char version[VENTURE_HOST_MARKER_NAME_MAX + 1u] = {};
	VentureHostInfo copy{};

	Check(venture::ReadHostMarker(&MarkerGetterGood, &copy, name, sizeof(name), version, sizeof(version))
		== venture::HostMarkerRead::Valid,
		"HOST-01: a well-formed marker reads as valid");
	Check(copy.name == name && std::strcmp(name, "MyPlatform") == 0,
		"the marker name is copied into the caller's buffer");
	Check(copy.version == version && std::strcmp(version, "v1.2.0") == 0,
		"the marker version is copied into the caller's buffer");

	Check(venture::ReadHostMarker(&MarkerGetterBadName, &copy, name, sizeof(name), version, sizeof(version))
		== venture::HostMarkerRead::Raised,
		"HOST-01: a wild name pointer is caught by the guarded read, not fatal");
	Check(venture::ReadHostMarker(&MarkerGetterBadStruct, &copy, name, sizeof(name), version, sizeof(version))
		== venture::HostMarkerRead::Raised,
		"HOST-01: a wild marker pointer is caught by the guarded read too");
	Check(venture::ReadHostMarker(&MarkerGetterOverlongName, &copy, name, sizeof(name), version, sizeof(version))
		== venture::HostMarkerRead::Invalid,
		"an over-long name is rejected instead of walking foreign memory");
	Check(venture::ReadHostMarker(&MarkerGetterOverlongVersion, &copy, name, sizeof(name), version, sizeof(version))
		== venture::HostMarkerRead::Invalid,
		"an over-long version is rejected");
	Check(venture::ReadHostMarker(nullptr, &copy, name, sizeof(name), version, sizeof(version))
		== venture::HostMarkerRead::Invalid,
		"a null export is rejected");
	Check(venture::ReadHostMarker(&MarkerGetterGood, nullptr, name, sizeof(name), version, sizeof(version))
		== venture::HostMarkerRead::Invalid,
		"a null output pointer is rejected");
	char tooSmall[8] = {};
	Check(venture::ReadHostMarker(&MarkerGetterGood, &copy, tooSmall, sizeof(tooSmall), version, sizeof(version))
		== venture::HostMarkerRead::Invalid,
		"a name buffer smaller than the ABI limit is rejected instead of over-read");
}

// -- HOST-02: bounded/truncation-safe formatting and name length limits ------
char g_longHostModules[4][40];
char g_longHostNames[4][64];
char g_tooLongHostName[80];
char g_tooLongHostModule[80];
char g_cappedHostName[64];
char g_cappedHostModule[64];

void FillLongHostNames()
{
	for (int index = 0; index < 4; ++index)
	{
		_snprintf_s(g_longHostModules[index], sizeof(g_longHostModules[index]), _TRUNCATE,
			"LongHost%d.dll", index);
		std::memset(g_longHostNames[index], static_cast<int>('A') + index, 63);
		g_longHostNames[index][63] = '\0';
	}
	std::memset(g_tooLongHostName, 'n', sizeof(g_tooLongHostName) - 1u);
	g_tooLongHostName[sizeof(g_tooLongHostName) - 1u] = '\0';
	std::memset(g_tooLongHostModule, 'm', sizeof(g_tooLongHostModule) - 1u);
	g_tooLongHostModule[sizeof(g_tooLongHostModule) - 1u] = '\0';
	std::memset(g_cappedHostName, 'Z', 63);
	g_cappedHostName[63] = '\0';
	std::memset(g_cappedHostModule, 'M', 63);
	g_cappedHostModule[63] = '\0';
}

bool EndsWithEllipsis(const char* const text)
{
	const std::size_t length = std::strlen(text);
	return length >= 3u && std::strcmp(text + length - 3u, "...") == 0;
}

void TestTruncationSafeFormatting()
{
	FillLongHostNames();

	venture::HostRegistry registry;
	registry.Register(MakeHost(Bit(venture::Host::Ares), "Ares.dll", "Ares", venture::HostKind::Builtin));
	for (int index = 0; index < 4; ++index)
	{
		Check(registry.RegisterDynamicHost(g_longHostModules[index], g_longHostNames[index], "", "") != 0,
			"a long (63-character) discovered host name registers");
	}

	// Four 63-character names plus " (custom)" need 294 bytes: the old strcat_s
	// append overflowed a 256-byte caller buffer and the CRT invalid-parameter
	// handler terminated the process.  Now the line is truncated with an ellipsis.
	char small[64] = {};
	bool truncated = false;
	const char* const smallResult = registry.FormatDetected(small, sizeof(small), &truncated);
	Check(smallResult == small && truncated, "HOST-02: a long detected set truncates instead of aborting");
	Check(std::strlen(small) == sizeof(small) - 1u, "the truncated result fills the buffer without overflowing it");
	Check(small[sizeof(small) - 1u] == '\0', "the truncated result stays NUL-terminated");
	Check(EndsWithEllipsis(small), "the truncated result ends with an ellipsis");

	char full[1024] = {};
	truncated = true;
	registry.FormatDetected(full, sizeof(full), &truncated);
	Check(!truncated, "HOST-02: a large enough buffer holds the whole list untruncated");
	// 4 discovered names x (63 + " (custom)") + 3 separators = 4 * 72 + 6 = 294
	Check(std::strlen(full) == 294u, "the untruncated list has the expected length");

	// FormatNames has the same contract: many undeclared bits in a small buffer.
	char names[32] = {};
	truncated = false;
	registry.FormatNames(0xFFFFFFF8u, names, sizeof(names), &truncated);
	Check(truncated && std::strlen(names) == sizeof(names) - 1u && EndsWithEllipsis(names),
		"HOST-02: FormatNames truncates a long bit list safely");

	// Degenerate buffers must not crash and must stay terminated.
	char one[1] = {};
	Check(registry.FormatDetected(one, sizeof(one)) == one && one[0] == '\0',
		"a one-byte buffer holds only the terminator");
	char two[2] = {};
	Check(registry.FormatDetected(two, sizeof(two)) == two && two[sizeof(two) - 1u] == '\0'
			&& std::strlen(two) <= 1u,
		"a two-byte buffer holds at most one character and stays NUL-terminated");
	Check(std::strcmp(registry.FormatDetected(nullptr, 16u), "") == 0,
		"a null buffer returns the empty string");
	Check(std::strcmp(registry.FormatNames(Bit(venture::Host::Ares), nullptr, 0u), "") == 0,
		"a zero-sized FormatNames buffer returns the empty string");

	// HOST-02 name length cap: Register rejects a name longer than the marker ABI
	// limit, and an exactly-63-character name is still accepted.
	const int problemsBefore = registry.ProblemCount();
	Check(!registry.Register(MakeHost(1u << 20, g_tooLongHostModule, "TooLongModule", venture::HostKind::Custom)),
		"a module name longer than 63 characters is rejected");
	Check(registry.ProblemCount() == problemsBefore + 1
		&& Contains(registry.ProblemAt(registry.ProblemCount() - 1), "module name must be 1..63"),
		"the over-long module name problem is recorded");
	Check(!registry.Register(MakeHost(1u << 21, "TooLongDisplay.dll", g_tooLongHostName, venture::HostKind::Custom)),
		"a display name longer than 63 characters is rejected");
	Check(Contains(registry.ProblemAt(registry.ProblemCount() - 1), "display name must be 1..63"),
		"the over-long display name problem is recorded");
	Check(registry.Register(MakeHost(1u << 22, g_cappedHostModule, "CappedModule", venture::HostKind::Custom)),
		"a 63-character module name is still accepted");
	Check(registry.Register(MakeHost(1u << 23, "CappedName.dll", g_cappedHostName, venture::HostKind::Custom)),
		"a 63-character display name is still accepted");
}

// -- HOST-05: the problem drain reports each problem exactly once -------------
int g_problemLineCount = 0;
char g_problemLines[2048] = {};

void AppendProblemLine(const char* const message)
{
	++g_problemLineCount;
	if (!message)
		return;

	const std::size_t used = std::strlen(g_problemLines);
	const std::size_t capacity = sizeof(g_problemLines) - 1u;
	std::size_t index = 0;
	while (message[index] != '\0' && used + index < capacity)
	{
		g_problemLines[used + index] = message[index];
		++index;
	}
	const std::size_t written = used + index;
	g_problemLines[written] = '\0';
	if (written + 1u < sizeof(g_problemLines))
	{
		g_problemLines[written] = '\n';
		g_problemLines[written + 1u] = '\0';
	}
}

void __cdecl CaptureHostProblem(const char* const message)
{
	AppendProblemLine(message);
}

void TestProblemDrain()
{
	venture::HostRegistry registry;
	registry.Register(MakeHost(Bit(venture::Host::Ares), "Ares.dll", "Ares", venture::HostKind::Builtin));
	registry.Detect(&LoadAres);
	Check(registry.ProblemCount() == 0, "HOST-05: a clean detection records no problem");

	g_problemLineCount = 0;
	g_problemLines[0] = '\0';
	Check(registry.ReportNewProblems(&CaptureHostProblem) == 0,
		"HOST-05: the drain reports nothing when no problem is pending");

	// The startup path takes its first drain right after Detect().  A problem
	// recorded later must reach the second drain instead of being lost.
	Check(!registry.Register(MakeHost(1u << 3, "Late.dll", "Late", venture::HostKind::Custom)),
		"a source-level registration after detection is rejected");
	Check(registry.ReportNewProblems(&CaptureHostProblem) == 1,
		"HOST-05: the drain reports the late-registration problem");
	Check(g_problemLineCount == 1, "the sink saw exactly one line");
	Check(Contains(g_problemLines, "after host detection"),
		"the drained problem explains the lifecycle");
	Check(registry.ReportNewProblems(&CaptureHostProblem) == 0,
		"HOST-05: a reported problem is never reported twice");

	// HOST-04's rejection plus the second drain: the exact combination the fixed
	// startup path relies on.
	const int problemsBefore = registry.ProblemCount();
	g_problemLineCount = 0;
	g_problemLines[0] = '\0';
	Check(registry.RegisterDynamicHost("LateHost.dll", "LateHost", "", "") == 0,
		"HOST-04: a discovered host registered after detection is rejected");
	Check(registry.ProblemCount() == problemsBefore + 1,
		"the frozen-set rejection is recorded as a problem");
	Check(registry.ReportNewProblems(&CaptureHostProblem) == 1,
		"HOST-05: the second drain reports the problem recorded after detection");
	Check(Contains(g_problemLines, "after host detection"),
		"the second-drain problem names the frozen set");

	// A null sink must not consume the pending problems.
	Check(!registry.Register(MakeHost(1u << 4, "Late2.dll", "Late2", venture::HostKind::Custom)),
		"a second late registration is rejected");
	g_problemLineCount = 0;
	g_problemLines[0] = '\0';
	Check(registry.ReportNewProblems(nullptr) == 0, "a null drain sink reports nothing");
	Check(registry.ReportNewProblems(&CaptureHostProblem) == 1,
		"the null sink did not advance the drain cursor");
	Check(Contains(g_problemLines, "after host detection"), "the pending problem is still reported");
}

int __cdecl main()
{
	TestBuiltinRegistration();
	TestDetectionAndQueries();
	TestCapacityLimits();
	TestFormattingEdgeCases();
	TestMarkerValidation();
	TestDynamicRegistration();
	TestHostMarkerReader();
	TestTruncationSafeFormatting();
	TestProblemDrain();

	std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAILED",
		g_failures, g_failures == 1 ? "" : "s");
	return g_failures == 0 ? 0 : 1;
}
