#include "Core/Log.h"

#include "Core/PlatformConfig.h"
#include "Core/PlatformPaths.h"
#include "Core/PlatformVersion.h"

#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace
{
HANDLE g_file = INVALID_HANDLE_VALUE;
venture::LogLevel g_minLevel = venture::LogLevel::Info;
bool g_debugOutput = false;

// A failing file write is reported to the (gated) debugger helper at most once
// per process: a full disk would otherwise emit one diagnostic per log line,
// which is worse than the dropped lines it reports.
bool g_writeFailureReported = false;

// START-01: Warn/Error lines written before the log file is open (the command and
// event registrars run as file-static initializers) are queued here and flushed
// by Open()/OpenAt().  The storage is fixed and trivially initialisable, so it is
// zero-initialised before any dynamic initializer runs - no static-initialisation
// order dependency, no allocation, no exceptions.  The queue is deliberately small
// (a handful of startup problems); overflow is counted and reported once at flush.
constexpr int kPendingCapacity = 8;
constexpr std::size_t kPendingLineSize = 256;
char g_pendingLines[kPendingCapacity][kPendingLineSize] = {};
int g_pendingCount = 0;
int g_pendingDropped = 0;

const char* LevelTag(const venture::LogLevel level)
{
	switch (level)
	{
	case venture::LogLevel::Debug: return "DEBUG";
	case venture::LogLevel::Info:  return "INFO ";
	case venture::LogLevel::Warn:  return "WARN ";
	case venture::LogLevel::Error: return "ERROR";
	default:                       return "?????";
	}
}

// The one place that talks to the debugger.  A RELEASE build keeps it silent
// unless [Platform] DebugOutput=yes was read from the INI; a DEBUG build always
// writes (see docs/Logging.md and scripts/verify_build.py's source check).
void EmitDebugOutput(const char* const text)
{
#ifdef DEBUG
	OutputDebugStringA(text);
#else
	if (g_debugOutput)
		OutputDebugStringA(text);
#endif
}

void WriteLineToFile(const char* const line)
{
	if (g_file == INVALID_HANDLE_VALUE || !line)
		return;

	const DWORD length = static_cast<DWORD>(std::strlen(line));
	DWORD written = 0;
	const BOOL ok = WriteFile(g_file, line, length, &written, nullptr);
	if (ok && written == length)
		return;

	// The line is dropped; there is nothing useful to do with a failed append.
	// Degrade to the gated debugger helper once so a persistent failure (e.g. a
	// full disk) is visible to a developer without spamming a line per message.
	if (!g_writeFailureReported)
	{
		g_writeFailureReported = true;
		EmitDebugOutput("[VentureCore] cannot write the log file; the line is dropped\n");
	}
}

// START-01: bounded, allocation-free queue for diagnostics logged before Open().
void QueuePendingDiagnostic(const char* const line)
{
	if (!line)
		return;

	if (g_pendingCount >= kPendingCapacity)
	{
		++g_pendingDropped;
		return;
	}

	_snprintf_s(g_pendingLines[g_pendingCount], kPendingLineSize, _TRUNCATE, "%s", line);
	++g_pendingCount;
}

// START-01: called by Open()/OpenAt() right after the separator header, so the
// regular file logging starts with whatever the static initializers reported.
void FlushPendingDiagnostics()
{
	for (int index = 0; index < g_pendingCount; ++index)
		WriteLineToFile(g_pendingLines[index]);
	g_pendingCount = 0;

	if (g_pendingDropped > 0)
	{
		char line[160] = {};
		_snprintf_s(
			line,
			sizeof(line),
			_TRUNCATE,
			"[VentureCore] %d startup diagnostic(s) dropped - the pre-open log buffer is full\r\n",
			g_pendingDropped);
		g_pendingDropped = 0;
		WriteLineToFile(line);
	}
}

// First line of every process: one stable separator the user can grep for.
// Written straight to the file, independent of the level filter, so the start of
// a run is always visible even when the configured level suppresses INFO lines.
void WriteSeparatorHeader()
{
	SYSTEMTIME time = {};
	GetLocalTime(&time);

	char line[512] = {};
	_snprintf_s(
		line,
		sizeof(line),
		_TRUNCATE,
		"=== %s %s (%s) process %lu started %04u-%02u-%02u %02u:%02u:%02u ===\r\n",
		VC_PLATFORM_NAME,
		VC_VERSION_DISPLAY,
		VC_BUILD_TYPE,
		GetCurrentProcessId(),
		time.wYear,
		time.wMonth,
		time.wDay,
		time.wHour,
		time.wMinute,
		time.wSecond);

	WriteLineToFile(line);
}

void WriteV(const venture::LogLevel level, const char* const format, va_list args)
{
	// A NULL format would reach _vsnprintf_s, whose invalid-parameter handler
	// terminates the process by default; the public logging API promises the
	// logger cannot crash the game, so drop the line instead.  (Callers must not
	// pass NULL - see Log.h - but a defensive check keeps the promise true.)
	if (!format)
		return;

	if (!venture::ShouldLog(g_minLevel, level))
		return;

	char message[2048] = {};
	_vsnprintf_s(message, sizeof(message), _TRUNCATE, format, args);

	SYSTEMTIME time = {};
	GetLocalTime(&time);

	char line[2304] = {};
	_snprintf_s(
		line,
		sizeof(line),
		_TRUNCATE,
		"[%02u:%02u:%02u.%03u] [%s] %s\r\n",
		time.wHour,
		time.wMinute,
		time.wSecond,
		time.wMilliseconds,
		LevelTag(level),
		message);

	// Before Open() there is no file handle: queue a startup problem (Warn/Error)
	// for the flush that Open()/OpenAt() performs, so a rejected registration from
	// a static initializer is not lost (START-01).  Info/Debug lines only describe
	// the normal path and are not queued.
	if (g_file == INVALID_HANDLE_VALUE)
	{
		if (level == venture::LogLevel::Warn || level == venture::LogLevel::Error)
			QueuePendingDiagnostic(line);
	}
	else
	{
		WriteLineToFile(line);
	}

	EmitDebugOutput(line);
}
}

