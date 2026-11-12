#include "Core/HostRegistry.h"

#include "Core/Lineage.h"

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>

// 32-bit game: the marker is six 4-byte fields.  Changing the layout requires a
// VENTURE_HOST_MARKER_ABI_VERSION bump and a matching reader.
static_assert(sizeof(VentureHostInfo) == 24, "VentureHostInfo layout changed - bump VENTURE_HOST_MARKER_ABI_VERSION");

namespace
{
const char* const kNone = "<none>";

// ASCII case-insensitive comparison used for the duplicate-name checks and for
// HasHostName().  Only the ASCII spelling is folded; host names are file/stem
// names, not localised text.
bool EqualsIgnoreCase(const char* const left, const char* const right)
{
	if (!left || !right)
		return left == right;

	const char* a = left;
	const char* b = right;
	while (*a != '\0' && *b != '\0')
	{
		char lowerA = *a;
		char lowerB = *b;
		if (lowerA >= 'A' && lowerA <= 'Z')
			lowerA = static_cast<char>(lowerA - 'A' + 'a');
		if (lowerB >= 'A' && lowerB <= 'Z')
			lowerB = static_cast<char>(lowerB - 'A' + 'a');
		if (lowerA != lowerB)
			return false;
		++a;
		++b;
	}
	return *a == '\0' && *b == '\0';
}

// Longest host name accepted by Register(): the marker ABI cap.  Names are
// compared and printed with bounded helpers, so a source-level name longer than
// this is rejected at registration instead of being truncated later (HOST-02).
constexpr std::size_t kHostNameLimit = VENTURE_HOST_MARKER_NAME_MAX;

// Bounded, truncation-safe text builder for the host diagnostics (HOST-02).
// The previous append used strcat_s and ignored its return value: a long enough
// detected-host name list made the C runtime's invalid-parameter handler
// terminate the process instead of truncating the line.  This builder never
// writes past `bufferSize`, records truncation, and Finish() turns the tail into
// "..." so a truncated list is visibly truncated in the log.
struct TextBuffer
{
	char* buffer;
	std::size_t bufferSize;
	std::size_t used;
	bool truncated;
};

void TextBufferAppend(TextBuffer& text, const char* const value)
{
	if (!text.buffer || text.bufferSize == 0 || !value)
		return;

	const std::size_t capacity = text.bufferSize - 1u; // room for the terminator
	for (const char* character = value; *character != '\0'; ++character)
	{
		if (text.used >= capacity)
		{
			text.truncated = true;
			break;
		}
		text.buffer[text.used] = *character;
		++text.used;
	}
	text.buffer[text.used] = '\0';
}

void TextBufferFinish(TextBuffer& text)
{
	if (!text.buffer || text.bufferSize == 0)
		return;

	const std::size_t capacity = text.bufferSize - 1u;
	if (text.truncated && capacity >= 4u)
	{
		// Replace the last three characters with an ellipsis: the buffer is full,
		// and the prefix up to capacity - 3 is still the beginning of the intended
		// text, so the result is a truncated prefix ending with "...".
		text.buffer[capacity - 3u] = '.';
		text.buffer[capacity - 2u] = '.';
		text.buffer[capacity - 1u] = '.';
		text.buffer[capacity] = '\0';
	}
	else
	{
		text.buffer[text.used] = '\0';
	}
}

void AppendHexBit(const unsigned bit, TextBuffer& text)
{
	char formatted[24] = {};
	_snprintf_s(formatted, sizeof(formatted), _TRUNCATE, "0x%08X", bit);
	TextBufferAppend(text, formatted);
}

std::string LowerAsciiKey(const std::string& text)
{
	std::string result = text;
	for (char& character : result)
	{
		if (character >= 'A' && character <= 'Z')
			character = static_cast<char>(character - 'A' + 'a');
	}
	return result;
}

// True when the module path is inside the Windows directory: those are the
// modules the process loaded for itself, not a game-side host platform.  The
// match is on a directory boundary (D-06), so "C:\WindowsApps\..." is not
// skipped.  The host scan writes its own path check instead of extracting a
// shared helper (B3 scope decision).
bool IsUnderWindowsDirectory(const char* const path)
{
	if (!path || path[0] == '\0')
		return false;

	char windowsDirectory[MAX_PATH] = {};
	const UINT length = GetWindowsDirectoryA(windowsDirectory, static_cast<UINT>(sizeof(windowsDirectory)));
	if (length == 0 || length >= MAX_PATH)
		return false;

	return venture::lineage_detail::IsPathUnderDirectory(path, windowsDirectory);
}

// Copies at most `capacity` characters from an untrusted pointer into
// `destination` (which must hold `capacity` bytes).  It is called only from the
// __try region of ReadHostMarker(): a pointer to unmapped memory faults on the
// first read, and the caller's __except turns that into "not a valid host"
// instead of a page fault in the game (HOST-01 marker defence).
//
// A terminator is deliberately NOT appended when the source has none within
// `capacity`: the validator runs strnlen_s(destination, capacity), which reports
// `capacity` for a full buffer and rejects the marker - fabricating a terminator
// would make an over-long name look valid.
void CopyUntrustedString(
	const char* const source,
	char* const destination,
	const std::size_t capacity)
{
	for (std::size_t index = 0; index < capacity; ++index)
	{
		const char character = source[index];
		destination[index] = character;
		if (character == '\0')
			return;
	}
}
}

