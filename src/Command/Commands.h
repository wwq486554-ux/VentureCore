#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

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
// This unit is engine-free on purpose: tests/test_commands.cpp links
// src/Command/Commands.cpp alone.  The two helpers below are the engine-free
// halves of the two command-layer fixes:
//   * FindPlatformCommandConflicts() - the shared-list name-collision scan that
//     the post-registration hook runs once the engine's own registration is
//     complete (HOOK-03);
//   * TryEventHouseIndex() - the command-to-event house attribution guard that
//     keeps HouseIndex = -1 out of the engine's event queue (HOOK-08).
//
// See docs/Commands.md.
// ---------------------------------------------------------------------------

namespace venture
{
// Case-insensitive ASCII comparison of two engine command names.  Command names
// are a single flat namespace shared by the engine and every injected DLL, so a
// second name that differs only in case would be ambiguous in the keyboard
// configuration (C-08).  Null pointers compare equal only to each other.
bool CommandNameEquals(const char* left, const char* right);

// --- shared engine command list: name-collision scan ------------------------
//
// The engine keeps its own CommandClass objects and every injected platform's
// objects in one flat array (CommandClass::Array) and its lookup is first-match,
// so a name that two entries share silently shadows whichever one comes later.
// The registration hook (0x533066) runs in the middle of the engine's own
// registration, so the scan has to happen after the whole list is final: the
// post-registration hook in src/Engine/Hooks.Commands.cpp builds one snapshot
// of the array and asks this function for the collisions that involve a
// platform command.
struct SharedCommandEntry
{
	const char* name; // the name the keyboard configuration keys on
	bool isPlatform;  // true for one of this platform's own CommandClass objects
};

struct CommandNameConflict
{
	std::size_t platformIndex; // index of the platform entry in the snapshot
	std::size_t otherIndex;    // index of the other entry in the snapshot
	const char* platformName;  // both point into the snapshot entries
	const char* otherName;
	// true: the platform entry comes first, so the game's first-match lookup
	// finds the platform command and shadows the other command.
	// false: the other command comes first; the platform command is shadowed.
	bool platformShadows;
};

// Scans one snapshot of the engine command list for name collisions that
// involve a platform entry (case-insensitively, the same comparison the engine
// and the registry use).  Entries with a null/empty name are ignored.  Pairs
// are returned in snapshot order (platform index, then other index); the
// function does not modify the snapshot and is testable without the engine.
std::vector<CommandNameConflict> FindPlatformCommandConflicts(
	const SharedCommandEntry* entries, std::size_t count);

// --- command -> event house attribution -------------------------------------
//
// Derives the engine's signed-byte EventClass::HouseIndex from the current
// player's house array index.  A negative currentPlayerArrayIndex means "there
// is no current player" and must not be turned into HouseIndex = -1: the engine
// sign-extends the field when it dispatches an event (0x4C6CC4
// `movsx ebx,byte [esi+2]`) and indexes HouseClass::Array with it
// unconditionally at 0x4C6CCA, so -1 is a 4-byte out-of-bounds read on every
// receiver before the event-type switch.  Values that do not fit the signed
// byte field are rejected for the same reason.  Returns false and leaves
// `houseIndex` untouched when there is no valid index; engine-free so the tests
// can drive the edge cases (HOOK-08).
bool TryEventHouseIndex(int currentPlayerArrayIndex, std::int8_t& houseIndex);

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
	// unique and compared case-insensitively (the engine and every injected DLL
	// share one flat command namespace); registration happens at load time,
	// before the engine asks for the command list.  Name every platform command
	// with the platform prefix (e.g. "Venture Test Command") - see
	// docs/Commands.md.  Once the platform has refused to
	// start (PlatformGate::IsRefused()) the registry is closed and every call
	// returns PlatformRefused: a refused DLL must not reach the engine's command
	// list (C-06).
	static RegisterResult Register(const Command& command);

	static std::size_t Count();

	// Iteration in registration order; nullptr for an index >= Count().
	static const Command* At(std::size_t index);

	// Lookup by engine name, case-insensitively; nullptr when the name is not
	// registered.
	static const Command* Find(const char* name);

	// Runtime switch for a registered command (the same flag the engine
	// registration path reads).  Returns false when the name is unknown.
	static bool SetEnabled(const char* name, bool enabled);
	static bool IsEnabled(const char* name);
};
}
