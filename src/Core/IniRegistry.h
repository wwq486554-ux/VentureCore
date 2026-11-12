#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace venture
{
// ---------------------------------------------------------------------------
// <Platform>.ini key registry and feature-switch enhancement (2.0).
//
// Before 2.0 every feature read its own keys with a raw getter and silently
// carried its own default; a mis-spelled key or an invalid value was invisible.
// 2.0 makes the platform INI layer a registry:
//
//   * a feature declares a key at its read site (IniBool / IniInt / IniString on
//     FeatureContext).  The registry stores section/key/type/default/range/
//     description/owning feature/source file and returns the validated value;
//   * a key present in the file but invalid (not a bool, not an integer, out of
//     range) falls back to the declared default and produces a warning - it is
//     never silent;
//   * the same (section, key) declared twice warns and keeps the first
//     declaration: every read of that key is resolved with the registered
//     declaration (type/default/range), so the warning, the stored state, the
//     invalid count and the template dump all describe the same declaration
//     (INI-01).  Re-declaring an identical declaration (a repeated base read or
//     a [Groups] switch shared by several modules) is not a duplicate;
//   * a key read through a pre-2.0 legacy getter keeps its lenient parsing but
//     is recorded as known (MarkKnown), so the unknown-key scan does not report
//     a key the module really reads and the template dump lists it (INI-08);
//   * every resolved key is logged at Debug level, and the startup block ends
//     with one `ini keys: N registered, M invalid, K unknown` line.  `invalid`
//     covers registered keys and invalid [Features]/[Groups] switch values; a
//     value consumed after that snapshot (the rules-INI stage) is reported
//     separately through LateInvalidCount() (INI-03);
//   * after the feature initialization the base enumerates the file and warns
//     about unknown feature switches ([Features]), unknown keys in a known
//     section and unknown [Platform]/[Groups] keys.  User-defined sections are
//     only logged at Debug level.  [Platform] IniWarnUnknownKeys=no turns the
//     unknown-key warnings off;
//   * `[Groups] <group>=yes|no` is an explicit master switch for the feature
//     group declared by FeatureDescriptor::group (one switch per module stays
//     unchanged otherwise);
//   * [Platform] DumpIniTemplate=yes (development only) renders the registry as
//     <Platform>.ini.generated next to the log: every value it lists is the
//     declared default (a declaration reference, not the effective values the
//     current file produced), the R1 route for keeping the .ini.example in sync.
//
// The registry is engine-free: the file is reached through the two wired
// function pointers (raw value lookup + entry enumeration) and diagnostics go
// through a log sink, so tests/test_ini_registry.cpp drives every rule with an
// in-memory file.
// ---------------------------------------------------------------------------

// Raw value lookup (production: Ini::GetString over <Platform>.ini; tests: an
// in-memory map).  The provider receives kIniMissingValue as `defaultValue` and
// must return it unchanged when the key is absent - that is how a missing key is
// told apart from a key with an empty value.
using IniRawStringFn = void (*)(
	const char* section,
	const char* key,
	const char* defaultValue,
	char* buffer,
	unsigned long bufferSize);

// Enumerates every (section, key) pair of the file (production:
// Ini::EnumerateEntries; tests: an in-memory map).
using IniEntryVisitorFn = void (*)(const char* section, const char* key, void* userData);
using IniEnumerateFn = void (*)(IniEntryVisitorFn visitor, void* userData);

// Diagnostics sink: warning = true for lines that must reach the user (invalid
// value, duplicate registration, unknown key), false for Debug detail.
using IniLogFn = void (*)(bool warning, const char* message);

// The value a raw provider returns for a missing key.
inline constexpr const char* kIniMissingValue = "\x01" "venture-ini-missing";

enum class IniValueType : std::uint8_t
{
	Bool,
	Int,
	String,
};

// One registered key: the declaration plus the value resolved at startup.
struct IniKeyInfo
{
	std::string section;
	std::string key;
	IniValueType type;
	std::string description;
	std::string featureId; // empty for a base / group key
	std::string sourceFile;
	std::string defaultText;
	std::string valueText; // resolved value
	bool invalid;          // the file value was rejected, valueText is the default
	bool hasRange;         // an int key with an explicit allowed range
	int minValue;
	int maxValue;
	bool sizeWarned;       // internal: the size-limit diagnostic was already emitted
	bool duplicateWarned;  // internal: the duplicate-declaration diagnostic was already emitted
};

// One [Features] switch as published by the feature registry (used by the
// unknown-key scan and the template dump).
struct IniFeatureSwitch
{
	const char* iniKey;
	const char* name;
	const char* group; // nullptr/empty = the switch belongs to no group
	bool defaultEnabled;
};

struct IniDiagnostic
{
	bool warning;
	std::string text;
};

struct IniScanResult
{
	int registered;
	// Invalid values reported during startup: rejected registered keys plus
	// invalid [Features]/[Groups] switch values (never silent), so the startup
	// summary covers a bad feature switch as well.
	int invalid;
	int unknown;
	// The [Features]/[Groups] switch part of `invalid` (diagnostic detail).
	int invalidSwitches;
	// Values that hit a hard Win32 profile API size limit while being read; the
	// effective value is the truncated text and a diagnostic was emitted.
	int truncated;
};

class IniRegistry
{
public:
	IniRegistry();

	// The process-wide registry (a function-local static, like the host and
	// feature registries).
	static IniRegistry& Instance();

	// ---- wiring -----------------------------------------------------------
	void SetRawReader(IniRawStringFn reader);
	void SetEnumerator(IniEnumerateFn enumerator);
	void SetLogSink(IniLogFn sink);

	// ---- lifecycle (called by src/Core/FeatureRegistry.cpp) ---------------
	void BeginFeature(const char* featureId);
	void EndFeature();

	// ---- feature-facing reads (the FeatureContext callbacks) --------------
	bool ReadBool(
		const char* section,
		const char* key,
		bool defaultValue,
		const char* description,
		const char* sourceFile);
	int ReadInt(
		const char* section,
		const char* key,
		int defaultValue,
		int minValue,
		int maxValue,
		const char* description,
		const char* sourceFile);
	void ReadString(
		const char* section,
		const char* key,
		const char* defaultValue,
		char* buffer,
		unsigned long bufferSize,
		const char* description,
		const char* sourceFile);

	// ---- base [Platform] reads (no owning feature) ------------------------
	bool ReadBaseBool(
		const char* section,
		const char* key,
		bool defaultValue,
		const char* description);
	void ReadBaseString(
		const char* section,
		const char* key,
		const char* defaultValue,
		char* buffer,
		unsigned long bufferSize,
		const char* description);

	// ---- feature switches and group master switches -----------------------
	// [Features] <iniKey>: parsed and validated; the switch set itself is
	// published by SetFeatureSwitches (it also feeds the unknown-key scan and
	// the template dump).
	bool ReadFeatureSwitch(const char* iniKey, bool defaultEnabled);
	// [Groups] <group>: registered as a key and read, default on.  A null/empty
	// group returns true (the feature belongs to no group).
	bool ReadGroupSwitch(const char* group);
	void SetFeatureSwitches(const IniFeatureSwitch* switches, int count);

	// ---- pre-2.0 legacy getters (INI-08) ----------------------------------
	// FeatureContext::IniGetBool/IniGetInt/IniGetString keep their pre-2.0 lenient
	// parsing (no validation, no effective-value logging).  The platform records
	// the key as known here before it delegates to the legacy getter, so a key
	// the module really reads is not reported as unknown and the template dump
	// lists it.  A no-op for an already registered key (the declarative
	// declaration wins).
	void MarkKnown(const char* section, const char* key, IniValueType type, const char* defaultText);

	// ---- startup audit ----------------------------------------------------
	// Enumerates the file once and returns the counters for the startup summary;
	// diagnostics are emitted through the log sink while scanning.  The call is
	// also the summary snapshot: values resolved later (see LateInvalidCount) are
	// not part of the returned counters.
	IniScanResult ScanUnknownKeys(bool warnUnknown);

	// ---- R1 template dump -------------------------------------------------
	// Renders the registry as INI text ([Platform], [Groups], [Features] and one
	// section per registered feature key).  Every value is the declared default
	// (INI-04) - a declaration reference, not the value the local file produced.
	// The caller prepends the platform/version header and writes it next to the
	// log.
	std::string RenderTemplate() const;

	// ---- introspection ----------------------------------------------------
	int KeyCount() const;
	int InvalidCount() const;
	// Invalid [Features]/[Groups] switch values read so far (the switch part of
	// IniScanResult::invalid).
	int InvalidSwitchCount() const;
	// Values cut at a hard profile-API size limit (raw-value buffer or a caller
	// string buffer) since the last Reset().
	int TruncatedCount() const;
	bool IsKeyInvalid(const char* section, const char* key) const;
	// Invalid values read after the last ScanUnknownKeys() snapshot: a key
	// consumed after the startup summary (the rules-INI stage reads [Platform]
	// IniTakeover) rejects its value only then.  Each key counts once - its
	// resolved `invalid` flag is per key - so the same bad value is never
	// double-counted.
	int LateInvalidCount() const;
	const IniKeyInfo* KeyAt(int index) const;
	int SwitchCount() const;
	const IniFeatureSwitch* SwitchAt(int index) const;

	// Test-only: drops every registration/resolved value and re-registers the
	// built-in [Platform] keys; the wiring (readers/sink) is kept.
	void Reset();

private:
	struct RawValue
	{
		bool present;
		bool possiblyTruncated; // the provider filled its whole buffer (INI-06)
		std::string text;
	};

	void RegisterBuiltinKeys();
	IniKeyInfo* FindKey(const char* section, const char* key);
	const IniKeyInfo* FindKey(const char* section, const char* key) const;
	IniKeyInfo& RegisterKey(
		const char* section,
		const char* key,
		IniValueType type,
		const std::string& defaultText,
		const char* description,
		const char* sourceFile,
		bool hasRange = false,
		int minValue = 0,
		int maxValue = 0);
	bool HasSection(const char* section) const;
	bool IsKnownSwitch(const char* key) const;
	RawValue ReadRaw(const char* section, const char* key) const;
	void UpdateResolved(IniKeyInfo& info, const std::string& valueText, bool invalid);
	void NoteRawTruncation(IniKeyInfo& info, const RawValue& raw);
	void CopyValueToBuffer(const IniKeyInfo& info, char* buffer, unsigned long bufferSize, const std::string& value);
	void EmitTypeMismatch(const IniKeyInfo& info, IniValueType requested) const;
	void Emit(bool warning, const std::string& text) const;
	void EmitValue(const IniKeyInfo& info, bool present) const;
	void EmitScanMessage(bool warningCandidate, const std::string& text);
	std::string SuggestSwitch(const char* unknown) const;
	static void ScanTrampoline(const char* section, const char* key, void* userData);
	void ScanEntry(const char* section, const char* key);

	std::vector<IniKeyInfo> keys_;
	std::vector<IniFeatureSwitch> switches_;
	IniRawStringFn rawReader_ = nullptr;
	IniEnumerateFn enumerator_ = nullptr;
	IniLogFn logSink_ = nullptr;
	std::string currentFeatureId_;
	int invalidSwitchCount_ = 0;
	int truncatedCount_ = 0;
	int summaryInvalidTotal_ = 0;
	// Transient scan state (ScanUnknownKeys owns it; the enumerator callback is
	// a C function pointer, so the state cannot live on the stack and be passed
	// as a lambda).
	bool scanWarnUnknown_ = false;
	int scanUnknownCount_ = 0;
	std::vector<std::string> scanReportedSections_;
};
}
