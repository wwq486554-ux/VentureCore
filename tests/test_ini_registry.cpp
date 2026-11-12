// Unit test: the 2.0 <Platform>.ini key registry.
//
// TEST_DEPS: src/Core/IniRegistry.cpp src/Core/FeatureRegistry.cpp src/Core/HostRegistry.cpp src/Core/Hosts.Builtin.cpp src/Core/Log.cpp src/Core/PlatformPaths.cpp
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
//   * the R1 template dump rendering (declared defaults, never local overrides);
//   * the pre-2.0 legacy-getter MarkKnown bridge and the VENTURE_INI_* macros
//     (2.0 feature-side INI API, INI-08/API-06);
//   * the size-limit diagnostics (INI-06) and the pre-open logger queue
//     (START-01 - the logger is exercised here because test_logging.cpp is not
//     in this batch's write scope; the logger behaviour is engine-free).

#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>

#include "Core/FeatureRegistry.h"
#include "Core/HostRegistry.h"
#include "Core/IniRegistry.h"
#include "Core/Log.h"

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

// Looks a registered key up by name (the KeyAt order is registration order, so a
// duplicate or a MarkKnown entry is not necessarily the last one).
const venture::IniKeyInfo* FindInfo(
	const venture::IniRegistry& ini,
	const char* const section,
	const char* const key)
{
	for (int index = 0; index < ini.KeyCount(); ++index)
	{
		const venture::IniKeyInfo* const info = ini.KeyAt(index);
		if (info && info->section == section && info->key == key)
			return info;
	}
	return nullptr;
}

// Reads a whole file with stdio (the logger is closed whenever this is used and
// the file lives in the test's working directory under Release/IntDir).
bool ReadWholeTextFile(const char* const path, std::string& contents)
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

// --- FeatureRegistry logging capture --------------------------------------
// API-05: FeatureContext callbacks are pinned to __cdecl and the test build uses
// /Gz (stdcall default), so every function assigned to one spells the convention.
void __cdecl CaptureFeatureLog(const char* const format, ...)
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

// FeatureContext adapters (API-06): the production ones live in Platform.cpp;
// these forward to the same registry API and let the VENTURE_INI_* macros - the
// 2.0 feature-side INI entry points - run for real.  They cover the macro
// signature/parameter order, the __FILE__ pass-through and the returned value.
bool __cdecl AdapterIniBool(
	const char* const section,
	const char* const key,
	const bool defaultValue,
	const char* const description,
	const char* const sourceFile)
{
	return venture::IniRegistry::Instance().ReadBool(section, key, defaultValue, description, sourceFile);
}

int __cdecl AdapterIniInt(
	const char* const section,
	const char* const key,
	const int defaultValue,
	const int minValue,
	const int maxValue,
	const char* const description,
	const char* const sourceFile)
{
	return venture::IniRegistry::Instance().ReadInt(
		section, key, defaultValue, minValue, maxValue, description, sourceFile);
}

void __cdecl AdapterIniString(
	const char* const section,
	const char* const key,
	const char* const defaultValue,
	char* const buffer,
	const unsigned long bufferSize,
	const char* const description,
	const char* const sourceFile)
{
	venture::IniRegistry::Instance().ReadString(
		section, key, defaultValue, buffer, bufferSize, description, sourceFile);
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

	Check(SawWarning("duplicate ini key [Shared] Switch"), "a conflicting duplicate key warns");
	Check(SawWarning("feature_a") && SawWarning("feature_b"), "the duplicate warning names both features");
	Check(SawWarning("every read is resolved with it"), "the duplicate warning names the value source");
	Check(ini.KeyCount() == builtinKeys + 1, "the duplicate does not add a second entry");
	Check(value, "the second caller is resolved with the first declaration (INI-01)");

	const venture::IniKeyInfo* const info = FindInfo(ini, "Shared", "Switch");
	Check(info && info->defaultText == "yes", "the registered default is kept");
	Check(info && info->valueText == "yes", "the stored value comes from the first declaration");
	Check(info && info->featureId == "feature_a", "the owning feature is still the first declarer");
}

