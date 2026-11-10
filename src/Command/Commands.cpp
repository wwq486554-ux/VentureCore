#include "Command/Commands.h"

#include <cstring>
#include <vector>

// ---------------------------------------------------------------------------
// Pure implementation of the command registry.  No engine header and no
// platform logger may be included here - tests/test_commands.cpp links this
// unit on its own.
// ---------------------------------------------------------------------------

namespace
{
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
		if (std::strcmp(command.name, name) == 0)
			return &command;
	}
	return nullptr;
}
}

namespace venture
{
Commands::RegisterResult Commands::Register(const Command& command)
{
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
