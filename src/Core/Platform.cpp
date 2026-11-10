#include "Core/Platform.h"

#include "Core/FeatureRegistry.h"
#include "Core/HostDetect.h"
#include "Core/Ini.h"
#include "Core/Lineage.h"
#include "Core/Log.h"
#include "Core/PlatformConfig.h"
#include "Core/PlatformVersion.h"
#include "Ext/ExtensionIndex.h"

#include <Syringe.h>
#include <windows.h>

#include <cstdarg>
#include <cstdio>

namespace
{
unsigned g_hostMask = 0;
venture::Tier g_tier = venture::Tier::T0;
bool g_started = false;

// Adapts the variadic FeatureContext logging callback to the platform logger.
void FeatureLog(const char* const format, ...)
{
	char message[1024] = {};
	va_list args;
	va_start(args, format);
	_vsnprintf_s(message, sizeof(message), _TRUNCATE, format, args);
	va_end(args);
	venture::Log::Info("%s", message);
}

// The platform is only supported under SyringeEx: the injected trampolines rely
// on its ESP/ZF/relative-instruction handling. Refuse to run under an older
// Syringe instead of corrupting the game silently. The game is terminated
// because the platform cannot be safely unloaded.
void RequireSyringeEx()
{
	if (SyringeFeatures::ESPModification
		&& SyringeFeatures::ZFPreservation
		&& SyringeFeatures::ReladdrInstructionFixup)
	{
		return;
	}

	MessageBoxW(
		nullptr,
		L"VentureCore requires SyringeEx to run, but the game appears to have been "
		L"launched with an older Syringe.\n\n"
		L"Please replace Syringe.exe in the game directory with the latest SyringeEx "
		L"release, then start the game again.",
		L"VentureCore - unsupported Syringe version",
		MB_OK | MB_ICONERROR);

	ExitProcess(1u);
}
}

namespace venture
{
bool Startup()
{
	if (g_started)
		return true;

	RequireSyringeEx();

	Log::Open();

	// The logger itself wrote the per-process separator header in Open(), so the
	// historic startup block starts directly with the version line below.
	Log::Info("%s %s (%s build)", VC_PLATFORM_NAME, VC_VERSION_DISPLAY, VC_BUILD_TYPE);
	Log::Info("git     : %s%s @ %s", VC_GIT_COMMIT, VC_GIT_DIRTY, VC_GIT_REF);
	Log::Info("YRpp    : %s (%s)", VC_YRPP_PIN_SHORT, VC_YRPP_PIN);

	if (Ini::Load())
	{
		Log::Info("ini     : %s (%s)", Ini::Path(), Ini::Exists() ? "found" : "not found - defaults apply");
	}
	else
	{
		Log::Warn("ini     : cannot resolve the game directory - defaults apply");
	}

	// Logging configuration.  LogLevel selects the minimum level written to the
	// log; VerboseLogging=yes is the legacy alias for debug and wins when both are
	// configured.  DebugOutput only matters for a RELEASE build (a Debug build
	// always writes to a debugger); an unrecognised LogLevel keeps the default and
	// is reported.  See docs/Logging.md.
	LogLevel logLevel = LogLevel::Info;
	const bool verboseLogging = Ini::GetBool(VC_SECTION_PLATFORM, "VerboseLogging", false);
	if (verboseLogging)
	{
		logLevel = LogLevel::Debug;
	}
	else
	{
		char levelText[32] = {};
		Ini::GetString(VC_SECTION_PLATFORM, "LogLevel", "info", levelText, static_cast<DWORD>(sizeof(levelText)));
		if (!ParseLogLevel(levelText, logLevel))
		{
			logLevel = LogLevel::Info;
			Log::Warn("log     : unrecognised LogLevel '%s' - using info", levelText);
		}
	}

	// Announce the resolved level before the filter is applied, so the log always
	// records the configuration it is running with.
	Log::Info("log     : level %s%s", LogLevelName(logLevel), verboseLogging ? " (VerboseLogging=yes)" : "");
	Log::SetMinLevel(logLevel);
	Log::SetDebugOutput(Ini::GetBool(VC_SECTION_PLATFORM, "DebugOutput", false));

	g_hostMask = DetectHostMask();
	g_tier = ResolveTier(g_hostMask);

	char hostNames[256] = {};
	HostMaskToString(g_hostMask, hostNames, static_cast<DWORD>(sizeof(hostNames)));
	Log::Info("hosts   : %s (tier %s)", hostNames, TierName(g_tier));
	void LogEventChannelStartup(); LogEventChannelStartup();

	// Lineage mutual exclusion: only one DLL of the VentureCore lineage may run
	// in this process (two would each build their own extension table and
	// savegame trailer). Scan the loaded modules and act on the configured
	// policy before any feature state is created.
	char policyText[32] = {};
	Ini::GetString(VC_SECTION_PLATFORM, "LineagePolicy", "refuse", policyText, static_cast<DWORD>(sizeof(policyText)));
	const LineagePolicy lineagePolicy = ParseLineagePolicy(policyText, LineagePolicy::Refuse);
	const LineageDecision lineage = DecideLineage(ScanForeignLineage(), lineagePolicy);

	if (!lineage.conflict)
	{
		Log::Info("lineage : no other %s-lineage DLL loaded", VC_PLATFORM_NAME);
	}
	else if (lineagePolicy == LineagePolicy::Warn)
	{
		Log::Warn(
			"lineage : conflicting %s-lineage DLL loaded: %s (LineagePolicy=warn - continuing)",
			VC_PLATFORM_NAME,
			lineage.details.c_str());
	}
	else
	{
		// No message box: the game keeps running without our features, and every
		// lifecycle entry point stays a no-op because g_started is never set.
		Log::Error(
			"lineage : conflicting %s-lineage DLL loaded: %s (LineagePolicy=refuse - platform stays disabled)",
			VC_PLATFORM_NAME,
			lineage.details.c_str());
		Log::Close();
		return false;
	}

	FeatureContext context{};
	context.platformName = VC_PLATFORM_NAME;
	context.iniPath = Ini::Path();
	context.hostMask = g_hostMask;
	context.tier = g_tier;
	context.IniGetBool = &Ini::GetBool;
	context.IniGetInt = &Ini::GetInt;
	context.IniGetString = &Ini::GetString;
	context.Log = &FeatureLog;

	InitializeFeatures(context);

	Log::Info("%s startup complete - %d feature(s) registered", VC_PLATFORM_NAME, FeatureCount());
	Log::Info("================================================================");

	g_started = true;
	return true;
}

void Shutdown()
{
	if (!g_started)
		return;

	Log::Info("%s shutting down", VC_PLATFORM_NAME);
	ExtensionIndex::Instance().ClearAll();
	Log::Close();
	g_started = false;
}

void OnScenarioClear()
{
	if (!g_started)
		return;

	// A failed savegame load returns from the engine's LoadGame before its normal
	// close point (A-04).  The three failure-exit hooks close the load window, but
	// a scenario change is the last line of defence: never carry an open window or
	// the deferred records of an aborted load into the next scenario.
	ExtensionIndex::Instance().ResetForScenarioClear();
	ExtensionIndex::Instance().ClearAll();
	Log::Info("scenario clear: extension state reset (%zu store(s) registered)",
		ExtensionIndex::Instance().StoreCount());
}

void OnInvalidatePointer(AbstractClass* const pInvalid, const bool removed)
{
	if (!g_started)
		return;

	ExtensionIndex::Instance().NotifyInvalidPointer(pInvalid, removed);
}

unsigned HostMask()
{
	return g_hostMask;
}

Tier CurrentTier()
{
	return g_tier;
}
}