// INI-01, DYN reproduction of the second review (inireg_dup_probe.exe Case 2):
// feature A declares range 0..100, feature B re-declares range 0..10 and the file
// value 50 is only outside B's range.  Before the fix B's read marked the key
// invalid and stored B's default while the warning cited A's declaration; now
// every read is resolved with the first declaration.
void TestDuplicateValueSource()
{
	ResetRegistry();
	venture::IniRegistry& ini = venture::IniRegistry::Instance();

	ini.BeginFeature("feature_a");
	ini.ReadInt("Dup", "Range", 10, 0, 100, "the first declaration", "a.cpp");
	ini.EndFeature();

	SetFileValue("Dup", "Range", "50");
	ini.BeginFeature("feature_b");
	g_logs.clear();
	const int value = ini.ReadInt("Dup", "Range", 5, 0, 10, "the second declaration", "b.cpp");
	ini.EndFeature();

	Check(value == 50, "the duplicate read is resolved with the first declaration's range");
	Check(ini.InvalidCount() == 0, "the duplicate does not poison the invalid counter with the second range");
	Check(ini.KeyCount() == 8, "the duplicate does not add a key (7 built-ins + 1)");
	Check(SawWarning("range: 0..100"), "the duplicate warning cites the registered range");
	Check(SawWarning("default/range differ and are ignored"), "the caller's declaration is named as ignored");

	const venture::IniKeyInfo* const info = FindInfo(ini, "Dup", "Range");
	Check(info && info->valueText == "50", "the stored value is the first declaration's resolution");
	Check(info && info->defaultText == "10", "the stored default is the first declaration's");

	// A value outside the *first* declaration's range: warning and fallback default
	// come from that same declaration.
	SetFileValue("Dup", "Range", "150");
	g_logs.clear();
	const int invalidValue = ini.ReadInt("Dup", "Range", 10, 0, 100, "the first declaration", "a.cpp");
	Check(invalidValue == 10, "an out-of-range value falls back to the registered default");
	Check(ini.InvalidCount() == 1, "the invalid state comes from the first declaration's range");
	Check(SawWarning("using the default '10'"), "the warning cites the registered default");

	// A duplicate with a different type cannot silently reinterpret the key.
	ini.ReadBool("Dup", "Flag", true, "a bool key", "flag.cpp");
	g_logs.clear();
	const int mismatched = ini.ReadInt("Dup", "Flag", 5, 0, 10, "an int read of a bool key", "flag2.cpp");
	Check(mismatched == 5, "a type-mismatched read returns its own default");
	Check(SawWarning("is registered as a bool key"), "the type mismatch is reported");
	const venture::IniKeyInfo* const flag = FindInfo(ini, "Dup", "Flag");
	Check(flag && flag->type == venture::IniValueType::Bool, "the registered type is unchanged");
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

void __cdecl NoopInitialize(const venture::FeatureContext& context)
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
	Check(result.invalid == 0, "the scan reports no invalid value here");
	Check(result.truncated == 0, "the scan reports no truncated value here");
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

	// Route every value through a *local override* first: the template must still
	// render the declared defaults, never the effective values (INI-04).
	SetFileValue("Platform", "IniWarnUnknownKeys", "no");
	SetFileValue("Groups", "Combat", "no");
	SetFileValue("Features", "RadialIndicator", "no");
	SetFileValue("RadialIndicator", "LargeRange", "42");
	(void)ini.ReadBaseBool("Platform", "IniWarnUnknownKeys", true, "warn about unknown keys");
	(void)ini.ReadGroupSwitch("Combat");
	(void)ini.ReadFeatureSwitch("RadialIndicator", true);
	(void)ini.ReadInt("RadialIndicator", "LargeRange", 7, 0, 100, "the large-range cutoff", "radial.cpp");

	const std::string text = ini.RenderTemplate();
	Check(Contains(text, "[Platform]"), "the template renders the [Platform] section");
	Check(Contains(text, "IniWarnUnknownKeys=yes"), "the template renders the declared default of a built-in key");
	Check(!Contains(text, "IniWarnUnknownKeys=no"), "a local [Platform] override does not leak into the template");
	Check(Contains(text, "[Groups]"), "the template renders the [Groups] section");
	Check(Contains(text, "Combat=yes"), "the template renders the group switch default");
	Check(!Contains(text, "Combat=no"), "a local [Groups] override does not leak into the template");
	Check(Contains(text, "[Features]"), "the template renders the [Features] section");
	Check(Contains(text, "RadialIndicator=yes"), "the template renders the feature switch defaultEnabled");
	Check(Contains(text, "[RadialIndicator]"), "the template renders a feature section");
	Check(Contains(text, "LargeRange=7"), "the template renders the declared default of a feature key");
	Check(!Contains(text, "LargeRange=42"), "a local feature-key override does not leak into the template");
	Check(Contains(text, "DECLARED DEFAULT"), "the template explains that it lists declared defaults");
	Check(Contains(text, "the large-range cutoff"), "the template renders the description");
	Check(Contains(text, "range: 0..100"), "the template renders the integer range");
	Check(Contains(text, "feature: radial"), "the template names the owning feature");
}


