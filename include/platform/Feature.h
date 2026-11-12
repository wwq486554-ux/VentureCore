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

// Services the base hands to a feature module during initialization.
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
	bool (*HasHost)(Host host);
	bool (*HasHostBit)(unsigned bit);
	bool (*HasHostName)(const char* name);

	// Legacy getters (pre-2.0).  Kept for existing modules; a new module should
	// register its keys with the IniBool/IniInt/IniString trio below (2.0), which
	// validates the value, logs the effective value, feeds the unknown-key scan
	// and the [Platform] DumpIniTemplate template dump.  See docs/Ini.md.
	bool (*IniGetBool)(const char* section, const char* key, bool defaultValue);
	int (*IniGetInt)(const char* section, const char* key, int defaultValue);
	void (*IniGetString)(
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
	bool (*IniBool)(const char* section, const char* key, bool defaultValue,
		const char* description, const char* sourceFile);
	int (*IniInt)(const char* section, const char* key, int defaultValue,
		int minValue, int maxValue, const char* description, const char* sourceFile);
	void (*IniString)(const char* section, const char* key, const char* defaultValue,
		char* buffer, unsigned long bufferSize, const char* description, const char* sourceFile);

	// printf-style, written to <Platform>.log.
	void (*Log)(const char* format, ...);
};

struct FeatureDescriptor
{
	const char* id;      // unique, lower_snake_case; internal identifier (log, lookup)
	const char* iniKey;  // switch key in the [Features] section of <Platform>.ini;
	                     // one per module, regardless of how many keys it owns
	const char* name;    // human readable
	unsigned requiredHosts; // AND: every bit must be present; 0 = no requirement
	unsigned anyOfHosts;    // OR: at least one bit must be present; 0 = no constraint
	bool* enabled;       // base writes the resolved switch here
	void (*Initialize)(const FeatureContext& context);

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

// Called by FeatureRegistrar; normally you never call this yourself.
void RegisterFeature(const FeatureDescriptor* descriptor);

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
