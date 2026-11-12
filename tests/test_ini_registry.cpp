// Unit test: the 2.0 <Platform>.ini key registry.
//
// TEST_DEPS: src/Core/IniRegistry.cpp src/Core/FeatureRegistry.cpp src/Core/HostRegistry.cpp src/Core/Hosts.Builtin.cpp
//
// The registry is engine-free: the file is reached through two injected function
// pointers (raw value lookup + entry enumeration) and diagnostics through a log
// sink, so every rule is exercised against an in-memory "file":
//   * bool/int/string reads, defaults, strict integer parsing and range checks;
//   * invalid values fall back to the declared default with a warning;
//   * duplicate (section, key) registrations warn and keep the first declaration;
//   * the built-in [Platform] keys are registered up front;
//   * [Groups] master switches and FeatureDescriptor::defaultEnabled/group;
//   * the unknown-key scan (feature switch suggestions, known-section keys,
//     Debug-only user sections, IniWarnUnknownKeys off);
//   * the R1 template dump rendering.

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "Core/FeatureRegistry.h"
#include "Core/HostRegistry.h"
#include "Core/IniRegistry.h"

namespace
{
int g_failures = 0;

struct FileEntry
{
	const char* section;
	const char* key;
	const char* value;
};

std::vector<FileEntry> g_file;

struct LogRecord
{
	bool warning;
	std::string text;
};

std::vector<LogRecord> g_logs;
std::vector<std::string> g_featureLogs;

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

bool Contains(const std::string& haystack, const char* const needle)
{
	return needle && haystack.find(needle) != std::string::npos;
}

void ResetFakes()
{
	g_file.clear();
	g_logs.clear();
	g_featureLogs.clear();
}

void SetFileValue(const char* const section, const char* const key, const char* const value)
{
	for (std::size_t index = 0; index < g_file.size(); ++index)
	{
		if (std::strcmp(g_file[index].section, section) == 0 && std::strcmp(g_file[index].key, key) == 0)
		{
			g_file[index].value = value;
			return;
		}
	}
	g_file.push_back(FileEntry{ section, key, value });
}

void RemoveFileValue(const char* const section, const char* const key)
{
	for (std::size_t index = 0; index < g_file.size(); ++index)
	{
		if (std::strcmp(g_file[index].section, section) == 0 && std::strcmp(g_file[index].key, key) == 0)
		{
			g_file.erase(g_file.begin() + static_cast<std::ptrdiff_t>(index));
			return;
		}
	}
}

// --- injected INI wiring ---------------------------------------------------
void FakeRawValue(
	const char* const section,
	const char* const key,
	const char* const defaultValue,
	char* const buffer,
	const unsigned long bufferSize)
{
	for (const FileEntry& entry : g_file)
	{
		if (std::strcmp(entry.section, section) == 0 && std::strcmp(entry.key, key) == 0)
		{
			strcpy_s(buffer, static_cast<std::size_t>(bufferSize), entry.value);
			return;
		}
	}
	strcpy_s(buffer, static_cast<std::size_t>(bufferSize), defaultValue);
}

void FakeEnumerate(const venture::IniEntryVisitorFn visitor, void* const userData)
{
	for (const FileEntry& entry : g_file)
		visitor(entry.section, entry.key, userData);
}

void FakeSink(const bool warning, const char* const message)
{
	g_logs.push_back(LogRecord{ warning, message ? message : "" });
}

bool SawWarning(const char* const needle)
{
	for (const LogRecord& record : g_logs)
	{
		if (record.warning && Contains(record.text, needle))
			return true;
	}
	return false;
}

bool SawDebug(const char* const needle)
{
	for (const LogRecord& record : g_logs)
	{
		if (!record.warning && Contains(record.text, needle))
			return true;
	}
	return false;
}

std::size_t CountLogs(const char* const needle, const bool warning)
{
	std::size_t count = 0;
	for (const LogRecord& record : g_logs)
	{
		if (record.warning == warning && Contains(record.text, needle))
			++count;
	}
	return count;
}

void WireRegistry()
{
	venture::IniRegistry& ini = venture::IniRegistry::Instance();
	ini.SetRawReader(&FakeRawValue);
	ini.SetEnumerator(&FakeEnumerate);
	ini.SetLogSink(&FakeSink);
}

void ResetRegistry()
{
	ResetFakes();
	venture::IniRegistry::Instance().Reset();
}

// --- FeatureRegistry logging capture --------------------------------------
void CaptureFeatureLog(const char* const format, ...)
{
	char line[512] = {};
	va_list args;
	va_start(args, format);
	_vsnprintf_s(line, sizeof(line), _TRUNCATE, format, args);
	va_end(args);
	g_featureLogs.push_back(line);
}

bool FeatureLogContains(const char* const needle)
{
	for (const std::string& line : g_featureLogs)
	{
		if (Contains(line, needle))
			return true;
	}
	return false;
}
}

