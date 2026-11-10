#include <EventClass.h>
#include <Helpers/Macro.h>

#include "Core/Log.h"
#include "Event/EventChannel.h"

#include <cstddef>
#include <cstdint>

// ---------------------------------------------------------------------------
// Event channel - engine adapter.
//
// The engine-facing half of src/Event/EventChannel.h: the four engine hooks,
// the engine sender installed into the channel at load time, and the startup
// log.  The registry/routing/codec logic itself is engine-free and unit-tested
// in tests/test_event_channel.cpp (see docs/EventChannel.md).
//
// All four hook sites and their window sizes are the ones the wider
// Phobos / Kratos / Mix ecosystem declares for the same engine functions; the
// semantics are copied from that reference behaviour, not from its code:
//   * 0x4C6CC8 size 5  - event dispatch: route our types, then let the engine
//                        continue its own switch (always return 0);
//   * 0x64B6FE size 6  - GetEventSize site 1: skip the vanilla size-table read
//                        for our types and resume with the size in EDX/EBP;
//   * 0x64BE7D size 6  - GetEventSize site 2: same, plus the size copy on the
//                        stack slot at [esp+0x20] (STACK_OFFSET(0xAC, -0x8C));
//   * 0x64C30E size 6  - GetEventSize site 3: same as site 1, ESI-based type.
// ---------------------------------------------------------------------------

namespace
{
// The channel frame mirrors the engine's event record byte for byte.  If YRpp
// ever moves EventClass, the build fails here instead of the game queue
// receiving a corrupted record.
static_assert(sizeof(EventClass) == 111, "engine EventClass ABI changed - re-check the YRpp pin");
static_assert(offsetof(EventClass, Type) == 0, "engine EventClass::Type moved");
static_assert(offsetof(EventClass, IsExecuted) == 1, "engine EventClass::IsExecuted moved");
static_assert(offsetof(EventClass, HouseIndex) == 2, "engine EventClass::HouseIndex moved");
static_assert(offsetof(EventClass, Frame) == 3, "engine EventClass::Frame moved");
static_assert(offsetof(EventClass, DataBuffer) == 7, "engine EventClass::DataBuffer moved");

static_assert(sizeof(venture::EventChannel::Frame) == sizeof(EventClass),
	"the channel frame must be the size of an engine event record");
static_assert(offsetof(venture::EventChannel::Frame, payload) == offsetof(EventClass, DataBuffer),
	"the channel frame payload must sit where the engine's DataBuffer sits");
static_assert(offsetof(venture::EventChannel::Frame, type) == offsetof(EventClass, Type),
	"the channel frame type field must sit where the engine's Type sits");
static_assert(offsetof(venture::EventChannel::Frame, isExecuted) == offsetof(EventClass, IsExecuted),
	"the channel frame executed flag must sit where the engine's IsExecuted sits");
static_assert(offsetof(venture::EventChannel::Frame, houseIndex) == offsetof(EventClass, HouseIndex),
	"the channel frame house index must sit where the engine's HouseIndex sits");
static_assert(offsetof(venture::EventChannel::Frame, frameNumber) == offsetof(EventClass, Frame),
	"the channel frame number must sit where the engine's Frame sits");

constexpr std::uint32_t kReturnGetSize1 = 0x64B71D; // last vanilla size-table instruction skipped
constexpr std::uint32_t kReturnGetSize2 = 0x64BE97;
constexpr std::uint32_t kReturnGetSize3 = 0x64C321;

// The engine-side half of EventChannel::Send: queue one serialized channel
// frame as a regular engine event.  EventClass::OutList is the engine's event
// out queue; this is exactly how the reference ecosystem raises the custom
// events it defines (an EventClass-shaped record added to OutList).
bool SendFrameToEngine(const std::uint8_t* const frame, const std::uint32_t frameSize)
{
	if (!frame || frameSize != sizeof(EventClass))
		return false;

	const EventClass* const pEvent = reinterpret_cast<const EventClass*>(frame);
	return EventClass::OutList.Add(*pEvent);
}

// Installed at load time, before the game can execute a command that raises an
// event; the channel itself never calls the engine directly.
struct SenderInstaller
{
	SenderInstaller() { venture::EventChannel::SetSender(&SendFrameToEngine); }
};

const SenderInstaller g_senderInstaller;
}

