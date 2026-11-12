#pragma once

#include <cstdint>

#include <platform/HostIds.h>

// ---------------------------------------------------------------------------
// Source-level feature API of the VentureCore base (2.0).
//
// A feature is one self-contained module under src/Features/<Name>/: it owns its
// engine hooks, its extension stores, its INI keys and its documentation, and it
// registers a FeatureDescriptor with the platform at load time. There is no
// runtime plugin loading and no C ABI: the module is compiled into the platform
// DLL, so a derived platform is free to change any of this.
//
// Contract:
//   * hooks are installed by SyringeEx before Startup() runs, so a hook may fire
//     even when the feature is switched off - every hook must gate on `enabled`;
//   * descriptors are registered at load time by a file-static registrar;
//     malformed declarations and duplicate id/iniKey values are rejected and
//     reported in the startup log, and registration closes as soon as feature
//     initialization starts (a feature cannot register another feature from its
//     own Initialize()).  See FeatureDescriptor and RegisterFeature below;
//   * the base writes the resolved switch into *descriptor.enabled before it
//     calls Initialize(), and Initialize() runs whether the feature is on or off
//     (so stores/cleanup hooks exist in both cases);
//   * a feature switch defaults to FeatureDescriptor::defaultEnabled (on unless
//     the descriptor overrides it);
//   * host dependencies (2.0) are declared per feature, not resolved to a
//     support tier: requiredHosts is AND, anyOfHosts is OR, 0 means "no
//     constraint".  When the detected host set does not satisfy them the feature
//     is disabled and the startup log names the missing host(s).  Both fields may
//     only reference compile-time known bits (the built-in hosts in
//     platform/HostIds.h plus the derived platform's Hosts.Custom.cpp bits); a
//     host discovered at run time is queried by name in Initialize() through
//     FeatureContext::HasHostName.  See docs/HostMatrix.md.
// ---------------------------------------------------------------------------

namespace venture
{
// The base default for a [Features] switch when a descriptor does not override
// FeatureDescriptor::defaultEnabled.  The platform is meant to be usable by
// dropping the DLL in and changing nothing else.
inline constexpr bool kDefaultFeatureEnabled = true;

// Calling convention (API-05): every callback pointer in FeatureContext and the
// FeatureDescriptor::Initialize hook is explicitly __cdecl, and so is
// RegisterFeature().  The project builds with /Gz (stdcall is the default
// calling convention), and a function pointer's convention is data, not part of
// a symbol name: a feature translation unit compiled without the platform's
// VentureCore.props would otherwise disagree with the base about who pops the
// arguments, with no diagnostic at link time.  A feature must either keep using
// the project props (recommended) or spell `__cdecl` on its own callbacks - the
// pinned pointer type turns a mismatch into a compile error instead of a silent
// stack imbalance.  (The FeatureRegistrar constructor is a C++ member function
// and is not affected by the implicit-convention switch.)

// Services the base hands to a feature module during initialization.
//
// Lifetime (API-03): `context` is a stack local of Platform::Startup(); it is
// valid only while Initialize() runs.  A feature that needs a service after
// initialization (from a hook, for example) must copy the pointer values it
// needs by value into its own storage.  Storing `&context` (or a pointer to one
// of its members) leaves a dangling reference once Startup() returns.  The
// pointed-to strings (`platformName`, `iniPath`) and the callback targets have
// process lifetime, so copying those pointer values is safe.
//
// Nullability: the base fills in every callback in Startup().  A unit test or a
// derived platform may build a partial context (the platform's own tests do,
// e.g. to exercise one adapter), so a feature whose code is also reused in such
// a harness must check a pointer before calling it; the base only guarantees
// that a null `Log` means "no logging available" (FeatureRegistry checks it).
//
// Buffer contract: IniString/IniGetString write at most `bufferSize` bytes
// *including* the NUL terminator, silently truncate a longer value (the registry
// still records the full value for diagnostics) and do nothing when `buffer` is
// null or `bufferSize` is 0.
//
// Host set: `hostMask` is the snapshot taken right after detection.  Only the
// compile-time known bits (built-in host bits plus the platform's
// Hosts.Custom.cpp bits) are predictable here; a host discovered at run time has
// no compile-time bit, so it is queried by name with HasHostName inside
// Initialize(), not by bit.
struct FeatureContext
{
	const char* platformName; // "VentureCore" unless renamed
	const char* iniPath;      // full path of <Platform>.ini (may not exist)
	unsigned hostMask;        // detected hosts, see platform/HostIds.h

	// Queries against the detected host set (2.0, replacing the removed
	// per-feature level field).  HasHost is for the built-in bits, HasHostBit for
	// any compile-time known bit (including this platform's custom hosts), and
	// HasHostName for a host known only by name (ASCII case-insensitive; a null
	// or empty name is false).
	bool (__cdecl* HasHost)(Host host);
	bool (__cdecl* HasHostBit)(unsigned bit);
	bool (__cdecl* HasHostName)(const char* name);

	// Legacy getters (pre-2.0).  Kept for existing modules; a new module should
	// register its keys with the IniBool/IniInt/IniString trio below (2.0), which
	// validates the value, logs the effective value, feeds the unknown-key scan
	// and the [Platform] DumpIniTemplate template dump.  See docs/Ini.md.
	bool (__cdecl* IniGetBool)(const char* section, const char* key, bool defaultValue);
	int (__cdecl* IniGetInt)(const char* section, const char* key, int defaultValue);
	void (__cdecl* IniGetString)(
		const char* section,
		const char* key,
		const char* defaultValue,
		char* buffer,
		unsigned long bufferSize);

