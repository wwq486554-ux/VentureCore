#include <CommandClass.h>
#include <Fundamentals.h>
#include <Helpers/Macro.h>
#include <HouseClass.h>
#include <Memory.h>

#include "Command/Commands.h"
#include "Core/Log.h"
#include "Core/Platform.h"
#include "Event/EventChannel.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

// ---------------------------------------------------------------------------
// Command registration - engine adapter.
//
// The pure registry lives in src/Command/Commands.h; this unit turns every
// enabled descriptor into an engine CommandClass object and appends it to
// CommandClass::Array while the engine registers its own commands:
//
//   0x533066 size 6 - CommandClassCallback_Register.  Same site and semantics
//     as the reference ecosystem declares (the audited Phobos/Kratos/Mix
//     `.syhks00` tables and one closed-source ecosystem platform).
//     objdump from 0x533066: `mov DWORD PTR [eax],0x7eb89c`
//     (6 bytes), next instruction 0x53306C.  Returning 0 lets Syringe's
//     trampoline re-execute the overwritten store, so the engine's own command
//     object is still initialised.
//
// The engine registers commands once per process, then reloads the hotkey map
// from KEYBOARDMD.INI [Hotkey] (0x533D20, verified in the engine): a command
// cannot carry a hard-coded default key from here.  Registered commands appear
// in Options -> Keyboard, where the player binds them - the ecosystem
// convention.  See docs/Commands.md.
//
// The engine's own registration is a long inline sequence *inside* the routine
// that calls this hook, so a name colliding with a command the engine registers
// after our 0x533066 entry is not visible yet.  A second, post-registration hook
// closes that gap (HOOK-03):
//
//   0x533C3E size 5 - scan the final CommandClass::Array for name collisions
//     that involve a platform command and warn with the real first-match
//     direction (both directions, engine commands and other DLLs' commands
//     alike).  objdump from 0x533C3E: `call 0x533D20` (5 bytes), next
//     instruction 0x533C43.  The call rebuilds CommandClass::Hotkeys and is
//     made after the last CommandClass::Array append in that routine (that
//     append is the `call 0x538780` at 0x533C39; no array reference follows
//     this site), so the handler observes the completed list before the engine
//     locks in the key map.  Free site: none of the five
//     audited ecosystem `.syhks00` tables (Ares / Kratos / Mix / Phobos and one
//     closed-source platform) has a window in 0x533B00-0x533E00.  The handler
//     returns 0, so the trampoline re-executes the call and the hotkey rebuild
//     still runs.
//
// The example command also raises the example event 0x70, so one key press
// exercises the whole path: command -> log -> event channel -> dispatch ->
// responder log (single-player test, see docs/EventChannel.md).  It never sends
// HouseIndex = -1 (HOOK-08): the engine sign-extends that field and indexes
// HouseClass::Array with it before the event-type switch (0x4C6CC4 / 0x4C6CCA),
// so a negative index would be an out-of-bounds read on every receiver.
// ---------------------------------------------------------------------------

namespace
{
using venture::Log;
using Channel = venture::EventChannel;

// The engine's command object for one platform descriptor.  The engine keeps a
// raw pointer in CommandClass::Array for the process lifetime, so the object is
// allocated with the engine allocator (GameCreate), exactly like the engine's
// own command objects.
class PlatformCommandClass final : public CommandClass
{
public:
	explicit PlatformCommandClass(const venture::Command& command) : m_command(command) {}

	const char* GetName() const override { return m_command.name; }
	const wchar_t* GetUIName() const override { return m_command.uiName; }
	const wchar_t* GetUICategory() const override { return m_command.uiCategory; }
	const wchar_t* GetUIDescription() const override { return m_command.uiDescription; }

	// Fire on the key-down event only; the key-up event is ignored, like the
	// engine's own command classes.
	bool ExtraTriggerCondition(const WWKey eInput) const override
	{
		return !(static_cast<unsigned>(eInput) & static_cast<unsigned>(WWKey::Release));
	}

