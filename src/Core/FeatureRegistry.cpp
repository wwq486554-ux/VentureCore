#include "Core/FeatureRegistry.h"

#include "Core/PlatformConfig.h"

#include "Core/HostRegistry.h"
#include "Core/IniRegistry.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include <string>
#include <vector>

namespace
{
using venture::FeatureDescriptor;

// Process-wide registration state.  Function-local static: safe to use from the
// static initializers of any translation unit, regardless of their order, and
// never destroyed while the DLL is loaded.
struct FeatureRegistryState
{
	std::vector<const FeatureDescriptor*> features;
	std::vector<std::string> problems;
	// API-07: set when the first InitializeFeatures() pass starts.  Feature
	// modules register at load time; a registration attempted from an
	// Initialize() (or later) must not mutate the set the loop is walking.
	bool registrationClosed;
};

FeatureRegistryState& State()
{
	static FeatureRegistryState state{};
	return state;
}

// Records a load-time problem for the startup log.  The registry runs from
// static initializers, before Log::Open(), so it never logs directly - the
// startup path drains FeatureRegistrationProblemCount() after
// InitializeFeatures().
void AddProblem(const char* const format, ...)
{
	char message[320] = {};
	va_list args;
	va_start(args, format);
	_vsnprintf_s(message, sizeof(message), _TRUNCATE, format, args);
	va_end(args);
	State().problems.push_back(message);
}

// ASCII case-insensitive compare for the duplicate id/iniKey checks, the same
// policy the INI registry uses for feature ids: two features that differ only in
// case are the same feature for the INI diagnostics.
bool EqualsIgnoreCase(const char* const left, const char* const right)
{
	if (!left || !right)
		return left == right;

	const char* a = left;
	const char* b = right;
	while (*a != '\0' && *b != '\0')
	{
		char lowerA = *a;
		char lowerB = *b;
		if (lowerA >= 'A' && lowerA <= 'Z')
			lowerA = static_cast<char>(lowerA - 'A' + 'a');
		if (lowerB >= 'A' && lowerB <= 'Z')
			lowerB = static_cast<char>(lowerB - 'A' + 'a');
		if (lowerA != lowerB)
			return false;
		++a;
		++b;
	}
	return *a == '\0' && *b == '\0';
}

// True when `text` is null or empty; used by the API-01 descriptor validation.
bool IsNullOrEmpty(const char* const text)
{
	return !text || text[0] == '\0';
}
}

