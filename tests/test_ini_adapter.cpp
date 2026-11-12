// Regression test: the production <Platform>.ini adapter (INI-02, batch A).
//
// TEST_DEPS: src/Core/Ini.cpp src/Core/PlatformPaths.cpp
//
// Why this test exists
// --------------------
// tests/test_ini_registry.cpp drives the 2.0 key registry through an in-memory
// fake (a vector with strcmp lookups), so it cannot catch a regression in the
// production adapter the registry is actually wired to at startup:
//
//   IniRegistry / FeatureContext
//     -> Ini::GetString / Ini::EnumerateEntries           (src/Core/Ini.cpp)
//     -> Ini::Load -> GetGameFilePath/GetGameDirectory    (src/Core/PlatformPaths.cpp)
//     -> GetPrivateProfileStringA / SectionA / SectionNamesA on
//        <directory of the running executable>\<Platform>.ini
//
// This test links the production adapter directly (no fake) and drives it over
// the real Win32 profile API: it copies the committed fixture
// tests/fixtures/ini_adapter.ini next to the running test executable as
// <Platform>.ini, calls Ini::Load(), and asserts the behaviour the registry
// depends on:
//
//   (1) a missing key/section returns the caller's default byte for byte - the
//       registry tells "missing" from "empty" by passing kIniMissingValue;
//   (2) a key present as "Key=" reads back as "" (not as the default);
//   (3) section and key lookups are case-insensitive;
//   (4) a value longer than the 512-byte window the registry reads it into is
//       truncated at 511 characters + NUL (the documented
//       GetPrivateProfileString boundary - not silent corruption);
//   (5) enumeration: file order, key trimming, the 255-character key boundary
//       of Ini::EnumerateEntries's char[256], and the oversized inputs
//       (>16 KB of section names, >32 KB single section) terminate and yield a
//       bounded subset of the file.  The exact visit counts of the oversized
//       cases are Win32-profile-API behaviour that this test cannot pin from
//       Wine: the safety properties the platform relies on are asserted and the
//       observed counts are printed as "note" lines (record behaviour, do not
//       pretend it is pinned).
//
// Fixture lookup is layout-independent: the test searches the directory of the
// running executable, then its parents, for tests/fixtures/ini_adapter.ini; the
// environment variable VENTURECORE_TEST_FIXTURES may name the fixtures
// directory explicitly.  No workspace path is compiled in.  The test writes
// only next to its own executable (the build directory) and deletes what it
// creates.
//
// The test depends on the console-output convention of the other tests under
// tests/: one "ok    <description>" line per passing assertion, "FAIL ...
// <description>" for a failure, and a final "PASS (N failures)" line (the
// scripts/linux/run_tests.sh runner parses this).

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>

#include "Core/Ini.h"
#include "Core/IniRegistry.h"
#include "Core/PlatformConfig.h"
#include "Core/PlatformPaths.h"

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

// Recorded observation, never a pass/fail judgement: used for the Win32 profile
// API details that differ between Windows and Wine and that the platform does
// not depend on.
void Note(const char* const format, ...)
{
	std::printf("note  ");
	va_list args;
	va_start(args, format);
	std::vprintf(format, args);
	va_end(args);
	std::printf("\n");
}

// --- small Win32 helpers ---------------------------------------------------

std::string StripFileName(const std::string& path)
{
	const std::size_t separator = path.find_last_of("\\/");
	return separator == std::string::npos ? std::string() : path.substr(0, separator);
}

std::string ParentDirectory(const std::string& directory)
{
	const std::size_t separator = directory.find_last_of("\\/");
	if (separator == std::string::npos)
		return std::string();
	// Keep drive roots like "Z:\" in one piece.
	if (separator >= 1 && directory[separator - 1] == ':')
		return directory.substr(0, separator + 1);
	return directory.substr(0, separator);
}

