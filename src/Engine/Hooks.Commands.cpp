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

// ---------------------------------------------------------------------------
// Command registration - engine adapter.
//
// The pure registry lives in src/Command/Commands.h; this unit turns every
// enabled descriptor into an engine CommandClass object and appends it to
// CommandClass::Array while the engine registers its own commands:
//
//   0x533066 size 6 - CommandClassCallback_Register.  Same site and semantics
//     as the reference ecosystem declares (Phobos/Kratos/Kaleido/Mix
//     .syhks00 tables).  objdump from 0x533066: `mov DWORD PTR [eax],0x7eb89c`
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
// The example command also raises the example event 0x70, so one key press
// exercises the whole path: command -> log -> event channel -> dispatch ->
// responder log (single-player test, see docs/EventChannel.md).
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

	++g_testEventSequence;
	TestEventPayload payload{kTestEventMagic, g_testEventSequence};

	const std::int8_t houseIndex = HouseClass::CurrentPlayer
		? static_cast<std::int8_t>(HouseClass::CurrentPlayer->ArrayIndex)
		: static_cast<std::int8_t>(-1);
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

// Registers the example command and its event at load time: before the engine
// asks for the command list and before any event can be dispatched.
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
}

// --- hook ------------------------------------------------------------------
// Engine's command-registration callback: create one engine command object per
// enabled descriptor and append it to the engine's command array.  The hook
// runs once per process; the startup log line per command lives here because
// this is the only point where the final, engine-visible list is known.
DEFINE_HOOK(0x533066, Venture_Commands_Register, 0x6)
{
	// C-06: a refused platform (lineage conflict) keeps its hooks installed but
	// must not act on the engine.
	if (!venture::IsStarted())
		return 0;

	Log::Info("commands: %zu command(s) in the platform registry", venture::Commands::Count());

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

		Log::Info("commands:   \"%s\" (category \"%ls\", ui name \"%ls\") registered",
			command->name, command->uiCategory, command->uiName);
	}

	return 0;
}
