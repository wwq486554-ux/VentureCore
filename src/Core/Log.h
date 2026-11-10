#pragma once

#include "Core/LogPolicy.h"

namespace venture
{
// Minimal append-only file logger. The log lives next to rules.ini in the game
// directory as <Platform>.log (see PlatformConfig.h). Logging must never crash
// the game: failing to open the log file degrades to debugger output (gated, see
// below); failing to write a line reports once to that helper and drops the
// line. File writes never throw or exit.
//
// Every process start begins with a separator header line (version, build type,
// PID, local date/time). The lines below it keep their historic shape, so greps
// for "<Platform> ...", "hosts   :" or "startup complete" keep working.
//
// Debugger output is compiled in unconditionally only in DEBUG builds. A RELEASE
// build stays silent unless [Platform] DebugOutput=yes was configured - see
// docs/Logging.md. scripts/verify_build.py enforces that every call site lives
// inside the single helper in Log.cpp, so route new diagnostics through it.
//
// Every formatting entry point takes a printf-style `format` that must NOT be
// NULL: a NULL format is dropped before it reaches the C runtime (whose
// invalid-parameter handler would terminate the process), but callers should not
// rely on that - pass a valid format string. File I/O is covered by the unit
// test tests/test_logging.cpp; the pure level rules by tests/test_log_levels.cpp.
class Log
{
public:
	// Opens <Platform>.log in the game directory (next to the host executable).
	// Returns true when the logger is usable, false when the game directory
	// cannot be resolved or the file cannot be opened (logging then only reaches
	// the gated debugger helper).
	static bool Open();

	// Opens `path` as the append-only log file (one separator header). Public so
	// a derived platform - or a test - can route the log explicitly; Open()
	// resolves the game directory and calls this. Returns false for a NULL/empty
	// path or when the file cannot be opened, and leaves file logging disabled.
	static bool OpenAt(const char* path);

	static void Close();
	static bool IsOpen();

	static void SetMinLevel(LogLevel level);
	static LogLevel MinLevel();

	// Enables debugger output in a RELEASE build ([Platform] DebugOutput=yes).
	// DEBUG builds always write to the debugger and ignore this switch.
	static void SetDebugOutput(bool enabled);

	// `format` must not be NULL; it is a printf-style format string.  A line is
	// filtered against the configured minimum level before it is formatted, so
	// suppressed lines cost one comparison.
	static void Write(LogLevel level, const char* format, ...);

	static void Debug(const char* format, ...);
	static void Info(const char* format, ...);
	static void Warn(const char* format, ...);
	static void Error(const char* format, ...);
};
}
