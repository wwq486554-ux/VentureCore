// Unit test: feature-registry validation and lifecycle (API-01 / API-02 /
// API-07) plus the compile-time calling-convention pins of the feature API
// (API-05).
//
// TEST_DEPS: src/Core/FeatureRegistry.cpp src/Core/HostRegistry.cpp src/Core/IniRegistry.cpp
//
// The feature registry is process-wide and engine-free: a descriptor is validated
// and registered at load time, the host requirements are checked against the
// known host mask while initializing (HOST-07 is covered by
// tests/test_host_gate.cpp), and registration closes when the first
// InitializeFeatures() pass starts (API-07).  Each test binary is a separate
// process, so this one can drive that once-only lifecycle in order: every
// registration case runs before the first InitializeFeatures() call.

#include <cstdio>
#include <cstring>
#include <type_traits>

#include "Core/FeatureRegistry.h"
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

bool AnyProblemContains(const char* const needle)
{
	for (int index = 0; index < venture::FeatureRegistrationProblemCount(); ++index)
	{
		if (Contains(venture::FeatureRegistrationProblemAt(index), needle))
			return true;
	}
	return false;
}

// -- API-05: calling-convention pins (compile-time red/green) ---------------
//
// The project builds with /Gz (stdcall is the default calling convention), so an
// unpinned function-pointer member is a __stdcall pointer and every assertion
// below fails to compile.  That is the regression test: dropping a pin from
// include/platform/Feature.h breaks the build instead of silently reintroducing
// a stack-imbalance window.
static_assert(std::is_same<decltype(venture::FeatureContext::HasHost), bool (__cdecl*)(venture::Host)>::value,
	"API-05: FeatureContext::HasHost must be pinned __cdecl");
static_assert(std::is_same<decltype(venture::FeatureContext::HasHostBit), bool (__cdecl*)(unsigned)>::value,
	"API-05: FeatureContext::HasHostBit must be pinned __cdecl");
static_assert(std::is_same<decltype(venture::FeatureContext::HasHostName), bool (__cdecl*)(const char*)>::value,
	"API-05: FeatureContext::HasHostName must be pinned __cdecl");
static_assert(std::is_same<decltype(venture::FeatureContext::IniGetBool),
	bool (__cdecl*)(const char*, const char*, bool)>::value,
	"API-05: FeatureContext::IniGetBool must be pinned __cdecl");
static_assert(std::is_same<decltype(venture::FeatureContext::IniGetInt),
	int (__cdecl*)(const char*, const char*, int)>::value,
	"API-05: FeatureContext::IniGetInt must be pinned __cdecl");
static_assert(std::is_same<decltype(venture::FeatureContext::IniGetString),
	void (__cdecl*)(const char*, const char*, const char*, char*, unsigned long)>::value,
	"API-05: FeatureContext::IniGetString must be pinned __cdecl");
static_assert(std::is_same<decltype(venture::FeatureContext::IniBool),
	bool (__cdecl*)(const char*, const char*, bool, const char*, const char*)>::value,
	"API-05: FeatureContext::IniBool must be pinned __cdecl");
static_assert(std::is_same<decltype(venture::FeatureContext::IniInt),
	int (__cdecl*)(const char*, const char*, int, int, int, const char*, const char*)>::value,
	"API-05: FeatureContext::IniInt must be pinned __cdecl");
static_assert(std::is_same<decltype(venture::FeatureContext::IniString),
	void (__cdecl*)(const char*, const char*, const char*, char*, unsigned long, const char*, const char*)>::value,
	"API-05: FeatureContext::IniString must be pinned __cdecl");
static_assert(std::is_same<decltype(venture::FeatureContext::Log), void (__cdecl*)(const char*, ...)>::value,
	"API-05: FeatureContext::Log must be pinned __cdecl");
static_assert(std::is_same<decltype(venture::FeatureDescriptor::Initialize),
	void (__cdecl*)(const venture::FeatureContext&)>::value,
	"API-05: FeatureDescriptor::Initialize must be pinned __cdecl");
static_assert(std::is_same<decltype(&venture::RegisterFeature),
	void (__cdecl*)(const venture::FeatureDescriptor*)>::value,
	"API-05: RegisterFeature must be pinned __cdecl");

