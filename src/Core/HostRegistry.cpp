#include "Core/HostRegistry.h"

#include <windows.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>

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

void Append(char* const buffer, const std::size_t bufferSize, const char* const text)
{
	if (!buffer || bufferSize == 0 || !text)
		return;
	strcat_s(buffer, bufferSize, text);
}

void AppendHexBit(const unsigned bit, char* const buffer, const std::size_t bufferSize)
{
	char text[24] = {};
	_snprintf_s(text, sizeof(text), _TRUNCATE, "0x%08X", bit);
	Append(buffer, bufferSize, text);
}
}

namespace venture
{
HostRegistry::HostRegistry()
	: knownMask_(0)
	, mask_(0)
	, detectionRan_(false)
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

	if (!isModuleLoaded)
		return 0;

	for (const HostDefinition& definition : definitions_)
	{
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

	// Deterministic order: ascending host bit (the built-in three first, then
	// the custom range).  FormatDetected() and the startup log rely on it.
	std::sort(
		detected_.begin(),
		detected_.end(),
		[](const DetectedHost& left, const DetectedHost& right) {
			return left.definition->bit < right.definition->bit;
		});

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
	const std::size_t bufferSize) const
{
	if (!buffer || bufferSize == 0)
		return "";

	buffer[0] = '\0';
	if (mask == 0)
	{
		Append(buffer, bufferSize, kNone);
		return buffer;
	}

	bool first = true;
	for (unsigned bitIndex = 0; bitIndex < 32; ++bitIndex)
	{
		const unsigned bit = 1u << bitIndex;
		if ((mask & bit) == 0)
			continue;

		if (!first)
			Append(buffer, bufferSize, ", ");

		const HostDefinition* const definition = FindByBit(bit);
		if (definition)
			Append(buffer, bufferSize, definition->displayName);
		else
			AppendHexBit(bit, buffer, bufferSize);
		first = false;
	}

	if (first)
		Append(buffer, bufferSize, kNone);
	return buffer;
}

const char* HostRegistry::FormatDetected(char* const buffer, const std::size_t bufferSize) const
{
	if (!buffer || bufferSize == 0)
		return "";

	buffer[0] = '\0';
	if (detected_.empty())
	{
		Append(buffer, bufferSize, kNone);
		return buffer;
	}

	bool first = true;
	for (const DetectedHost& detectedHost : detected_)
	{
		if (!first)
			Append(buffer, bufferSize, ", ");
		Append(buffer, bufferSize, detectedHost.definition->displayName);
		if (detectedHost.definition->kind == HostKind::Custom)
			Append(buffer, bufferSize, " (custom)");
		first = false;
	}
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

void HostRegistry::AddProblem(const char* const format, ...)
{
	char message[256] = {};
	va_list args;
	va_start(args, format);
	_vsnprintf_s(message, sizeof(message), _TRUNCATE, format, args);
	va_end(args);
	problems_.push_back(message);
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

bool HasHost(const Host host)
{
	return Hosts().HasBit(HostBit(host));
}

bool HasHostBit(const unsigned bit)
{
	return Hosts().HasBit(bit);
}

bool HasHostName(const char* const name)
{
	return Hosts().HasName(name);
}
}
