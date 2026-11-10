#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include <platform/Lineage.h>

#include "Core/PlatformConfig.h"

// ---------------------------------------------------------------------------
// Lineage mutual exclusion: at most one DLL of the VentureCore lineage may run
// in one process (see docs/Lineage.md).
//
// This header is deliberately engine-free: it holds the marker *record* the
// scanner reports, the policy parsed from the INI, and the pure decision logic,
// so tests/test_lineage.cpp can exercise every branch without loading a DLL.
// The module enumeration and the C exports live in Lineage.cpp.
// ---------------------------------------------------------------------------

namespace venture
{
// [Platform] LineagePolicy.  Refuse is the default: a second DLL of the same
// lineage would build a second extension table and a second savegame trailer.
enum class LineagePolicy
{
	Refuse, // log the conflict and keep the platform disabled
	Warn,   // log the conflict and continue (diagnostics only)
};

// One loaded module that answered the lineage probe with a matching marker.
struct ForeignLineageModule
{
	std::string moduleName;   // file name reported by the module snapshot
	std::string modulePath;   // full path (diagnostics only)
	unsigned magic = 0;       // marker magic read back from that module
	unsigned abiVersion = 0;  // marker ABI version
	std::string platformName; // marker name (may be empty on a mismatched build)
	std::string version;      // marker version (may be empty)
	unsigned savegameId = 0;  // marker savegame trailer id
};

// Outcome of the mutual-exclusion decision.  `details` names every conflicting
// module and is written to the log verbatim.
struct LineageDecision
{
	bool conflict = false;         // at least one matching foreign marker
	bool allowStartup = true;      // false only for a conflict under Refuse
	bool savegameMismatch = false; // a conflicting module uses another savegame id
	std::string details;           // "module (platform version, abi N, savegame 0x...)", "; "-separated
};

// -- pure helpers (engine-free, unit-tested) --------------------------------
namespace lineage_detail
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

// 8 lower-case hex digits, so log messages and the test predicates agree.
inline std::string Hex32(const unsigned value)
{
	static const char kDigits[] = "0123456789abcdef";
	std::string text(8, '0');
	for (int index = 7; index >= 0; --index)
	{
		text[static_cast<std::size_t>(index)] =
			kDigits[(value >> static_cast<unsigned>((7 - index) * 4)) & 0xFu];
	}
	return text;
}
}

// Parses a [Platform] LineagePolicy value: "refuse" or "warn", case-insensitive.
// Returns `fallback` for nullptr, an empty string and any unrecognised value.
inline LineagePolicy ParseLineagePolicy(const char* const text, const LineagePolicy fallback = LineagePolicy::Refuse)
{
	if (!text)
		return fallback;

	if (lineage_detail::EqualsIgnoreCase(text, "refuse"))
		return LineagePolicy::Refuse;
	if (lineage_detail::EqualsIgnoreCase(text, "warn"))
		return LineagePolicy::Warn;

	return fallback;
}

// Pure decision: what should the platform do with the modules the scanner found?
// Records whose marker magic does not match are ignored here as well as in the
// scanner, so a caller can hand in unfiltered records safely.
inline LineageDecision DecideLineage(
	const std::vector<ForeignLineageModule>& foreign,
	const LineagePolicy policy)
{
	LineageDecision decision{};

	for (const ForeignLineageModule& module : foreign)
	{
		if (module.magic != VENTURE_LINEAGE_MAGIC)
			continue;

		if (decision.conflict)
			decision.details += "; ";
		decision.conflict = true;

		decision.details += module.moduleName.empty() ? std::string("<unnamed module>") : module.moduleName;
		decision.details += " (";
		decision.details += module.platformName.empty() ? std::string("unknown platform") : module.platformName;
		decision.details += " ";
		decision.details += module.version.empty() ? std::string("unknown version") : module.version;
		decision.details += ", abi ";
		decision.details += std::to_string(module.abiVersion);
		decision.details += ", savegame 0x";
		decision.details += lineage_detail::Hex32(module.savegameId);
		decision.details += ")";

		if (module.savegameId != VC_SAVEGAME_ID)
		{
			// The other DLL of the same lineage writes a different savegame trailer
			// layout: worth recording even when the policy allows coexistence.
			decision.savegameMismatch = true;
			decision.details += " [savegame id differs from this build: 0x";
			decision.details += lineage_detail::Hex32(VC_SAVEGAME_ID);
			decision.details += "]";
		}
	}

	decision.allowStartup = !(decision.conflict && policy == LineagePolicy::Refuse);
	return decision;
}

// -- module scan (implemented in Lineage.cpp) --------------------------------
//
// Enumerates the modules of the current process (kernel32's Toolhelp snapshot,
// so no extra import library is needed), skips this DLL and the obvious system
// modules, and probes every remaining module for VENTURE_LINEAGE_EXPORT_GET_INFO.
// Never throws or exits: an unavailable snapshot degrades to "no conflict".
std::vector<ForeignLineageModule> ScanForeignLineage();

// Same scan, reduced to the count used by VentureLineage_CountConflicts().
unsigned CountForeignLineage();
}
