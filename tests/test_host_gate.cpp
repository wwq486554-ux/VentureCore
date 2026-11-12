// Unit test: the 2.0 host gate of the feature registry.
//
// TEST_DEPS: src/Core/HostRegistry.cpp src/Core/FeatureRegistry.cpp src/Core/Hosts.Builtin.cpp src/Core/IniRegistry.cpp
//
// Covers both layers of the host model:
//   * the pure gate rules (requiredHosts AND, anyOfHosts OR, 0 = no constraint)
//     over the full present-set matrix;
//   * FeatureRegistry::InitializeFeatures() end to end with a fake INI and a
//     captured log, including the "only Phobos, no Ares" case that the removed
//     support tier used to get wrong (a requiredHosts=Ares feature must be
//     disabled there, even though Phobos is the "higher" host).
//
// The registry probes are injected, so no host DLL or game process is needed.

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "Core/FeatureRegistry.h"
#include "Core/HostRegistry.h"
#include "Core/IniRegistry.h"

namespace
{
int g_failures = 0;
int g_initializeCalls = 0;
char g_log[16384] = {};

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

bool LogContains(const char* const needle)
{
	return needle && std::strstr(g_log, needle) != nullptr;
}

void ResetLog()
{
	g_log[0] = '\0';
}

// FeatureContext::Log callback: append every line, so the gate's reasons can be
// asserted independently of the order the features are registered in.
void __cdecl CaptureLog(const char* const format, ...)
{
	char line[512] = {};
	va_list args;
	va_start(args, format);
	_vsnprintf_s(line, sizeof(line), _TRUNCATE, format, args);
	va_end(args);

	const std::size_t used = std::strlen(g_log);
	const std::size_t remaining = sizeof(g_log) - used;
	if (remaining > 2)
	{
		strcat_s(g_log + used, remaining, line);
		strcat_s(g_log + used, remaining, "\n");
	}
}

// Fake <Platform>.ini raw reader for the INI registry: [Features] SwitchOff is
// "no", every other key is absent (the provider echoes the sentinel default).
void FakeIniRawValue(
	const char* const section,
	const char* const key,
	const char* const defaultValue,
	char* const buffer,
	const unsigned long bufferSize)
{
	if (std::strcmp(section, "Features") == 0 && std::strcmp(key, "SwitchOff") == 0)
	{
		strcpy_s(buffer, static_cast<std::size_t>(bufferSize), "no");
		return;
	}
	strcpy_s(buffer, static_cast<std::size_t>(bufferSize), defaultValue);
}

void IniSink(const bool warning, const char* const message)
{
	(void)warning;
	(void)message;
}

// Injected host probes.
bool ProbeAresAndPhobos(const char* const moduleName)
{
	return std::strcmp(moduleName, "Ares.dll") == 0 || std::strcmp(moduleName, "Phobos.dll") == 0;
}

bool ProbePhobosOnly(const char* const moduleName)
{
	return std::strcmp(moduleName, "Phobos.dll") == 0;
}

void __cdecl NoopInitialize(const venture::FeatureContext& context)
{
	(void)context;
	++g_initializeCalls;
}

unsigned Bit(const venture::Host host)
{
	return venture::HostBit(host);
}

// -- descriptors: one per gate branch --------------------------------------
bool g_enabledNoRequirement = false;
bool g_enabledRequiresAres = false;
bool g_enabledRequiresPhobos = false;
bool g_enabledRequiresKratos = false;
bool g_enabledRequiresAresAndKratos = false;
bool g_enabledAnyOfKratosOrPhobos = false;
bool g_enabledAnyOfKratos = false;
bool g_enabledSwitchOff = false;

const venture::FeatureDescriptor kNoRequirement{
	"test_no_requirement", "NoRequirement", "No requirement",
	0u, 0u, &g_enabledNoRequirement, &NoopInitialize };

const venture::FeatureDescriptor kRequiresAres{
	"test_requires_ares", "RequiresAres", "Requires Ares",
	Bit(venture::Host::Ares), 0u, &g_enabledRequiresAres, &NoopInitialize };

const venture::FeatureDescriptor kRequiresPhobos{
	"test_requires_phobos", "RequiresPhobos", "Requires Phobos",
	Bit(venture::Host::Phobos), 0u, &g_enabledRequiresPhobos, &NoopInitialize };

const venture::FeatureDescriptor kRequiresKratos{
	"test_requires_kratos", "RequiresKratos", "Requires Kratos",
	Bit(venture::Host::Kratos), 0u, &g_enabledRequiresKratos, &NoopInitialize };

const venture::FeatureDescriptor kRequiresAresAndKratos{
	"test_requires_ares_and_kratos", "RequiresAresKratos", "Requires Ares and Kratos",
	Bit(venture::Host::Ares) | Bit(venture::Host::Kratos), 0u, &g_enabledRequiresAresAndKratos, &NoopInitialize };

const venture::FeatureDescriptor kAnyOfKratosOrPhobos{
	"test_any_of_kratos_or_phobos", "AnyOfKratosPhobos", "Any of Kratos or Phobos",
	0u, Bit(venture::Host::Kratos) | Bit(venture::Host::Phobos), &g_enabledAnyOfKratosOrPhobos, &NoopInitialize };

const venture::FeatureDescriptor kAnyOfKratos{
	"test_any_of_kratos", "AnyOfKratos", "Any of Kratos",
	0u, Bit(venture::Host::Kratos), &g_enabledAnyOfKratos, &NoopInitialize };

const venture::FeatureDescriptor kSwitchOff{
	"test_switch_off", "SwitchOff", "Switch off",
	0u, 0u, &g_enabledSwitchOff, &NoopInitialize };

// HOST-07: a host requirement naming a bit no registered host defines.  Bit 20 is
// outside the built-in range and outside every custom bit this test registers, so
// it can never be present: the gate keeps the feature disabled and the registry
// records the stale declaration as a startup problem.
bool g_enabledUnknownHostBit = false;
const venture::FeatureDescriptor kRequiresUnknownHostBit{
	"test_unknown_host_bit", "UnknownHostBit", "Requires an unknown host bit",
	1u << 20, 0u, &g_enabledUnknownHostBit, &NoopInitialize };
}

