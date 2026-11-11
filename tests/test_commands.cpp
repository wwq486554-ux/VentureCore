// Unit test: platform command registry.
//
// The registry in src/Command/Commands.h is engine-free by design (the engine
// CommandClass objects are created in src/Engine/Hooks.Commands.cpp), so the
// test links Commands.cpp alone and checks the data the engine-facing half
// consumes: unique names, UI fields, enable/disable, iteration.
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

	std::printf("\n");
	if (g_failures == 0)
	{
		std::printf("test_commands: all checks passed\n");
		return 0;
	}

	std::printf("test_commands: %d check(s) FAILED\n", g_failures);
	return 1;
}
