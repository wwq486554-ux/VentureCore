// Unit test: the compiled logger - file I/O around the pure level policy.
//
// tests/test_log_levels.cpp covers the header-only policy in src/Core/LogPolicy.h;
// this test drives the real logger (src/Core/Log.cpp): Log::OpenAt on a file
// under %TEMP%, the per-process separator header, level filtering, the
// `[HH:MM:SS.mmm] [LEVEL] message` line format, a repeated Open (no duplicated
// separator), Close idempotence, a failed open, NULL/`%` format strings and
// Log::Open() (the game-directory path).  The explicit OpenAt path is the seam
// that makes the logger testable without a game process; it is public because a
// derived platform can use it too.
//
// TEST_DEPS: src/Core/Log.cpp src/Core/PlatformPaths.cpp

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>

#include "Core/Log.h"
#include "Core/PlatformConfig.h"
#include "Core/PlatformPaths.h"
#include "Core/PlatformVersion.h"

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

bool StartsWith(const std::string& text, const char* const prefix)
{
	const std::size_t length = std::strlen(prefix);
	return text.size() >= length && text.compare(0, length, prefix) == 0;
}

bool EndsWith(const std::string& text, const char* const suffix)
{
	const std::size_t length = std::strlen(suffix);
	return text.size() >= length && text.compare(text.size() - length, length, suffix) == 0;
}

bool ContainsLine(const std::vector<std::string>& lines, const char* const needle)
{
	for (const std::string& line : lines)
	{
		if (Contains(line, needle))
			return true;
	}
	return false;
}

const std::string* FindLineContaining(const std::vector<std::string>& lines, const char* const needle)
{
	for (const std::string& line : lines)
	{
		if (Contains(line, needle))
			return &line;
	}
	return nullptr;
}

// Reads a whole file through the Win32 API.  The logger holds its write handle
// with FILE_APPEND_DATA; the test always closes the logger before reading.
bool ReadWholeFile(const char* const path, std::string& contents)
{
	contents.clear();
	HANDLE file = CreateFileA(
		path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE)
		return false;

	char buffer[4096] = {};
	DWORD read = 0;
	while (ReadFile(file, buffer, static_cast<DWORD>(sizeof(buffer)), &read, nullptr) && read > 0)
		contents.append(buffer, static_cast<std::size_t>(read));
	CloseHandle(file);
	return true;
}

// Splits on CRLF; a trailing CRLF does not produce an empty last line.
std::vector<std::string> SplitLines(const std::string& text)
{
	std::vector<std::string> lines;
	std::size_t start = 0;
	while (start < text.size())
	{
		const std::size_t end = text.find("\r\n", start);
		if (end == std::string::npos)
		{
			lines.push_back(text.substr(start));
			break;
		}
		lines.push_back(text.substr(start, end - start));
		start = end + 2;
	}
	return lines;
}

unsigned CountSeparatorLines(const std::vector<std::string>& lines)
{
	unsigned count = 0;
	for (const std::string& line : lines)
	{
		if (StartsWith(line, "=== ") && EndsWith(line, " ==="))
			++count;
	}
	return count;
}

// Shape of one formatted line: `[HH:MM:SS.mmm] [LEVEL] message`, with the fixed
// five-column level tag (`DEBUG`, `INFO `, `WARN `, `ERROR`).
bool LooksLikeLogLine(const std::string& line, const char* const tag, const char* const message)
{
	const std::size_t messageLength = std::strlen(message);
	if (line.size() != 23u + messageLength)
		return false;
	if (line[0] != '[' || line[3] != ':' || line[6] != ':' || line[9] != '.'
		|| line[13] != ']' || line[14] != ' ' || line[15] != '[' || line[21] != ']'
		|| line[22] != ' ')
		return false;

	static const char kDigitPositions[] = { 1, 2, 4, 5, 7, 8, 10, 11, 12 };
	for (const char position : kDigitPositions)
	{
		const char character = line[static_cast<std::size_t>(position)];
		if (character < '0' || character > '9')
			return false;
	}

	if (line.compare(16, 5, tag) != 0)
		return false;
	return line.compare(23, messageLength, message) == 0;
}

bool TempDirectory(char* const buffer, const DWORD size)
{
	const DWORD length = GetTempPathA(size, buffer);
	return length > 0 && length < size;
}

bool TempLogPath(char* const buffer, const DWORD size, const char* const name)
{
	char directory[MAX_PATH] = {};
	if (!TempDirectory(directory, static_cast<DWORD>(sizeof(directory))))
		return false;
	const int written = std::snprintf(
		buffer, static_cast<std::size_t>(size), "%s%s-%lu.log", directory, name,
		GetCurrentProcessId());
	return written > 0 && static_cast<DWORD>(written) < size;
}
}

