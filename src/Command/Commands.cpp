#include "Command/Commands.h"

#include "Core/PlatformGate.h"

#include <cstring>
#include <vector>

// ---------------------------------------------------------------------------
// Pure implementation of the command registry.  No engine header and no
// platform logger may be included here - tests/test_commands.cpp links this
// unit on its own.
// ---------------------------------------------------------------------------

namespace
{
char LowerAscii(const char value)
{
	if (value >= 'A' && value <= 'Z')
		return static_cast<char>(value - 'A' + 'a');
	return value;
}

// Function-local static: safe from the static initializers of any translation
// unit, and never destroyed at unload (the game process owns the DLL).
std::vector<venture::Command>& Registry()
{
	static std::vector<venture::Command> commands;
	return commands;
}

venture::Command* FindMutable(const char* const name)
{
	if (!name || !*name)
		return nullptr;

	for (venture::Command& command : Registry())
	{
		if (venture::CommandNameEquals(command.name, name))
			return &command;
	}
	return nullptr;
}
}

namespace venture
{
bool CommandNameEquals(const char* const left, const char* const right)
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

std::vector<CommandNameConflict> FindPlatformCommandConflicts(
	const SharedCommandEntry* const entries, const std::size_t count)
{
	std::vector<CommandNameConflict> conflicts;
	if (!entries)
		return conflicts;

	for (std::size_t platform = 0; platform < count; ++platform)
	{
		if (!entries[platform].isPlatform
			|| !entries[platform].name
			|| !*entries[platform].name)
		{
			continue;
		}

		for (std::size_t other = 0; other < count; ++other)
		{
			if (other == platform
				|| entries[other].isPlatform
				|| !entries[other].name
				|| !*entries[other].name)
			{
				continue;
			}

			if (!CommandNameEquals(entries[platform].name, entries[other].name))
				continue;

			conflicts.push_back(CommandNameConflict{
				platform,
				other,
				entries[platform].name,
				entries[other].name,
				platform < other,
			});
		}
	}

	return conflicts;
}

bool TryEventHouseIndex(const int currentPlayerArrayIndex, std::int8_t& houseIndex)
{
	// The engine's HouseIndex field is a signed byte that the dispatch entry
	// sign-extends before indexing HouseClass::Array, so a valid index must fit
	// the signed byte range.  A real house index is far smaller; anything else
	// means the caller has no current player and must not send the event.
	if (currentPlayerArrayIndex < 0 || currentPlayerArrayIndex > 0x7F)
		return false;

	houseIndex = static_cast<std::int8_t>(currentPlayerArrayIndex);
	return true;
}

Commands::RegisterResult Commands::Register(const Command& command)
{
	// A refused platform keeps no registration path to the engine (C-06).
	if (PlatformGate::IsRefused())
		return RegisterResult::PlatformRefused;

	if (!command.name || !*command.name
		|| !command.uiName
		|| !command.uiCategory
		|| !command.uiDescription
		|| !command.handler)
	{
		return RegisterResult::InvalidCommand;
	}

	if (FindMutable(command.name) != nullptr)
		return RegisterResult::DuplicateName;

	Registry().push_back(command);
	return RegisterResult::Ok;
}

std::size_t Commands::Count()
{
	return Registry().size();
}

const Command* Commands::At(const std::size_t index)
{
	const std::vector<Command>& commands = Registry();
	return index < commands.size() ? &commands[index] : nullptr;
}

const Command* Commands::Find(const char* const name)
{
	return FindMutable(name);
}

bool Commands::SetEnabled(const char* const name, const bool enabled)
{
	Command* const command = FindMutable(name);
	if (!command)
		return false;

	command->enabled = enabled;
	return true;
}

bool Commands::IsEnabled(const char* const name)
{
	const Command* const command = FindMutable(name);
	return command ? command->enabled : false;
}
}
