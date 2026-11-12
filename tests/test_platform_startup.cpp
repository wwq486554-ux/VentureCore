// Integration test: the production startup path of src/Core/Platform.cpp.
//
// TEST_DEPS: src/Core/Platform.cpp src/Core/PlatformPaths.cpp src/Core/Log.cpp src/Core/Ini.cpp src/Core/IniRegistry.cpp src/Core/FeatureRegistry.cpp src/Core/HostRegistry.cpp src/Core/Hosts.Builtin.cpp src/Core/Hosts.Custom.cpp src/Core/IniTakeover.cpp src/Core/Lineage.cpp src/Ext/AbstractExt.cpp src/Ext/ExtensionIndex.cpp
//
// Why this test exists
// --------------------
// TOOL-13 (2.1 review): Platform.cpp - the translation unit that starts the
// whole platform - was compiled only into the DLL, so no test ever compiled or
// ran it.  Its startup path is deliberately free of engine types (it uses
// <Syringe.h> for the feature flags and the platform's own engine-free Core/Ext
// modules), so it can be linked into a console test and executed for real:
//
//   Startup() -> gate, SyringeEx check, log, <Platform>.ini, host detection,
//                lineage scan, extension/problem sinks, feature registry,
//                unknown-key audit, "startup complete", gate Running
//   Shutdown()/Startup() lifecycle and the four-state gate.
//
// What the test does NOT exercise: the engine-facing hooks (Engine/Hooks.*.cpp,
// load_probe.sh / probe_hosts.sh territory), a real `SyringeEx` injection and a
// second DLL of the lineage (load_probe.sh loads two copies).  The event-channel
// startup log lives in the engine-side src/Engine/Hooks.Events.cpp; this test
// provides the link symbol as a stub so Platform.cpp links without the engine
// half.
//
// The INI and the log are written next to the test executable - in production
// that directory is the game directory (the platform resolves both through
// GetGameFilePath); here it is Release/IntDir/linux/tests and the test removes
// both files again.
//
// Link inputs: Platform.cpp itself needs <Syringe.h> from external/YRpp (a plain
// header without engine classes - the DLL cannot be built without the YRpp tree
// either), but this test does not include a YRpp header itself, so it still
// needs no engine ABI types and no gamemd.exe.
//
// main is `int __cdecl main(void)` because the runner compiles tests with /Gz
// (stdcall is the default), exactly like the platform DLL.

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>

#include <windows.h>

#include "Core/FeatureRegistry.h"
#include "Core/HostRegistry.h"
#include "Core/Ini.h"
#include "Core/Log.h"
#include "Core/Platform.h"
#include "Core/PlatformConfig.h"
#include "Core/PlatformGate.h"
#include "Core/PlatformPaths.h"
#include "Core/PlatformVersion.h"

// SyringeEx sets these feature flags at injection time and RequireSyringeEx()
// aborts the process without them.  external/YRpp/Syringe.h declares them as
// `extern "C" __declspec(dllexport) inline bool` variables, and the Platform.cpp
// linked below is the translation unit that emits their definition, so declaring
// the C-linkage names here is enough - the test does not have to include the YRpp
// header (and must not grow an engine dependency for three bools).
extern "C"
{
	extern bool ESPModification;
	extern bool ZFPreservation;
	extern bool ReladdrInstructionFixup;
}

// Link seam for src/Core/Platform.cpp: Startup() calls the event-channel startup
// log, which is defined (engine-facing, needs YRpp) in src/Engine/Hooks.Events.cpp.
// The event channel itself is covered by tests/test_event_channel.cpp; this test
// only has to satisfy the symbol so the real Platform.cpp links into the test.
// Kept in the global namespace: Platform.cpp declares it at block scope, which
// binds to the global namespace.
void LogEventChannelStartup()
{
}

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

bool Contains(const std::string& text, const char* const needle)
{
	return text.find(needle) != std::string::npos;
}

int CountOccurrences(const std::string& text, const char* const needle)
{
	const std::size_t length = std::strlen(needle);
	if (length == 0)
		return 0;

	int count = 0;
	for (std::size_t position = text.find(needle); position != std::string::npos;
		position = text.find(needle, position + length))
	{
		++count;
	}
	return count;
}

// Resolves <game directory>/<fileName> through the production path helper: in
// this test the executable's directory stands in for the game directory.
bool GamePath(const char* const fileName, char* const buffer, const std::size_t bufferSize)
{
	return venture::GetGameFilePath(fileName, buffer, static_cast<DWORD>(bufferSize));
}

bool WriteWholeFile(const char* const path, const char* const contents)
{
	FILE* file = nullptr;
	if (fopen_s(&file, path, "wb") != 0 || !file)
		return false;

	const std::size_t length = std::strlen(contents);
	const bool ok = std::fwrite(contents, 1, length, file) == length;
	std::fclose(file);
	return ok;
}

bool ReadWholeFile(const char* const path, std::string& contents)
{
	contents.clear();
	FILE* file = nullptr;
	if (fopen_s(&file, path, "rb") != 0 || !file)
		return false;

	char buffer[4096] = {};
	std::size_t read = 0;
	while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
		contents.append(buffer, read);
	std::fclose(file);
	return true;
}

void RemoveFile(const char* const path)
{
	if (path && path[0] != '\0')
		std::remove(path);
}
}

