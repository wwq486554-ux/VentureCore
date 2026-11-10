// Unit test: lineage mutual-exclusion decision logic.
//
// TEST_DEPS:
// (header-only test: the pure decision logic and the marker constants live in
// src/Core/Lineage.h and include/platform/Lineage.h, so no non-test source is
// linked.  The Windows-side module scan is exercised by scripts/linux/load_probe.sh,
// which loads two copies of the built DLL into one process.)
//
// The records below are synthetic: they stand in for loaded modules that
// answered the lineage probe, so every branch of the decision can be covered
// without a game process.
//
// TEST_DEPS is a single line - the runner parses the value after the colon as a
// space-separated list of repository-relative sources to link.

#include <cstdio>
#include <string>
#include <vector>

#include "Core/Lineage.h"

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

bool DetailsContain(const venture::LineageDecision& decision, const char* const needle)
{
	return decision.details.find(needle) != std::string::npos;
}

venture::ForeignLineageModule MakeModule(
	const char* const name,
	const unsigned savegameId = VC_SAVEGAME_ID,
	const unsigned magic = VENTURE_LINEAGE_MAGIC)
{
	venture::ForeignLineageModule module;
	module.moduleName = name;
	module.modulePath = std::string("Z:\\game\\") + name;
	module.magic = magic;
	module.abiVersion = VENTURE_LINEAGE_ABI_VERSION;
	module.platformName = "VentureCore";
	module.version = "v0.2.0.0-experimental";
	module.savegameId = savegameId;
	return module;
}
}

int __cdecl main()
{
	using venture::DecideLineage;
	using venture::LineagePolicy;

	// -- marker constants ----------------------------------------------------
	Check(VENTURE_LINEAGE_MAGIC == 0x564C4E31u, "magic is the documented 'VLN1' value");
	Check(VENTURE_LINEAGE_ABI_VERSION == 1u, "marker ABI version is 1");
	Check(VC_SAVEGAME_ID == 0x56433031u, "savegame id is the platform's VC_SAVEGAME_ID value");

	// -- no conflict ---------------------------------------------------------
	const venture::LineageDecision clean = DecideLineage({}, LineagePolicy::Refuse);
	Check(!clean.conflict, "no modules -> no conflict");
	Check(clean.allowStartup, "no modules -> startup allowed");
	Check(!clean.savegameMismatch, "no modules -> no savegame mismatch");
	Check(clean.details.empty(), "no modules -> empty details");

	// -- one conflict, Warn: continue ---------------------------------------
	const std::vector<venture::ForeignLineageModule> one{MakeModule("OtherPlatform.dll")};
	const venture::LineageDecision warned = DecideLineage(one, LineagePolicy::Warn);
	Check(warned.conflict, "one marker under Warn -> conflict");
	Check(warned.allowStartup, "one marker under Warn -> startup allowed");
	Check(!warned.savegameMismatch, "same savegame id -> no mismatch");
	Check(DetailsContain(warned, "OtherPlatform.dll"), "details name the conflicting module");
	Check(DetailsContain(warned, "VentureCore"), "details carry the platform name");
	Check(DetailsContain(warned, "v0.2.0.0-experimental"), "details carry the platform version");
	Check(DetailsContain(warned, "savegame 0x56433031"), "details carry the savegame id");

	// -- one conflict, Refuse: deny -----------------------------------------
	const venture::LineageDecision refused = DecideLineage(one, LineagePolicy::Refuse);
	Check(refused.conflict, "one marker under Refuse -> conflict");
	Check(!refused.allowStartup, "one marker under Refuse -> startup denied");

	// -- several conflicts ---------------------------------------------------
	const std::vector<venture::ForeignLineageModule> several{
		MakeModule("FirstCopy.dll"),
		MakeModule("SecondCopy.dll"),
	};
	const venture::LineageDecision multiple = DecideLineage(several, LineagePolicy::Refuse);
	Check(multiple.conflict, "two markers -> conflict");
	Check(!multiple.allowStartup, "two markers under Refuse -> startup denied");
	Check(DetailsContain(multiple, "FirstCopy.dll") && DetailsContain(multiple, "SecondCopy.dll"),
		"details name every conflicting module");
	Check(DetailsContain(multiple, "; "), "details separate several modules");

	// -- magic mismatch is ignored ------------------------------------------
	const std::vector<venture::ForeignLineageModule> wrongMagic{
		MakeModule("Unrelated.dll", VC_SAVEGAME_ID, 0x12345678u),
	};
	const venture::LineageDecision ignored = DecideLineage(wrongMagic, LineagePolicy::Refuse);
	Check(!ignored.conflict, "a module with a different magic is not a lineage conflict");
	Check(ignored.allowStartup, "an ignored module does not deny startup");
	Check(ignored.details.empty(), "an ignored module leaves the details empty");

	// -- savegame id mismatch is recorded -----------------------------------
	const std::vector<venture::ForeignLineageModule> otherSavegame{
		MakeModule("NewerCopy.dll", 0x56433032u),
	};
	const venture::LineageDecision mismatched = DecideLineage(otherSavegame, LineagePolicy::Warn);
	Check(mismatched.conflict, "savegame mismatch still is a conflict");
	Check(mismatched.allowStartup, "savegame mismatch under Warn still allows startup");
	Check(mismatched.savegameMismatch, "a different savegame id is recorded");
	Check(DetailsContain(mismatched, "savegame id differs from this build: 0x56433031"),
		"details explain the savegame id mismatch");

	// -- policy parsing ------------------------------------------------------
	Check(venture::ParseLineagePolicy("refuse") == LineagePolicy::Refuse, "policy: refuse");
	Check(venture::ParseLineagePolicy("warn") == LineagePolicy::Warn, "policy: warn");
	Check(venture::ParseLineagePolicy("WARN") == LineagePolicy::Warn, "policy: case-insensitive");
	Check(venture::ParseLineagePolicy("Refuse") == LineagePolicy::Refuse, "policy: case-insensitive (refuse)");
	Check(venture::ParseLineagePolicy(nullptr) == LineagePolicy::Refuse, "policy: missing value defaults to refuse");
	Check(venture::ParseLineagePolicy("") == LineagePolicy::Refuse, "policy: empty value defaults to refuse");
	Check(venture::ParseLineagePolicy("warning") == LineagePolicy::Refuse,
		"policy: unrecognised value defaults to refuse");
	Check(venture::ParseLineagePolicy("bogus", LineagePolicy::Warn) == LineagePolicy::Warn,
		"policy: explicit fallback is honoured");

	std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAILED",
		g_failures, g_failures == 1 ? "" : "s");
	return g_failures == 0 ? 0 : 1;
}