	void Execute(const WWKey eInput) const override
	{
		(void)eInput;
		if (m_command.handler)
			m_command.handler();
	}

private:
	venture::Command m_command;
};

// --- example command and example event -------------------------------------
#pragma pack(push, 1)
struct TestEventPayload
{
	std::uint32_t magic;    // 'VENT': sanity check on both ends
	std::uint32_t sequence; // incremented once per raised event
};
#pragma pack(pop)

constexpr std::uint8_t kTestEventType = venture::EventChannelFirstType; // 0x70
constexpr std::uint32_t kTestEventMagic = 0x56454E54u;                  // 'VENT'
std::uint32_t g_testEventSequence = 0;

void RespondTestEvent(const std::uint8_t* const data, const std::uint32_t size)
{
	if (!data || size != sizeof(TestEventPayload))
	{
		Log::Warn("[Venture] event 0x%02X: unexpected payload size %u",
			static_cast<unsigned>(kTestEventType), size);
		return;
	}

	TestEventPayload payload{};
	std::memcpy(&payload, data, sizeof(payload));

	if (payload.magic != kTestEventMagic)
	{
		Log::Warn("[Venture] event 0x%02X: bad payload magic 0x%08X",
			static_cast<unsigned>(kTestEventType), payload.magic);
		return;
	}

	Log::Info("[Venture] event 0x%02X received (sequence=%u) - command and event paths are live",
		static_cast<unsigned>(kTestEventType), payload.sequence);
}

// The example command: one key press exercises command -> log -> event send ->
// dispatch -> responder.
void ExecuteTestCommand()
{
	Log::Info("[Venture] command \"Venture Test Command\" executed");

	// HOOK-08: never send HouseIndex = -1.  The engine's dispatch entry
	// sign-extends the field (0x4C6CC4 `movsx ebx,byte [esi+2]`) and indexes
	// HouseClass::Array with it unconditionally at 0x4C6CCA
	// (`mov edi,[ecx+ebx*4]`) before the event-type switch, so a negative index
	// is a 4-byte out-of-bounds read on every receiver.  With no current player
	// there is no house to attribute the event to: drop the event instead of
	// creating the negative index.
	HouseClass* const currentPlayer = HouseClass::CurrentPlayer;
	const int currentPlayerIndex = currentPlayer
		? static_cast<int>(currentPlayer->ArrayIndex)
		: -1;
	std::int8_t houseIndex = 0;
	if (!venture::TryEventHouseIndex(currentPlayerIndex, houseIndex))
	{
		Log::Warn("[Venture] command: no usable current player - event 0x%02X not "
			"raised (HouseIndex must not be -1; see docs/Commands.md)",
			static_cast<unsigned>(kTestEventType));
		return;
	}

	++g_testEventSequence;
	TestEventPayload payload{kTestEventMagic, g_testEventSequence};
	const std::uint32_t frameNumber = static_cast<std::uint32_t>(Unsorted::CurrentFrame);

	if (!Channel::Send(
			kTestEventType,
			houseIndex,
			frameNumber,
			reinterpret_cast<const std::uint8_t*>(&payload),
			static_cast<std::uint32_t>(sizeof(payload))))
	{
		Log::Warn("[Venture] command could not raise event 0x%02X",
			static_cast<unsigned>(kTestEventType));
	}
}

// The engine command objects this translation unit appended to
// CommandClass::Array.  The post-registration check uses this pointer set to
// tell the platform's own entries from the engine's (and other injected DLLs')
// entries: the build disables RTTI (/GR-), so the object type cannot be queried
// at run time.  Declared before the static registrar below so its dynamic
// initialization is finished before anything can run the registration hook.
std::vector<const CommandClass*> g_platformCommands;

// Registers the example command and its event at load time: before the engine
// asks for the command list and before any event can be dispatched.
//
// This object is a file-static initializer, so the two Log::Warn calls below run
// before Platform::Startup() and therefore before Log::Open().  They are not
// lost: the logger queues Warn/Error lines written while the file is closed and
// flushes them into <Platform>.log when it opens (START-01).
struct Registrar
{
	Registrar()
	{
		const venture::Commands::RegisterResult commandResult = venture::Commands::Register({
			"Venture Test Command",
			L"Venture Test Command",
			L"VentureCore",
			L"Logs a line and raises the platform example event 0x70",
			true,
			&ExecuteTestCommand,
		});
		if (commandResult != venture::Commands::RegisterResult::Ok)
		{
			Log::Warn("[Venture] could not register the example command (result %d)",
				static_cast<int>(commandResult));
		}

		const Channel::RegisterResult eventResult = Channel::Register(
			kTestEventType, "Venture_Event_Test", sizeof(TestEventPayload), &RespondTestEvent);
		if (eventResult != Channel::RegisterResult::Ok)
		{
			Log::Warn("[Venture] could not register the example event 0x%02X (result %d)",
				static_cast<unsigned>(kTestEventType), static_cast<int>(eventResult));
		}
	}
};

const Registrar g_registrar;

bool IsPlatformCommand(const CommandClass* const command)
{
	for (const CommandClass* const platformCommand : g_platformCommands)
	{
		if (platformCommand == command)
			return true;
	}
	return false;
}

// Snapshot of the engine's shared command list plus the collision scan.  One
// warning per collision, with the real first-match direction: a platform entry
// that comes first shadows the other command, a platform entry that comes
// later is the shadowed one.  The registration hook is mid-way through the
// engine's own registration, so this only runs once the array is final
// (HOOK-03; see the file header for the 0x533C3E window evidence).
void CheckRegisteredCommandNames()
{
	std::vector<venture::SharedCommandEntry> entries;
	entries.reserve(static_cast<std::size_t>(CommandClass::Array.Count));

	for (int index = 0; index < CommandClass::Array.Count; ++index)
	{
		const CommandClass* const pCommand = CommandClass::Array[index];
		if (!pCommand)
			continue;

		entries.push_back(venture::SharedCommandEntry{
			pCommand->GetName(),
			IsPlatformCommand(pCommand),
		});
	}

	std::size_t platformCount = 0;
	for (const venture::SharedCommandEntry& entry : entries)
	{
		if (entry.isPlatform)
			++platformCount;
	}

	const std::vector<venture::CommandNameConflict> conflicts =
		venture::FindPlatformCommandConflicts(entries.data(), entries.size());

	for (const venture::CommandNameConflict& conflict : conflicts)
	{
		if (conflict.platformShadows)
		{
			Log::Warn(
				"commands:   platform command \"%s\" was registered before \"%s\" (engine or another "
				"DLL) - the game's first-match lookup finds the platform command first, so the other "
				"command is shadowed; the mandatory platform prefix prevents this (see docs/Commands.md)",
				conflict.platformName,
				conflict.otherName);
		}
		else
		{
			Log::Warn(
				"commands:   platform command \"%s\" was registered after \"%s\" (engine or another "
				"DLL) - the game's first-match lookup finds the other command first, so the platform "
				"command is shadowed; rename it with the platform prefix (see docs/Commands.md)",
				conflict.platformName,
				conflict.otherName);
		}
	}

	Log::Info(
		"commands: post-registration check - %zu platform command(s) among %zu engine-list entry(s), "
		"%zu name conflict(s)",
		platformCount,
		entries.size(),
		conflicts.size());
}
}