void TestGateRules()
{
	const unsigned ares = Bit(venture::Host::Ares);
	const unsigned phobos = Bit(venture::Host::Phobos);
	const unsigned kratos = Bit(venture::Host::Kratos);

	const unsigned presents[] = {
		0u, ares, phobos, kratos, ares | phobos, ares | kratos, phobos | kratos, ares | phobos | kratos };
	const unsigned requireds[] = { 0u, ares, kratos, ares | phobos, ares | kratos };
	const unsigned anyOfs[] = { 0u, kratos, phobos | kratos, ares | kratos };

	int mismatches = 0;
	for (const unsigned present : presents)
	{
		for (const unsigned required : requireds)
		{
			for (const unsigned anyOf : anyOfs)
			{
				const bool expectedRequired = (present & required) == required;
				const bool expectedAnyOf = anyOf == 0 || (present & anyOf) != 0;
				if (venture::RequiredHostsPresent(present, required) != expectedRequired)
					++mismatches;
				if (venture::AnyOfHostsPresent(present, anyOf) != expectedAnyOf)
					++mismatches;
				if (venture::HostRequirementSatisfied(present, required, anyOf) != (expectedRequired && expectedAnyOf))
					++mismatches;
			}
		}
	}
	Check(mismatches == 0, "gate rules match the AND/OR definition over the full matrix");

	Check(venture::RequiredHostsPresent(0u, 0u), "requiredHosts=0 is always satisfied");
	Check(venture::AnyOfHostsPresent(0u, 0u), "anyOfHosts=0 is always satisfied");
	Check(!venture::RequiredHostsPresent(phobos, ares), "requiredHosts=Ares is not satisfied by Phobos alone");
	Check(venture::AnyOfHostsPresent(phobos, ares | phobos), "anyOfHosts is satisfied by one of its bits");
	Check(venture::IsSingleHostBit(ares), "IsSingleHostBit accepts one bit");
	Check(!venture::IsSingleHostBit(ares | phobos), "IsSingleHostBit rejects several bits");
	Check(!venture::IsSingleHostBit(0u), "IsSingleHostBit rejects zero");
}