// INI-03: an invalid [Features] switch must be part of the startup summary, and a
// value consumed after that snapshot ([Platform] IniTakeover is read at the
// rules-INI stage) is reported separately instead of silently missing.
void TestInvalidSwitchAndLateInvalid()
{
	ResetRegistry();
	venture::IniRegistry& ini = venture::IniRegistry::Instance();

	Check(ini.InvalidSwitchCount() == 0, "no invalid switch is counted before a read");

	SetFileValue("Features", "DefaultOff", "maybe");
	g_logs.clear();
	RunFeatureGate();
	Check(ini.InvalidSwitchCount() == 1, "an invalid [Features] switch value is counted");
	Check(SawWarning("[Features] DefaultOff: 'maybe' is not a valid bool"),
		"the invalid feature switch still warns");

	const venture::IniScanResult scan = ini.ScanUnknownKeys(true);
	Check(scan.invalidSwitches == 1, "the scan result reports the invalid switch count");
	Check(scan.invalid == 1, "the startup summary's invalid counter includes the invalid switch");
	Check(ini.LateInvalidCount() == 0, "a value resolved before the summary snapshot is not late");

	// The rules-INI stage reads this key after the summary was written.
	SetFileValue("Platform", "IniTakeover", "sometimes");
	g_logs.clear();
	Check(ini.ReadBaseBool("Platform", "IniTakeover", true, "takeover switch"),
		"an invalid IniTakeover falls back to its declared default (yes)");
	Check(ini.IsKeyInvalid("Platform", "IniTakeover"), "the late-invalid key can be queried by name");
	Check(ini.LateInvalidCount() == 1, "a value resolved after the summary is counted as late");
	Check(SawWarning("is not a valid bool"), "the registry warns about the late invalid value");

	// The flag is per key: reading the same bad value again is not a second count.
	(void)ini.ReadBaseBool("Platform", "IniTakeover", true, "takeover switch");
	Check(ini.LateInvalidCount() == 1, "the same bad value is not double-counted");

	// A key resolved from a valid/missing value is not reported invalid.
	Check(!ini.IsKeyInvalid("Platform", "DumpIniTemplate"), "a key that was never invalid is not reported invalid");
}

// INI-08: a pre-2.0 legacy getter bypasses validation, but the key is recorded as
// known so the scan/template see it.
void TestMarkKnown()
{
	ResetRegistry();
	venture::IniRegistry& ini = venture::IniRegistry::Instance();
	const int builtinKeys = ini.KeyCount();

	ini.BeginFeature("legacy_feature");
	ini.MarkKnown("LegacyFeature", "Mode", venture::IniValueType::String, "safe");
	ini.EndFeature();
	Check(ini.KeyCount() == builtinKeys + 1, "MarkKnown registers the legacy-read key once");

	SetFileValue("LegacyFeature", "Mode", "fast");
	const venture::IniKeyInfo* const info = FindInfo(ini, "LegacyFeature", "Mode");
	Check(info && info->type == venture::IniValueType::String, "the legacy key keeps its declared type");
	Check(info && info->defaultText == "safe", "the legacy default is recorded");
	Check(info && info->invalid == false, "a legacy key is never marked invalid (it is not validated)");
	Check(info && info->featureId == "legacy_feature", "the owning feature is recorded");
	Check(info && info->valueText == "safe",
		"the effective value is not recorded - the legacy getter stays lenient and unvalidated");

	const venture::IniScanResult scan = ini.ScanUnknownKeys(true);
	Check(scan.unknown == 0, "a key read through a legacy getter is not reported as unknown");
	Check(!SawWarning("unknown key [LegacyFeature] Mode"), "no unknown-key warning for the marked key");
	Check(Contains(ini.RenderTemplate(), "Mode=safe"), "the template lists the legacy-read key");
	Check(Contains(ini.RenderTemplate(), "legacy Ini::Get*"), "the template flags it as a legacy read");

	// A declarative declaration wins over a later MarkKnown, and a first MarkKnown
	// declaration wins over a second one (first declaration wins, like the trio).
	ini.ReadInt("Numbers", "Count", 5, 0, 10, "declared with the trio", "declared.cpp");
	ini.MarkKnown("Numbers", "Count", venture::IniValueType::String, "x");
	const venture::IniKeyInfo* const declared = FindInfo(ini, "Numbers", "Count");
	Check(declared && declared->type == venture::IniValueType::Int, "MarkKnown does not override a declaration");

	ini.MarkKnown("LegacyFeature", "Mode", venture::IniValueType::Int, "7");
	const venture::IniKeyInfo* const first = FindInfo(ini, "LegacyFeature", "Mode");
	Check(first && first->type == venture::IniValueType::String && first->defaultText == "safe",
		"MarkKnown keeps the first declaration");

	const int keysBefore = ini.KeyCount();
	ini.MarkKnown(nullptr, "X", venture::IniValueType::Bool, "no");
	ini.MarkKnown("", "X", venture::IniValueType::Bool, "no");
	ini.MarkKnown("LegacyFeature", "", venture::IniValueType::Bool, "no");
	Check(ini.KeyCount() == keysBefore, "null/empty MarkKnown arguments are ignored");
}

