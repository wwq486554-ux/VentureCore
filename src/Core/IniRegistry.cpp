#include "Core/IniRegistry.h"

#include "Core/PlatformConfig.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
// ---------------------------------------------------------------------------
// Small string helpers (ASCII only: keys and values are INI tokens)
// ---------------------------------------------------------------------------

char LowerAscii(const char character)
{
	return (character >= 'A' && character <= 'Z')
		? static_cast<char>(character - 'A' + 'a')
		: character;
}

std::string Trim(const std::string& text)
{
	std::size_t begin = 0;
	std::size_t end = text.size();
	while (begin < end && (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r' || text[begin] == '\n'))
		++begin;
	while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r' || text[end - 1] == '\n'))
		--end;
	return text.substr(begin, end - begin);
}

bool EqualsIgnoreCase(const std::string& left, const std::string& right)
{
	if (left.size() != right.size())
		return false;
	for (std::size_t index = 0; index < left.size(); ++index)
	{
		if (LowerAscii(left[index]) != LowerAscii(right[index]))
			return false;
	}
	return true;
}

std::string LowerAscii(const std::string& text)
{
	std::string result = text;
	for (char& character : result)
		character = LowerAscii(character);
	return result;
}

std::string BoolText(const bool value)
{
	return value ? "yes" : "no";
}

std::string IntText(const int value)
{
	char text[24] = {};
	_snprintf_s(text, sizeof(text), _TRUNCATE, "%d", value);
	return text;
}

const char* TypeText(const venture::IniValueType type)
{
	switch (type)
	{
	case venture::IniValueType::Bool:   return "bool";
	case venture::IniValueType::Int:    return "int";
	case venture::IniValueType::String: return "string";
	default:                            return "?";
	}
}

bool ParseBoolToken(const std::string& text, bool& out)
{
	if (EqualsIgnoreCase(text, "yes") || EqualsIgnoreCase(text, "true") || EqualsIgnoreCase(text, "on")
		|| text == "1")
	{
		out = true;
		return true;
	}
	if (EqualsIgnoreCase(text, "no") || EqualsIgnoreCase(text, "false") || EqualsIgnoreCase(text, "off")
		|| text == "0")
	{
		out = false;
		return true;
	}
	return false;
}

// Strict integer token: the whole (trimmed) text must be one base-10 number.
// The pre-2.0 getter accepted a numeric prefix ("12abc" parsed as 12) and had no
// overflow check; the registry rejects both and falls back to the default.
bool ParseIntToken(const std::string& text, long& out)
{
	if (text.empty())
		return false;

	errno = 0;
	const char* const begin = text.c_str();
	char* end = nullptr;
	const long parsed = std::strtol(begin, &end, 10);
	if (end == begin || errno == ERANGE)
		return false;

	// Allow trailing whitespace only; anything else is garbage.
	while (*end == ' ' || *end == '\t')
		++end;
	if (*end != '\0')
		return false;

	out = parsed;
	return true;
}

int LevenshteinDistance(const std::string& left, const std::string& right)
{
	const std::size_t leftSize = left.size();
	const std::size_t rightSize = right.size();
	std::vector<std::size_t> previous(rightSize + 1);
	std::vector<std::size_t> current(rightSize + 1);
	for (std::size_t column = 0; column <= rightSize; ++column)
		previous[column] = column;

	for (std::size_t row = 1; row <= leftSize; ++row)
	{
		current[0] = row;
		for (std::size_t column = 1; column <= rightSize; ++column)
		{
			const std::size_t substitution = previous[column - 1]
				+ (left[row - 1] == right[column - 1] ? 0u : 1u);
			const std::size_t insertion = current[column - 1] + 1;
			const std::size_t deletion = previous[column] + 1;
			current[column] = (std::min)(substitution, (std::min)(insertion, deletion));
		}
		previous.swap(current);
	}
	return static_cast<int>(previous[rightSize]);
}

void AppendLine(std::string& out, const std::string& line)
{
	out += line;
	out += "\r\n";
}

// The [Platform] keys the base itself reads.  Registering them up front makes
// them known to the unknown-key scan and gives the template dump their
// type/default/description even before the first read (e.g. IniTakeover is only
// consumed when the engine parses rules(md).ini, after the startup scan).
struct BuiltinKey
{
	const char* key;
	venture::IniValueType type;
	const char* defaultText;
	const char* description;
};

const BuiltinKey kBuiltinKeys[] = {
	{ "LogLevel", venture::IniValueType::String, "info",
		"minimum log level: error|warn|info|debug" },
	{ "VerboseLogging", venture::IniValueType::Bool, "no",
		"legacy alias: yes is the same as LogLevel=debug" },
	{ "DebugOutput", venture::IniValueType::Bool, "no",
		"RELEASE builds: also write the log to the attached debugger" },
	{ "LineagePolicy", venture::IniValueType::String, "refuse",
		"conflict of a second DLL of this lineage: refuse|warn" },
	{ "IniTakeover", venture::IniValueType::Bool, "yes",
		"hide platform-owned rules(md).ini keys from host platforms loaded next to us" },
	{ "IniWarnUnknownKeys", venture::IniValueType::Bool, "yes",
		"warn about unknown <Platform>.ini keys at startup" },
	{ "DumpIniTemplate", venture::IniValueType::Bool, "no",
		"development: write <Platform>.ini.generated next to the log" },
};
}

namespace venture
{
IniRegistry& IniRegistry::Instance()
{
	static IniRegistry registry;
	return registry;
}

IniRegistry::IniRegistry()
{
	RegisterBuiltinKeys();
}

void IniRegistry::SetRawReader(const IniRawStringFn reader)
{
	rawReader_ = reader;
}

void IniRegistry::SetEnumerator(const IniEnumerateFn enumerator)
{
	enumerator_ = enumerator;
}

void IniRegistry::SetLogSink(const IniLogFn sink)
{
	logSink_ = sink;
}

void IniRegistry::BeginFeature(const char* const featureId)
{
	currentFeatureId_ = featureId ? featureId : "";
}

void IniRegistry::EndFeature()
{
	currentFeatureId_.clear();
}

void IniRegistry::Reset()
{
	keys_.clear();
	switches_.clear();
	currentFeatureId_.clear();
	scanWarnUnknown_ = false;
	scanUnknownCount_ = 0;
	scanReportedSections_.clear();
	RegisterBuiltinKeys();
}

void IniRegistry::RegisterBuiltinKeys()
{
	for (const BuiltinKey& builtin : kBuiltinKeys)
	{
		IniKeyInfo info{};
		info.section = VC_SECTION_PLATFORM;
		info.key = builtin.key;
		info.type = builtin.type;
		info.description = builtin.description;
		info.defaultText = builtin.defaultText;
		info.valueText = builtin.defaultText;
		info.invalid = false;
		info.hasRange = false;
		info.minValue = 0;
		info.maxValue = 0;
		keys_.push_back(info);
	}
}

IniKeyInfo* IniRegistry::FindKey(const char* const section, const char* const key)
{
	if (!section || !key)
		return nullptr;
	const std::string sectionText = Trim(section);
	const std::string keyText = Trim(key);
	for (IniKeyInfo& info : keys_)
	{
		if (EqualsIgnoreCase(info.section, sectionText) && EqualsIgnoreCase(info.key, keyText))
			return &info;
	}
	return nullptr;
}

const IniKeyInfo* IniRegistry::FindKey(const char* const section, const char* const key) const
{
	if (!section || !key)
		return nullptr;
	const std::string sectionText = Trim(section);
	const std::string keyText = Trim(key);
	for (const IniKeyInfo& info : keys_)
	{
		if (EqualsIgnoreCase(info.section, sectionText) && EqualsIgnoreCase(info.key, keyText))
			return &info;
	}
	return nullptr;
}

IniKeyInfo& IniRegistry::RegisterKey(
	const char* const section,
	const char* const key,
	const IniValueType type,
	const std::string& defaultText,
	const char* const description,
	const char* const sourceFile,
	const bool hasRange,
	const int minValue,
	const int maxValue)
{
	if (IniKeyInfo* const existing = FindKey(section, key))
	{
		if (!currentFeatureId_.empty() && !existing->featureId.empty()
			&& !EqualsIgnoreCase(existing->featureId, currentFeatureId_))
		{
			Emit(true,
				"duplicate ini key [" + existing->section + "] " + existing->key
				+ " registered by feature '" + currentFeatureId_ + "' and feature '"
				+ existing->featureId + "' - keeping the first declaration ("
				+ existing->sourceFile + ")");
		}
		else if (!currentFeatureId_.empty() && existing->featureId.empty())
		{
			Emit(true,
				"ini key [" + existing->section + "] " + existing->key
				+ " is registered by the base - the feature '" + currentFeatureId_
				+ "' declaration is ignored");
		}
		return *existing;
	}

	IniKeyInfo info{};
	info.section = Trim(section ? section : "");
	info.key = Trim(key ? key : "");
	info.type = type;
	info.description = description ? description : "";
	info.featureId = currentFeatureId_;
	info.sourceFile = sourceFile ? sourceFile : "";
	info.defaultText = defaultText;
	info.valueText = defaultText;
	info.invalid = false;
	info.hasRange = hasRange;
	info.minValue = minValue;
	info.maxValue = maxValue;
	keys_.push_back(info);
	return keys_.back();
}

bool IniRegistry::HasSection(const char* const section) const
{
	if (!section)
		return false;
	const std::string sectionText = Trim(section);
	for (const IniKeyInfo& info : keys_)
	{
		if (EqualsIgnoreCase(info.section, sectionText))
			return true;
	}
	return false;
}

bool IniRegistry::IsKnownSwitch(const char* const key) const
{
	if (!key)
		return false;
	const std::string keyText = Trim(key);
	for (const IniFeatureSwitch& featureSwitch : switches_)
	{
		if (featureSwitch.iniKey && EqualsIgnoreCase(Trim(featureSwitch.iniKey), keyText))
			return true;
	}
	return false;
}

IniRegistry::RawValue IniRegistry::ReadRaw(const char* const section, const char* const key) const
{
	RawValue result{false, std::string()};
	if (!rawReader_ || !section || !key)
		return result;

	char buffer[512] = {};
	rawReader_(section, key, kIniMissingValue, buffer, static_cast<unsigned long>(sizeof(buffer)));
	if (std::strcmp(buffer, kIniMissingValue) == 0)
		return result;

	result.present = true;
	result.text = Trim(buffer);
	return result;
}

void IniRegistry::UpdateResolved(IniKeyInfo& info, const std::string& valueText, const bool invalid)
{
	info.valueText = valueText;
	info.invalid = invalid;
}

void IniRegistry::Emit(const bool warning, const std::string& text) const
{
	if (!logSink_ || text.empty())
		return;
	logSink_(warning, text.c_str());
}

void IniRegistry::EmitValue(const IniKeyInfo& info, const bool present) const
{
	std::string text = "key [" + info.section + "] " + info.key + " = " + info.valueText;
	if (present)
		text += " (default: " + info.defaultText + ")";
	else
		text += " (default applied)";
	if (info.invalid)
		text += " [invalid value rejected]";
	Emit(false, text);
}

bool IniRegistry::ReadBool(
	const char* const section,
	const char* const key,
	const bool defaultValue,
	const char* const description,
	const char* const sourceFile)
{
	IniKeyInfo& info = RegisterKey(section, key, IniValueType::Bool, BoolText(defaultValue), description, sourceFile);
	const RawValue raw = ReadRaw(info.section.c_str(), info.key.c_str());

	bool value = defaultValue;
	bool invalid = false;
	if (raw.present && !ParseBoolToken(raw.text, value))
	{
		value = defaultValue;
		invalid = true;
	}

	UpdateResolved(info, BoolText(value), invalid);
	EmitValue(info, raw.present);
	if (invalid)
	{
		Emit(true,
			"key [" + info.section + "] " + info.key + ": '" + raw.text
			+ "' is not a valid bool - using the default '" + info.defaultText + "'");
	}
	return value;
}

int IniRegistry::ReadInt(
	const char* const section,
	const char* const key,
	const int defaultValue,
	const int minValue,
	const int maxValue,
	const char* const description,
	const char* const sourceFile)
{
	IniKeyInfo& info = RegisterKey(
		section, key, IniValueType::Int, IntText(defaultValue), description, sourceFile,
		true, minValue, maxValue);
	const RawValue raw = ReadRaw(info.section.c_str(), info.key.c_str());

	int value = defaultValue;
	bool invalid = false;
	std::string reason;
	if (raw.present)
	{
		long parsed = 0;
		if (!ParseIntToken(raw.text, parsed))
		{
			invalid = true;
			reason = "is not a valid integer";
		}
		else if (parsed < INT_MIN || parsed > INT_MAX)
		{
			invalid = true;
			reason = "is outside the 32-bit range";
		}
		else
		{
			const int candidate = static_cast<int>(parsed);
			if (minValue <= maxValue && (candidate < minValue || candidate > maxValue))
			{
				invalid = true;
				reason = "is outside the allowed range " + IntText(minValue) + ".." + IntText(maxValue);
			}
			else
			{
				value = candidate;
			}
		}
	}

	UpdateResolved(info, IntText(value), invalid);
	EmitValue(info, raw.present);
	if (invalid)
	{
		Emit(true,
			"key [" + info.section + "] " + info.key + ": '" + raw.text + "' " + reason
			+ " - using the default '" + info.defaultText + "'");
	}
	return value;
}

void IniRegistry::ReadString(
	const char* const section,
	const char* const key,
	const char* const defaultValue,
	char* const buffer,
	const unsigned long bufferSize,
	const char* const description,
	const char* const sourceFile)
{
	const std::string defaultText = defaultValue ? defaultValue : "";
	IniKeyInfo& info = RegisterKey(section, key, IniValueType::String, defaultText, description, sourceFile);
	const RawValue raw = ReadRaw(info.section.c_str(), info.key.c_str());
	const std::string value = raw.present ? raw.text : defaultText;

	UpdateResolved(info, value, false);
	if (buffer && bufferSize > 0)
	{
		_snprintf_s(
			buffer,
			static_cast<std::size_t>(bufferSize),
			_TRUNCATE,
			"%s",
			value.c_str());
	}
	EmitValue(info, raw.present);
}

bool IniRegistry::ReadBaseBool(
	const char* const section,
	const char* const key,
	const bool defaultValue,
	const char* const description)
{
	// Base reads happen outside a feature Initialize(), so the owner stays empty.
	return ReadBool(section, key, defaultValue, description, nullptr);
}

void IniRegistry::ReadBaseString(
	const char* const section,
	const char* const key,
	const char* const defaultValue,
	char* const buffer,
	const unsigned long bufferSize,
	const char* const description)
{
	ReadString(section, key, defaultValue, buffer, bufferSize, description, nullptr);
}

bool IniRegistry::ReadFeatureSwitch(const char* const iniKey, const bool defaultEnabled)
{
	const std::string key = Trim(iniKey ? iniKey : "");
	const RawValue raw = ReadRaw(VC_SECTION_FEATURES, key.c_str());

	bool value = defaultEnabled;
	bool invalid = false;
	if (raw.present && !ParseBoolToken(raw.text, value))
	{
		value = defaultEnabled;
		invalid = true;
	}

	std::string text = "switch [" VC_SECTION_FEATURES "] " + key + " = " + BoolText(value);
	if (raw.present)
		text += " (default: " + BoolText(defaultEnabled) + ")";
	else
		text += " (default applied)";
	if (invalid)
		text += " [invalid value rejected]";
	Emit(false, text);
	if (invalid)
	{
		Emit(true,
			"[" VC_SECTION_FEATURES "] " + key + ": '" + raw.text
			+ "' is not a valid bool - using the default '" + BoolText(defaultEnabled) + "'");
	}
	return value;
}

bool IniRegistry::ReadGroupSwitch(const char* const group)
{
	if (!group || group[0] == '\0')
		return true;

	// The group master switch is a real [Groups] key: register it so the
	// unknown-key scan and the template dump know it.  Two descriptors sharing a
	// group pass through here twice; the key is registered once (no warning).
	const std::string savedFeature = currentFeatureId_;
	currentFeatureId_.clear();
	const bool value = ReadBool(
		VC_SECTION_GROUPS,
		group,
		true,
		"feature group master switch ([Groups]; the features of a group are disabled together)",
		nullptr);
	currentFeatureId_ = savedFeature;
	return value;
}

void IniRegistry::SetFeatureSwitches(const IniFeatureSwitch* const switches, const int count)
{
	switches_.clear();
	if (!switches || count <= 0)
		return;
	switches_.reserve(static_cast<std::size_t>(count));
	for (int index = 0; index < count; ++index)
		switches_.push_back(switches[index]);
}

IniScanResult IniRegistry::ScanUnknownKeys(const bool warnUnknown)
{
	IniScanResult result{KeyCount(), InvalidCount(), 0};
	scanWarnUnknown_ = warnUnknown;
	scanUnknownCount_ = 0;
	scanReportedSections_.clear();

	if (enumerator_)
		enumerator_(&IniRegistry::ScanTrampoline, this);

	result.unknown = scanUnknownCount_;
	scanUnknownCount_ = 0;
	scanReportedSections_.clear();
	return result;
}

void IniRegistry::ScanTrampoline(const char* section, const char* key, void* const userData)
{
	if (userData)
		static_cast<IniRegistry*>(userData)->ScanEntry(section, key);
}

void IniRegistry::ScanEntry(const char* const rawSection, const char* const rawKey)
{
	const std::string section = Trim(rawSection ? rawSection : "");
	const std::string key = Trim(rawKey ? rawKey : "");
	if (section.empty() || key.empty())
		return;

	// A registered key (built-in [Platform] key, group switch or feature key).
	if (FindKey(section.c_str(), key.c_str()))
		return;

	// [Features]: known through the descriptor switch set.
	if (EqualsIgnoreCase(section, VC_SECTION_FEATURES))
	{
		if (IsKnownSwitch(key.c_str()))
			return;

		++scanUnknownCount_;
		std::string message = "unknown feature switch [" VC_SECTION_FEATURES "] " + key + " - ignored";
		const std::string suggestion = SuggestSwitch(key.c_str());
		if (!suggestion.empty())
			message += " (did you mean " + suggestion + "?)";
		EmitScanMessage(true, message);
		return;
	}

	// Unknown key in a section the platform owns ([Platform], [Groups] or a
	// feature section): warn, the user probably mis-typed a key.
	if (HasSection(section.c_str()))
	{
		++scanUnknownCount_;
		EmitScanMessage(true, "unknown key [" + section + "] " + key + " - ignored");
		return;
	}

	// A user-defined / third-party section: Debug only, one line per section.
	++scanUnknownCount_;
	bool reported = false;
	for (const std::string& reportedSection : scanReportedSections_)
	{
		if (EqualsIgnoreCase(reportedSection, section))
		{
			reported = true;
			break;
		}
	}
	if (!reported)
	{
		scanReportedSections_.push_back(section);
		Emit(false, "ignoring user-defined section [" + section + "] (unknown-key warnings cover platform/feature sections only)");
	}
}

void IniRegistry::EmitScanMessage(const bool warningCandidate, const std::string& text)
{
	Emit(scanWarnUnknown_ && warningCandidate, text);
}

std::string IniRegistry::SuggestSwitch(const char* const unknown) const
{
	if (!unknown)
		return std::string();

	const std::string needle = LowerAscii(Trim(unknown));
	std::string best;
	int bestDistance = 3; // accept a suggestion only for distance <= 2
	for (const IniFeatureSwitch& featureSwitch : switches_)
	{
		if (!featureSwitch.iniKey)
			continue;
		const std::string candidate = Trim(featureSwitch.iniKey);
		if (EqualsIgnoreCase(candidate, Trim(unknown)))
			return candidate;
		const int distance = LevenshteinDistance(needle, LowerAscii(candidate));
		if (distance < bestDistance)
		{
			bestDistance = distance;
			best = candidate;
		}
	}
	return bestDistance <= 2 ? best : std::string();
}

std::string IniRegistry::RenderTemplate() const
{
	std::string out;

	// [Platform]
	AppendLine(out, "[" VC_SECTION_PLATFORM "]");
	AppendLine(out, "; Platform-wide options, read by the base.");
	for (const IniKeyInfo& info : keys_)
	{
		if (!EqualsIgnoreCase(info.section, VC_SECTION_PLATFORM))
			continue;
		std::string annotation = "; " + info.description;
		annotation += " (type: " + std::string(TypeText(info.type)) + ", default: " + info.defaultText + ")";
		AppendLine(out, annotation);
		AppendLine(out, info.key + "=" + info.valueText);
	}
	out += "\r\n";

	// [Groups]
	AppendLine(out, "[" VC_SECTION_GROUPS "]");
	AppendLine(out, "; Optional master switch per feature group: no disables every module of the group.");
	bool hasGroup = false;
	for (const IniKeyInfo& info : keys_)
	{
		if (!EqualsIgnoreCase(info.section, VC_SECTION_GROUPS))
			continue;
		hasGroup = true;
		AppendLine(out, "; " + info.description);
		AppendLine(out, info.key + "=" + info.valueText);
	}
	if (!hasGroup)
		AppendLine(out, "; (this platform declares no feature group)");
	out += "\r\n";

	// [Features]
	AppendLine(out, "[" VC_SECTION_FEATURES "]");
	AppendLine(out, "; One switch per feature module; yes/no, missing = the module's default.");
	for (const IniFeatureSwitch& featureSwitch : switches_)
	{
		if (!featureSwitch.iniKey)
			continue;
		std::string annotation = "; " + std::string(featureSwitch.name ? featureSwitch.name : featureSwitch.iniKey);
		if (featureSwitch.group && featureSwitch.group[0] != '\0')
			annotation += " (group: " + std::string(featureSwitch.group) + ")";
		annotation += std::string(", default: ") + BoolText(featureSwitch.defaultEnabled);
		AppendLine(out, annotation);
		AppendLine(out, std::string(featureSwitch.iniKey) + "=" + BoolText(featureSwitch.defaultEnabled));
	}

	// Feature sections, sorted by section name for a stable file.  Keys of one
	// section keep their registration order.
	std::vector<std::string> sections;
	for (const IniKeyInfo& info : keys_)
	{
		if (EqualsIgnoreCase(info.section, VC_SECTION_PLATFORM) || EqualsIgnoreCase(info.section, VC_SECTION_GROUPS))
			continue;
		bool known = false;
		for (const std::string& section : sections)
		{
			if (EqualsIgnoreCase(section, info.section))
			{
				known = true;
				break;
			}
		}
		if (!known)
			sections.push_back(info.section);
	}
	std::sort(sections.begin(), sections.end(), [](const std::string& left, const std::string& right) {
		return LowerAscii(left) < LowerAscii(right);
	});

	for (const std::string& section : sections)
	{
		out += "\r\n";
		AppendLine(out, "[" + section + "]");
		for (const IniKeyInfo& info : keys_)
		{
			if (!EqualsIgnoreCase(info.section, section))
				continue;
			std::string annotation = "; " + info.description;
			annotation += " (type: " + std::string(TypeText(info.type)) + ", default: " + info.defaultText;
			if (info.hasRange)
				annotation += ", range: " + IntText(info.minValue) + ".." + IntText(info.maxValue);
			if (!info.featureId.empty())
				annotation += ", feature: " + info.featureId;
			annotation += ")";
			AppendLine(out, annotation);
			AppendLine(out, info.key + "=" + info.valueText);
		}
	}

	return out;
}

int IniRegistry::KeyCount() const
{
	return static_cast<int>(keys_.size());
}

int IniRegistry::InvalidCount() const
{
	int invalid = 0;
	for (const IniKeyInfo& info : keys_)
	{
		if (info.invalid)
			++invalid;
	}
	return invalid;
}

const IniKeyInfo* IniRegistry::KeyAt(const int index) const
{
	if (index < 0 || index >= KeyCount())
		return nullptr;
	return &keys_[static_cast<std::size_t>(index)];
}

int IniRegistry::SwitchCount() const
{
	return static_cast<int>(switches_.size());
}

const IniFeatureSwitch* IniRegistry::SwitchAt(const int index) const
{
	if (index < 0 || index >= SwitchCount())
		return nullptr;
	return &switches_[static_cast<std::size_t>(index)];
}
}
