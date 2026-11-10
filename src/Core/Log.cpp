#include "Core/Log.h"

#include "Core/PlatformConfig.h"
#include "Core/PlatformPaths.h"
#include "Core/PlatformVersion.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace
{
HANDLE g_file = INVALID_HANDLE_VALUE;
venture::LogLevel g_minLevel = venture::LogLevel::Info;
bool g_debugOutput = false;

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

	DWORD written = 0;
	WriteFile(g_file, line, static_cast<DWORD>(std::strlen(line)), &written, nullptr);
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

	WriteLineToFile(line);
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
	return true;
}

void Log::Close()
{
	if (g_file != INVALID_HANDLE_VALUE)
	{
		CloseHandle(g_file);
		g_file = INVALID_HANDLE_VALUE;
	}
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
