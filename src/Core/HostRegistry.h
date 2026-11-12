#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <platform/HostIds.h>
#include <platform/HostMarker.h>

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
//      by module name, then the loaded modules are scanned for the
//      VentureHost_GetInfo marker (platform/HostMarker.h), which registers
//      independent platforms as dynamic custom hosts; the present set is then
//      frozen;
//   3. queries: FeatureContext::HasHost/HasHostBit/HasHostName and the
//      FeatureRegistry gate read the frozen set.
//
// Contracts:
//   * moduleName / displayName are static-lifetime strings (string literals) of
//     1..VENTURE_HOST_MARKER_NAME_MAX (63) characters - the same cap as the
//     marker ABI, so one long name cannot overflow a bounded diagnostic line;
//   * Register() runs before Detect() - the static initializers guarantee
//     that; a late registration is rejected and reported as a problem, and so
//     is a RegisterDynamicHost() call after the first Detect() (HOST-04: the
//     detected set the feature gate is built from is frozen);
//   * the detected set is written once on the game's main thread and then only
//     read, like the rest of the startup state;
//   * registration problems are drained with ReportNewProblems(): the startup
//     path reports the load-time problems once after Detect() and drains again
//     after feature initialization, so a problem recorded after the first drain
//     is not lost (HOST-05);
//   * the startup diagnostics are built with a bounded append (HOST-02): a long
//     or numerous host list is truncated instead of letting the C runtime's
//     invalid-parameter handler terminate the game;
//   * this unit is engine-free apart from the Win32 calls in Detect()
//     (module probing, module enumeration, the SEH-protected marker read), so
//     tests drive the pure parts directly.
// ---------------------------------------------------------------------------

enum class HostKind : std::uint8_t
{
	Builtin, // compiled into the base: Ares / Phobos / Kratos (bits 1<<0..1<<2)
	Custom,  // registered by a derived platform with an explicit bit >= 1<<3
	Dynamic, // discovered at run time through the VentureHost_GetInfo marker
};

struct HostDefinition
{
	unsigned bit;            // exactly one bit set
	const char* moduleName;  // file name probed with GetModuleHandleA ("Ares.dll"), <= 63 chars
	const char* displayName; // spelling used in logs and HasHostName(), 1..63 chars
	HostKind kind;
	const char* version = nullptr; // dynamic hosts: the marker version (logs only)
};

// One host found at startup: its definition plus the resolved module path (for
// the debug-level startup lines; empty when the path cannot be resolved, as in
// a unit test with an injected probe).
struct DetectedHost
{
	const HostDefinition* definition;
	char modulePath[260];
};

// Sink for HostRegistry::ReportNewProblems().  Pinned __cdecl for the same
// reason as the FeatureContext callbacks (API-05): the project defaults to
// stdcall (/Gz), and a callback crossing a translation-unit boundary must not
// depend on that default.
typedef void (__cdecl* HostProblemSink)(const char* message);

class HostRegistry
{
public:
	HostRegistry();

	// Static phase.  Returns false and records a problem when the definition is
	// invalid (empty or over-long name, a bad bit, ...) or conflicts with an
	// earlier one (the first definition wins).
	bool Register(const HostDefinition& definition);

	// Detection phase.  Probes every registered host with `isModuleLoaded`
	// (production passes &IsModuleLoaded, a GetModuleHandleA wrapper; a test
	// passes a fake).  Must be called after all Register() calls; calling it
	// again re-runs the probe and replaces the detected set.  The first call
	// closes registration: Register() and RegisterDynamicHost() reject a later
	// call with a problem, so the set the feature gate was built from stays
	// frozen (HOST-04).
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
	//
	// Both formatters use a bounded append (HOST-02): the result is truncated to
	// fit `bufferSize`, keeps its NUL terminator and its tail becomes "..." when
	// anything was cut, so a long/hostile name list can never overflow the
	// caller's buffer or abort the process through the C runtime.  `truncated`
	// (optional) tells the caller whether that happened.
	const char* FormatNames(
		unsigned mask, char* buffer, std::size_t bufferSize, bool* truncated = nullptr) const;
	// Names of the detected set, in ascending bit order; custom hosts carry a
	// " (custom)" suffix ("Ares, MyPlatform (custom)").  Truncation: as above.
	const char* FormatDetected(
		char* buffer, std::size_t bufferSize, bool* truncated = nullptr) const;

