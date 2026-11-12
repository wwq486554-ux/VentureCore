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
//   * feature switch default comes from VC_DEFAULT_FEATURE_ENABLED (on);
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

	bool (*IniGetBool)(const char* section, const char* key, bool defaultValue);
	int (*IniGetInt)(const char* section, const char* key, int defaultValue);
	void (*IniGetString)(
		const char* section,
		const char* key,
		const char* defaultValue,
		char* buffer,
		unsigned long bufferSize);

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