void TestBoolReads()
{
	ResetRegistry();
	venture::IniRegistry& ini = venture::IniRegistry::Instance();
	const int builtinKeys = ini.KeyCount();

	Check(ini.ReadBool("MyFeature", "Missing", true, "missing key", "unit_test.cpp"),
		"a missing bool key keeps the declared default");
	Check(ini.KeyCount() == builtinKeys + 1, "the read registers the key");
	Check(ini.InvalidCount() == 0, "a missing key is not invalid");

	SetFileValue("MyFeature", "Enabled", "OFF");
	Check(!ini.ReadBool("MyFeature", "Enabled", true, "enable it", "unit_test.cpp"),
		"a valid bool token is parsed (case-insensitive off)");

	SetFileValue("MyFeature", "Enabled", "maybe");
	g_logs.clear();
	Check(ini.ReadBool("MyFeature", "Enabled", true, "enable it", "unit_test.cpp"),
		"an invalid bool falls back to the default");
	Check(SawWarning("is not a valid bool"), "an invalid bool warns");
	Check(ini.InvalidCount() == 1, "the invalid value is counted");

	const venture::IniKeyInfo* const info = ini.KeyAt(ini.KeyCount() - 1);
	Check(info && info->section == "MyFeature" && info->key == "Enabled", "the key table entry is kept");
}

void TestIntReads()
{
	ResetRegistry();
	venture::IniRegistry& ini = venture::IniRegistry::Instance();

	SetFileValue("Numbers", "Count", "42");
	Check(ini.ReadInt("Numbers", "Count", 10, 0, 100, "a count", "unit_test.cpp") == 42,
		"a valid integer inside the range is returned");

	SetFileValue("Numbers", "Count", "200");
	g_logs.clear();
	Check(ini.ReadInt("Numbers", "Count", 10, 0, 100, "a count", "unit_test.cpp") == 10,
		"an out-of-range integer falls back to the default");
	Check(SawWarning("outside the allowed range 0..100"), "the range warning names the range");

	SetFileValue("Numbers", "Count", "12abc");
	g_logs.clear();
	Check(ini.ReadInt("Numbers", "Count", 10, 0, 100, "a count", "unit_test.cpp") == 10,
		"a trailing-garbage integer falls back to the default");
	Check(SawWarning("is not a valid integer"), "the integer warning is emitted");

	SetFileValue("Numbers", "Count", "99999999999999999999");
	g_logs.clear();
	Check(ini.ReadInt("Numbers", "Count", 10, 0, 100, "a count", "unit_test.cpp") == 10,
		"an overflowing integer falls back to the default");

	SetFileValue("Numbers", "Free", "500");
	Check(ini.ReadInt("Numbers", "Free", 0, 1, 0, "no range", "unit_test.cpp") == 500,
		"minValue > maxValue means no range check");

	Check(ini.InvalidCount() == 1, "the rejected key is counted once (per key, not per read)");
}

void TestStringReads()
{
	ResetRegistry();
	venture::IniRegistry& ini = venture::IniRegistry::Instance();

	char buffer[64] = {};
	ini.ReadString("Strings", "Name", "fallback", buffer, sizeof(buffer), "a name", "unit_test.cpp");
	Check(std::strcmp(buffer, "fallback") == 0, "a missing string key returns the default");

	SetFileValue("Strings", "Name", "hello game");
	ini.ReadString("Strings", "Name", "fallback", buffer, sizeof(buffer), "a name", "unit_test.cpp");
	Check(std::strcmp(buffer, "hello game") == 0, "a present string value is returned");

	const venture::IniKeyInfo* const info = ini.KeyAt(ini.KeyCount() - 1);
	Check(info && info->type == venture::IniValueType::String, "the string key is registered as a string");
}

void TestDuplicateRegistration()
{
	ResetRegistry();
	venture::IniRegistry& ini = venture::IniRegistry::Instance();
	const int builtinKeys = ini.KeyCount();

	ini.BeginFeature("feature_a");
	ini.ReadBool("Shared", "Switch", true, "the first declaration", "a.cpp");
	ini.EndFeature();

	ini.BeginFeature("feature_b");
	g_logs.clear();
	const bool value = ini.ReadBool("Shared", "Switch", false, "the second declaration", "b.cpp");
	ini.EndFeature();

	Check(SawWarning("duplicate ini key [Shared] Switch"), "a duplicate key warns");
	Check(SawWarning("feature_a") && SawWarning("feature_b"), "the duplicate warning names both features");
	Check(ini.KeyCount() == builtinKeys + 1, "the duplicate does not add a second entry");
	Check(!value, "the second caller still gets its own parsed/default value");
}