namespace venture
{
bool Log::Open()
{
	if (g_file != INVALID_HANDLE_VALUE)
		return true;

	char path[MAX_PATH * 2] = {};
	if (!GetGameFilePath(VC_PLATFORM_LOG_NAME, path, static_cast<DWORD>(sizeof(path))))
	{
		EmitDebugOutput("[VentureCore] cannot resolve the game directory; file logging disabled\n");
		return false;
	}

	return OpenAt(path);
}

bool Log::OpenAt(const char* const path)
{
	if (g_file != INVALID_HANDLE_VALUE)
		return true;

	if (!path || path[0] == '\0')
	{
		// CreateFileA(NULL, ...) is not diagnosed here; reject it explicitly.
		EmitDebugOutput("[VentureCore] cannot open the log file (no path); file logging disabled\n");
		return false;
	}

	g_file = CreateFileA(
		path,
		FILE_APPEND_DATA,
		FILE_SHARE_READ | FILE_SHARE_WRITE,
		nullptr,
		OPEN_ALWAYS,
		FILE_ATTRIBUTE_NORMAL,
		nullptr);

	if (g_file == INVALID_HANDLE_VALUE)
	{
		EmitDebugOutput("[VentureCore] cannot open the log file; file logging disabled\n");
		return false;
	}

	WriteSeparatorHeader();
	FlushPendingDiagnostics();
	return true;
}

void Log::Close()
{
	if (g_file != INVALID_HANDLE_VALUE)
	{
		CloseHandle(g_file);
		g_file = INVALID_HANDLE_VALUE;
	}

	// The base closes the log only at shutdown, after Open() at startup already
	// flushed the queue; drop whatever a (future) caller queued for a log that is
	// not going to be opened any more so a later Open() cannot resurrect stale
	// lines from a previous lifecycle.
	g_pendingCount = 0;
	g_pendingDropped = 0;
}

bool Log::IsOpen()
{
	return g_file != INVALID_HANDLE_VALUE;
}

void Log::SetMinLevel(const LogLevel level)
{
	g_minLevel = level;
}

LogLevel Log::MinLevel()
{
	return g_minLevel;
}

void Log::SetDebugOutput(const bool enabled)
{
	g_debugOutput = enabled;
}

int Log::PendingCount()
{
	return g_pendingCount;
}

void Log::DiscardPending()
{
	g_pendingCount = 0;
	g_pendingDropped = 0;
}

void Log::Write(const LogLevel level, const char* const format, ...)
{
	va_list args;
	va_start(args, format);
	WriteV(level, format, args);
	va_end(args);
}

void Log::Debug(const char* const format, ...)
{
	va_list args;
	va_start(args, format);
	WriteV(LogLevel::Debug, format, args);
	va_end(args);
}

void Log::Info(const char* const format, ...)
{
	va_list args;
	va_start(args, format);
	WriteV(LogLevel::Info, format, args);
	va_end(args);
}

void Log::Warn(const char* const format, ...)
{
	va_list args;
	va_start(args, format);
	WriteV(LogLevel::Warn, format, args);
	va_end(args);
}

void Log::Error(const char* const format, ...)
{
	va_list args;
	va_start(args, format);
	WriteV(LogLevel::Error, format, args);
	va_end(args);
}
}