// --- hook: register the platform commands ----------------------------------
// Engine's command-registration callback: create one engine command object per
// enabled descriptor and append it to the engine's command array.  The hook
// runs once per process, mid-way through the engine's own inline registration
// sequence; the post-registration hook below does the final name check.
DEFINE_HOOK(0x533066, Venture_Commands_Register, 0x6)
{
	// C-06: a refused platform (lineage conflict) keeps its hooks installed but
	// must not act on the engine.
	if (!venture::IsStarted())
		return 0;

	// The engine calls the registration callback once per process; clear the
	// pointer set so a (defensive) second call cannot mix old and new entries.
	g_platformCommands.clear();

	Log::Info("commands: %zu command(s) in the platform registry", venture::Commands::Count());
	int appended = 0;

	for (std::size_t index = 0; index < venture::Commands::Count(); ++index)
	{
		const venture::Command* const command = venture::Commands::At(index);
		if (!command)
			continue;

		if (!command->enabled)
		{
			Log::Info("commands:   \"%s\" (category \"%ls\") skipped - disabled",
				command->name, command->uiCategory);
			continue;
		}

		PlatformCommandClass* const pCommand = GameCreate<PlatformCommandClass>(*command);
		if (!pCommand)
		{
			Log::Warn("commands:   \"%s\" - engine command allocation failed", command->name);
			continue;
		}

		if (!CommandClass::Array.AddItem(pCommand))
		{
			Log::Warn("commands:   \"%s\" - the engine command array refused the command", command->name);
			continue;
		}

		g_platformCommands.push_back(pCommand);
		++appended;
		Log::Info("commands:   \"%s\" (category \"%ls\", ui name \"%ls\") registered",
			command->name, command->uiCategory, command->uiName);
	}

	Log::Info("commands: %d command(s) appended to the engine command list", appended);
	return 0;
}

// --- hook: verify the completed command list (HOOK-03) ---------------------
// 0x533C3E is the `call 0x533D20` inside the same engine registration routine
// that rebuilds CommandClass::Hotkeys; it runs after the last
// CommandClass::Array append (the `call 0x538780` at 0x533C39; no array
// reference follows this site), i.e. once every engine command and every other
// DLL's command is in the array.  Hooking its
// return-0 window re-executes the call, so the engine's hotkey rebuild is
// unchanged.  See the file header and docs/HookCatalogue.md for the window
// evidence and the free-site audit.
DEFINE_HOOK(0x533C3E, Venture_Commands_VerifyRegistered, 0x5)
{
	// C-06: a refused platform keeps its hooks installed but must not act on the
	// engine; the registration hook also returned early, so there is nothing to
	// verify.
	if (!venture::IsStarted())
		return 0;

	CheckRegisteredCommandNames();
	return 0;
}