namespace venture
{
void __cdecl RegisterFeature(const FeatureDescriptor* const descriptor)
{
	FeatureRegistryState& state = State();

	// API-07: the descriptor set is frozen when initialization starts.  The old
	// code let an Initialize() call RegisterFeature() and then either reallocated
	// the vector the loop was iterating (undefined behaviour) or silently skipped
	// the new descriptor (counted by FeatureCount(), never initialized).
	if (state.registrationClosed)
	{
		AddProblem(
			"feature registration rejected: \"%s\" was registered after feature "
			"initialization started (registration is closed)",
			descriptor && descriptor->id ? descriptor->id : "<null descriptor>");
		return;
	}

	// API-01: a malformed descriptor is rejected loudly instead of being dropped
	// silently.  The field order gives the most useful single reason first.
	if (!descriptor)
	{
		AddProblem("feature registration rejected: the descriptor is null");
		return;
	}
	if (IsNullOrEmpty(descriptor->id))
	{
		AddProblem("feature registration rejected: the id must be non-empty");
		return;
	}
	if (IsNullOrEmpty(descriptor->iniKey))
	{
		AddProblem(
			"feature registration rejected: \"%s\" - the [Features] iniKey must be non-empty",
			descriptor->id);
		return;
	}
	if (IsNullOrEmpty(descriptor->name))
	{
		AddProblem(
			"feature registration rejected: \"%s\" - the display name must be non-empty",
			descriptor->id);
		return;
	}
	if (!descriptor->Initialize || !descriptor->enabled)
	{
		AddProblem(
			"feature registration rejected: \"%s\" - Initialize and enabled must be non-null",
			descriptor->id);
		return;
	}

	// Re-registering the same descriptor is a benign duplicate (a module may be
	// linked through two paths); it is not a second feature.
	for (const FeatureDescriptor* const existing : state.features)
	{
		if (existing == descriptor)
			return;
	}

	// API-02: the id and the [Features] iniKey are the registry's keys.  A
	// duplicate would make FindFeature() ambiguous, double-render the switch and -
	// when the id is shared - suppress the cross-feature INI warnings (the INI
	// registry only warns when two different feature ids own the same key).
	for (const FeatureDescriptor* const existing : state.features)
	{
		if (EqualsIgnoreCase(existing->id, descriptor->id))
		{
			AddProblem(
				"feature registration rejected: \"%s\" - the id is already registered "
				"(first definition wins)",
				descriptor->id);
			return;
		}
		if (EqualsIgnoreCase(existing->iniKey, descriptor->iniKey))
		{
			AddProblem(
				"feature registration rejected: \"%s\" - the [Features] iniKey \"%s\" is "
				"already registered by feature \"%s\" (first definition wins)",
				descriptor->id, descriptor->iniKey, existing->id);
			return;
		}
	}

	state.features.push_back(descriptor);
}

void InitializeFeatures(const FeatureContext& context)
{
	FeatureRegistryState& state = State();

	// API-07: close registration before walking the set, and walk a snapshot.
	// Initialize() is user code: the old range-for over the live vector let it
	// push_back() (iterator invalidation, undefined behaviour) and the late
	// descriptor was never initialized even when the buffer did not move.
	state.registrationClosed = true;
	const std::vector<const FeatureDescriptor*> descriptors = state.features;

	IniRegistry& ini = IniRegistry::Instance();

	// Publish the [Features] switch set before reading anything: the unknown-key
	// scan and the R1 template dump both need the complete list.  The switch
	// values themselves are read through the registry, so an invalid value is
	// warned about instead of silently falling back.
	std::vector<IniFeatureSwitch> switches;
	switches.reserve(descriptors.size());
	for (const FeatureDescriptor* const descriptor : descriptors)
	{
		IniFeatureSwitch featureSwitch{};
		featureSwitch.iniKey = descriptor->iniKey;
		featureSwitch.name = descriptor->name;
		featureSwitch.group = descriptor->group;
		featureSwitch.defaultEnabled = descriptor->defaultEnabled;
		switches.push_back(featureSwitch);
	}
	ini.SetFeatureSwitches(switches.data(), static_cast<int>(switches.size()));

	// HOST-07: a host requirement that names a bit no registered host defines can
	// never become present, so the feature would silently stay disabled.  The gate
	// below still treats such a bit as absent (the safe answer); the declaration
	// problem is recorded for the startup log, naming the feature.
	const unsigned knownHostMask = Hosts().KnownMask();

	for (const FeatureDescriptor* const descriptor : descriptors)
	{
		const bool switchOn = ini.ReadFeatureSwitch(descriptor->iniKey, descriptor->defaultEnabled);
		const bool groupOn = (descriptor->group && descriptor->group[0] != '\0')
			? ini.ReadGroupSwitch(descriptor->group)
			: true;

		// Host gate (2.0, replacing the removed support-level scalar):
		// requiredHosts is AND, anyOfHosts is OR, and 0 in either field means
		// "no constraint".  The descriptor may only reference compile-time known
		// bits; a host discovered at run time is queried by name by the feature
		// itself.
		const bool requiredOk = RequiredHostsPresent(context.hostMask, descriptor->requiredHosts);
		const bool anyOfOk = AnyOfHostsPresent(context.hostMask, descriptor->anyOfHosts);
		const bool enabled = switchOn && groupOn && requiredOk && anyOfOk;

		*descriptor->enabled = enabled;

		const unsigned unknownHostBits =
			(descriptor->requiredHosts | descriptor->anyOfHosts) & ~knownHostMask;
		if (unknownHostBits != 0)
		{
			AddProblem(
				"feature \"%s\" (%s): requiredHosts/anyOfHosts reference unknown host "
				"bit(s) 0x%08X that no registered host defines (known host bits: 0x%08X) "
				"- the feature can never be enabled by them",
				descriptor->id, descriptor->name, unknownHostBits, knownHostMask);
		}

		if (context.Log)
		{
			// The buffers are larger than the worst legitimate single host name so a
			// normal declaration is never truncated; the formatters themselves are
			// truncation-safe, so a long or hostile list is cut instead of aborting
			// the process (HOST-02).
			char required[256] = {};
			char anyOf[256] = {};
			char present[512] = {};

			if (enabled)
			{
				context.Log("[Features] %s (%s): enabled", descriptor->id, descriptor->name);
			}
			else if (!switchOn)
			{
				context.Log("[Features] %s (%s): disabled by %s [%s] %s",
					descriptor->id, descriptor->name, VC_PLATFORM_INI_NAME, VC_SECTION_FEATURES, descriptor->iniKey);
			}
			else if (!groupOn)
			{
				context.Log("[Features] %s (%s): disabled by %s [%s] %s",
					descriptor->id, descriptor->name, VC_PLATFORM_INI_NAME, VC_SECTION_GROUPS, descriptor->group);
			}
			else if (!requiredOk)
			{
				const unsigned missing = descriptor->requiredHosts & ~context.hostMask;
				Hosts().FormatNames(missing, required, sizeof(required));
				Hosts().FormatDetected(present, sizeof(present));
				context.Log("[Features] %s (%s): disabled - requires host%s %s (present: %s)",
					descriptor->id, descriptor->name, IsSingleHostBit(missing) ? "" : "s", required, present);
			}
			else
			{
				Hosts().FormatNames(descriptor->anyOfHosts, anyOf, sizeof(anyOf));
				Hosts().FormatDetected(present, sizeof(present));
				context.Log("[Features] %s (%s): disabled - requires any of [%s] (present: %s)",
					descriptor->id, descriptor->name, anyOf, present);
			}
		}

		// Tag every key the module registers during Initialize() with its id, so
		// duplicate/invalid diagnostics can name the owner.
		ini.BeginFeature(descriptor->id);
		descriptor->Initialize(context);
		ini.EndFeature();
	}
}

int FeatureCount()
{
	return static_cast<int>(State().features.size());
}

const FeatureDescriptor* FeatureAt(const int index)
{
	if (index < 0 || index >= FeatureCount())
		return nullptr;
	return State().features[static_cast<std::size_t>(index)];
}

const FeatureDescriptor* FindFeature(const char* const id)
{
	if (!id)
		return nullptr;

	for (const FeatureDescriptor* const descriptor : State().features)
	{
		if (std::strcmp(descriptor->id, id) == 0)
			return descriptor;
	}
	return nullptr;
}

int FeatureRegistrationProblemCount()
{
	return static_cast<int>(State().problems.size());
}

const char* FeatureRegistrationProblemAt(const int index)
{
	if (index < 0 || index >= FeatureRegistrationProblemCount())
		return "";
	return State().problems[static_cast<std::size_t>(index)].c_str();
}
}