// INI-06: the profile API and the caller's buffer are hard size limits, but the
// truncation is diagnosed and counted instead of being silent.
void TestTruncationDiagnostics()
{
	ResetRegistry();
	venture::IniRegistry& ini = venture::IniRegistry::Instance();

	static const std::string kLongValue(511, 'A'); // exactly the provider buffer limit
	SetFileValue("Big", "Raw", kLongValue.c_str());

	char buffer[16] = {};
	g_logs.clear();
	ini.ReadString("Big", "Raw", "fallback", buffer, sizeof(buffer), "a long raw value", "unit_test.cpp");
	Check(std::strlen(buffer) == 15, "the caller buffer is filled up to its limit and NUL-terminated");
	Check(SawWarning("fills the profile API buffer"), "a completely filled provider buffer warns about truncation");
	Check(SawDebug("does not fit the caller buffer"), "a caller-buffer truncation is diagnosed at Debug level");
	Check(ini.TruncatedCount() == 2, "both truncation diagnostics are counted");

	const venture::IniScanResult scan = ini.ScanUnknownKeys(true);
	Check(scan.truncated == 2, "the startup scan result exposes the truncation count");

	// A value exactly one character short of the limit is not diagnosed.
	ResetRegistry();
	static const std::string kFitsValue(510, 'B');
	SetFileValue("Big", "Fits", kFitsValue.c_str());
	g_logs.clear();
	char bigBuffer[512] = {};
	ini.ReadString("Big", "Fits", "fallback", bigBuffer, sizeof(bigBuffer), "a fitting value", "unit_test.cpp");
	Check(!SawWarning("fills the profile API buffer"), "a value that fits the provider buffer is not diagnosed");
	Check(ini.TruncatedCount() == 0, "no truncation is counted for a fitting value");
}

