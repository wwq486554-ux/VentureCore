#include "Core/Platform.h"

#include "Core/FeatureRegistry.h"
#include "Core/HostRegistry.h"
#include "Core/Ini.h"
#include "Core/IniRegistry.h"
#include "Core/IniTakeover.h"
#include "Core/Lineage.h"
#include "Core/Log.h"
#include "Core/PlatformConfig.h"
#include "Core/PlatformGate.h"
#include "Core/PlatformVersion.h"
#include "Ext/ExtensionIndex.h"

#include <Syringe.h>
#include <windows.h>

#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>

namespace
{
unsigned g_hostMask = 0;
// Whether the platform may act lives in the pure, header-only PlatformGate
// (Pending / Running / Refused), so engine-free registries can check it too.

// Adapts the variadic FeatureContext logging callback to the platform logger.
// A NULL format is dropped here: the C runtime's invalid-parameter handler would
// otherwise terminate the process (API-04, same guard as Log::WriteV).
void __cdecl FeatureLog(const char* const format, ...)
{
	if (!format)
		return;

	char message[1024] = {};
	va_list args;
	va_start(args, format);
	_vsnprintf_s(message, sizeof(message), _TRUNCATE, format, args);
	va_end(args);
	venture::Log::Info("%s", message);
}

// Rejected extension-type registrations are reported through this sink: the Ext
// layer is engine- and logger-free, so it asks the platform to log the problem
// (D-22).  A rejected type id is poisoned and can no longer be fetched, which
// turns a broken composition into a loud, safe failure instead of a wrong cast.
void ExtTypeProblemLog(const char* const message)
{
	venture::Log::Error("%s", message);
}

// Refused INI-takeover registrations are reported through this sink for the same
// reason: the takeover core is logger-free (C-09).
void IniTakeoverWarnLog(const char* const message)
{
	venture::Log::Warn("%s", message);
}

// The 2.0 INI key registry is logger-free; its diagnostics (invalid values,
// duplicate registrations, unknown keys, effective values) come through here.
void IniRegistryLog(const bool warning, const char* const message)
{
	if (warning)
		venture::Log::Warn("ini     : %s", message);
	else
		venture::Log::Debug("ini     : %s", message);
}

// Host registration problems are recorded while the detection set is built and
// by a registration rejected after detection (HOST-05).  The registry hands out
// each problem exactly once; the startup path drains before and after feature
// initialization.
void __cdecl HostProblemLog(const char* const message)
{
	venture::Log::Error("hosts   : %s", message);
}

// FeatureContext adapters for the declarative INI readers.  The macros in
// include/platform/Feature.h pass __FILE__ as the source file.
bool __cdecl FeatureIniBool(
	const char* const section,
	const char* const key,
	const bool defaultValue,
	const char* const description,
	const char* const sourceFile)
{
	return venture::IniRegistry::Instance().ReadBool(section, key, defaultValue, description, sourceFile);
}

int __cdecl FeatureIniInt(
	const char* const section,
	const char* const key,
	const int defaultValue,
	const int minValue,
	const int maxValue,
	const char* const description,
	const char* const sourceFile)
{
	return venture::IniRegistry::Instance().ReadInt(
		section, key, defaultValue, minValue, maxValue, description, sourceFile);
}

void __cdecl FeatureIniString(
	const char* const section,
	const char* const key,
	const char* const defaultValue,
	char* const buffer,
	const unsigned long bufferSize,
	const char* const description,
	const char* const sourceFile)
{
	venture::IniRegistry::Instance().ReadString(
		section, key, defaultValue, buffer, bufferSize, description, sourceFile);
}

// Legacy (pre-2.0) FeatureContext getters.  The value keeps the lenient
// Ini::Get* parsing (no validation, no effective-value logging), but the key is
// recorded as known first (IniRegistry::MarkKnown) so the startup unknown-key
// scan does not report a key the module really reads and the R1 template dump
// lists it (INI-08).  __cdecl because the FeatureContext callbacks are pinned
// (API-05; the project builds with /Gz = stdcall default).
bool __cdecl FeatureIniGetBool(
	const char* const section,
	const char* const key,
	const bool defaultValue)
{
	venture::IniRegistry::Instance().MarkKnown(
		section, key, venture::IniValueType::Bool, defaultValue ? "yes" : "no");
	return venture::Ini::GetBool(section, key, defaultValue);
}

int __cdecl FeatureIniGetInt(
	const char* const section,
	const char* const key,
	const int defaultValue)
{
	char defaultText[24] = {};
	_snprintf_s(defaultText, sizeof(defaultText), _TRUNCATE, "%d", defaultValue);
	venture::IniRegistry::Instance().MarkKnown(
		section, key, venture::IniValueType::Int, defaultText);
	return venture::Ini::GetInt(section, key, defaultValue);
}

void __cdecl FeatureIniGetString(
	const char* const section,
	const char* const key,
	const char* const defaultValue,
	char* const buffer,
	const unsigned long bufferSize)
{
	venture::IniRegistry::Instance().MarkKnown(
		section, key, venture::IniValueType::String, defaultValue ? defaultValue : "");
	venture::Ini::GetString(section, key, defaultValue, buffer, bufferSize);
}

// R1 template dump ([Platform] DumpIniTemplate=yes, development only): renders
// the key registry next to the log so it can be compared with the .ini.example.
// The rendered values are the **declared defaults** of the registry, not the
// effective values of the local .ini (INI-04): the template is a declaration
// list, so it stays independent of read order and of which keys the local run
// happened to touch.
void DumpIniTemplate()
{
	const char* const iniPath = venture::Ini::Path();
	if (!iniPath || iniPath[0] == '\0')
	{
		venture::Log::Warn("ini template: cannot resolve <Platform>.ini - dump skipped");
		return;
	}

	char path[MAX_PATH * 2] = {};
	_snprintf_s(path, sizeof(path), _TRUNCATE, "%s.generated", iniPath);

	std::string text;
	text += "; " VC_PLATFORM_NAME " " VC_VERSION_DISPLAY " - generated INI template\r\n";
	text += "; Generated by [Platform] DumpIniTemplate=yes from the key registry (2.0).\r\n";
	text += "; Development aid: compare it with " VC_PLATFORM_INI_NAME ".example.\r\n";
	text += "\r\n";
	text += venture::IniRegistry::Instance().RenderTemplate();

	FILE* file = nullptr;
	if (fopen_s(&file, path, "wb") != 0 || !file)
	{
		venture::Log::Warn("ini template: cannot write %s", path);
		return;
	}
	const std::size_t written = std::fwrite(text.data(), 1, text.size(), file);
	std::fclose(file);
	if (written != text.size())
	{
		venture::Log::Warn("ini template: short write to %s", path);
		return;
	}
	venture::Log::Info("ini template: wrote %s (%d registered key(s))", path,
		venture::IniRegistry::Instance().KeyCount());
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

// [Platform] LineagePolicy parsing is case-insensitive and falls back to
// "refuse" for anything it does not recognise (Lineage.h).  The fallback is
// silent in the parser, so report a mis-spelled value here instead of silently
// enforcing "refuse" while the log line still cites the configured text
// (START-03).
bool IsRecognisedLineagePolicy(const char* const text)
{
	return _stricmp(text, "refuse") == 0 || _stricmp(text, "warn") == 0;
}
}

namespace venture
{
bool Startup()
{
	// The gate is tri-state: a second ExeRun after a successful start is a no-op,
	// a refusal is final for the process (do not retry the scan and write a
	// second startup block to the log), and a Shutdown() is final too - the
	// platform cannot be restarted in the same process (START-04).
	if (PlatformGate::IsRunning())
		return true;
	if (PlatformGate::IsRefused())
		return false;
	if (PlatformGate::IsFinished())
		return false;

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

	// 2.0 INI registry: the platform's own <Platform>.ini reads go through it, so
	// they are validated, counted and logged, unknown keys are reported once after
	// feature startup and the R1 template dump has a single source of truth.  The
	// legacy getters remain available to feature modules for compatibility; the
	// value keeps the pre-2.0 lenient parsing (no validation, no effective-value
	// logging), but the key is recorded as known before the read (MarkKnown), so
	// the unknown-key scan and the template dump see it - see docs/Ini.md and
	// docs/Updating.md.  (START-08: the previous comment claimed every read went
	// through the registry, which the FeatureContext wiring below contradicted.)
	IniRegistry& ini = IniRegistry::Instance();
	ini.SetRawReader(&Ini::GetString);
	ini.SetEnumerator(&Ini::EnumerateEntries);
	ini.SetLogSink(&IniRegistryLog);

	// Logging configuration.  LogLevel selects the minimum level written to the
	// log; VerboseLogging=yes is the legacy alias for debug and wins when both are
	// configured.  DebugOutput only matters for a RELEASE build (a Debug build
	// always writes to a debugger); an unrecognised LogLevel keeps the default and
	// is reported.  See docs/Logging.md.
	LogLevel logLevel = LogLevel::Info;
	const bool verboseLogging = ini.ReadBaseBool(VC_SECTION_PLATFORM, "VerboseLogging", false,
		"legacy alias: yes is the same as LogLevel=debug");
	if (verboseLogging)
	{
		logLevel = LogLevel::Debug;
	}
	else
	{
		char levelText[32] = {};
		ini.ReadBaseString(VC_SECTION_PLATFORM, "LogLevel", "info", levelText,
			static_cast<unsigned long>(sizeof(levelText)),
			"minimum log level: error|warn|info|debug");
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
	Log::SetDebugOutput(ini.ReadBaseBool(VC_SECTION_PLATFORM, "DebugOutput", false,
		"RELEASE builds: also write the log to the attached debugger"));

	g_hostMask = Hosts().Detect(&IsModuleLoaded);

	// Host problems are recorded while the set is built: an invalid/duplicate
	// source-level registration (Hosts.Custom.cpp) or a rejected/invalid host
	// marker found during the second-phase scan.  The drain reports each problem
	// once; a second drain runs after feature initialization (HOST-05).
	Hosts().ReportNewProblems(&HostProblemLog);

	if (Hosts().DetectedCount() == 0)
	{
		Log::Info("hosts   : <none>");
	}
	else
	{
		// HOST-02: FormatDetected appends truncation-safely and always
		// NUL-terminates; 1024 bytes cover the worst legal case with room to
		// spare, and anything longer becomes "..." instead of a crash.
		char hostNames[1024] = {};
		bool hostNamesTruncated = false;
		Hosts().FormatDetected(hostNames, sizeof(hostNames), &hostNamesTruncated);
		Log::Info("hosts   : %s (%d host(s))", hostNames, Hosts().DetectedCount());
		if (hostNamesTruncated)
			Log::Warn("hosts   : the detected-host name list was truncated to fit the log buffer");
	}

	// Debug level only: the resolved module path of every detected host makes a
	// "host did not inject" report diagnosable.
	for (int host = 0, hosts = Hosts().DetectedCount(); host < hosts; ++host)
	{
		const DetectedHost* const detected = Hosts().DetectedAt(host);
		if (!detected || detected->modulePath[0] == '\0')
			continue;
		const char* const version = detected->definition->version;
		if (version && version[0] != '\0')
			Log::Debug("hosts   : %s -> %s (marker version %s)", detected->definition->displayName, detected->modulePath, version);
		else
			Log::Debug("hosts   : %s -> %s", detected->definition->displayName, detected->modulePath);
	}

	// Lineage mutual exclusion: only one DLL of the VentureCore lineage may run
	// in this process (two would each build their own extension table and
	// savegame trailer). Scan the loaded modules and act on the configured
	// policy before any feature state is created.
	char policyText[32] = {};
	ini.ReadBaseString(VC_SECTION_PLATFORM, "LineagePolicy", "refuse", policyText,
		static_cast<unsigned long>(sizeof(policyText)),
		"conflict of a second DLL of this lineage: refuse|warn");
	const LineagePolicy lineagePolicy = ParseLineagePolicy(policyText, LineagePolicy::Refuse);
	if (policyText[0] != '\0' && !IsRecognisedLineagePolicy(policyText))
	{
		Log::Warn("lineage : unrecognised LineagePolicy '%s' - using refuse", policyText);
	}
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
		// No message box: the game keeps running without our features.  The gate is
		// marked Refused, so every engine-facing handler returns immediately, the
		// command/event registries accept no new entries, and the platform leaves
		// the engine's command list, event stream, rules INI and save stream alone
		// (C-06).
		Log::Error(
			"lineage : conflicting %s-lineage DLL loaded: %s (LineagePolicy=refuse - platform stays disabled)",
			VC_PLATFORM_NAME,
			lineage.details.c_str());
		Log::Close();
		PlatformGate::MarkRefused();
		return false;
	}

	// Extension types register themselves lazily (feature store construction or
	// first Fetch); route their validation problems to the logger before any
	// feature is initialized (D-22).  The rules-INI takeover registrations happen
	// in the same feature initialization, so route their deny-list warnings too
	// (C-09).
	SetExtTypeProblemSink(&ExtTypeProblemLog);
	// START-07: extension type registrations that ran during static initialization
	// (a feature store's first use / an EnsureExtTypeRegistered<> in a static
	// initializer) were recorded in the Ext layer's bounded startup queue because
	// no sink was installed yet.  Flush them to the logger before feature
	// initialization can add more.
	ExtTypeProblemDrain(&ExtTypeProblemLog);
	IniTakeover::SetWarnSink(&IniTakeoverWarnLog);

	FeatureContext context{};
	context.platformName = VC_PLATFORM_NAME;
	context.iniPath = Ini::Path();
	context.hostMask = g_hostMask;
	context.HasHost = &HasHost;
	context.HasHostBit = &HasHostBit;
	context.HasHostName = &HasHostName;
	context.IniGetBool = &FeatureIniGetBool;
	context.IniGetInt = &FeatureIniGetInt;
	context.IniGetString = &FeatureIniGetString;
	context.IniBool = &FeatureIniBool;
	context.IniInt = &FeatureIniInt;
	context.IniString = &FeatureIniString;
	context.Log = &FeatureLog;

	InitializeFeatures(context);

	// Diagnostics that could only be produced after the first host drain: a host
	// registration rejected by the frozen detection set (HOST-05), a malformed or
	// duplicate feature descriptor (API-01/API-02), and a feature whose host
	// requirement names a bit no registered host defines (HOST-07).
	Hosts().ReportNewProblems(&HostProblemLog);
	for (int problem = 0, problems = FeatureRegistrationProblemCount(); problem < problems; ++problem)
		Log::Error("feature : %s", FeatureRegistrationProblemAt(problem));

	// Only an allowed platform announces its event channel (a refused DLL logs
	// nothing beyond the lineage error above).  The announce runs after
	// InitializeFeatures: a feature may register an event channel from its
	// Initialize(), and the announce must count what is actually registered
	// (START-06).
	void LogEventChannelStartup(); LogEventChannelStartup();

	// One startup pass over the file: unknown [Platform]/[Groups]/[Features]
	// entries and unknown keys in a feature section are warned about, a
	// user-defined section is only Debug-logged, and the summary line reports the
	// registry state.  See docs/Ini.md.
	//
	// Both switches are read *before* the scan/summary so an invalid value is part
	// of the validated, counted set the summary reports (INI-03); the template
	// dump itself runs after the summary so it describes the final registry state.
	const bool warnUnknownKeys = ini.ReadBaseBool(VC_SECTION_PLATFORM, "IniWarnUnknownKeys", true,
		"warn about unknown <Platform>.ini keys at startup");
	const bool dumpIniTemplate = ini.ReadBaseBool(VC_SECTION_PLATFORM, "DumpIniTemplate", false,
		"development: write <Platform>.ini.generated next to the log");
	const IniScanResult iniScan = ini.ScanUnknownKeys(warnUnknownKeys);
	Log::Info("ini keys: %d registered, %d invalid, %d unknown",
		iniScan.registered, iniScan.invalid, iniScan.unknown);

	// INI-06: the Win32 profile API has fixed enumeration buffers; report when a
	// buffer was too small so an oversized file is never scanned silently.  Debug
	// level: the scan still used everything the API returned.
	const std::size_t enumerationLimits = Ini::EnumerationLimitCount();
	if (enumerationLimits > 0)
	{
		Log::Debug("ini     : %zu enumeration limit(s) hit - a section list, a section body or a key name exceeds a Win32 profile API buffer; the unknown-key scan may be incomplete (see docs/Ini.md)", enumerationLimits);
	}

	if (dumpIniTemplate)
	{
		DumpIniTemplate();
	}

	// START-07: every rejected registration is reported: the static-phase ones
	// were queued until the sink existed and flushed above, later ones went to the
	// sink directly.  The total also counts a problem the bounded startup queue
	// had to drop (the drain reports that as one summary line).
	const std::size_t extTypeProblems = ExtTypeProblemCount();
	if (extTypeProblems != 0)
	{
		Log::Error(
			"ext types: %zu registration problem(s) - the rejected types are reported above "
			"(a static-initialization problem is queued until the logger is open, then flushed; "
			"a startup-queue overflow is reported as one summary line) and cannot be fetched "
			"(a wrong TypeId/BaseTypeId pair in an extension class is a composition error)",
			extTypeProblems);
	}

	Log::Info("%s startup complete - %d feature(s) registered", VC_PLATFORM_NAME, FeatureCount());
	Log::Info("================================================================");

	PlatformGate::MarkRunning();
	return true;
}

void Shutdown()
{
	if (!IsStarted())
		return;

	// Teardown entry point for a derived platform (the base itself never calls
	// it).  It releases the engine-visible state and closes the log; because the
	// feature registries are not restartable (InitializeFeatures is once-only),
	// the gate becomes Finished instead of Pending, so a later Startup() returns
	// false rather than initializing everything a second time (START-04).
	Log::Info("%s shutting down", VC_PLATFORM_NAME);
	ExtensionIndex::Instance().ClearAll();
	Log::Close();
	PlatformGate::MarkFinished();
}

void OnScenarioClear()
{
	if (!IsStarted())
		return;

	// A failed savegame load returns from the engine's LoadGame before its normal
	// close point (A-04).  The three failure-exit hooks close the load window, but
	// a scenario change is the last line of defence: never carry an open window or
	// the deferred records of an aborted load into the next scenario.
	//
	// The engine also clears the scenario *inside* a normal savegame load: the
	// object-state reader (0x67E730) calls ResetScenario (0x6851F0) as its first
	// step (0x67E739), after our load window was opened at 0x67E44D.  Resetting the
	// window there would make every object the load constructs allocate its
	// extension immediately, so the trailer's saved rows could no longer be adopted
	// (G1 E1 real game, 2026-11-10: "0 adopted / 44 failed").  While a load is in
	// progress the window therefore stays open; the hooks are the actual A-04 fix.
	if (ExtensionIndex::Instance().IsLoadingSaveGame())
		Log::Info("scenario clear: inside a savegame load - keeping the load window open");
	else
		ExtensionIndex::Instance().ResetForScenarioClear();
	ExtensionIndex::Instance().ClearAll();
	Log::Info("scenario clear: extension state reset (%zu store(s) registered)",
		ExtensionIndex::Instance().StoreCount());
}

void OnInvalidatePointer(AbstractClass* const pInvalid, const bool removed)
{
	if (!IsStarted())
		return;

	ExtensionIndex::Instance().NotifyInvalidPointer(pInvalid, removed);
}

bool IsStarted()
{
	return PlatformGate::IsRunning();
}

unsigned HostMask()
{
	return g_hostMask;
}
}