namespace venture
{
void EventChannel::LogStartup()
{
	Log::Info("event channel: 0x%02X-0x%02X, %zu registered handler(s)",
		static_cast<unsigned>(EventChannelFirstType),
		static_cast<unsigned>(EventChannelLastType),
		EventChannel::Count());

	for (std::size_t index = 0; index < EventChannel::Count(); ++index)
	{
		const Entry* const entry = EventChannel::EntryAt(index);
		if (!entry)
			continue;

		Log::Info("event channel:   0x%02X %-24s %u byte(s)",
			static_cast<unsigned>(entry->type), entry->name, entry->payloadSize);
	}

	Log::Warn("event channel: mixed-client risk - a multiplayer peer that does not run this "
		"platform does not know event types 0x70-0x7F; its event-size hooks return the vanilla "
		"sizes and our events are misparsed, which desynchronises the game. There is no automatic "
		"peer detection: every peer must run a platform build that registers the same event types.");
}
}

// Called from Platform::Startup() (one line, see the call site).  A free
// function in the global namespace: the call site is a block-scope declaration
// (the startup source must not grow an include just for one log call), and a
// block-scope declaration binds to the global namespace.
void LogEventChannelStartup()
{
	venture::EventChannel::LogStartup();
}

// --- hooks -----------------------------------------------------------------
// Event dispatch: EventClass* in ESI, event type in [esi].  The engine's own
// switch below this site does not know 0x70-0x7F, so our handler must run here
// and the original instructions still have to execute afterwards (return 0).
DEFINE_HOOK(0x4C6CC8, Venture_Event_Dispatch, 0x5)
{
	GET(EventClass*, pEvent, ESI);

	if (pEvent)
	{
		const std::uint8_t type = static_cast<std::uint8_t>(pEvent->Type);
		const std::uint32_t size = venture::EventChannel::GetDataSize(type);
		if (size != 0)
		{
			venture::EventChannel::Dispatch(
				type,
				reinterpret_cast<const std::uint8_t*>(pEvent->DataBuffer),
				size);
		}
	}

	return 0;
}

// GetEventSize site 1 (EDI = event type): replace the vanilla size-table read
// (the table only holds the engine's own types) and resume at the instruction
// that consumes the size.
DEFINE_HOOK(0x64B6FE, Venture_Event_GetSize1, 0x6)
{
	const std::uint8_t type = static_cast<std::uint8_t>(R->EDI() & 0xFFu);
	const std::uint32_t size = venture::EventChannel::GetDataSize(type);

	if (size != 0)
	{
		R->EDX(size);
		R->EBP(size);
		return kReturnGetSize1;
	}

	return 0;
}

// GetEventSize site 2 (EDI = event type): the original block also stores the
// size in the caller's stack frame at [esp+0x20]; that slot must be written as
// well, exactly like the reference implementation does.
DEFINE_HOOK(0x64BE7D, Venture_Event_GetSize2, 0x6)
{
	const std::uint8_t type = static_cast<std::uint8_t>(R->EDI() & 0xFFu);
	const std::uint32_t size = venture::EventChannel::GetDataSize(type);

	if (size != 0)
	{
		REF_STACK(size_t, sizeInStack, STACK_OFFSET(0xAC, -0x8C));
		sizeInStack = size;
		R->ECX(size);
		R->EBP(size);
		return kReturnGetSize2;
	}

	return 0;
}

// GetEventSize site 3 (ESI = event type).
DEFINE_HOOK(0x64C30E, Venture_Event_GetSize3, 0x6)
{
	const std::uint8_t type = static_cast<std::uint8_t>(R->ESI() & 0xFFu);
	const std::uint32_t size = venture::EventChannel::GetDataSize(type);

	if (size != 0)
	{
		R->ECX(size);
		R->EBP(size);
		return kReturnGetSize3;
	}

	return 0;
}