// API-06: the 2.0 feature-side INI API (VENTURE_INI_* + FeatureContext adapters)
// had no test and no consumer; this exercises the whole adapter path.
void TestFeatureIniApi()
{
	ResetRegistry();

	venture::FeatureContext context{};
	context.platformName = "VentureCore";
	context.IniBool = &AdapterIniBool;
	context.IniInt = &AdapterIniInt;
	context.IniString = &AdapterIniString;

	SetFileValue("MyFeature", "LogEveryUnit", "no");
	SetFileValue("MyFeature", "ProgressEvery", "250");
	SetFileValue("MyFeature", "Mode", "fast");

	const bool logEveryUnit = VENTURE_INI_BOOL(
		context, "MyFeature", "LogEveryUnit", true, "log one line per handled unit");
	const int progressEvery = VENTURE_INI_INT(
		context, "MyFeature", "ProgressEvery", 100, 0, 10000, "log one line every N units");
	char mode[16] = {};
	VENTURE_INI_STRING(
		context, "MyFeature", "Mode", "safe", mode, sizeof(mode), "which strategy to use");

	Check(!logEveryUnit, "VENTURE_INI_BOOL returns the value read through the adapter");
	Check(progressEvery == 250, "VENTURE_INI_INT returns the value read through the adapter");
	Check(std::strcmp(mode, "fast") == 0, "VENTURE_INI_STRING copies the value through the adapter");

	venture::IniRegistry& ini = venture::IniRegistry::Instance();
	const venture::IniKeyInfo* const info = FindInfo(ini, "MyFeature", "ProgressEvery");
	Check(info && info->type == venture::IniValueType::Int
		&& info->minValue == 0 && info->maxValue == 10000,
		"the macro forwards type/default/range to the registry");
	Check(info && Contains(info->description, "log one line every N units"),
		"the macro forwards the description");
	Check(info && Contains(info->sourceFile, "test_ini_registry.cpp"),
		"the macro passes __FILE__ as the declaration site");

	// A missing key returns the macro-declared default through the same path.
	Check(VENTURE_INI_BOOL(context, "MyFeature", "Missing", true, "absent"),
		"a missing bool key returns the macro default");
	char modeBuffer[16] = {};
	VENTURE_INI_STRING(context, "MyFeature", "Missing", "fallback", modeBuffer, sizeof(modeBuffer), "absent");
	Check(std::strcmp(modeBuffer, "fallback") == 0, "a missing string key returns the macro default");
}

// START-01: Warning/Error lines logged before Log::Open() (the command/event
// registrars run as file-static initializers) are buffered and flushed into the
// log file when it opens, instead of being lost.
void TestStaticPhaseLogBuffering()
{
	using venture::Log;

	// %TEMP% keeps the test independent of the Wine working directory (the same
	// pattern as tests/test_logging.cpp).
	char tempDirectory[MAX_PATH] = {};
	const DWORD tempLength = GetTempPathA(static_cast<DWORD>(sizeof(tempDirectory)), tempDirectory);
	Check(tempLength > 0 && tempLength < sizeof(tempDirectory), "resolve %TEMP% for the pre-open log test");
	char path[MAX_PATH * 2] = {};
	_snprintf_s(
		path,
		sizeof(path),
		_TRUNCATE,
		"%sventurecore-preopen-%lu.log",
		tempDirectory,
		GetCurrentProcessId());
	const char* const kPath = path;

	Log::Close();
	Log::SetMinLevel(venture::LogLevel::Info);
	Log::DiscardPending();
	DeleteFileA(kPath);

	Check(Log::PendingCount() == 0, "the pre-open diagnostic queue starts empty");
	Log::Warn("[Venture] could not register the example command (result %d)", 7);
	Log::Info("[Venture] an informational startup line is not buffered");
	Check(Log::PendingCount() == 1, "a warning logged before Open() is buffered (START-01)");

	Check(Log::OpenAt(kPath), "the log opens and flushes the buffered startup diagnostics");
	Check(Log::PendingCount() == 0, "OpenAt drains the pre-open diagnostic queue");
	Log::Warn("[Venture] a line logged after Open() is written directly");
	Log::Close();

	std::string contents;
	Check(ReadWholeTextFile(kPath, contents), "the log file can be read after Close()");
	Check(Contains(contents, "=== "), "the log still starts with the process separator header");
	Check(Contains(contents, "could not register the example command (result 7)"),
		"the buffered static-initializer warning reached the log file");
	Check(contents.find("could not register the example command") > contents.find("=== "),
		"the buffered line is written after the separator header");
	Check(!Contains(contents, "an informational startup line is not buffered"),
		"Info lines logged before Open() are not queued");
	DeleteFileA(kPath);
}

int __cdecl main()
{
	WireRegistry();

	// START-01 first: it needs the logger in its initial (never opened) state.
	TestStaticPhaseLogBuffering();

	TestBoolReads();
	TestIntReads();
	TestStringReads();
	TestDuplicateRegistration();
	TestDuplicateValueSource();
	TestBuiltinPlatformKeys();
	TestFeatureDescriptorIniSemantics();
	TestInvalidSwitchAndLateInvalid();
	TestUnknownKeyScan();
	TestTemplateRender();
	TestMarkKnown();
	TestTruncationDiagnostics();
	TestFeatureIniApi();

	std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAILED",
		g_failures, g_failures == 1 ? "" : "s");
	return g_failures == 0 ? 0 : 1;
}
