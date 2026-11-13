// Unit test: platform command registry.
//
// The registry in src/Command/Commands.h is engine-free by design (the engine
// CommandClass objects are created in src/Engine/Hooks.Commands.cpp), so the
// test links Commands.cpp alone and checks the data the engine-facing half
// consumes: unique names, UI fields, enable/disable, iteration, plus the two
// engine-free helpers of the command layer - the shared-list name-conflict scan
// the post-registration hook runs (HOOK-03) and the command-to-event house
// index guard that keeps HouseIndex = -1 out of the event queue (HOOK-08).
//
// TEST_DEPS: src/Command/Commands.cpp

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>

#include "Command/Commands.h"
#include "Core/PlatformGate.h"

namespace
{
using Commands = venture::Commands;
using RegisterResult = Commands::RegisterResult;

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

bool SameWide(const wchar_t* const left, const wchar_t* const right)
{
	if (!left || !right)
		return false;
	return std::wcscmp(left, right) == 0;
}

void HandlerOne() {}
void HandlerTwo() {}
}

int __cdecl main(void)
{
	// --- empty registry -----------------------------------------------------
	std::printf("-- empty registry --\n");
	Check(Commands::Count() == 0, "registry starts empty");
	Check(Commands::At(0) == nullptr, "no descriptor at index 0");
	Check(Commands::Find("Venture Test Command") == nullptr, "unknown name is not found");
	Check(!Commands::IsEnabled("Venture Test Command"), "unknown name is not enabled");
	Check(!Commands::SetEnabled("Venture Test Command", true), "cannot enable an unknown name");

	// --- invalid descriptors ------------------------------------------------
	std::printf("-- validation --\n");
	Check(Commands::Register(venture::Command{}) == RegisterResult::InvalidCommand,
		"all-null descriptor rejected");
	Check(Commands::Register({
			nullptr, L"Name", L"Category", L"Description", true, &HandlerOne})
			== RegisterResult::InvalidCommand,
		"null engine name rejected");
	Check(Commands::Register({
			"", L"Name", L"Category", L"Description", true, &HandlerOne})
			== RegisterResult::InvalidCommand,
		"empty engine name rejected");
	Check(Commands::Register({
			"Test Command", nullptr, L"Category", L"Description", true, &HandlerOne})
			== RegisterResult::InvalidCommand,
		"null UI name rejected");
	Check(Commands::Register({
			"Test Command", L"Name", nullptr, L"Description", true, &HandlerOne})
			== RegisterResult::InvalidCommand,
		"null UI category rejected");
	Check(Commands::Register({
			"Test Command", L"Name", L"Category", nullptr, true, &HandlerOne})
			== RegisterResult::InvalidCommand,
		"null UI description rejected");
	Check(Commands::Register({
			"Test Command", L"Name", L"Category", L"Description", true, nullptr})
			== RegisterResult::InvalidCommand,
		"null handler rejected");
	Check(Commands::Count() == 0, "rejected descriptors are not registered");

	// --- name comparison (C-08) --------------------------------------------
	std::printf("-- name comparison --\n");
	Check(venture::CommandNameEquals("Venture Test Command", "venture test command"),
		"names compare case-insensitively");
	Check(!venture::CommandNameEquals("Venture Test Command", "Venture Test Command2"),
		"a longer name is not equal");
	Check(!venture::CommandNameEquals("Venture Test Command", "Venture Test Comman"),
		"a shorter name is not equal");
	Check(venture::CommandNameEquals(nullptr, nullptr), "two null names are equal");
	Check(!venture::CommandNameEquals(nullptr, "name"), "a null name differs from a name");

	// --- registration and fields -------------------------------------------
	std::printf("-- registration --\n");
	Check(Commands::Register({
			"Venture Test Command",
			L"Venture Test Command",
			L"VentureCore",
			L"Logs a line and raises the platform example event",
			true,
			&HandlerOne})
			== RegisterResult::Ok,
		"example command registers");
	Check(Commands::Count() == 1, "one command registered");

	const venture::Command* const first = Commands::Find("Venture Test Command");
	Check(first != nullptr, "registered name is found");
	Check(first != nullptr && first->name != nullptr
			&& std::strcmp(first->name, "Venture Test Command") == 0,
		"engine name preserved");
	Check(first != nullptr && SameWide(first->uiName, L"Venture Test Command"),
		"UI name preserved");
	Check(first != nullptr && SameWide(first->uiCategory, L"VentureCore"),
		"UI category preserved");
	Check(first != nullptr && first->uiDescription != nullptr && *first->uiDescription != L'\0',
		"UI description preserved");
	Check(first != nullptr && first->handler == &HandlerOne, "handler preserved");
	Check(first != nullptr && first->enabled, "registered command is enabled by its descriptor");

	Check(Commands::Register({
			"Venture Test Command",
			L"Different UI name",
			L"Different category",
			L"Different description",
			false,
			&HandlerTwo})
			== RegisterResult::DuplicateName,
		"duplicate engine name rejected");
	Check(Commands::Count() == 1, "duplicate does not change the count");

	Check(Commands::Register({
			"venture test command",
			L"Case variant",
			L"Debug",
			L"A name that differs only in case",
			true,
			&HandlerTwo})
			== RegisterResult::DuplicateName,
		"a name differing only in case is rejected as a duplicate");
	Check(Commands::Count() == 1, "the case-variant duplicate does not change the count");
	Check(Commands::Find("venture test command") == first, "lookup is case-insensitive");
	Check(Commands::Find("VENTURE TEST COMMAND") == first, "lookup ignores all case differences");
	Check(Commands::Find("Venture Test") == nullptr, "a partial name is not found");
	Check(Commands::SetEnabled("vEnTuRe TeSt CoMmAnD", false), "enable matches case-insensitively");
	Check(!Commands::IsEnabled("Venture Test Command"), "the case-insensitive enable took effect");
	Check(Commands::SetEnabled("Venture Test Command", true), "re-enable restores the command");

	Check(Commands::Register({
			"Venture Second Command",
			L"Second UI Name",
			L"Debug",
			L"Second description",
			false,
			&HandlerTwo})
			== RegisterResult::Ok,
		"second command registers with enabled=false");
	Check(Commands::Count() == 2, "two commands registered");
	Check(!Commands::IsEnabled("Venture Second Command"),
		"a descriptor registered as disabled stays disabled");

	// --- iteration ----------------------------------------------------------
	std::printf("-- iteration --\n");
	Check(Commands::At(0) != nullptr && std::strcmp(Commands::At(0)->name, "Venture Test Command") == 0,
		"iteration: first command");
	Check(Commands::At(1) != nullptr && std::strcmp(Commands::At(1)->name, "Venture Second Command") == 0,
		"iteration: second command");
	Check(Commands::At(2) == nullptr, "iteration ends after the last command");
	Check(Commands::At(1) != nullptr && SameWide(Commands::At(1)->uiCategory, L"Debug"),
		"iteration exposes the category the engine shows");

	// --- enable / disable ---------------------------------------------------
	std::printf("-- enable/disable --\n");
	Check(Commands::SetEnabled("Venture Test Command", false), "disable a known command");
	Check(!Commands::IsEnabled("Venture Test Command"), "disabled command reports disabled");
	Check(Commands::Find("Venture Test Command") != nullptr
			&& !Commands::Find("Venture Test Command")->enabled,
		"the descriptor the engine path reads reflects the switch");
	Check(Commands::SetEnabled("Venture Test Command", true), "re-enable a known command");
	Check(Commands::IsEnabled("Venture Test Command"), "re-enabled command reports enabled");
	Check(Commands::SetEnabled("Venture Second Command", true), "enable the second command");
	Check(Commands::IsEnabled("Venture Second Command"), "second command reports enabled");
	Check(!Commands::SetEnabled("No Such Command", true), "unknown name cannot be enabled");
	Check(Commands::Count() == 2, "enable/disable never changes the registry size");

	// --- startup gate (C-06) ------------------------------------------------
	// Static registrars populate the registry while the gate is pending; once the
	// platform has refused to start the registry is closed, so a refused DLL can
	// never reach the engine's command list.
	std::printf("-- startup gate --\n");
	Check(venture::PlatformGate::IsPending(), "the gate starts pending");
	venture::PlatformGate::MarkRefused();
	Check(Commands::Register({
			"Venture Gated Command",
			L"Venture Gated Command",
			L"VentureCore",
			L"Registered after a refusal - must be rejected",
			true,
			&HandlerOne})
			== RegisterResult::PlatformRefused,
		"a refused platform rejects new commands");
	Check(Commands::Count() == 2, "the rejected registration is not stored");
	Check(Commands::Find("Venture Gated Command") == nullptr, "the refused command is not findable");
	venture::PlatformGate::MarkRunning();
	Check(Commands::Register({
			"Venture Gated Command",
			L"Venture Gated Command",
			L"VentureCore",
			L"Registered after the start - accepted",
			true,
			&HandlerOne})
			== RegisterResult::Ok,
		"a running platform accepts commands");
	Check(Commands::Count() == 3, "the running registration is stored");
	venture::PlatformGate::MarkPending();

	// --- shared engine-list name conflicts (HOOK-03) -------------------------
	// The engine's first-match command list is one flat namespace shared by the
	// engine and every injected DLL.  The post-registration hook snapshots it
	// once the engine's own registration is complete and feeds the snapshot to
	// the engine-free scan tested here.
	std::printf("-- engine-list conflicts --\n");
	{
		const venture::SharedCommandEntry entries[] = {
			{"Venture Test Command", true},
			{"Select All", false},
		};
		Check(venture::FindPlatformCommandConflicts(entries, 2).empty(),
			"different names produce no conflict");
	}
	Check(venture::FindPlatformCommandConflicts(nullptr, 0).empty(),
		"a null snapshot produces no conflict");
	{
		const venture::SharedCommandEntry entries[] = {
			{"Venture Test Command", true},
			{"venture test command", false},
		};
		const auto conflicts = venture::FindPlatformCommandConflicts(entries, 2);
		Check(conflicts.size() == 1, "a case-insensitive duplicate is one conflict");
		if (conflicts.size() == 1)
		{
			Check(conflicts[0].platformIndex == 0 && conflicts[0].otherIndex == 1,
				"the conflict reports the snapshot indexes");
			Check(conflicts[0].platformShadows,
				"platform entry first: the other command is shadowed");
			Check(conflicts[0].platformName == entries[0].name
					&& conflicts[0].otherName == entries[1].name,
				"the conflict reports the colliding names");
		}
	}
	{
		const venture::SharedCommandEntry entries[] = {
			{"select all", false},
			{"Select All", true},
		};
		const auto conflicts = venture::FindPlatformCommandConflicts(entries, 2);
		Check(conflicts.size() == 1, "an engine command registered first is one conflict");
		if (conflicts.size() == 1)
		{
			Check(!conflicts[0].platformShadows,
				"engine command first: the platform command is shadowed");
			Check(conflicts[0].platformIndex == 1 && conflicts[0].otherIndex == 0,
				"the conflict indexes follow the snapshot order");
		}
	}
	{
		const venture::SharedCommandEntry entries[] = {
			{"Select All", false},
			{"select all", false},
		};
		Check(venture::FindPlatformCommandConflicts(entries, 2).empty(),
			"engine-engine duplicates are not the platform's conflict");
	}
	{
		const venture::SharedCommandEntry entries[] = {
			{"Venture Twin", true},
			{"venture twin", true},
		};
		Check(venture::FindPlatformCommandConflicts(entries, 2).empty(),
			"platform-platform duplicates are the registry's job, not the scan's");
	}
	{
		const venture::SharedCommandEntry entries[] = {
			{nullptr, false},
			{"", false},
			{nullptr, true},
			{"", true},
		};
		Check(venture::FindPlatformCommandConflicts(entries, 4).empty(),
			"null/empty names never match");
	}
	{
		const venture::SharedCommandEntry entries[] = {
			{"Select All", false},
			{"select all", true},
			{"SELECT ALL", false},
		};
		const auto conflicts = venture::FindPlatformCommandConflicts(entries, 3);
		Check(conflicts.size() == 2, "both colliding engine entries are reported");
		Check(conflicts.size() == 2
				&& conflicts[0].platformIndex == 1 && conflicts[0].otherIndex == 0
				&& !conflicts[0].platformShadows,
			"the first reported pair precedes the platform entry");
		Check(conflicts.size() == 2
				&& conflicts[1].platformIndex == 1 && conflicts[1].otherIndex == 2
				&& conflicts[1].platformShadows,
			"the second reported pair follows the platform entry");
	}

	// --- command-to-event house index (HOOK-08) ------------------------------
	// The example command must never queue HouseIndex = -1: the engine
	// sign-extends the field (0x4C6CC4) and indexes HouseClass::Array with it
	// (0x4C6CCA) before the event-type switch.  The pure helper is the send-side
	// guard, so its edges are checked without the engine.
	std::printf("-- event house index --\n");
	{
		std::int8_t houseIndex = 42;
		Check(!venture::TryEventHouseIndex(-1, houseIndex),
			"a missing current player (-1) is rejected");
		Check(houseIndex == 42, "a rejected index leaves the output untouched");
		Check(!venture::TryEventHouseIndex(-2, houseIndex),
			"any negative index is rejected");
		Check(venture::TryEventHouseIndex(0, houseIndex) && houseIndex == 0,
			"house 0 is accepted");
		Check(venture::TryEventHouseIndex(7, houseIndex) && houseIndex == 7,
			"a normal house index is accepted");
		Check(venture::TryEventHouseIndex(127, houseIndex) && houseIndex == 127,
			"the largest signed-byte index is accepted");
		Check(!venture::TryEventHouseIndex(128, houseIndex),
			"an index that does not fit the signed byte is rejected");
	}

	std::printf("\n");
	if (g_failures == 0)
	{
		std::printf("PASS (0 failures)\n");
		return 0;
	}

	std::printf("FAIL (%d failure%s)\n", g_failures, g_failures == 1 ? "" : "s");
	return 1;
}
