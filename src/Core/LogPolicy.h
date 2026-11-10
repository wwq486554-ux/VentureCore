#pragma once

#include <cstdint>

// ---------------------------------------------------------------------------
// Pure log policy: level parsing, level filtering and sampling.
//
// Engine-free and header-only on purpose: Log itself is compiled into the DLL,
// but these rules are ordinary arithmetic, so tests/test_log_levels.cpp covers
// every branch without a game process (and without linking Log.cpp).
//
// See docs/Logging.md for the INI keys and the rationale.
// ---------------------------------------------------------------------------

namespace venture
{
enum class LogLevel : std::uint8_t
{
	Debug = 0,
	Info = 1,
	Warn = 2,
	Error = 3,
};

namespace log_detail
{
inline char LowerAscii(const char character)
{
	return (character >= 'A' && character <= 'Z')
		? static_cast<char>(character - 'A' + 'a')
		: character;
}

inline bool EqualsIgnoreCase(const char* const left, const char* const right)
{
	if (!left || !right)
		return left == right;

	const char* a = left;
	const char* b = right;
	while (*a && *b)
	{
		if (LowerAscii(*a) != LowerAscii(*b))
			return false;
		++a;
		++b;
	}
	return *a == '\0' && *b == '\0';
}
}

// Parses a [Platform] LogLevel value.
//
// Accepted forms (case-insensitive):
//   error | warn | info | debug
//   numeric 0-3               (0 = debug, 1 = info, 2 = warn, 3 = error)
//
// Returns false and leaves `out` untouched for nullptr, an empty string and any
// other value; the caller keeps its default.  `Ini::GetString` already trims the
// value read from the file, so no whitespace handling is done here.
inline bool ParseLogLevel(const char* const text, LogLevel& out)
{
	if (!text || text[0] == '\0')
		return false;

	if (log_detail::EqualsIgnoreCase(text, "debug")) { out = LogLevel::Debug; return true; }
	if (log_detail::EqualsIgnoreCase(text, "info"))  { out = LogLevel::Info;  return true; }
	if (log_detail::EqualsIgnoreCase(text, "warn"))  { out = LogLevel::Warn;  return true; }
	if (log_detail::EqualsIgnoreCase(text, "error")) { out = LogLevel::Error; return true; }

	if (text[0] >= '0' && text[0] <= '3' && text[1] == '\0')
	{
		out = static_cast<LogLevel>(text[0] - '0');
		return true;
	}

	return false;
}

// Lower-case name used by the startup log ("level info").
inline const char* LogLevelName(const LogLevel level)
{
	switch (level)
	{
	case LogLevel::Debug: return "debug";
	case LogLevel::Info:  return "info";
	case LogLevel::Warn:  return "warn";
	case LogLevel::Error: return "error";
	default:              return "?";
	}
}

// True when a message at `level` passes the `minLevel` filter: the logger keeps
// the configured level and everything more severe.
inline bool ShouldLog(const LogLevel minLevel, const LogLevel level)
{
	return static_cast<unsigned>(level) >= static_cast<unsigned>(minLevel);
}

// Sampling helper for high-frequency diagnostics:
//
//   if (ShouldLogSample(counter, kEvery)) Log::Info(...);
//
// true for every `every`-th counter value (counter = 1, 2, ... -> every=100
// reports at 100, 200, 300, ...).  A counter of 0 also reports, so an unsigned
// counter wrapping around does not lose the slot.
//
// `every == 0` disables throttling: every call reports, i.e. the sampling
// interval is switched off rather than the logging.
inline bool ShouldLogSample(const unsigned counter, const unsigned every)
{
	return every == 0 || (counter % every) == 0;
}
}