	// Declarative key registration (2.0): read, register, validate and log one
	// key.  `description` is the user-facing description shown by the template
	// dump; `sourceFile` is normally passed by the VENTURE_INI_* macros below
	// (__FILE__).  An invalid bool/integer falls back to defaultValue with a
	// warning, and an integer outside [minValue, maxValue] is rejected the same
	// way (minValue > maxValue means "no range").  A duplicate (section, key)
	// registration from another feature produces a warning; the first
	// declaration wins.
	bool (__cdecl* IniBool)(const char* section, const char* key, bool defaultValue,
		const char* description, const char* sourceFile);
	int (__cdecl* IniInt)(const char* section, const char* key, int defaultValue,
		int minValue, int maxValue, const char* description, const char* sourceFile);
	void (__cdecl* IniString)(const char* section, const char* key, const char* defaultValue,
		char* buffer, unsigned long bufferSize, const char* description, const char* sourceFile);

	// printf-style, written to <Platform>.log.  May be null (see Nullability
	// above); a null `format` is dropped instead of reaching the CRT.
	void (__cdecl* Log)(const char* format, ...);
};

// A feature's declaration, registered at load time (see VENTURE_REGISTER_FEATURE).
//
// Lifetime: the registry stores a pointer to the descriptor for the life of the
// DLL, so the descriptor and the bool it writes `enabled` through must have
// static lifetime - the documented pattern is a file-static `const
// FeatureDescriptor` plus VENTURE_REGISTER_FEATURE.  Registration order across
// translation units is unspecified, and the registry rejects a malformed
// descriptor (missing/empty id, iniKey or name, null Initialize/enabled) or a
// duplicate id/iniKey (ASCII case-insensitive) at load time; both are reported
// in the startup log (API-01/API-02).
struct FeatureDescriptor
{
	const char* id;      // unique, lower_snake_case; internal identifier (log, lookup)
	const char* iniKey;  // switch key in the [Features] section of <Platform>.ini;
	                     // one per module, regardless of how many keys it owns
	const char* name;    // human readable
	unsigned requiredHosts; // AND: every bit must be present; 0 = no requirement
	unsigned anyOfHosts;    // OR: at least one bit must be present; 0 = no constraint
	bool* enabled;       // base writes the resolved switch here
	void (__cdecl* Initialize)(const FeatureContext& context);

	// Optional trailing fields (aggregate default member initializers): a
	// descriptor that does not list them keeps the documented defaults.
	//
	// group: name of this module's group in the [Groups] section of
	// <Platform>.ini.  A group master switch ([Groups] <name>=no, default yes)
	// disables every module of the group; null/empty = the module is in no
	// group.  The group does not replace the per-module [Features] switch.
	const char* group = nullptr;
	// defaultEnabled: the [Features] switch value used when the key is absent
	// from <Platform>.ini (default true, i.e. the platform works drop-in).
	bool defaultEnabled = kDefaultFeatureEnabled;
};

// Called by FeatureRegistrar; normally you never call this yourself.  A
// malformed descriptor (null/empty id, iniKey or name, null Initialize/enabled)
// and a duplicate id/iniKey are rejected at load time and reported in the
// startup log; registration after InitializeFeatures() has started is rejected
// too (API-01/API-02/API-07).
void __cdecl RegisterFeature(const FeatureDescriptor* descriptor);

struct FeatureRegistrar
{
	explicit FeatureRegistrar(const FeatureDescriptor* const descriptor)
	{
		RegisterFeature(descriptor);
	}

	FeatureRegistrar(const FeatureRegistrar&) = delete;
	FeatureRegistrar& operator=(const FeatureRegistrar&) = delete;
};
}

// Registers a file-static FeatureDescriptor with the platform at load time.
// Usage:
//   static const venture::FeatureDescriptor Descriptor{ ... };
//   VENTURE_REGISTER_FEATURE(Descriptor);
#define VENTURE_REGISTER_FEATURE(descriptorVariable) \
	static ::venture::FeatureRegistrar g_ventureFeatureRegistrar_##descriptorVariable(&(descriptorVariable))

// Register + read one <Platform>.ini key with the 2.0 INI registry, passing the
// registration site's __FILE__ for the duplicate/invalid diagnostics.  Prefer
// these over the legacy IniGet* getters in new feature code:
//
//   bool enabled = VENTURE_INI_BOOL(context, "MyFeature", "Enabled", true,
//       "turn the optional behaviour on");
//   int count = VENTURE_INI_INT(context, "MyFeature", "Count", 10, 0, 100,
//       "how many objects to handle per tick");
#define VENTURE_INI_BOOL(context, section, key, defaultValue, description) \
	((context).IniBool((section), (key), (defaultValue), (description), __FILE__))
#define VENTURE_INI_INT(context, section, key, defaultValue, minValue, maxValue, description) \
	((context).IniInt((section), (key), (defaultValue), (minValue), (maxValue), (description), __FILE__))
#define VENTURE_INI_STRING(context, section, key, defaultValue, buffer, bufferSize, description) \
	((context).IniString((section), (key), (defaultValue), (buffer), (bufferSize), (description), __FILE__))