// -- descriptors under test -------------------------------------------------
bool g_enabledValidA = false;
bool g_enabledValidB = false;
bool g_enabledMalformed = false;
bool g_enabledDuplicate = false;
bool g_enabledRegistersB = false;
bool g_enabledLateB = false;
bool g_enabledPostInit = false;

void __cdecl NoopInitialize(const venture::FeatureContext& context)
{
	(void)context;
}

const venture::FeatureDescriptor kValidA{
	"test_valid_a", "ValidA", "Valid A",
	0u, 0u, &g_enabledValidA, &NoopInitialize };

const venture::FeatureDescriptor kValidB{
	"test_valid_b", "ValidB", "Valid B",
	0u, 0u, &g_enabledValidB, &NoopInitialize };

// -- API-01: malformed descriptors are rejected and counted -----------------
const venture::FeatureDescriptor kNullId{
	nullptr, "NullId", "Null id",
	0u, 0u, &g_enabledMalformed, &NoopInitialize };

const venture::FeatureDescriptor kEmptyId{
	"", "EmptyId", "Empty id",
	0u, 0u, &g_enabledMalformed, &NoopInitialize };

const venture::FeatureDescriptor kNullIniKey{
	"test_null_ini_key", nullptr, "Null iniKey",
	0u, 0u, &g_enabledMalformed, &NoopInitialize };

const venture::FeatureDescriptor kEmptyIniKey{
	"test_empty_ini_key", "", "Empty iniKey",
	0u, 0u, &g_enabledMalformed, &NoopInitialize };

const venture::FeatureDescriptor kNullName{
	"test_null_name", "NullName", nullptr,
	0u, 0u, &g_enabledMalformed, &NoopInitialize };

const venture::FeatureDescriptor kEmptyName{
	"test_empty_name", "EmptyName", "",
	0u, 0u, &g_enabledMalformed, &NoopInitialize };

const venture::FeatureDescriptor kNullInitialize{
	"test_null_initialize", "NullInitialize", "Null Initialize",
	0u, 0u, &g_enabledMalformed, nullptr };

const venture::FeatureDescriptor kNullEnabled{
	"test_null_enabled", "NullEnabled", "Null enabled",
	0u, 0u, nullptr, &NoopInitialize };

// -- API-02: duplicate id / iniKey (ASCII case-insensitive) -----------------
const venture::FeatureDescriptor kDuplicateId{
	"TEST_VALID_A", "DuplicateId", "Duplicate id",
	0u, 0u, &g_enabledDuplicate, &NoopInitialize };

const venture::FeatureDescriptor kDuplicateIniKey{
	"test_duplicate_key", "valida", "Duplicate switch",
	0u, 0u, &g_enabledDuplicate, &NoopInitialize };

// -- API-07: registration from inside Initialize() --------------------------
const venture::FeatureDescriptor kLateB{
	"test_late_b", "LateB", "Late B",
	0u, 0u, &g_enabledLateB, &NoopInitialize };

void __cdecl RegistersBFromInitialize(const venture::FeatureContext& context)
{
	(void)context;
	// The old registry let this mutate the vector it was iterating (iterator
	// invalidation) and never initialized the descriptor.  It is now rejected and
	// recorded as a problem.
	venture::RegisterFeature(&kLateB);
}

const venture::FeatureDescriptor kRegistersB{
	"test_registers_b", "RegistersB", "Registers B",
	0u, 0u, &g_enabledRegistersB, &RegistersBFromInitialize };

const venture::FeatureDescriptor kPostInit{
	"test_post_init", "PostInit", "Registered after initialization",
	0u, 0u, &g_enabledPostInit, &NoopInitialize };