namespace venture
{
HostRegistry::HostRegistry()
	: knownMask_(0)
	, mask_(0)
	, detectionRan_(false)
	, reportedProblems_(0)
{
	// The whole 32-bit host space is a fixed-size table; reserving it up front
	// keeps the HostDefinition pointers in DetectedHost valid (registration is
	// rejected once the table is full, and no registration is accepted after
	// detection).
	definitions_.reserve(32);
}

bool HostRegistry::Register(const HostDefinition& definition)
{
	const char* const moduleName = definition.moduleName;
	const char* const displayName = definition.displayName;

	if (detectionRan_)
	{
		AddProblem(
			"host registration rejected: \"%s\" was registered after host detection ran",
			displayName ? displayName : "<unnamed>");
		return false;
	}

	if (!moduleName || moduleName[0] == '\0' || !displayName || displayName[0] == '\0')
	{
		AddProblem("host registration rejected: the module and display names must be non-empty");
		return false;
	}

	// Name length cap (HOST-02): a name longer than the marker ABI limit is
	// rejected here so every downstream compare/print uses a bounded buffer.
	// strnlen_s is bounded, so an unterminated string is rejected as over-long
	// instead of being walked.
	const std::size_t moduleNameLength = strnlen_s(moduleName, kHostNameLimit + 1u);
	if (moduleNameLength == 0 || moduleNameLength > kHostNameLimit)
	{
		AddProblem(
			"host registration rejected: the module name must be 1..%u characters",
			static_cast<unsigned>(kHostNameLimit));
		return false;
	}
	const std::size_t displayNameLength = strnlen_s(displayName, kHostNameLimit + 1u);
	if (displayNameLength == 0 || displayNameLength > kHostNameLimit)
	{
		AddProblem(
			"host registration rejected: the display name must be 1..%u characters",
			static_cast<unsigned>(kHostNameLimit));
		return false;
	}

	if (definition.bit == 0 || (definition.bit & (definition.bit - 1)) != 0)
	{
		AddProblem(
			"host registration rejected: \"%s\" bit 0x%X is not exactly one bit",
			displayName, definition.bit);
		return false;
	}

	const unsigned builtinMask = HostBit(Host::Ares) | HostBit(Host::Phobos) | HostBit(Host::Kratos);
	if (definition.kind == HostKind::Builtin)
	{
		if ((definition.bit & ~builtinMask) != 0)
		{
			AddProblem(
				"host registration rejected: built-in host \"%s\" bit 0x%X must be one of 1<<0..1<<2",
				displayName, definition.bit);
			return false;
		}
	}
	else if (definition.bit < (1u << 3))
	{
		AddProblem(
			"host registration rejected: custom host \"%s\" bit 0x%X must be >= 1<<3 "
			"(bits 0-2 are the built-in hosts)",
			displayName, definition.bit);
		return false;
	}

	for (const HostDefinition& existing : definitions_)
	{
		if (existing.bit == definition.bit)
		{
			AddProblem(
				"host registration rejected: bit 0x%X is already taken by \"%s\"",
				definition.bit, existing.displayName);
			return false;
		}
		if (EqualsIgnoreCase(existing.moduleName, moduleName))
		{
			AddProblem(
				"host registration rejected: module name \"%s\" is already registered by \"%s\"",
				moduleName, existing.displayName);
			return false;
		}
		if (EqualsIgnoreCase(existing.displayName, displayName))
		{
			AddProblem(
				"host registration rejected: display name \"%s\" is already registered "
				"(by module \"%s\")",
				displayName, existing.moduleName);
			return false;
		}
	}

	if (definitions_.size() >= 32)
	{
		AddProblem("host registration rejected: \"%s\" - the host table is full (32 bits)", displayName);
		return false;
	}

	int customHosts = 0;
	for (const HostDefinition& existing : definitions_)
	{
		if (existing.kind == HostKind::Custom)
			++customHosts;
	}
	if (definition.kind == HostKind::Custom && customHosts >= 29)
	{
		AddProblem(
			"host registration rejected: \"%s\" - the 29 custom host slots are exhausted",
			displayName);
		return false;
	}

	definitions_.push_back(definition);
	knownMask_ |= definition.bit;
	return true;
}

unsigned HostRegistry::Detect(bool (*isModuleLoaded)(const char* moduleName))
{
	detected_.clear();
	mask_ = 0;
	detectionRan_ = true;

	if (isModuleLoaded)
	{
		// Probe every registered definition: built-in hosts, source-level custom
		// hosts and hosts discovered by a previous Detect() call.  Registration
		// is closed after the first Detect(), so the count is stable here.
		const std::size_t registeredCount = definitions_.size();
		for (std::size_t index = 0; index < registeredCount; ++index)
		{
			const HostDefinition& definition = definitions_[index];
			if (!isModuleLoaded(definition.moduleName))
				continue;

			DetectedHost detectedHost{};
			detectedHost.definition = &definition;
			detectedHost.modulePath[0] = '\0';

			// Resolve the path for the debug-level startup lines.  A test probe has
			// no real module behind it, so the lookup simply stays empty there.
			const HMODULE module = GetModuleHandleA(definition.moduleName);
			if (module)
			{
				const DWORD length = GetModuleFileNameA(
					module, detectedHost.modulePath, static_cast<DWORD>(sizeof(detectedHost.modulePath)));
				if (length == 0 || length >= static_cast<DWORD>(sizeof(detectedHost.modulePath)))
					detectedHost.modulePath[0] = '\0';
			}

			mask_ |= definition.bit;
			detected_.push_back(detectedHost);
		}
	}

	// Second phase: discover independent platforms that export the host marker.
	DiscoverSelfIdentifyingHosts();

	// Deterministic order: ascending host bit (the built-in three first, then the
	// custom/dynamic range).  FormatDetected() and the startup log rely on it.
	SortDetected();
	return mask_;
}

int HostRegistry::DetectedCount() const
{
	return static_cast<int>(detected_.size());
}

const DetectedHost* HostRegistry::DetectedAt(const int index) const
{
	if (index < 0 || index >= DetectedCount())
		return nullptr;
	return &detected_[static_cast<std::size_t>(index)];
}

bool HostRegistry::HasBit(const unsigned bit) const
{
	return bit != 0 && (mask_ & bit) == bit;
}

bool HostRegistry::HasName(const char* const name) const
{
	if (!name || name[0] == '\0')
		return false;

	for (const DetectedHost& detectedHost : detected_)
	{
		if (EqualsIgnoreCase(detectedHost.definition->displayName, name))
			return true;
	}
	return false;
}

int HostRegistry::DefinitionCount() const
{
	return static_cast<int>(definitions_.size());
}

const HostDefinition* HostRegistry::DefinitionAt(const int index) const
{
	if (index < 0 || index >= DefinitionCount())
		return nullptr;
	return &definitions_[static_cast<std::size_t>(index)];
}

const HostDefinition* HostRegistry::FindByBit(const unsigned bit) const
{
	for (const HostDefinition& definition : definitions_)
	{
		if (definition.bit == bit)
			return &definition;
	}
	return nullptr;
}

const HostDefinition* HostRegistry::FindByName(const char* const name) const
{
	if (!name)
		return nullptr;

	for (const HostDefinition& definition : definitions_)
	{
		if (EqualsIgnoreCase(definition.displayName, name))
			return &definition;
	}
	return nullptr;
}

const char* HostRegistry::FormatNames(
	const unsigned mask,
	char* const buffer,
	const std::size_t bufferSize,
	bool* const truncated) const
{
	if (!buffer || bufferSize == 0)
	{
		if (truncated)
			*truncated = false;
		return "";
	}

	TextBuffer text{buffer, bufferSize, 0, false};
	buffer[0] = '\0';
	if (mask == 0)
	{
		TextBufferAppend(text, kNone);
		TextBufferFinish(text);
		if (truncated)
			*truncated = text.truncated;
		return buffer;
	}

	bool first = true;
	for (unsigned bitIndex = 0; bitIndex < 32; ++bitIndex)
	{
		const unsigned bit = 1u << bitIndex;
		if ((mask & bit) == 0)
			continue;

		if (!first)
			TextBufferAppend(text, ", ");

		const HostDefinition* const definition = FindByBit(bit);
		if (definition)
			TextBufferAppend(text, definition->displayName);
		else
			AppendHexBit(bit, text);
		first = false;
	}

	if (first)
		TextBufferAppend(text, kNone);
	TextBufferFinish(text);
	if (truncated)
		*truncated = text.truncated;
	return buffer;
}

const char* HostRegistry::FormatDetected(
	char* const buffer,
	const std::size_t bufferSize,
	bool* const truncated) const
{
	if (!buffer || bufferSize == 0)
	{
		if (truncated)
			*truncated = false;
		return "";
	}

	TextBuffer text{buffer, bufferSize, 0, false};
	buffer[0] = '\0';
	if (detected_.empty())
	{
		TextBufferAppend(text, kNone);
		TextBufferFinish(text);
		if (truncated)
			*truncated = text.truncated;
		return buffer;
	}

	bool first = true;
	for (const DetectedHost& detectedHost : detected_)
	{
		if (!first)
			TextBufferAppend(text, ", ");
		TextBufferAppend(text, detectedHost.definition->displayName);
		if (detectedHost.definition->kind != HostKind::Builtin)
			TextBufferAppend(text, " (custom)");
		first = false;
	}
	TextBufferFinish(text);
	if (truncated)
		*truncated = text.truncated;
	return buffer;
}

int HostRegistry::ProblemCount() const
{
	return static_cast<int>(problems_.size());
}

const char* HostRegistry::ProblemAt(const int index) const
{
	if (index < 0 || index >= ProblemCount())
		return "";
	return problems_[static_cast<std::size_t>(index)].c_str();
}

int HostRegistry::ReportNewProblems(const HostProblemSink sink)
{
	if (!sink)
		return 0;

	int reported = 0;
	const int total = ProblemCount();
	while (reportedProblems_ < total)
	{
		// Copy the message out before the sink runs: the sink usually logs, and a
		// log line must stay valid even if the sink re-enters the registry.
		char message[256] = {};
		_snprintf_s(
			message,
			sizeof(message),
			_TRUNCATE,
			"%s",
			problems_[static_cast<std::size_t>(reportedProblems_)].c_str());
		++reportedProblems_;
		sink(message);
		++reported;
	}
	return reported;
}

void HostRegistry::AddProblem(const char* const format, ...)
{
	char message[256] = {};
	va_list args;
	va_start(args, format);
	_vsnprintf_s(message, sizeof(message), _TRUNCATE, format, args);
	va_end(args);
	problems_.push_back(message);
}

const char* HostRegistry::OwnString(const char* const text)
{
	if (!text)
		return nullptr;
	ownedStrings_.push_back(std::make_unique<std::string>(text));
	return ownedStrings_.back()->c_str();
}

const HostDefinition* HostRegistry::FindByModuleName(const char* const moduleName) const
{
	if (!moduleName)
		return nullptr;
	for (const HostDefinition& definition : definitions_)
	{
		if (definition.moduleName && EqualsIgnoreCase(definition.moduleName, moduleName))
			return &definition;
	}
	return nullptr;
}

unsigned HostRegistry::AllocateCustomBit() const
{
	// The custom/dynamic range is 1<<3 .. 1<<31: 29 slots shared between the two
	// second-phase registration routes.  Lowest free bit keeps allocation
	// deterministic and independent of static-registration order.
	for (unsigned bit = (1u << 3); bit != 0; bit <<= 1)
	{
		if ((knownMask_ & bit) == 0)
			return bit;
	}
	return 0;
}

void HostRegistry::SortDetected()
{
	std::sort(detected_.begin(), detected_.end(), [](const DetectedHost& left, const DetectedHost& right) {
		return left.definition->bit < right.definition->bit;
	});
}

unsigned HostRegistry::RegisterDynamicHost(
	const char* const moduleName,
	const char* const displayName,
	const char* const version,
	const char* const modulePath)
{
	// HOST-04: detection closes registration.  A discovered host registered after
	// the fact would change the detected set the feature gate was built from
	// (Platform.cpp snapshots the mask into the FeatureContext), so reject it and
	// record the problem for the post-initialization drain (HOST-05) instead of
	// silently extending the set.
	if (detectionRan_)
	{
		AddProblem(
			"self-identified host \"%s\" from module \"%s\" rejected: registered after "
			"host detection ran (the detected host set is frozen)",
			displayName ? displayName : "<unnamed>",
			moduleName ? moduleName : "<unnamed>");
		return 0;
	}
	return RegisterDynamicHostInternal(moduleName, displayName, version, modulePath);
}

unsigned HostRegistry::RegisterDynamicHostInternal(
	const char* const moduleName,
	const char* const displayName,
	const char* const version,
	const char* const modulePath)
{
	if (!moduleName || moduleName[0] == '\0' || !displayName || displayName[0] == '\0')
	{
		AddProblem("self-identified host rejected: the module and display names must be non-empty");
		return 0;
	}

	// A module can be both statically registered (first phase) and export the
	// marker; keep the first definition and do not add a second host entry.
	if (const HostDefinition* const existing = FindByModuleName(moduleName))
		return existing->bit;

	for (const HostDefinition& existing : definitions_)
	{
		if (existing.displayName && EqualsIgnoreCase(existing.displayName, displayName))
		{
			AddProblem(
				"self-identified host \"%s\" from module \"%s\" ignored: the name is "
				"already registered (first definition wins)",
				displayName, moduleName);
			return 0;
		}
	}

	const unsigned bit = AllocateCustomBit();
	if (bit == 0)
	{
		AddProblem(
			"self-identified host \"%s\" from module \"%s\" ignored: all 29 custom "
			"host slots are in use",
			displayName, moduleName);
		return 0;
	}

	HostDefinition definition{};
	definition.bit = bit;
	definition.moduleName = OwnString(moduleName);
	definition.displayName = OwnString(displayName);
	definition.kind = HostKind::Dynamic;
	definition.version = OwnString(version);
	definitions_.push_back(definition);
	knownMask_ |= bit;

	DetectedHost detectedHost{};
	detectedHost.definition = &definitions_.back();
	_snprintf_s(
		detectedHost.modulePath,
		sizeof(detectedHost.modulePath),
		_TRUNCATE,
		"%s",
		modulePath ? modulePath : "");
	mask_ |= bit;
	detected_.push_back(detectedHost);
	SortDetected();
	return bit;
}

void HostRegistry::DiscoverSelfIdentifyingHosts()
{
	struct Candidate
	{
		std::string moduleName;
		std::string modulePath;
		std::string displayName;
		std::string version;
	};

	std::vector<Candidate> candidates;

	// Toolhelp from kernel32 (no extra import library).  An unavailable snapshot
	// degrades to "no dynamically discovered host", like the lineage scan.
	const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
	if (snapshot != INVALID_HANDLE_VALUE)
	{
		MODULEENTRY32 entry{};
		entry.dwSize = sizeof(entry);
		if (Module32First(snapshot, &entry))
		{
			do
			{
				if (!entry.hModule || IsUnderWindowsDirectory(entry.szExePath))
					continue;

				// HOST-06: the Toolhelp snapshot only says the module was loaded
				// when it was taken.  Hold a temporary reference (not
				// UNCHANGED_REFCOUNT, and not PIN - that would leak the handle and
				// prevent a normal unload) so a FreeLibrary from another thread
				// cannot pull the module out while GetProcAddress, the marker call
				// and the string copy run.  Released before the marker is validated;
				// the validator only touches the copied strings.
				HMODULE pinnedModule = nullptr;
				if (!GetModuleHandleExA(
						GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
						reinterpret_cast<LPCSTR>(entry.hModule),
						&pinnedModule)
					|| !pinnedModule)
				{
					// The module went away between the snapshot and the pin - it is
					// not a host of this process.
					continue;
				}

				// A DLL of this lineage is not an independent host: the lineage
				// mutual-exclusion check owns it, so it cannot masquerade as a host
				// through this protocol.  This also skips our own DLL.
				if (GetProcAddress(pinnedModule, VENTURE_LINEAGE_EXPORT_GET_INFO))
				{
					FreeLibrary(pinnedModule);
					continue;
				}

				const auto getInfo = reinterpret_cast<VentureHostGetInfoFn>(
					GetProcAddress(pinnedModule, VENTURE_HOST_MARKER_EXPORT_GET_INFO));
				if (!getInfo)
				{
					FreeLibrary(pinnedModule);
					continue;
				}

				// HOST-01: the whole untrusted read (export call plus every pointer
				// it returns) runs inside ReadHostMarker's POD-only SEH region; what
				// comes back is copied into caller storage, so a wild name pointer is
				// a rejection instead of a page fault.  The pin is still held while
				// the copy runs and is released right after it.
				char markerName[VENTURE_HOST_MARKER_NAME_MAX + 1u] = {};
				char markerVersion[VENTURE_HOST_MARKER_NAME_MAX + 1u] = {};
				VentureHostInfo marker{};
				const HostMarkerRead markerRead = ReadHostMarker(
					getInfo,
					&marker,
					markerName,
					sizeof(markerName),
					markerVersion,
					sizeof(markerVersion));
				FreeLibrary(pinnedModule);
				pinnedModule = nullptr;

				if (markerRead == HostMarkerRead::Raised)
				{
					AddProblem(
						"host marker in module \"%s\" raised an exception - ignored",
						entry.szModule);
					continue;
				}
				if (markerRead != HostMarkerRead::Valid)
				{
					AddProblem(
						"host marker in module \"%s\" is invalid (magic, ABI or name/version) - ignored",
						entry.szModule);
					continue;
				}

				Candidate candidate;
				candidate.moduleName = entry.szModule;
				candidate.modulePath = entry.szExePath;
				// The marker strings were copied into local storage while the module
				// was pinned; the registry owns its copies from here on.
				candidate.displayName = marker.name ? marker.name : "";
				candidate.version = marker.version ? marker.version : "";
				candidates.push_back(std::move(candidate));
			} while (Module32Next(snapshot, &entry));
		}
		CloseHandle(snapshot);
	}

	// Deterministic bit allocation and "first definition wins": the snapshot
	// order is not documented, so sort the candidates by module file name.
	std::sort(candidates.begin(), candidates.end(), [](const Candidate& left, const Candidate& right) {
		return LowerAsciiKey(left.moduleName) < LowerAsciiKey(right.moduleName);
	});
	for (const Candidate& candidate : candidates)
	{
		RegisterDynamicHostInternal(
			candidate.moduleName.c_str(),
			candidate.displayName.c_str(),
			candidate.version.c_str(),
			candidate.modulePath.c_str());
	}
}

HostMarkerRead ReadHostMarker(
	const VentureHostGetInfoFn getInfo,
	VentureHostInfo* const copy,
	char* const nameBuffer,
	const std::size_t nameBufferSize,
	char* const versionBuffer,
	const std::size_t versionBufferSize)
{
	// The validator below runs strnlen_s(..., 64) on both copies, so the caller
	// must provide at least the documented 64 bytes; a smaller buffer is rejected
	// instead of being over-read.
	constexpr std::size_t kCopyCapacity = VENTURE_HOST_MARKER_NAME_MAX + 1u;
	if (!getInfo || !copy || !nameBuffer || !versionBuffer
		|| nameBufferSize < kCopyCapacity || versionBufferSize < kCopyCapacity)
	{
		return HostMarkerRead::Invalid;
	}

	*copy = VentureHostInfo{};
	nameBuffer[0] = '\0';
	versionBuffer[0] = '\0';

	// POD-only guarded region (HOST-01): the untrusted export is called here and
	// every byte read through its pointers is copied into caller storage before
	// the region exits.  No C++ object with a destructor is constructed inside,
	// which is what makes __try legal in this build (no /EH).
	__try
	{
		const VentureHostInfo* const info = getInfo();
		if (!info)
			return HostMarkerRead::Invalid;

		// The alignment/stride of a hostile struct is not trusted either: read
		// only the fields the contract defines, each read guarded by the region.
		copy->magic = info->magic;
		copy->abiVersion = info->abiVersion;
		if (info->name)
		{
			CopyUntrustedString(info->name, nameBuffer, kCopyCapacity);
			copy->name = nameBuffer;
		}
		if (info->version)
		{
			CopyUntrustedString(info->version, versionBuffer, kCopyCapacity);
			copy->version = versionBuffer;
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return HostMarkerRead::Raised;
	}

	// Validate the copy, not the foreign pointer.  The contract check is the same
	// one the marker protocol documents (IsValidHostMarker).
	return IsValidHostMarker(copy) ? HostMarkerRead::Valid : HostMarkerRead::Invalid;
}

bool IsValidHostMarker(const VentureHostInfo* const info)
{
	if (!info)
		return false;
	if (info->magic != VENTURE_HOST_MARKER_MAGIC || info->abiVersion != VENTURE_HOST_MARKER_ABI_VERSION)
		return false;
	if (!info->name || info->name[0] == '\0')
		return false;

	// Bounded length checks: both strings must be NUL-terminated within the
	// documented maximum, so a truncated/hostile marker cannot make the scan walk
	// through arbitrary memory.
	const std::size_t nameLength = strnlen_s(info->name, VENTURE_HOST_MARKER_NAME_MAX + 1u);
	if (nameLength == 0 || nameLength > VENTURE_HOST_MARKER_NAME_MAX)
		return false;

	if (info->version)
	{
		const std::size_t versionLength = strnlen_s(info->version, VENTURE_HOST_MARKER_NAME_MAX + 1u);
		if (versionLength > VENTURE_HOST_MARKER_NAME_MAX)
			return false;
	}
	return true;
}

HostRegistry& Hosts()
{
	static HostRegistry registry;
	return registry;
}

bool IsModuleLoaded(const char* const moduleName)
{
	return moduleName && GetModuleHandleA(moduleName) != nullptr;
}

bool __cdecl HasHost(const Host host)
{
	return Hosts().HasBit(HostBit(host));
}

bool __cdecl HasHostBit(const unsigned bit)
{
	return Hosts().HasBit(bit);
}

bool __cdecl HasHostName(const char* const name)
{
	return Hosts().HasName(name);
}
}