	// -- registration problems (reported once at startup) -------------------
	int ProblemCount() const;
	const char* ProblemAt(int index) const;
	// Reports the problems recorded since the previous call through `sink`, in
	// recording order, and returns how many were reported.  Platform::Startup()
	// calls it once after Detect() (load-time and marker problems) and again
	// after InitializeFeatures() - a host registration rejected by the frozen
	// set is recorded there and must not be lost (HOST-05).  A problem is never
	// reported twice; a null sink reports nothing.
	int ReportNewProblems(HostProblemSink sink);

	// Registers a host discovered through the VentureHost_GetInfo marker (2.0
	// second phase): allocates the lowest free custom bit, rejects a duplicate
	// display name (first definition wins) and the exhausted 29-slot limit, and
	// records the marker version.  Returns the allocated bit, or 0 when the host
	// is rejected (the problem is recorded for the startup log).
	// Only accepted before the first Detect(): the module scan calls it while
	// detection runs; a call from anywhere else after detection is rejected and
	// recorded as a problem instead of mutating the detected set the feature gate
	// was built from (HOST-04).  The unit test drives it directly before
	// detection.
	unsigned RegisterDynamicHost(
		const char* moduleName,
		const char* displayName,
		const char* version,
		const char* modulePath);

private:
	void AddProblem(const char* format, ...);
	const char* OwnString(const char* text);
	const HostDefinition* FindByModuleName(const char* moduleName) const;
	unsigned AllocateCustomBit() const;
	void SortDetected();
	void DiscoverSelfIdentifyingHosts();
	// The detection-phase implementation of RegisterDynamicHost(); the public
	// entry point adds the frozen-set guard (HOST-04).
	unsigned RegisterDynamicHostInternal(
		const char* moduleName,
		const char* displayName,
		const char* version,
		const char* modulePath);

	std::vector<HostDefinition> definitions_;
	std::vector<DetectedHost> detected_;
	std::vector<std::string> problems_;
	// Stable backing storage for the strings of dynamically discovered hosts
	// (definitions_ and detected_ only keep raw pointers to them).
	std::vector<std::unique_ptr<std::string>> ownedStrings_;
	unsigned knownMask_;
	unsigned mask_;
	bool detectionRan_;
	// Cursor into problems_ for ReportNewProblems() (HOST-05).
	int reportedProblems_;
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

// Validates a marker returned by a module's VentureHost_GetInfo() export (2.0
// second phase).  Pure: magic/ABI must match, name non-null/1..63 chars and
// NUL-terminated, version null-or-<=63 chars and NUL-terminated.  This is the
// *trusted-data* validator - it dereferences `info`, so an untrusted marker must
// first be copied with ReadHostMarker() below; the unit test drives it with
// synthetic in-process markers.
bool IsValidHostMarker(const VentureHostInfo* info);

// How reading an untrusted VentureHost_GetInfo export ended (2.0 marker defence,
// HOST-01).
enum class HostMarkerRead
{
	Valid,   // the whole marker contract held
	Invalid, // readable, but magic/ABI/name/version violate the contract
	Raised,  // the export call or one of the pointers it returned raised
};

// Reads a foreign host marker with an untrusted-pointer guard: calls `getInfo()`
// and copies its magic/abiVersion and its name/version strings into caller
// storage inside one POD-only __try/__except region, so an export that raises, a
// wild marker pointer or a wild name/version pointer becomes
// HostMarkerRead::Raised (or ::Invalid) instead of an access violation in the
// game (HOST-01).  No C++ object with a destructor lives in the guarded region.
//
// `copy` is the resulting marker in trusted storage: `copy->name` points at
// `nameBuffer` and `copy->version` at `versionBuffer` (or is null when the marker
// has no version), and the copies are NUL-terminated when they fit the contract.
// Both buffers must hold VENTURE_HOST_MARKER_NAME_MAX + 1 bytes; a smaller buffer
// is rejected as Invalid.  The copied marker is then validated with
// IsValidHostMarker(), so ::Valid means the complete contract held.  The scanner
// calls this before registering a discovered host; the unit test drives it with
// synthetic getters, including a marker whose name pointer is (const char*)1.
HostMarkerRead ReadHostMarker(
	const VentureHostGetInfoFn getInfo,
	VentureHostInfo* copy,
	char* nameBuffer,
	std::size_t nameBufferSize,
	char* versionBuffer,
	std::size_t versionBufferSize);

// FeatureContext callbacks: queries against the detected set.
// Pinned __cdecl like every other FeatureContext callback (API-05): see the
// calling-convention note in include/platform/Feature.h.
bool __cdecl HasHost(Host host);
bool __cdecl HasHostBit(unsigned bit);
bool __cdecl HasHostName(const char* name);

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
