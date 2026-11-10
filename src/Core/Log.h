#pragma once

#include "Core/LogPolicy.h"

namespace venture
{
// Minimal append-only file logger. The log lives next to rules.ini in the game
// directory as <Platform>.log (see PlatformConfig.h). Logging must never crash
// the game: any file-system failure degrades to debugger output (gated, see
// below) and keeps the platform running; file writes never throw or exit.
//
// Every process start begins with a separator header line (version, build type,
// PID, local date/time). The lines below it keep their historic shape, so greps
// for "<Platform> ...", "hosts   :" or "startup complete" keep working.
//
// Debugger output is compiled in unconditionally only in DEBUG builds. A RELEASE
// build stays silent unless [Platform] DebugOutput=yes was configured - see
// docs/Logging.md. scripts/verify_build.py enforces that every call site lives
// inside the single helper in Log.cpp, so route new diagnostics through it.
class Log
{
public:
	static bool Open();
	static void Close();
	static bool IsOpen();

	static void SetMinLevel(LogLevel level);
	static LogLevel MinLevel();

	// Enables debugger output in a RELEASE build ([Platform] DebugOutput=yes).
	// DEBUG builds always write to the debugger and ignore this switch.
	static void SetDebugOutput(bool enabled);

	static void Write(LogLevel level, const char* format, ...);

	static void Debug(const char* format, ...);
	static void Info(const char* format, ...);
	static void Warn(const char* format, ...);
	static void Error(const char* format, ...);
};
}