int __cdecl main(void)
{
	using venture::PlatformGate;

	// -- paths ---------------------------------------------------------------
	char iniPath[MAX_PATH * 2] = {};
	char logPath[MAX_PATH * 2] = {};
	const bool havePaths =
		GamePath(VC_PLATFORM_INI_NAME, iniPath, sizeof(iniPath))
		&& GamePath(VC_PLATFORM_LOG_NAME, logPath, sizeof(logPath));
	Check(havePaths, "the executable directory resolves as the game directory");
	if (havePaths)
	{
		// A leftover file from an interrupted earlier run must not decide this
		// run; the test owns both files and writes/removes them itself.
		RemoveFile(iniPath);
		RemoveFile(logPath);
	}

	// -- gate before startup -------------------------------------------------
	Check(PlatformGate::IsPending(), "the startup gate is Pending before Startup()");
	Check(!venture::IsStarted(), "IsStarted() is false before Startup()");
	Check(venture::HostMask() == 0, "HostMask() is empty before Startup()");

	// -- a real <Platform>.ini next to the executable -----------------------
	// Exercises Ini::Load/Exists and the IniRegistry reads inside Startup(); the
	// values are valid and change observable behaviour (LogLevel=debug).
	if (havePaths)
	{
		const char* const ini =
			"[Platform]\r\n"
			"LogLevel=debug\r\n"
			"VerboseLogging=no\r\n"
			"DebugOutput=no\r\n"
			"IniWarnUnknownKeys=no\r\n";
		Check(WriteWholeFile(iniPath, ini), "wrote <Platform>.ini next to the test executable");
	}

	// SyringeEx sets these flags at injection time.  RequireSyringeEx() would
	// show a message box and terminate the process without them, so a plain
	// console process has to set them directly.
	ESPModification = true;
	ZFPreservation = true;
	ReladdrInstructionFixup = true;

	// -- startup -------------------------------------------------------------
	Check(venture::Startup(), "Startup() succeeds with a valid INI and no foreign module");
	Check(venture::IsStarted(), "IsStarted() is true after Startup()");
	Check(PlatformGate::IsRunning(), "the startup gate is Running after Startup()");
	Check(venture::Log::IsOpen(), "the log file is open after Startup()");
	Check(venture::HostMask() == 0, "HostMask() is empty: no optional host is loaded in this process");
	Check(venture::Hosts().DefinitionCount() == 3,
		"Hosts.Builtin.cpp registered the three built-in host definitions");
	Check(venture::Hosts().DetectedCount() == 0, "no host is detected in a plain console process");
	Check(venture::Hosts().ProblemCount() == 0, "host registration and detection recorded no problem");
	Check(venture::FeatureCount() == 0, "no feature module is linked into this test");
	Check(venture::Ini::Exists(), "the INI written by the test is found by the platform");

	// A second Startup() while the gate is Running is a no-op that still
	// reports success (an ExeRun hook may fire more than once).
	Check(venture::Startup(), "a second Startup() while running is a no-op returning true");

	// -- shutdown ------------------------------------------------------------
	venture::Shutdown();
	Check(!venture::IsStarted(), "Shutdown() stops the platform");
	Check(PlatformGate::IsFinished(),
		"Shutdown() marks the gate Finished: the platform is not restartable (START-04)");
	Check(!venture::Log::IsOpen(), "Shutdown() closes the log");

	// -- log assertions (first startup block) --------------------------------
	// Read after Shutdown(): the logger's append handle is closed then, so the
	// file can be opened safely and every line written so far is flushed.
	std::string log;
	if (havePaths)
	{
		Check(ReadWholeFile(logPath, log), "the log file can be read after the first startup");
		Check(CountOccurrences(log, "startup complete - 0 feature(s) registered") == 1,
			"the first startup wrote exactly one 'startup complete' block");
		Check(Contains(log, VC_PLATFORM_NAME " " VC_VERSION_DISPLAY),
			"the startup block records the platform name and version");
		Check(Contains(log, "YRpp    : " VC_YRPP_PIN_SHORT " (" VC_YRPP_PIN ")"),
			"the startup block records the full YRpp pin and its display prefix");
		Check(Contains(log, "(found)"), "the startup block records that <Platform>.ini was found");
		Check(Contains(log, "log     : level debug"), "the INI LogLevel=debug was applied");
		Check(Contains(log, "hosts   : <none>"), "the startup block records that no host is present");
		Check(Contains(log, "lineage : no other " VC_PLATFORM_NAME "-lineage DLL loaded"),
			"the startup block records a clean lineage scan");
	}

	// -- a shutdown is final: no second startup (START-04) -------------------
	// Feature initialization is once-only, so Shutdown() moves the gate to
	// Finished and a later Startup() reports failure without re-initializing the
	// features or re-opening the log.
	Check(!venture::Startup(), "Startup() after Shutdown() returns false: the platform cannot restart");
	Check(!venture::IsStarted(), "the platform stays stopped after the refused restart");
	Check(PlatformGate::IsFinished(), "the gate stays Finished after the refused restart");
	if (havePaths)
	{
		Check(ReadWholeFile(logPath, log), "the log file can be read after the refused restart");
		Check(CountOccurrences(log, "startup complete - 0 feature(s) registered") == 1,
			"the refused restart did not write a second startup block");
	}

	// -- a refusal is final --------------------------------------------------
	PlatformGate::MarkRefused();
	Check(!venture::Startup(), "Startup() returns false after the gate was marked Refused");
	Check(PlatformGate::IsRefused(), "the refused gate stays Refused: a refusal is not retried");
	if (havePaths)
	{
		std::string afterRefusal;
		Check(ReadWholeFile(logPath, afterRefusal), "the log file can be read after the refused Startup()");
		Check(CountOccurrences(afterRefusal, "startup complete - 0 feature(s) registered") == 1,
			"the refused Startup() did not write another startup block");
	}
	PlatformGate::MarkPending();

	// -- cleanup -------------------------------------------------------------
	if (havePaths)
	{
		RemoveFile(iniPath);
		RemoveFile(logPath);
	}

	std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAILED",
		g_failures, g_failures == 1 ? "" : "s");
	return g_failures == 0 ? 0 : 1;
}