void TestBuiltinPlatformKeys()
{
	ResetRegistry();
	venture::IniRegistry& ini = venture::IniRegistry::Instance();

	Check(ini.KeyCount() == 7, "the seven built-in [Platform] keys are registered up front");

	SetFileValue("Platform", "IniWarnUnknownKeys", "no");
	Check(!ini.ReadBaseBool("Platform", "IniWarnUnknownKeys", true, "warn about unknown keys"),
		"a built-in base key is read through the registry");

	SetFileValue("Platform", "DebugOutput", "maybe");
	g_logs.clear();
	Check(ini.ReadBaseBool("Platform", "DebugOutput", false, "debugger output") == false,
		"an invalid base bool falls back to the default");
	Check(SawWarning("is not a valid bool"), "the base key warning is visible");

	int platformKeys = 0;
	for (int index = 0; index < ini.KeyCount(); ++index)
	{
		const venture::IniKeyInfo* const info = ini.KeyAt(index);
		if (info && info->section == "Platform")
			++platformKeys;
	}
	Check(platformKeys == 7, "base reads update the registered keys instead of adding duplicates");
}

// -- FeatureRegistry: switch + group + defaultEnabled -----------------------
bool g_enabledDefaultOn = false;
bool g_enabledDefaultOff = false;
bool g_enabledGrouped = false;

void NoopInitialize(const venture::FeatureContext& context)
{
	(void)context;
}

const venture::FeatureDescriptor kDefaultOn{
	"test_default_on", "DefaultOn", "Default on feature",
	0u, 0u, &g_enabledDefaultOn, &NoopInitialize };

const venture::FeatureDescriptor kDefaultOff{
	"test_default_off", "DefaultOff", "Default off feature",
	0u, 0u, &g_enabledDefaultOff, &NoopInitialize,
	nullptr, false };

const venture::FeatureDescriptor kGrouped{
	"test_grouped", "Grouped", "Grouped feature",
	0u, 0u, &g_enabledGrouped, &NoopInitialize,
	"Combat", true };

void RunFeatureGate()
{
	venture::FeatureContext context{};
	context.platformName = "VentureCore";
	context.iniPath = "";
	context.hostMask = 0u;
	context.Log = &CaptureFeatureLog;
	g_featureLogs.clear();
	venture::InitializeFeatures(context);
}

void TestFeatureDescriptorIniSemantics()
{
	ResetRegistry();
	venture::IniRegistry& ini = venture::IniRegistry::Instance();
	(void)ini;

	venture::RegisterFeature(&kDefaultOn);
	venture::RegisterFeature(&kDefaultOff);
	venture::RegisterFeature(&kGrouped);

	// No file entries: defaultEnabled decides, groups default on.
	RunFeatureGate();
	Check(g_enabledDefaultOn, "a descriptor without defaultEnabled defaults to enabled");
	Check(!g_enabledDefaultOff, "defaultEnabled=false disables the module when the switch is absent");
	Check(g_enabledGrouped, "a missing [Groups] switch defaults to on");

	// Explicit user values win.
	SetFileValue("Features", "DefaultOff", "yes");
	SetFileValue("Groups", "Combat", "no");
	RunFeatureGate();
	Check(g_enabledDefaultOff, "an explicit [Features] value overrides defaultEnabled");
	Check(!g_enabledGrouped, "a [Groups] master switch disables the grouped module");
	Check(FeatureLogContains("disabled by VentureCore.ini [Groups] Combat"),
		"the group log line names the [Groups] switch");

	// Feature switch off beats the group default.
	SetFileValue("Features", "DefaultOn", "no");
	RemoveFileValue("Groups", "Combat");
	RunFeatureGate();
	Check(!g_enabledDefaultOn, "[Features] off disables the module");
	Check(FeatureLogContains("disabled by VentureCore.ini [Features] DefaultOn"),
		"the feature switch log line is unchanged");
	Check(g_enabledGrouped, "removing the [Groups] override re-enables the grouped module");

	// A switch with an invalid value warns and falls back to defaultEnabled.
	SetFileValue("Features", "DefaultOff", "maybe");
	g_logs.clear();
	RunFeatureGate();
	Check(!g_enabledDefaultOff, "an invalid feature switch falls back to defaultEnabled");
	Check(SawWarning("[Features] DefaultOff: 'maybe' is not a valid bool"),
		"the invalid feature switch warns");
}

