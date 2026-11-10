#pragma once

#include <cstdint>

// ---------------------------------------------------------------------------
// Source-level feature API of the VentureCore base.
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
//   * feature switch default comes from VC_DEFAULT_FEATURE_ENABLED (on).
// ---------------------------------------------------------------------------

namespace venture
{
enum class Tier : std::uint8_t
{
	T0 = 0, // SyringeEx only
	T1 = 1, // + Ares
	T2 = 2, // + Phobos
	T3 = 3, // + Kratos / Kaleidoscope (best effort)
};

inline const char* TierName(const Tier tier)
{
	switch (tier)
	{
	case Tier::T0: return "T0";
	case Tier::T1: return "T1";
	case Tier::T2: return "T2";
	case Tier::T3: return "T3";
	default:       return "??";
	}
}

// Services the base hands to a feature module during initialization.
struct FeatureContext
{
	const char* platformName; // "VentureCore" unless renamed
	const char* iniPath;      // full path of <Platform>.ini (may not exist)
	unsigned hostMask;        // detected optional hosts, see HostDetect.h
	Tier tier;                // resolved support tier

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
	const char* iniKey;  // switch key in the [Features] section of <Platform>.ini
	const char* name;    // human readable
	Tier minTier;        // lowest support tier that can run this feature
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