int __cdecl main()
{
	using venture::Log;
	using venture::LogLevel;

	// -- setup ---------------------------------------------------------------
	char logPath[MAX_PATH * 2] = {};
	Check(TempLogPath(logPath, static_cast<DWORD>(sizeof(logPath)), "venturecore-test-logging"),
		"resolve the %TEMP% log path");
	DeleteFileA(logPath);

	Log::Close();
	Log::SetMinLevel(LogLevel::Info);
	Log::SetDebugOutput(false);
	Check(!Log::IsOpen(), "the logger starts closed");

	// -- 1. OpenAt, separator header, repeated Open, Close -------------------
	Check(Log::OpenAt(logPath), "Log::OpenAt(%TEMP% file) returns true");
	Check(Log::IsOpen(), "IsOpen() is true after OpenAt");
	Check(Log::OpenAt(logPath), "a repeated OpenAt while open returns true");
	Check(Log::OpenAt(logPath), "a second repeated OpenAt while open returns true");
	Log::Close();
	Check(!Log::IsOpen(), "Close() closes the file");
	Log::Close();
	Check(!Log::IsOpen(), "a repeated Close() is a no-op");

	std::string contents;
	Check(ReadWholeFile(logPath, contents), "the log file exists and can be read");
	std::vector<std::string> lines = SplitLines(contents);
	Check(lines.size() == 1u, "the fresh log file contains exactly one line");
	if (!lines.empty())
	{
		const std::string& header = lines[0];
		Check(StartsWith(header, "=== "), "the first line is the separator header");
		Check(Contains(header, VC_PLATFORM_NAME), "the separator carries the platform name");
		Check(Contains(header, VC_VERSION_DISPLAY), "the separator carries the version display");
		Check(Contains(header, VC_BUILD_TYPE), "the separator carries the build type");
		Check(Contains(header, "process "), "the separator carries the process id");
		Check(Contains(header, "started "), "the separator carries the start date/time");
		Check(EndsWith(header, " ==="), "the separator header ends with ' ==='");
	}
	Check(CountSeparatorLines(lines) == 1u, "a repeated OpenAt did not duplicate the separator");

	// -- 2. level filtering and line format ----------------------------------
	Log::SetMinLevel(LogLevel::Debug);
	Check(Log::OpenAt(logPath), "reopening the existing file appends to it");
	Log::Debug("visible debug %d", 1);
	Log::Info("visible info %s", "text");
	Log::Warn("visible warn %u", 2u);
	Log::Error("visible error");
	Log::Write(LogLevel::Info, "visible write %d", 3);
	Log::Info("%s", "pre-formatted string (the FeatureContext::Log shape)");
	Log::Info("percent literal 100%% and number %d", 7);
	Log::Info("format matrix %d %u %s %x", -1, 42u, "str", 0xBEEF);

	// Platform.cpp's FeatureLog formats into its own 1024-byte buffer with
	// _vsnprintf_s and forwards it as Log::Info("%s", message); reproduce that
	// hand-off (same truncating MSVC formatter, variadic form) with a
	// percent-bearing message longer than the feature buffer.
	std::string longText(1500, 'x');
	longText[0] = '%';
	char featureMessage[1024] = {};
	_snprintf_s(featureMessage, sizeof(featureMessage), _TRUNCATE, "feature: %s", longText.c_str());
	Log::Info("%s", featureMessage);

	Log::SetMinLevel(LogLevel::Warn);
	Log::Debug("hidden debug");
	Log::Info("hidden info");
	Log::Warn("kept warn");
	Log::Error("kept error");
	Log::Close();

	Check(ReadWholeFile(logPath, contents), "the log file can be read after the second run");
	lines = SplitLines(contents);
	const std::size_t lineCountAfterSecondRun = lines.size();
	Check(CountSeparatorLines(lines) == 2u, "a real reopen appends exactly one new separator");
	Check(ContainsLine(lines, "visible debug 1"), "Log::Debug writes at LogLevel=debug");
	Check(ContainsLine(lines, "visible info text"), "Log::Info writes at LogLevel=debug");
	Check(ContainsLine(lines, "visible warn 2"), "Log::Warn writes at LogLevel=debug");
	Check(ContainsLine(lines, "visible error"), "Log::Error writes at LogLevel=debug");
	Check(ContainsLine(lines, "visible write 3"), "Log::Write writes at LogLevel=debug");
	Check(ContainsLine(lines, "pre-formatted string"), "the '%s' hand-off keeps the message");
	Check(ContainsLine(lines, "percent literal 100% and number 7"), "literal '%%' stays a single '%'");
	Check(ContainsLine(lines, "format matrix -1 42 str beef"), "printf formatting is applied");
	Check(ContainsLine(lines, "feature: %"), "the FeatureLog-shaped hand-off survives truncation");
	Check(!ContainsLine(lines, "hidden debug"), "LogLevel=warn filters out debug lines");
	Check(!ContainsLine(lines, "hidden info"), "LogLevel=warn filters out info lines");
	Check(ContainsLine(lines, "kept warn"), "LogLevel=warn keeps warn lines");
	Check(ContainsLine(lines, "kept error"), "LogLevel=warn keeps error lines");

	const std::string* infoLine = FindLineContaining(lines, "visible info text");
	Check(infoLine != nullptr && LooksLikeLogLine(*infoLine, "INFO ", "visible info text"),
		"INFO line format [HH:MM:SS.mmm] [INFO ] message");
	const std::string* debugLine = FindLineContaining(lines, "visible debug 1");
	Check(debugLine != nullptr && LooksLikeLogLine(*debugLine, "DEBUG", "visible debug 1"),
		"DEBUG line format [HH:MM:SS.mmm] [DEBUG] message");
	const std::string* warnLine = FindLineContaining(lines, "visible warn 2");
	Check(warnLine != nullptr && LooksLikeLogLine(*warnLine, "WARN ", "visible warn 2"),
		"WARN line format [HH:MM:SS.mmm] [WARN ] message");
	const std::string* errorLine = FindLineContaining(lines, "kept error");
	Check(errorLine != nullptr && LooksLikeLogLine(*errorLine, "ERROR", "kept error"),
		"ERROR line format [HH:MM:SS.mmm] [ERROR] message");

	// -- 3. failed open: returns false, does not crash -----------------------
	char tempDirectory[MAX_PATH] = {};
	Check(TempDirectory(tempDirectory, static_cast<DWORD>(sizeof(tempDirectory))),
		"resolve %TEMP% for the failure case");
	char missingPath[MAX_PATH * 2] = {};
	std::snprintf(missingPath, sizeof(missingPath), "%sventurecore-missing-%lu\\VentureCore.log",
		tempDirectory, GetCurrentProcessId());
	Check(!Log::OpenAt(missingPath), "OpenAt in a missing directory returns false");
	Check(!Log::IsOpen(), "a failed OpenAt leaves the logger closed");
	Log::Info("logging without an open file does not crash");
	Log::Error("logging without an open file does not crash (error)");
	Log::Close();
	Check(!Log::IsOpen(), "Close() after a failed open is a no-op");
	Check(!Log::OpenAt(nullptr), "OpenAt(nullptr) is rejected without crashing");
	Check(!Log::OpenAt(""), "OpenAt(\"\") is rejected without crashing");
	Check(!Log::IsOpen(), "the rejected paths leave the logger closed");

	// -- 4. NULL format strings are dropped, not passed to the CRT -----------
	Log::SetMinLevel(LogLevel::Debug);
	Check(Log::OpenAt(logPath), "reopen for the NULL-format cases");
	Log::Debug(nullptr);
	Log::Info(nullptr);
	Log::Warn(nullptr);
	Log::Error(nullptr);
	Log::Write(LogLevel::Info, nullptr);
	Log::Info("alive after the NULL-format calls");
	Log::Close();

	Check(ReadWholeFile(logPath, contents), "the log file can be read after the NULL-format run");
	lines = SplitLines(contents);
	Check(CountSeparatorLines(lines) == 3u, "the NULL-format run appended one separator");
	Check(ContainsLine(lines, "alive after the NULL-format calls"), "the logger works after a NULL format");
	Check(lines.size() == lineCountAfterSecondRun + 2u,
		"NULL-format calls wrote no lines (only the separator and the alive line were added)");

	// -- 5. Log::Open() resolves the game directory (the executable's dir) ----
	char gameLogPath[MAX_PATH * 2] = {};
	const bool haveGamePath = venture::GetGameFilePath(
		VC_PLATFORM_LOG_NAME, gameLogPath, static_cast<DWORD>(sizeof(gameLogPath)));
	Check(haveGamePath, "resolve the game-directory log path");
	DeleteFileA(gameLogPath);

	// The separator is written regardless of the level filter.
	Log::SetMinLevel(LogLevel::Error);
	Check(Log::Open(), "Log::Open() opens <Platform>.log in the game directory");
	Check(Log::IsOpen(), "IsOpen() is true after Log::Open()");
	Log::SetMinLevel(LogLevel::Info);
	Log::Info("open path test");
	Log::Close();

	Check(ReadWholeFile(gameLogPath, contents), "the game-directory log file exists");
	lines = SplitLines(contents);
	Check(CountSeparatorLines(lines) == 1u,
		"Log::Open() wrote the separator header (independent of the level filter)");
	Check(!lines.empty() && StartsWith(lines[0], "=== "),
		"the first line of the game-directory log is the separator");
	Check(ContainsLine(lines, "open path test"), "Log::Open() path writes lines");
	DeleteFileA(gameLogPath);

	// -- cleanup -------------------------------------------------------------
	DeleteFileA(logPath);

	std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAILED",
		g_failures, g_failures == 1 ? "" : "s");
	return g_failures == 0 ? 0 : 1;
}
