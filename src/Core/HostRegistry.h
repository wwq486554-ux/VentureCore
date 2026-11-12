#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <platform/HostIds.h>

namespace venture
{
// ---------------------------------------------------------------------------
// Host registry: the fact layer of the host model plus the declaration gate.
//
// A host is an optional platform DLL loaded next to the base (Ares / Phobos /
// Kratos, or a private platform registered by a derived platform).  Detection
// answers only "which hosts are present"; there is no "support tier" scalar
// (removed in 2.0 - see docs/HostMatrix.md).  A feature declares what it needs
// instead: FeatureDescriptor::requiredHosts (all of) and anyOfHosts (any of).
//
// Phases:
//   1. static registration at DLL load time: Hosts.Builtin.cpp registers the
//      built-in three, a derived platform's Hosts.Custom.cpp registers private
//      hosts with an explicit bit;
//   2. detection once in Platform::Startup(): every registered host is probed
//      by module name and the present set is frozen;
//   3. queries: FeatureContext::HasHost/HasHostBit/HasHostName and the
//      FeatureRegistry gate read the frozen set.
//
// Contracts:
//   * moduleName / displayName are static-lifetime strings (string literals);
//   * Register() runs before Detect() - the static initializers guarantee
//     that; a late registration is rejected and reported as a problem;
//   * the detected set is written once on the game's main thread and then only
//     read, like the rest of the startup state;
//   * this unit is engine-free apart from the two Win32 calls in Detect()
//     (GetModuleHandleA / GetModuleFileNameA), so tests drive it directly.
// ---------------------------------------------------------------------------

enum class HostKind : std::uint8_t
{
	Builtin, // compiled into the base: Ares / Phobos / Kratos (bits 1<<0..1<<2)
	Custom,  // registered by a derived platform with an explicit bit >= 1<<3
};

struct HostDefinition
{
	unsigned bit;            // exactly one bit set
	const char* moduleName;  // file name probed with GetModuleHandleA ("Ares.dll")
	const char* displayName; // spelling used in logs and HasHostName()
	HostKind kind;
};

// One host found at startup: its definition plus the resolved module path (for
// the debug-level startup lines; empty when the path cannot be resolved, as in
// a unit test with an injected probe).
struct DetectedHost
{
	const HostDefinition* definition;
	char modulePath[260];
};

class HostRegistry
{
public:
	HostRegistry();

	// Static phase.  Returns false and records a problem when the definition is
	// invalid or conflicts with an earlier one (the first definition wins).
	bool Register(const HostDefinition& definition);

	// Detection phase.  Probes every registered host with `isModuleLoaded`
	// (production passes &IsModuleLoaded, a GetModuleHandleA wrapper; a test
	// passes a fake).  Must be called after all Register() calls; calling it
	// again re-runs the probe and replaces the detected set.
	unsigned Detect(bool (*isModuleLoaded)(const char* moduleName));

	// -- detected facts -----------------------------------------------------
	unsigned Mask() const { return mask_; }
	int DetectedCount() const;
	const DetectedHost* DetectedAt(int index) const;
	bool HasBit(unsigned bit) const;
	bool HasName(const char* name) const;

	// -- registered definitions (independent of the detected set) -----------
	int DefinitionCount() const;
	const HostDefinition* DefinitionAt(int index) const;
	const HostDefinition* FindByBit(unsigned bit) const;
	const HostDefinition* FindByName(const char* name) const;
	unsigned KnownMask() const { return knownMask_; }

	// -- formatting (never returns null; "<none>" for an empty result) ------
	// Names of the registered hosts whose bit is in `mask`, in ascending bit
	// order ("Ares, Phobos").  A bit with no registered definition is printed
	// as a hexadecimal value so a malformed declaration stays visible.
	const char* FormatNames(unsigned mask, char* buffer, std::size_t bufferSize) const;
	// Names of the detected set, in ascending bit order; custom hosts carry a
	// " (custom)" suffix ("Ares, MyPlatform (custom)").
	const char* FormatDetected(char* buffer, std::size_t bufferSize) const;

	// -- registration problems (reported once at startup) -------------------
	int ProblemCount() const;
	const char* ProblemAt(int index) const;

private:
	void AddProblem(const char* format, ...);

	std::vector<HostDefinition> definitions_;
	std::vector<DetectedHost> detected_;
	std::vector<std::string> problems_;
	unsigned knownMask_;
	unsigned mask_;
	bool detectionRan_;
};

// The process-wide registry the base registers into at load time.  Constructed
// on first use, so the static initializers of Hosts.Builtin.cpp / a derived
// platform's Hosts.Custom.cpp can register regardless of translation-unit
// initialization order (the same pattern as the feature registry).
HostRegistry& Hosts();

// Registers a definition into the process-wide registry (called by the
// file-static registrars below).
struct HostRegistrar
{
	explicit HostRegistrar(const HostDefinition& definition)
	{
		Hosts().Register(definition);
	}

	HostRegistrar(const HostRegistrar&) = delete;
	HostRegistrar& operator=(const HostRegistrar&) = delete;
};

// Win32 probe used by Platform::Startup(): GetModuleHandleA(moduleName) != 0.
bool IsModuleLoaded(const char* moduleName);

// FeatureContext callbacks: queries against the detected set.
bool HasHost(Host host);
bool HasHostBit(unsigned bit);
bool HasHostName(const char* name);

// --- declaration gate ------------------------------------------------------
// HostRequirementSatisfied(present, required, 0)   : every required bit present
// HostRequirementSatisfied(present, 0, anyOf)      : at least one of the OR bits
// HostRequirementSatisfied(present, 0, 0)          : always true
inline bool RequiredHostsPresent(const unsigned present, const unsigned required)
{
	return (present & required) == required;
}

inline bool AnyOfHostsPresent(const unsigned present, const unsigned anyOf)
{
	return anyOf == 0 || (present & anyOf) != 0;
}

inline bool HostRequirementSatisfied(
	const unsigned present,
	const unsigned required,
	const unsigned anyOf)
{
	return RequiredHostsPresent(present, required) && AnyOfHostsPresent(present, anyOf);
}

// True when `mask` holds exactly one bit; used to spell "host" vs "hosts" in
// the feature gate log.
inline bool IsSingleHostBit(const unsigned mask)
{
	return mask != 0 && (mask & (mask - 1)) == 0;
}
}

// Registers a private host of a derived platform at DLL load time (put the call
// at file scope in src/Core/Hosts.Custom.cpp).  The bit must be an explicit bit
// in the custom range (>= 1u<<3); bits 0-2 belong to the built-in hosts.
// `uniqueId` is a C identifier used to name the file-static objects.
#define VENTURE_REGISTER_HOST(uniqueId, hostBit, moduleFileName, hostDisplayName) \
	namespace \
	{ \
	const ::venture::HostDefinition g_ventureHostDefinition_##uniqueId{ \
		(hostBit), (moduleFileName), (hostDisplayName), ::venture::HostKind::Custom }; \
	const ::venture::HostRegistrar g_ventureHostRegistrar_##uniqueId(g_ventureHostDefinition_##uniqueId); \
	}