void TestDescriptorValidation()
{
	Check(venture::FeatureCount() == 0, "API-01: the registry starts empty");

	venture::RegisterFeature(nullptr);
	venture::RegisterFeature(&kNullId);
	venture::RegisterFeature(&kEmptyId);
	venture::RegisterFeature(&kNullIniKey);
	venture::RegisterFeature(&kEmptyIniKey);
	venture::RegisterFeature(&kNullName);
	venture::RegisterFeature(&kEmptyName);
	venture::RegisterFeature(&kNullInitialize);
	venture::RegisterFeature(&kNullEnabled);

	Check(venture::FeatureCount() == 0, "API-01: every malformed descriptor is rejected");
	Check(venture::FeatureRegistrationProblemCount() == 9,
		"API-01: each malformed descriptor is counted as a problem");
	Check(AnyProblemContains("descriptor is null"), "API-01: the null descriptor is reported");
	Check(AnyProblemContains("id must be non-empty"), "API-01: a null/empty id is reported");
	Check(AnyProblemContains("iniKey must be non-empty"), "API-01: a null/empty iniKey is reported");
	Check(AnyProblemContains("display name must be non-empty"), "API-01: a null/empty name is reported");
	Check(AnyProblemContains("Initialize and enabled must be non-null"),
		"API-01: a null Initialize/enabled is reported");
	Check(!g_enabledMalformed, "API-01: a rejected descriptor's enabled flag is not written");

	// A benign re-registration of the same descriptor is not a problem.
	venture::RegisterFeature(&kValidA);
	venture::RegisterFeature(&kValidA);
	Check(venture::FeatureCount() == 1, "a descriptor registers once even when registered twice");
	Check(venture::FeatureRegistrationProblemCount() == 9,
		"re-registering the same descriptor is not reported as a problem");
	Check(!g_enabledValidA, "the registry does not write enabled before initialization");

	venture::RegisterFeature(&kValidB);
	Check(venture::FeatureCount() == 2, "a second valid descriptor registers");

	// API-02: duplicate id and duplicate iniKey are rejected, first wins.
	venture::RegisterFeature(&kDuplicateId);
	Check(venture::FeatureCount() == 2, "API-02: a duplicate id is rejected");
	Check(AnyProblemContains("the id is already registered"), "API-02: the duplicate id is reported");
	Check(venture::FeatureRegistrationProblemCount() == 10,
		"API-02: the duplicate id is counted as a problem");

	venture::RegisterFeature(&kDuplicateIniKey);
	Check(venture::FeatureCount() == 2, "API-02: a duplicate [Features] iniKey is rejected");
	Check(AnyProblemContains("iniKey") && AnyProblemContains("already registered by feature"),
		"API-02: the duplicate iniKey problem names the owner");
	Check(venture::FeatureRegistrationProblemCount() == 11,
		"API-02: the duplicate iniKey is counted as a problem");
}

void TestInitializationLifecycle()
{
	const int problemsBefore = venture::FeatureRegistrationProblemCount();

	venture::RegisterFeature(&kRegistersB);
	Check(venture::FeatureCount() == 3, "API-07: the feature that registers another one is accepted at load time");
	Check(venture::FeatureRegistrationProblemCount() == problemsBefore,
		"registering a valid descriptor adds no problem");

	venture::FeatureContext context{};
	context.platformName = "VentureCore";
	context.iniPath = "";
	context.hostMask = 0u;
	context.Log = nullptr;

	venture::InitializeFeatures(context);

	Check(g_enabledValidA && g_enabledValidB && g_enabledRegistersB,
		"the three registered features are initialized and gated");
	Check(venture::FeatureCount() == 3,
		"API-07: the descriptor registered from Initialize() does not join the registry");
	Check(!g_enabledLateB, "API-07: the late descriptor's enabled flag is never written");
	Check(AnyProblemContains("registered after feature initialization started"),
		"API-07: the late registration is reported as a problem");
	Check(venture::FeatureRegistrationProblemCount() == problemsBefore + 1,
		"API-07: exactly one problem is recorded for the rejected late registration");

	// Registration stays closed after the pass as well: a feature-module-style
	// registration after startup is rejected instead of never being initialized.
	venture::RegisterFeature(&kPostInit);
	Check(venture::FeatureCount() == 3, "API-07: registration after the initialization pass is rejected");
	Check(!g_enabledPostInit, "API-07: the post-initialization descriptor is not initialized");
	Check(venture::FeatureRegistrationProblemCount() == problemsBefore + 2,
		"API-07: the post-initialization registration is recorded");
}
}

int __cdecl main()
{
	TestDescriptorValidation();
	TestInitializationLifecycle();

	std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAILED",
		g_failures, g_failures == 1 ? "" : "s");
	return g_failures == 0 ? 0 : 1;
}