void TestFeatureGateEndToEnd()
{
	// The global registry gets the built-in hosts from Hosts.Builtin.cpp.
	Check(venture::Hosts().DefinitionCount() == 3, "the built-in registry has the three built-in hosts");
	Check(venture::Hosts().ProblemCount() == 0, "the built-in registrations are problem-free");

	venture::RegisterFeature(&kNoRequirement);
	venture::RegisterFeature(&kRequiresAres);
	venture::RegisterFeature(&kRequiresPhobos);
	venture::RegisterFeature(&kRequiresKratos);
	venture::RegisterFeature(&kRequiresAresAndKratos);
	venture::RegisterFeature(&kAnyOfKratosOrPhobos);
	venture::RegisterFeature(&kAnyOfKratos);
	venture::RegisterFeature(&kSwitchOff);
	venture::RegisterFeature(&kRequiresUnknownHostBit);

	venture::IniRegistry& ini = venture::IniRegistry::Instance();
	ini.SetRawReader(&FakeIniRawValue);
	ini.SetEnumerator(nullptr);
	ini.SetLogSink(&IniSink);

	venture::FeatureContext context{};
	context.platformName = "VentureCore";
	context.iniPath = "Z:\\game\\VentureCore.ini";
	context.hostMask = venture::Hosts().Detect(&ProbeAresAndPhobos);
	context.HasHost = &venture::HasHost;
	context.HasHostBit = &venture::HasHostBit;
	context.HasHostName = &venture::HasHostName;
	context.IniGetInt = nullptr;
	context.IniGetString = nullptr;
	context.Log = &CaptureLog;

	Check(context.hostMask == (Bit(venture::Host::Ares) | Bit(venture::Host::Phobos)),
		"detection found Ares and Phobos");
	Check(context.HasHost != nullptr && context.HasHostBit != nullptr && context.HasHostName != nullptr,
		"the FeatureContext host callbacks are wired");

	ResetLog();
	g_initializeCalls = 0;
	venture::InitializeFeatures(context);

	Check(g_enabledNoRequirement, "no requirement: enabled");
	Check(g_enabledRequiresAres, "requiredHosts=Ares: enabled while Ares is present");
	Check(g_enabledRequiresPhobos, "requiredHosts=Phobos: enabled while Phobos is present");
	Check(!g_enabledRequiresKratos, "requiredHosts=Kratos: disabled while Kratos is absent");
	Check(!g_enabledRequiresAresAndKratos, "requiredHosts=Ares+Kratos: disabled when one is missing");
	Check(g_enabledAnyOfKratosOrPhobos, "anyOfHosts=Kratos|Phobos: enabled by Phobos");
	Check(!g_enabledAnyOfKratos, "anyOfHosts=Kratos: disabled while Kratos is absent");
	Check(!g_enabledSwitchOff, "[Features] switch off: disabled");
	Check(g_initializeCalls == 9, "Initialize() runs for every feature, enabled or not");

	// HOST-07: a declared host bit no registered host defines is reported as a
	// declaration problem naming the feature, instead of silently disabling it.
	Check(!g_enabledUnknownHostBit,
		"HOST-07: a host bit no registered host defines keeps the feature disabled");
	Check(venture::FeatureRegistrationProblemCount() >= 1,
		"HOST-07: the unknown host bit is recorded as a startup problem");
	bool sawUnknownHostBitProblem = false;
	for (int problem = 0; problem < venture::FeatureRegistrationProblemCount(); ++problem)
	{
		const char* const message = venture::FeatureRegistrationProblemAt(problem);
		if (std::strstr(message, "test_unknown_host_bit") != nullptr
			&& std::strstr(message, "unknown host bit") != nullptr)
		{
			sawUnknownHostBitProblem = true;
		}
	}
	Check(sawUnknownHostBitProblem, "HOST-07: the problem names the feature and the unknown bit");

	Check(LogContains("[Features] test_no_requirement (No requirement): enabled"),
		"the enabled log line has no tier suffix");
	Check(LogContains(
		"[Features] test_requires_kratos (Requires Kratos): disabled - requires host Kratos (present: Ares, Phobos)"),
		"the requiredHosts log line names the missing host and the present set");
	Check(LogContains("disabled - requires host Kratos (present: Ares, Phobos)"),
		"a single missing required host is spelled in the singular");
	Check(LogContains(
		"[Features] test_any_of_kratos (Any of Kratos): disabled - requires any of [Kratos] (present: Ares, Phobos)"),
		"the anyOfHosts log line lists the acceptable set");
	Check(LogContains("[Features] test_switch_off (Switch off): disabled by VentureCore.ini [Features] SwitchOff"),
		"the INI switch line is unchanged");

	// Regression for the removed tier false positive: Phobos alone is not Ares.
	ResetLog();
	context.hostMask = venture::Hosts().Detect(&ProbePhobosOnly);
	venture::InitializeFeatures(context);

	Check(!g_enabledRequiresAres, "Phobos alone: requiredHosts=Ares is disabled (no tier false positive)");
	Check(g_enabledRequiresPhobos, "Phobos alone: requiredHosts=Phobos is enabled");
	Check(!g_enabledRequiresAresAndKratos, "Phobos alone: requiredHosts=Ares+Kratos is disabled");
	Check(g_enabledAnyOfKratosOrPhobos, "Phobos alone: anyOfHosts=Kratos|Phobos is enabled");
	Check(LogContains(
		"[Features] test_requires_ares (Requires Ares): disabled - requires host Ares (present: Phobos)"),
		"the missing-host line reports the actual present set");
	Check(LogContains(
		"[Features] test_requires_ares_and_kratos (Requires Ares and Kratos): disabled - requires hosts Ares, Kratos (present: Phobos)"),
		"several missing required hosts are spelled in the plural");

	// The FeatureContext callbacks read the same detected set.
	Check(venture::HasHost(venture::Host::Phobos), "HasHost(Phobos) is true for the detected set");
	Check(!venture::HasHost(venture::Host::Ares), "HasHost(Ares) is false for the detected set");
	Check(venture::HasHostBit(Bit(venture::Host::Phobos)), "HasHostBit(Phobos bit) is true");
	Check(venture::HasHostName("phobos"), "HasHostName matches case-insensitively");
	Check(!venture::HasHostName(nullptr), "HasHostName(nullptr) is false");
	Check(!venture::HasHostName("Ares"), "HasHostName of an absent host is false");
}

int __cdecl main()
{
	TestGateRules();
	TestFeatureGateEndToEnd();

	std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAILED",
		g_failures, g_failures == 1 ? "" : "s");
	return g_failures == 0 ? 0 : 1;
}
