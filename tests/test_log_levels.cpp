// Unit test: log policy - level parsing, level filtering and sampling.
//
// TEST_DEPS:
// (header-only test: the pure rules live in src/Core/LogPolicy.h, so this test
// links no non-test source.  The compiled logger around them - file I/O, the
// per-process separator header and the gated debugger output - is covered by
// tests/test_logging.cpp, which links src/Core/Log.cpp; scripts/verify_build.py
// only enforces where OutputDebugStringA may appear, it does not test log I/O.)

#include <cstdio>
#include <string>

#include "Core/LogPolicy.h"

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
}

int __cdecl main()
{
	using venture::LogLevel;
	using venture::LogLevelName;
	using venture::ParseLogLevel;
	using venture::ShouldLog;
	using venture::ShouldLogSample;

	// -- level enum order ----------------------------------------------------
	Check(static_cast<unsigned>(LogLevel::Debug) == 0u, "Debug is level 0");
	Check(static_cast<unsigned>(LogLevel::Info) == 1u, "Info is level 1");
	Check(static_cast<unsigned>(LogLevel::Warn) == 2u, "Warn is level 2");
	Check(static_cast<unsigned>(LogLevel::Error) == 3u, "Error is level 3");

	// -- parsing: names (case-insensitive) -----------------------------------
	LogLevel parsed = LogLevel::Error;

	Check(ParseLogLevel("debug", parsed) && parsed == LogLevel::Debug, "parse 'debug'");
	Check(ParseLogLevel("info", parsed) && parsed == LogLevel::Info, "parse 'info'");
	Check(ParseLogLevel("warn", parsed) && parsed == LogLevel::Warn, "parse 'warn'");
	Check(ParseLogLevel("error", parsed) && parsed == LogLevel::Error, "parse 'error'");
	Check(ParseLogLevel("DEBUG", parsed) && parsed == LogLevel::Debug, "parse is case-insensitive ('DEBUG')");
	Check(ParseLogLevel("Info", parsed) && parsed == LogLevel::Info, "parse is case-insensitive ('Info')");

	// -- parsing: numeric 0-3 ------------------------------------------------
	Check(ParseLogLevel("0", parsed) && parsed == LogLevel::Debug, "parse numeric 0 -> debug");
	Check(ParseLogLevel("1", parsed) && parsed == LogLevel::Info, "parse numeric 1 -> info");
	Check(ParseLogLevel("2", parsed) && parsed == LogLevel::Warn, "parse numeric 2 -> warn");
	Check(ParseLogLevel("3", parsed) && parsed == LogLevel::Error, "parse numeric 3 -> error");

	// -- parsing: invalid values keep the default ---------------------------
	parsed = LogLevel::Info;
	Check(!ParseLogLevel(nullptr, parsed) && parsed == LogLevel::Info, "nullptr is invalid and leaves the value untouched");
	Check(!ParseLogLevel("", parsed) && parsed == LogLevel::Info, "empty string is invalid");
	Check(!ParseLogLevel("verbose", parsed) && parsed == LogLevel::Info, "unknown name is invalid");
	Check(!ParseLogLevel("warning", parsed) && parsed == LogLevel::Info, "'warning' is not an accepted spelling");
	Check(!ParseLogLevel("4", parsed) && parsed == LogLevel::Info, "numeric 4 is out of range");
	Check(!ParseLogLevel("-1", parsed) && parsed == LogLevel::Info, "negative numeric is out of range");
	Check(!ParseLogLevel("10", parsed) && parsed == LogLevel::Info, "two-digit numeric is out of range");
	Check(!ParseLogLevel("deubg", parsed) && parsed == LogLevel::Info, "typo is invalid");

	// -- filtering matrix ----------------------------------------------------
	// A message passes when its level is at least the configured minimum.
	for (unsigned min = 0; min <= 3; ++min)
	{
		for (unsigned level = 0; level <= 3; ++level)
		{
			const bool expected = level >= min;
			char what[96] = {};
			std::snprintf(what, sizeof(what), "ShouldLog(min=%u, level=%u) == %s",
				min, level, expected ? "true" : "false");
			Check(ShouldLog(static_cast<LogLevel>(min), static_cast<LogLevel>(level)) == expected, what);
		}
	}
	Check(ShouldLog(LogLevel::Info, LogLevel::Warn) && ShouldLog(LogLevel::Info, LogLevel::Error),
		"info filter keeps warn and error");
	Check(!ShouldLog(LogLevel::Info, LogLevel::Debug), "info filter drops debug");
	Check(ShouldLog(LogLevel::Debug, LogLevel::Debug), "debug filter keeps debug");
	Check(!ShouldLog(LogLevel::Error, LogLevel::Warn), "error filter drops warn");
	Check(ShouldLog(LogLevel::Error, LogLevel::Error), "error filter keeps error");

	// -- level names ---------------------------------------------------------
	Check(std::string(LogLevelName(LogLevel::Debug)) == "debug", "name of debug");
	Check(std::string(LogLevelName(LogLevel::Info)) == "info", "name of info");
	Check(std::string(LogLevelName(LogLevel::Warn)) == "warn", "name of warn");
	Check(std::string(LogLevelName(LogLevel::Error)) == "error", "name of error");

	// -- sampling ------------------------------------------------------------
	Check(ShouldLogSample(0, 1) && ShouldLogSample(1, 1) && ShouldLogSample(7, 1),
		"every=1 reports every counter value");

	Check(ShouldLogSample(0, 100), "every=100 reports counter 0");
	Check(!ShouldLogSample(1, 100), "every=100 skips counter 1");
	Check(!ShouldLogSample(99, 100), "every=100 skips counter 99");
	Check(ShouldLogSample(100, 100), "every=100 reports counter 100");
	Check(!ShouldLogSample(101, 100), "every=100 skips counter 101");
	Check(!ShouldLogSample(199, 100), "every=100 skips counter 199");
	Check(ShouldLogSample(200, 100), "every=100 reports counter 200");

	Check(ShouldLogSample(0, 0) && ShouldLogSample(1, 0) && ShouldLogSample(7, 0) && ShouldLogSample(100, 0),
		"every=0 disables throttling (every call reports)");

	// Unsigned counter wrap: the modulo keeps working across 0xFFFFFFFF -> 0.
	Check(ShouldLogSample(0xFFFFFFFFu, 2) == false, "wrap: 0xFFFFFFFF is odd for every=2");
	Check(ShouldLogSample(0u, 2), "wrap: counter 0 reports for every=2");
	Check(!ShouldLogSample(1u, 2), "wrap: counter 1 is odd for every=2");
	Check(ShouldLogSample(2u, 2), "wrap: counter 2 reports for every=2");
	Check(ShouldLogSample(0xFFFFFFFFu, 1), "wrap: every=1 always reports");
	Check(ShouldLogSample(0xFFFFFFFFu, 100) == false, "wrap: 0xFFFFFFFF is not a multiple of 100");
	Check(ShouldLogSample(0u, 100), "wrap: counter 0 is the next sampled slot after the wrap");

	std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAILED",
		g_failures, g_failures == 1 ? "" : "s");
	return g_failures == 0 ? 0 : 1;
}