void TestUnknownKeyScan()
{
	ResetRegistry();
	venture::IniRegistry& ini = venture::IniRegistry::Instance();

	// A module-owned key and a switch set, as the feature registry publishes it.
	ini.BeginFeature("radial");
	ini.ReadInt("RadialIndicator", "LargeRange", 0, 0, 100, "the large-range cutoff", "radial.cpp");
	ini.EndFeature();
	ini.ReadGroupSwitch("Combat");

	const venture::IniFeatureSwitch switches[] = {
		{ "RadialIndicator", "Radial indicator", "Combat", true },
	};
	ini.SetFeatureSwitches(switches, 1);

	g_file.clear();
	SetFileValue("Platform", "LogLevel", "info");            // known base key
	SetFileValue("Platform", "Bogus", "1");                   // unknown base key
	SetFileValue("Features", "RadialIndicator", "yes");       // known switch
	SetFileValue("Features", "RaidalIndicator", "yes");       // typo -> suggestion
	SetFileValue("Groups", "Combat", "yes");                  // known group
	SetFileValue("RadialIndicator", "LargeRange", "42");      // known feature key
	SetFileValue("RadialIndicator", "LargeRagne", "1");       // unknown key, known section
	SetFileValue("User", "Anything", "1");                    // user section
	SetFileValue("User", "Other", "2");

	const venture::IniScanResult result = ini.ScanUnknownKeys(true);
	Check(result.registered == ini.KeyCount(), "the scan reports the registered key count");
	Check(result.unknown == 5, "the scan counts every unknown entry");
	Check(SawWarning("unknown key [Platform] Bogus"), "an unknown [Platform] key warns");
	Check(SawWarning("unknown feature switch [Features] RaidalIndicator - ignored (did you mean RadialIndicator?)"),
		"an unknown [Features] switch warns with a name suggestion");
	Check(SawWarning("unknown key [RadialIndicator] LargeRagne"), "an unknown key in a feature section warns");
	Check(!SawWarning("User"), "a user-defined section is not warned about");
	Check(CountLogs("ignoring user-defined section [User]", false) == 1,
		"a user-defined section gets one Debug line");

	g_logs.clear();
	const venture::IniScanResult quiet = ini.ScanUnknownKeys(false);
	Check(quiet.unknown == 5, "IniWarnUnknownKeys=no does not change the count");
	Check(!SawWarning("unknown"), "IniWarnUnknownKeys=no suppresses the warnings");
	Check(SawDebug("unknown feature switch [Features] RaidalIndicator"),
		"with warnings off the unknown entries are Debug-logged");
}

void TestTemplateRender()
{
	ResetRegistry();
	venture::IniRegistry& ini = venture::IniRegistry::Instance();

	ini.BeginFeature("radial");
	ini.ReadInt("RadialIndicator", "LargeRange", 7, 0, 100, "the large-range cutoff", "radial.cpp");
	ini.EndFeature();
	ini.ReadGroupSwitch("Combat");
	const venture::IniFeatureSwitch switches[] = {
		{ "RadialIndicator", "Radial indicator", "Combat", true },
	};
	ini.SetFeatureSwitches(switches, 1);

	const std::string text = ini.RenderTemplate();
	Check(Contains(text, "[Platform]"), "the template renders the [Platform] section");
	Check(Contains(text, "IniWarnUnknownKeys=yes"), "the template renders a built-in key with its value");
	Check(Contains(text, "[Groups]"), "the template renders the [Groups] section");
	Check(Contains(text, "Combat=yes"), "the template renders a group master switch");
	Check(Contains(text, "[Features]"), "the template renders the [Features] section");
	Check(Contains(text, "RadialIndicator=yes"), "the template renders a feature switch");
	Check(Contains(text, "[RadialIndicator]"), "the template renders a feature section");
	Check(Contains(text, "LargeRange=7"), "the template renders the key with its resolved value");
	Check(Contains(text, "the large-range cutoff"), "the template renders the description");
	Check(Contains(text, "range: 0..100"), "the template renders the integer range");
	Check(Contains(text, "feature: radial"), "the template names the owning feature");
}

int __cdecl main()
{
	WireRegistry();

	TestBoolReads();
	TestIntReads();
	TestStringReads();
	TestDuplicateRegistration();
	TestBuiltinPlatformKeys();
	TestFeatureDescriptorIniSemantics();
	TestUnknownKeyScan();
	TestTemplateRender();

	std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAILED",
		g_failures, g_failures == 1 ? "" : "s");
	return g_failures == 0 ? 0 : 1;
}
