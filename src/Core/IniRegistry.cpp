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

// The raw-value buffer handed to the provider.  A completely filled buffer is
// what the INI-06 truncation diagnostic is based on (the Win32 profile API
// cannot return more characters than this).
constexpr std::size_t kRawValueBufferSize = 512;

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

// Duplicate declarations are resolved with the registered declaration (INI-01);
// a bool default is stored normalised so "YES" and "yes" are the same
// declaration, while a string default compares byte for byte.
bool SameDefaultText(const venture::IniKeyInfo& info, const std::string& defaultText)
{
	if (info.type == venture::IniValueType::Bool)
		return EqualsIgnoreCase(info.defaultText, defaultText);
	return info.defaultText == defaultText;
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
	invalidSwitchCount_ = 0;
	truncatedCount_ = 0;
	summaryInvalidTotal_ = 0;
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
		// INI-01: the first declaration is the single source of truth.  Every
		// read - not only this call site - is resolved with the registered
		// type/default/range by the Read* methods, so a later declaration can no
		// longer overwrite the stored value/invalid state with a value judged by
		// its own default and range while the warning still cited the first one.
		const bool declarationDiffers = existing->type != type
			|| !SameDefaultText(*existing, defaultText)
			|| existing->hasRange != hasRange
			|| (hasRange && (existing->minValue != minValue || existing->maxValue != maxValue));
		const bool baseOwned = existing->featureId.empty();
		const bool ownerDiffers = !currentFeatureId_.empty() && !baseOwned
			&& !EqualsIgnoreCase(existing->featureId, currentFeatureId_);
		const bool declaredByAnother = ownerDiffers
			|| (!currentFeatureId_.empty() && baseOwned);

		if (!existing->duplicateWarned && (declarationDiffers || declaredByAnother))
		{
			existing->duplicateWarned = true;
			const std::string callerOwner = currentFeatureId_.empty()
				? "the base"
				: "feature '" + currentFeatureId_ + "'";
			const std::string registeredOwner = existing->featureId.empty()
				? "the base"
				: "feature '" + existing->featureId + "'";
			const std::string callerSource = sourceFile ? sourceFile : "<unknown>";
			const std::string registeredSource =
				existing->sourceFile.empty() ? "<unknown>" : existing->sourceFile;

			std::string message = "duplicate ini key [" + existing->section + "] " + existing->key
				+ " declared by " + callerOwner + " (" + callerSource + ") and "
				+ registeredOwner + " (" + registeredSource
				+ ") - keeping the first declaration; every read is resolved with it (type: "
				+ TypeText(existing->type) + ", default: " + existing->defaultText;
			if (existing->type == IniValueType::Int
				&& existing->hasRange && existing->minValue <= existing->maxValue)
			{
				message += ", range: " + IntText(existing->minValue) + ".." + IntText(existing->maxValue);
			}
			message += ")";
			if (declarationDiffers)
				message += "; this call site's default/range differ and are ignored";
			Emit(true, message);
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
	info.sizeWarned = false;
	info.duplicateWarned = false;
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
	RawValue result{false, false, std::string()};
	if (!rawReader_ || !section || !key)
		return result;

	char buffer[kRawValueBufferSize] = {};
	rawReader_(section, key, kIniMissingValue, buffer, static_cast<unsigned long>(sizeof(buffer)));
	if (std::strcmp(buffer, kIniMissingValue) == 0)
		return result;

	// The provider copies at most sizeof(buffer) - 1 characters (the Win32
	// profile API limit).  A completely filled buffer means the value may have
	// been cut; the read methods warn about that instead of silently validating a
	// fragment (INI-06).
	result.possiblyTruncated = std::strlen(buffer) >= sizeof(buffer) - 1;
	result.present = true;
	result.text = Trim(buffer);
	return result;
}

void IniRegistry::UpdateResolved(IniKeyInfo& info, const std::string& valueText, const bool invalid)
{
	info.valueText = valueText;
	info.invalid = invalid;
}

// INI-06: a raw value that fills the provider buffer may have been cut by the
// profile API.  Report it once per key instead of silently validating the
// fragment; the effective value is still the (possibly shortened) text.
void IniRegistry::NoteRawTruncation(IniKeyInfo& info, const RawValue& raw)
{
	if (!raw.possiblyTruncated || info.sizeWarned)
		return;

	info.sizeWarned = true;
	++truncatedCount_;
	Emit(true,
		"key [" + info.section + "] " + info.key + ": the value fills the profile API buffer ("
		+ IntText(static_cast<int>(kRawValueBufferSize - 1))
		+ " characters) - the effective value may be truncated");
}

// INI-06: the caller's string buffer is a hard limit too; the copy stays safe
// and NUL-terminated (the safe CRT truncation mode), but it is no longer silent.
void IniRegistry::CopyValueToBuffer(
	const IniKeyInfo& info,
	char* const buffer,
	const unsigned long bufferSize,
	const std::string& value)
{
	if (!buffer || bufferSize == 0)
		return;

	if (value.size() >= static_cast<std::size_t>(bufferSize))
	{
		++truncatedCount_;
		Emit(false,
			"key [" + info.section + "] " + info.key + ": the value ("
			+ IntText(static_cast<int>(value.size())) + " characters) does not fit the caller buffer ("
			+ IntText(static_cast<int>(bufferSize)) + " bytes) - truncated");
	}

	_snprintf_s(buffer, static_cast<std::size_t>(bufferSize), _TRUNCATE, "%s", value.c_str());
}

void IniRegistry::EmitTypeMismatch(const IniKeyInfo& info, const IniValueType requested) const
{
	Emit(true,
		"key [" + info.section + "] " + info.key + " is registered as a " + TypeText(info.type)
		+ " key - this " + TypeText(requested)
		+ " read is ignored and returns its own default (one type per key; the first declaration wins)");
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
	if (info.type != IniValueType::Bool)
	{
		EmitTypeMismatch(info, IniValueType::Bool);
		return defaultValue;
	}

	const RawValue raw = ReadRaw(info.section.c_str(), info.key.c_str());
	NoteRawTruncation(info, raw);

	// Resolve with the registered declaration - this call site's on the first
	// read, the first declaration's on a duplicate (INI-01) - so the stored
	// value, the invalid flag, the warning text and the template dump describe
	// exactly one declaration.
	bool fallback = defaultValue;
	(void)ParseBoolToken(info.defaultText, fallback); // keeps the caller default if it is not a token
	bool value = fallback;
	bool invalid = false;
	if (raw.present && !ParseBoolToken(raw.text, value))
	{
		value = fallback;
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
	if (info.type != IniValueType::Int)
	{
		EmitTypeMismatch(info, IniValueType::Int);
		return defaultValue;
	}

	const RawValue raw = ReadRaw(info.section.c_str(), info.key.c_str());
	NoteRawTruncation(info, raw);

	// The registered declaration drives the parse, the range check and the
	// fallback default (INI-01: a duplicate caller's default/range are ignored).
	long fallback = defaultValue;
	if (!ParseIntToken(info.defaultText, fallback))
		fallback = defaultValue;
	int value = static_cast<int>(fallback);
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
			if (info.hasRange && info.minValue <= info.maxValue
				&& (candidate < info.minValue || candidate > info.maxValue))
			{
				invalid = true;
				reason = "is outside the allowed range " + IntText(info.minValue)
					+ ".." + IntText(info.maxValue);
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
	if (info.type != IniValueType::String)
	{
		EmitTypeMismatch(info, IniValueType::String);
		if (buffer && bufferSize > 0)
		{
			_snprintf_s(
				buffer,
				static_cast<std::size_t>(bufferSize),
				_TRUNCATE,
				"%s",
				defaultText.c_str());
		}
		return;
	}

	const RawValue raw = ReadRaw(info.section.c_str(), info.key.c_str());
	NoteRawTruncation(info, raw);
	const std::string value = raw.present ? raw.text : info.defaultText;

	UpdateResolved(info, value, false);
	CopyValueToBuffer(info, buffer, bufferSize, value);
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

void IniRegistry::MarkKnown(
	const char* const section,
	const char* const key,
	const IniValueType type,
	const char* const defaultText)
{
	if (!section || !key)
		return;

	const std::string sectionText = Trim(section);
	const std::string keyText = Trim(key);
	if (sectionText.empty() || keyText.empty())
		return;

	// Already declared: the declarative declaration wins, nothing to record.
	if (FindKey(sectionText.c_str(), keyText.c_str()))
		return;

	// INI-08: the value itself is still parsed leniently by the legacy getter
	// (the platform adapter calls this before delegating), so this entry records
	// the declaration only - it is known to the unknown-key scan and listed by
	// the template dump, but not validated and never logged as an effective value.
	IniKeyInfo info{};
	info.section = sectionText;
	info.key = keyText;
	info.type = type;
	info.description =
		"read through a legacy Ini::Get* getter (lenient parsing, not validated; "
		"declare the key with the INI registry to validate it)";
	info.featureId = currentFeatureId_;
	info.sourceFile.clear();
	info.defaultText = defaultText ? defaultText : "";
	info.valueText = info.defaultText;
	info.invalid = false;
	info.hasRange = false;
	info.minValue = 0;
	info.maxValue = 0;
	info.sizeWarned = false;
	info.duplicateWarned = false;
	keys_.push_back(info);
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
		// Counted for the startup summary: an invalid [Features]/[Groups] switch
		// value used to live only in this local and the summary's invalid counter
		// missed it, so a broken switch looked like a clean startup (INI-03).
		++invalidSwitchCount_;
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
	const int invalidSwitches = InvalidSwitchCount();
	IniScanResult result{
		KeyCount(),
		InvalidCount() + invalidSwitches,
		0,
		invalidSwitches,
		TruncatedCount()};

	// Snapshot boundary for LateInvalidCount(): this is the startup summary pass,
	// so anything rejected later (the rules-INI stage reads [Platform]
	// IniTakeover after the block) is reported there instead of being silently
	// missing from the summary (INI-03).
	summaryInvalidTotal_ = result.invalid;

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

	// R1 dump (INI-04): a declaration reference.  Every rendered value is the
	// declared default, never the value this machine's <Platform>.ini happens to
	// contain, so the template can be compared with the shipped .example
	// regardless of local overrides; the values actually read are in the log.
	AppendLine(out,
		"; " VC_PLATFORM_INI_NAME ": declared defaults from the " VC_PLATFORM_NAME
		" key registry (R1 template dump)");
	AppendLine(out, "; Every value below is the DECLARED DEFAULT, not the value in the INI file.");
	out += "\r\n";

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
		AppendLine(out, info.key + "=" + info.defaultText);
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
		AppendLine(out, info.key + "=" + info.defaultText);
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
			AppendLine(out, info.key + "=" + info.defaultText);
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

int IniRegistry::InvalidSwitchCount() const
{
	return invalidSwitchCount_;
}

int IniRegistry::TruncatedCount() const
{
	return truncatedCount_;
}

bool IniRegistry::IsKeyInvalid(const char* const section, const char* const key) const
{
	const IniKeyInfo* const info = FindKey(section, key);
	return info && info->invalid;
}

int IniRegistry::LateInvalidCount() const
{
	const int current = InvalidCount() + InvalidSwitchCount();
	return current > summaryInvalidTotal_ ? current - summaryInvalidTotal_ : 0;
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
