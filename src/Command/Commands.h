#pragma once

#include <cstddef>
#include <cstdint>

// ---------------------------------------------------------------------------
// Platform player commands: a testable registry of command descriptors.
//
// The engine exposes commands through CommandClass (see
// external/YRpp/CommandClass.h): every command object is appended to
// CommandClass::Array and shows up in the game's keyboard configuration under
// its UI category, from where the player binds a hotkey.  The platform keeps
// the descriptor data in this engine-free registry and src/Engine/
// Hooks.Commands.cpp creates the engine CommandClass object for every enabled
// descriptor when the engine calls its command-registration callback.
//
// See docs/Commands.md.
// ---------------------------------------------------------------------------

namespace venture
{
struct Command
{
	const char* name;             // unique name (CommandClass::GetName); the keyboard config keys on it
	const wchar_t* uiName;        // name shown in the keyboard dialog (GetUIName)
	const wchar_t* uiCategory;    // dialog category (GetUICategory)
	const wchar_t* uiDescription; // dialog description (GetUIDescription)
	bool enabled;                 // disabled commands are not registered with the engine
	void (*handler)();            // runs when the player triggers the command
};

class Commands
{
public:
	enum class RegisterResult : std::uint8_t
	{
		Ok = 0,
		InvalidCommand, // a required field is missing
		PlatformRefused, // the platform refused to start (lineage conflict) - the registry is closed
		DuplicateName,  // the name is already registered
	};

	// Registers one command descriptor (copied into the registry).  Names are
	// unique and compared exactly; registration happens at load time, before
	// the engine asks for the command list.  Once the platform has refused to
	// start (PlatformGate::IsRefused()) the registry is closed and every call
	// returns PlatformRefused: a refused DLL must not reach the engine's command
	// list (C-06).
	static RegisterResult Register(const Command& command);

	static std::size_t Count();

	// Iteration in registration order; nullptr for an index >= Count().
	static const Command* At(std::size_t index);

	// Lookup by engine name; nullptr when the name is not registered.
	static const Command* Find(const char* name);

	// Runtime switch for a registered command (the same flag the engine
	// registration path reads).  Returns false when the name is unknown.
	static bool SetEnabled(const char* name, bool enabled);
	static bool IsEnabled(const char* name);
};
}