bool FileExists(const std::string& path)
{
	const DWORD attributes = GetFileAttributesA(path.c_str());
	return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::string ExecutableDirectory()
{
	char path[MAX_PATH * 2] = {};
	const DWORD length = GetModuleFileNameA(nullptr, path, static_cast<DWORD>(sizeof(path)));
	if (length == 0 || length >= static_cast<DWORD>(sizeof(path)))
		return std::string();
	return StripFileName(path);
}

std::string CurrentDirectory()
{
	char path[MAX_PATH * 2] = {};
	const DWORD length = GetCurrentDirectoryA(static_cast<DWORD>(sizeof(path)), path);
	if (length == 0 || length >= static_cast<DWORD>(sizeof(path)))
		return std::string();
	return path;
}

// The file the production adapter resolves: <exe directory>\<Platform>.ini.
std::string GameIniPath()
{
	const std::string directory = ExecutableDirectory();
	if (directory.empty())
		return std::string();
	return directory + "\\" + VC_PLATFORM_INI_NAME;
}

bool FindFixturesDirectory(std::string& result)
{
	char overridePath[MAX_PATH * 2] = {};
	const DWORD overrideLength = GetEnvironmentVariableA(
		"VENTURECORE_TEST_FIXTURES", overridePath, static_cast<DWORD>(sizeof(overridePath)));
	if (overrideLength > 0 && overrideLength < static_cast<DWORD>(sizeof(overridePath)))
	{
		const std::string overridden = overridePath;
		if (FileExists(overridden + "\\ini_adapter.ini"))
		{
			result = overridden;
			return true;
		}
	}

	const std::string starts[] = { ExecutableDirectory(), CurrentDirectory() };
	for (const std::string& start : starts)
	{
		std::string directory = start;
		for (int level = 0; level < 8 && !directory.empty(); ++level)
		{
			if (FileExists(directory + "\\tests\\fixtures\\ini_adapter.ini"))
			{
				result = directory + "\\tests\\fixtures";
				return true;
			}
			directory = ParentDirectory(directory);
		}
	}
	return false;
}

bool WriteTextFile(const std::string& path, const std::string& content)
{
	FILE* file = nullptr;
	if (fopen_s(&file, path.c_str(), "wb") != 0 || !file)
		return false;
	const std::size_t written = std::fwrite(content.data(), 1, content.size(), file);
	std::fclose(file);
	return written == content.size();
}

// Installs tests/fixtures/<fixture> as <exe directory>\<Platform>.ini and
// resolves it through the production path (PlatformPaths + Ini::Load).
bool InstallFixture(const std::string& fixturesDirectory, const char* const fixture)
{
	const std::string target = GameIniPath();
	if (target.empty())
		return false;
	DeleteFileA(target.c_str());
	if (!CopyFileA((fixturesDirectory + "\\" + fixture).c_str(), target.c_str(), FALSE))
		return false;
	return venture::Ini::Load();
}

bool RemoveGameIni()
{
	const std::string target = GameIniPath();
	return target.empty() || DeleteFileA(target.c_str()) != FALSE || GetLastError() == ERROR_FILE_NOT_FOUND;
}

std::string ReadValue(const char* const section, const char* const key, const char* const defaultValue = "")
{
	char buffer[1024] = {};
	venture::Ini::GetString(section, key, defaultValue, buffer, static_cast<DWORD>(sizeof(buffer)));
	return buffer;
}

bool AllChars(const char* const text, const std::size_t length, const char expected)
{
	if (std::strlen(text) != length)
		return false;
	for (std::size_t index = 0; index < length; ++index)
	{
		if (text[index] != expected)
			return false;
	}
	return true;
}

// --- enumeration collection ------------------------------------------------

struct Entry
{
	std::string section;
	std::string key;
};

struct Collector
{
	std::vector<Entry> entries;
};

void CollectEntry(const char* const section, const char* const key, void* const userData)
{
	Collector* const collector = static_cast<Collector*>(userData);
	if (!collector)
		return;
	collector->entries.push_back(Entry{ section ? section : "", key ? key : "" });
}

std::vector<std::string> KeysOfSection(const Collector& collector, const char* const section)
{
	std::vector<std::string> keys;
	for (const Entry& entry : collector.entries)
	{
		if (_stricmp(entry.section.c_str(), section) == 0)
			keys.push_back(entry.key);
	}
	return keys;
}

bool ContainsKey(const std::vector<std::string>& keys, const char* const key)
{
	for (const std::string& candidate : keys)
	{
		if (candidate == key)
			return true;
	}
	return false;
}

bool HasSection(const Collector& collector, const char* const section)
{
	for (const Entry& entry : collector.entries)
	{
		if (_stricmp(entry.section.c_str(), section) == 0)
			return true;
	}
	return false;
}

// --- oversized-input generators -------------------------------------------

// 1600 sections named "Section0000"..: 1600 * 12 bytes of section names is
// >16 KB, i.e. more than GetPrivateProfileSectionNamesA can return into the
// 16384-byte buffer Ini::EnumerateEntries uses.
std::string OversizedSectionNameFile()
{
	std::string text;
	for (int index = 0; index < 1600; ++index)
	{
		char block[64] = {};
		std::snprintf(block, sizeof(block), "[Section%04d]\r\nKey=1\r\n", index);
		text += block;
	}
	return text;
}

// One section of 3000 keys (~63 KB): more than GetPrivateProfileSectionA can
// return into the 32768-byte buffer Ini::EnumerateEntries uses.
std::string OversizedSectionFile()
{
	std::string text = "[Big]\r\n";
	for (int index = 0; index < 3000; ++index)
	{
		char line[64] = {};
		std::snprintf(line, sizeof(line), "Key%05d=Value%05d\r\n", index, index);
		text += line;
	}
	return text;
}

// True when `key` can be a whole generated key ("Key%05d") or a prefix of one
// (the last entry of a buffer-truncated section may be cut mid-key; the adapter
// never invents a key that is not in the file).
bool IsGeneratedOversizedKey(const std::string& key)
{
	if (key.size() < 3 || key.size() > 8 || key.compare(0, 3, "Key") != 0)
		return false;
	for (std::size_t position = 3; position < key.size(); ++position)
	{
		if (key[position] < '0' || key[position] > '9')
			return false;
	}
	return true;
}

// True when `section` is one of the generated "Section%04d" names.
bool IsGeneratedSectionName(const std::string& section)
{
	if (section.size() != 11 || section.compare(0, 7, "Section") != 0)
		return false;
	for (std::size_t position = 7; position < 11; ++position)
	{
		if (section[position] < '0' || section[position] > '9')
			return false;
	}
	return true;
}

// --- 1. PlatformPaths ------------------------------------------------------

void TestPlatformPaths()
{
	char tiny[4] = {};
	Check(!venture::GetGameDirectory(tiny, 3), "GetGameDirectory rejects a buffer smaller than 4 bytes");
	Check(!venture::GetGameFilePath(VC_PLATFORM_INI_NAME, tiny, 3),
		"GetGameFilePath rejects a buffer that cannot hold the path (strcat_s failure is reported)");

	char directory[MAX_PATH * 2] = {};
	Check(venture::GetGameDirectory(directory, static_cast<DWORD>(sizeof(directory))),
		"GetGameDirectory resolves the directory of the running executable");
	const std::string gameDirectory = directory;
	Check(!gameDirectory.empty() && (gameDirectory.back() == '\\' || gameDirectory.back() == '/'),
		"GetGameDirectory keeps the trailing directory separator");

	char path[MAX_PATH * 2] = {};
	Check(venture::GetGameFilePath(VC_PLATFORM_INI_NAME, path, static_cast<DWORD>(sizeof(path))),
		"GetGameFilePath resolves <game directory>\\<VC_PLATFORM_INI_NAME>");
	const std::string iniPath = path;
	const std::string iniName = VC_PLATFORM_INI_NAME;
	Check(!gameDirectory.empty() && iniPath.compare(0, gameDirectory.size(), gameDirectory) == 0,
		"the resolved INI path starts with the game directory");
	Check(iniPath.size() >= iniName.size()
			&& iniPath.compare(iniPath.size() - iniName.size(), iniName.size(), iniName) == 0,
		"the resolved INI path ends with the platform INI file name");
}

// --- 2. state before Ini::Load --------------------------------------------

void TestBeforeLoad()
{
	Check(venture::Ini::Path() != nullptr && venture::Ini::Path()[0] == '\0',
		"Ini::Path is empty before Ini::Load");
	Check(!venture::Ini::Exists(), "Ini::Exists is false before Ini::Load");

	char buffer[64] = {};
	venture::Ini::GetString(VC_SECTION_PLATFORM, "LogLevel", "before-load-default", buffer,
		static_cast<DWORD>(sizeof(buffer)));
	Check(std::strcmp(buffer, "before-load-default") == 0,
		"GetString returns the caller's default before Ini::Load");

	Collector collector;
	venture::Ini::EnumerateEntries(&CollectEntry, &collector);
	Check(collector.entries.empty(), "EnumerateEntries is a no-op before Ini::Load");
}

// --- 3. fixture loads through the production path --------------------------

void TestFixtureLoadedState()
{
	Check(venture::Ini::Exists(), "Ini::Exists is true after the fixture is installed");

	const std::string iniPath = venture::Ini::Path();
	const std::string iniName = VC_PLATFORM_INI_NAME;
	Check(iniPath.size() >= iniName.size()
			&& iniPath.compare(iniPath.size() - iniName.size(), iniName.size(), iniName) == 0,
		"Ini::Path points at the fixture file name");
	Check(StripFileName(iniPath) == ExecutableDirectory(),
		"Ini::Path lives in the executable directory (the production game-directory path)");
}

// --- 4. missing key / missing section / empty value ------------------------

void TestMissingAndEmpty()
{
	Check(std::strlen(venture::kIniMissingValue) == 20,
		"the registry sentinel kIniMissingValue is 20 characters long");

	char buffer[64] = {};
	venture::Ini::GetString(VC_SECTION_PLATFORM, "NoSuchKeyAtAll", venture::kIniMissingValue, buffer,
		static_cast<DWORD>(sizeof(buffer)));
	Check(std::strcmp(buffer, venture::kIniMissingValue) == 0,
		"a missing key returns the caller's default (the sentinel) unchanged");
	Check(std::strlen(buffer) == std::strlen(venture::kIniMissingValue),
		"the missing-key sentinel keeps its full length (missing is distinguishable from empty)");

	char sectionBuffer[64] = {};
	venture::Ini::GetString("NoSuchSectionAtAll", "LogLevel", venture::kIniMissingValue, sectionBuffer,
		static_cast<DWORD>(sizeof(sectionBuffer)));
	Check(std::strcmp(sectionBuffer, venture::kIniMissingValue) == 0,
		"a missing section returns the caller's default (the sentinel) unchanged");

	char emptyBuffer[64] = {};
	venture::Ini::GetString("Values", "Empty", venture::kIniMissingValue, emptyBuffer,
		static_cast<DWORD>(sizeof(emptyBuffer)));
	Check(emptyBuffer[0] == '\0' && std::strcmp(emptyBuffer, venture::kIniMissingValue) != 0,
		"\"Key=\" is present with an empty value, not reported as missing (reads as \"\")");

	Check(ReadValue("Values", "MissingButNeeded", "fallback") == "present",
		"a present value overrides the caller's default");
	Check(ReadValue("Values", "NoSuchKey", "fallback") == "fallback",
		"a missing key with a normal default returns that default");
}

// --- 5. case insensitivity -------------------------------------------------

void TestCaseInsensitivity()
{
	Check(ReadValue("case", "mixedkey") == "MixedValue", "section and key lookups are case-insensitive (lower case)");
	Check(ReadValue("CASE", "MIXEDKEY") == "MixedValue", "section and key lookups are case-insensitive (upper case)");
	Check(ReadValue("Case", "mIxEdKeY") == "MixedValue", "mixed-case section and key resolve to the same value");
	Check(ReadValue("Case", "mIxEdCaSe") == "case-preserved-value", "a value keeps its original case");
	Check(ReadValue("Case", "UpperOnly") == "UPPER", "an upper-case value is returned unchanged");
}

// --- 6. the 512-byte read window ------------------------------------------

void TestReadWindowTruncation()
{
	char buffer[512] = {};
	venture::Ini::GetString("Values", "Long511", "", buffer, static_cast<DWORD>(sizeof(buffer)));
	Check(AllChars(buffer, 511, 'a'), "a 511-character value fills the 512-byte registry read window exactly");

	Check(std::strlen(buffer) == 511, "a 511-character value is not truncated");

	char truncated[512] = {};
	venture::Ini::GetString("Values", "Long512", "", truncated, static_cast<DWORD>(sizeof(truncated)));
	Check(AllChars(truncated, 511, 'b'),
		"a 512-character value is truncated at 511 characters + NUL (documented profile-API window)");

	char longer[512] = {};
	venture::Ini::GetString("Values", "Long600", "", longer, static_cast<DWORD>(sizeof(longer)));
	Check(AllChars(longer, 511, 'c'),
		"a 600-character value is truncated at the same 511-character window");

	char narrow[32] = {};
	venture::Ini::GetString("Values", "Long511", "", narrow, static_cast<DWORD>(sizeof(narrow)));
	Check(AllChars(narrow, 31, 'a'), "the truncation boundary follows the caller's buffer size (31 + NUL in 32 bytes)");
}

// --- 6b. recorded profile-API details (never asserted) --------------------

// The Win32 profile API trims leading/trailing whitespace from values and
// strips surrounding double quotes; the exact trimming rules are documented
// loosely and have differed between implementations (Windows vs Wine, and
// between Windows versions).  The platform does not depend on any of this, so
// the observations are *recorded* (note lines) instead of asserted: a FAIL here
// would pin Wine and Windows to behaviour that neither the registry nor the
// template dump relies on.
void TestRecordedProfileQuirks()
{
	char buffer[256] = {};

	venture::Ini::GetString("Values", "Spaces", "default", buffer, static_cast<DWORD>(sizeof(buffer)));
	Note("profile API records: [Values] Spaces=<3 spaces> reads back as \"%s\" (%zu character(s))",
		buffer, std::strlen(buffer));

	venture::Ini::GetString("Values", "Padded", "default", buffer, static_cast<DWORD>(sizeof(buffer)));
	Note("profile API records: [Values] Padded=<3 spaces>padded value<3 spaces> reads back as \"%s\"",
		buffer);

	venture::Ini::GetString("Values", "Quoted", "default", buffer, static_cast<DWORD>(sizeof(buffer)));
	Note("profile API records: [Values] Quoted=\"quoted value\" reads back as \"%s\" (%zu character(s))",
		buffer, std::strlen(buffer));
}

// --- 7. enumeration over the small fixture --------------------------------

void TestSmallEnumeration()
{
	Collector collector;
	venture::Ini::EnumerateEntries(&CollectEntry, &collector);
	Check(!collector.entries.empty(), "EnumerateEntries visits the installed fixture");

	Check(HasSection(collector, "Platform"), "enumeration reports the [Platform] section");
	Check(HasSection(collector, "Values"), "enumeration reports the [Values] section");
	Check(HasSection(collector, "Enum"), "enumeration reports the [Enum] section");

	const std::vector<std::string> enumKeys = KeysOfSection(collector, "Enum");
	Check(enumKeys.size() == 3, "enumeration reports the three [Enum] keys in file order");
	Check(enumKeys.size() == 3 && enumKeys[0] == "Alpha" && enumKeys[1] == "Beta",
		"enumeration keeps the file order (Alpha then Beta)");
	Check(enumKeys.size() == 3 && enumKeys[2] == "SpacedKey",
		"enumeration trims spaces around keys (\" SpacedKey \" becomes \"SpacedKey\")");

	const std::vector<std::string> valueKeys = KeysOfSection(collector, "Values");
	Check(ContainsKey(valueKeys, "Empty"), "enumeration reports a key whose value is empty");
	Check(ContainsKey(valueKeys, "Long600"), "enumeration reports the long-value keys");
	const std::vector<std::string> platformKeys = KeysOfSection(collector, "Platform");
	Check(ContainsKey(platformKeys, "LogLevel"), "enumeration reports real [Platform] keys");
	Check(!ContainsKey(platformKeys, "Alpha"), "enumeration keeps sections apart");
}

// --- 8. the 255-character key boundary ------------------------------------

void TestKeyLengthBoundary(const std::string& fixturesDirectory)
{
	Check(InstallFixture(fixturesDirectory, "ini_enum_keys.ini"),
		"install tests/fixtures/ini_enum_keys.ini and call Ini::Load");

	Collector collector;
	venture::Ini::EnumerateEntries(&CollectEntry, &collector);
	const std::vector<std::string> keys = KeysOfSection(collector, "Keys");
	Check(keys.size() == 2, "enumeration skips keys of 256+ characters (char key[256] in Ini::EnumerateEntries)");
	Check(keys.size() == 2 && keys[0] == std::string(255, 'K'),
		"a 255-character key is enumerated unchanged (the last key that fits)");
	Check(keys.size() == 2 && keys[1] == "Zeta", "enumeration continues after a skipped over-long key");
	if (keys.size() != 2 || keys[0] != std::string(255, 'K') || keys[1] != "Zeta")
	{
		Note("key-length fixture: %zu key(s) enumerated", keys.size());
	}
}

// --- 9. oversized section-name list ---------------------------------------

void TestOversizedSectionList()
{
	const std::string target = GameIniPath();
	Check(!target.empty() && WriteTextFile(target, OversizedSectionNameFile()),
		"write the >16 KB section-name fixture next to the test executable");
	Check(venture::Ini::Load(), "Ini::Load resolves the oversized section-name fixture");

	Collector collector;
	venture::Ini::EnumerateEntries(&CollectEntry, &collector);

	bool everySectionGenerated = true;
	for (const Entry& entry : collector.entries)
	{
		if (!IsGeneratedSectionName(entry.section))
			everySectionGenerated = false;
	}

	Check(!collector.entries.empty(),
		"enumeration over a >16 KB section-name list still visits at least one section");
	Check(collector.entries.size() < 1600,
		"the 16384-byte section-name buffer bounds the enumeration (fewer than the 1600 sections)");
	Check(everySectionGenerated,
		"every enumerated section is a complete generated name (a buffer-truncated name cannot resolve entries)");
	Note("oversized section names: visited %zu of 1600 section(s) - exact cap is Win32 profile-API behaviour",
		collector.entries.size());
}

// --- 10. oversized single section -----------------------------------------

void TestOversizedSingleSection()
{
	const std::string target = GameIniPath();
	Check(!target.empty() && WriteTextFile(target, OversizedSectionFile()),
		"write the >32 KB single-section fixture next to the test executable");
	Check(venture::Ini::Load(), "Ini::Load resolves the oversized single-section fixture");

	Collector collector;
	venture::Ini::EnumerateEntries(&CollectEntry, &collector);

	bool everyKeyGenerated = true;
	bool orderKept = true;
	bool sectionOk = true;
	int previousComplete = -1;
	std::size_t completeKeys = 0;
	std::size_t boundaryKeys = 0;
	for (const Entry& entry : collector.entries)
	{
		if (_stricmp(entry.section.c_str(), "Big") != 0)
			sectionOk = false;
		if (!IsGeneratedOversizedKey(entry.key))
		{
			everyKeyGenerated = false;
			continue;
		}
		if (entry.key.size() == 8)
		{
			const int index = std::atoi(entry.key.c_str() + 3);
			if (index < previousComplete)
				orderKept = false;
			previousComplete = index;
			++completeKeys;
		}
		else
		{
			++boundaryKeys;
		}
	}

	Check(!collector.entries.empty(),
		"enumeration over a >32 KB single section still visits at least one key");
	Check(collector.entries.size() < 3000,
		"the 32768-byte single-section buffer bounds the enumeration (fewer than the 3000 keys)");
	Check(sectionOk, "the oversized single-section entries stay in their section");
	Check(everyKeyGenerated,
		"every enumerated key is a generated key or a prefix of one (no invented keys from the truncated block)");
	Check(orderKept, "complete keys keep their file order");
	Note("oversized single section: visited %zu of 3000 key(s), of which %zu complete and %zu cut at the buffer edge",
		collector.entries.size(), completeKeys, boundaryKeys);
}

// --- 11. the fixture disappears (missing file = all defaults) --------------

void TestMissingFile()
{
	Check(RemoveGameIni(), "remove the installed fixture (the game directory has no INI file)");
	Check(venture::Ini::Load(), "Ini::Load still resolves the path when the file is missing");
	Check(!venture::Ini::Exists(), "Ini::Exists is false for a missing file");

	char buffer[64] = {};
	venture::Ini::GetString(VC_SECTION_PLATFORM, "LogLevel", "fallback-default", buffer,
		static_cast<DWORD>(sizeof(buffer)));
	Check(std::strcmp(buffer, "fallback-default") == 0,
		"GetString returns the caller's default when the file is missing");
	Check(venture::Ini::GetBool(VC_SECTION_PLATFORM, "DumpIniTemplate", true),
		"the legacy GetBool returns its default when the file is missing");
	Check(venture::Ini::GetInt(VC_SECTION_PLATFORM, "SomeNumber", 7) == 7,
		"the legacy GetInt returns its default when the file is missing");

	Collector collector;
	venture::Ini::EnumerateEntries(&CollectEntry, &collector);
	Check(collector.entries.empty(), "EnumerateEntries is a no-op when the file is missing");
}

// --- fixture-dependent group ----------------------------------------------

void RunFixtureTests(const std::string& fixturesDirectory)
{
	const bool installed = InstallFixture(fixturesDirectory, "ini_adapter.ini");
	Check(installed, "install tests/fixtures/ini_adapter.ini as <Platform>.ini and call Ini::Load");
	if (!installed)
		return;

	TestFixtureLoadedState();
	TestMissingAndEmpty();
	TestCaseInsensitivity();
	TestReadWindowTruncation();
	TestRecordedProfileQuirks();
	TestSmallEnumeration();
	TestKeyLengthBoundary(fixturesDirectory);
	TestOversizedSectionList();
	TestOversizedSingleSection();
	TestMissingFile();
}
}

int __cdecl main(void)
{
	std::printf("ini adapter regression test (production Ini.cpp + PlatformPaths.cpp)\n\n");

	TestPlatformPaths();
	TestBeforeLoad();

	std::string fixturesDirectory;
	const bool foundFixtures = FindFixturesDirectory(fixturesDirectory);
	Check(foundFixtures, "locate the committed tests/fixtures/ directory from the test executable");
	if (foundFixtures)
	{
		RunFixtureTests(fixturesDirectory);
	}

	RemoveGameIni();

	std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAILED", g_failures,
		g_failures == 1 ? "" : "s");
	return g_failures == 0 ? 0 : 1;
}
