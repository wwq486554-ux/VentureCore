#include <EventClass.h>
#include <Helpers/Macro.h>

#include "Core/Log.h"
#include "Core/Platform.h"
#include "Event/EventChannel.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

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
//   * 0x64B6FE size 6  - GetEventSize site 1: answer the size in EDX/EBP;
//   * 0x64BE7D size 6  - GetEventSize site 2: same, plus the size copy on the
//                        stack slot at [esp+0x20] (STACK_OFFSET(0xAC, -0x8C));
//   * 0x64C30E size 6  - GetEventSize site 3: same as site 1, ESI-based type.
//
// The three size hooks are fail-closed (C-01).  EventChannel::GetWireSize()
// maps every byte value to one of three cases: a registered type answers its
// payload size, the engine's own 0x00-0x2E answer 0 so the vanilla table path
// runs, and every unregistered number above 0x2E is answered with one byte -
// never with the engine's out-of-bounds table read (0x8208EC + type), whose
// garbage value the multiplayer decoder would copy into its 104-byte event
// buffer.  Consuming one byte keeps that copy memory-safe; it cannot keep a
// foreign stream aligned (see docs/EventChannel.md).
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

// --- the engine event queue, written locally -------------------------------
// Licensing constraint - see the "YRpp" section of NOTICE.md.  In the pinned
// YRpp revision exactly one header carries an explicit copyleft notice:
// QueueClass.h is GPL-3.0-or-later and defines every queue member function
// inline.  Calling one of those members (OutList.Add(...)) compiles that
// implementation into this DLL and puts the *distributed binary* under
// GPL-3.0, which the MIT grant in LICENSE.md does not do.  The push is
// therefore written here, against the engine's queue layout and behaviour, and
// no member of QueueClass is instantiated - the declaration is used only to
// reach the engine object at EventClass::OutList (0x00A802C8).  A build that
// reintroduces such a call fails scripts/check_copyleft.py.
//
// EventClass::OutList is the engine's QueueClass<EventClass, 128>.  The layout
// below is the engine's, and the static_asserts pin it two ways: against the
// concrete offsets the engine's own code uses, and against YRpp's declaration,
// so a future pin that changes either fails the build instead of making the
// push write into the wrong slot.
struct EventOutQueue
{
	int Count;          // 0x00 - records queued
	int Head;           // 0x04 - index the engine drains from
	int Tail;           // 0x08 - index this file appends at
	EventClass Array[EventClass::MAX_EVENTS];   // 0x0C
	int Timings[EventClass::MAX_EVENTS];        // 0x378C
};

static_assert(EventClass::MAX_EVENTS == 128,
	"the engine event queue holds 128 records - re-check the YRpp pin");
static_assert(offsetof(EventOutQueue, Array) == 0x0C,
	"the engine event slots start at 0x0C");
static_assert(offsetof(EventOutQueue, Timings) == 0x378C,
	"the engine queue timestamps start at 0x378C");
static_assert(sizeof(EventOutQueue) == 0x398C,
	"the engine event queue is 0x398C bytes");
static_assert(sizeof(EventOutQueue) == sizeof(EventClass::OutList),
	"this mirror must stay the size of the engine's queue type");

// The engine's timeGetTime import thunk (gamemd.exe 0x7E1530 - the tick counter
// a queued event is stamped with).  YRpp declares the same thunk as
// Imports::TimeGetTime, but only through the deprecated reference<> helper, so it
// is spelled here with the supported macro instead of suppressing C4996 at the
// use site; the address is an engine fact, not a YRpp implementation.
DEFINE_REFERENCE(Imports::FP_TimeGetTime, EngineTimeGetTime, 0x7E1530u)

// Append one record to the engine's out queue.  Same data flow as the engine
// performs for a vanilla event: copy the record into the slot the tail points
// at, stamp it with the engine tick counter, advance the tail modulo the queue
// size and bump the count.  Returns false when the queue is full.
bool QueueEventForEngine(const EventClass& event)
{
	EventOutQueue& queue = *reinterpret_cast<EventOutQueue*>(&EventClass::OutList);

	// A negative count is corrupt state rather than "space available": treat it
	// like a full queue instead of writing outside the slot array.
	if (static_cast<unsigned>(queue.Count) >= EventClass::MAX_EVENTS)
		return false;

	constexpr int slotMask = EventClass::MAX_EVENTS - 1;
	const int slot = queue.Tail & slotMask;

	std::memcpy(&queue.Array[slot], &event, sizeof(EventClass));
	queue.Timings[slot] = static_cast<int>(EngineTimeGetTime());
	queue.Tail = (slot + 1) & slotMask;
	queue.Count += 1;

	return true;
}

// The engine-side half of EventChannel::Send: queue one serialized channel
// frame as a regular engine event.  This is exactly how the reference ecosystem
// raises the custom events it defines - an EventClass-shaped record added to
// the engine's out queue.
bool SendFrameToEngine(const std::uint8_t* const frame, const std::uint32_t frameSize)
{
	if (!frame || frameSize != sizeof(EventClass))
		return false;

	return QueueEventForEngine(*reinterpret_cast<const EventClass*>(frame));
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

	Log::Info("event channel: unregistered types 0x%02X-0xFF are answered with %u byte(s) "
		"fail-closed (engine types 0x00-0x%02X fall through to the vanilla table)",
		static_cast<unsigned>(EventChannelLastVanillaType) + 1u,
		static_cast<unsigned>(EventChannelFailClosedSize),
		static_cast<unsigned>(EventChannelLastVanillaType));

	Log::Warn("event channel: mixed-client risk - a multiplayer peer that does not run this "
		"platform does not know event types 0x70-0x7F. In this build an unregistered type above "
		"0x2E is consumed as 1 byte fail-closed: that cannot overrun the engine event buffer, but "
		"the event stream is misparsed, which desynchronises the game. A peer build without this "
		"policy falls back to the engine's out-of-bounds size-table read instead. There is no "
		"automatic peer detection: every peer must run a platform build that registers the same "
		"event types and payload sizes.");
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
	// C-06: a refused platform (lineage conflict) keeps its hooks installed but
	// must not act on the engine.
	if (!venture::IsStarted())
		return 0;

	GET(EventClass*, pEvent, ESI);

	if (pEvent)
	{
		const std::uint8_t type = static_cast<std::uint8_t>(pEvent->Type);
		constexpr std::uint32_t availableSize =
			sizeof(EventClass) - offsetof(EventClass, DataBuffer);

		// The registry answers how many bytes the responder expects; Dispatch
		// checks that against the capacity of the engine event record - the real
		// buffer length, not a copy of the value being validated (C-02).  A
		// registered size that does not fit can only come from a broken
		// registry/ABI combination: drop the event loudly instead of letting the
		// responder read past the record.
		if (!venture::EventChannel::Dispatch(
				type,
				reinterpret_cast<const std::uint8_t*>(pEvent->DataBuffer),
				availableSize))
		{
			const std::uint32_t payloadSize = venture::EventChannel::GetDataSize(type);
			if (payloadSize != 0)
			{
				venture::Log::Warn(
					"event channel: event 0x%02X needs %u payload byte(s) but the engine event "
					"record only provides %u - event dropped",
					static_cast<unsigned>(type),
					payloadSize,
					static_cast<unsigned>(availableSize));
			}
		}
	}

	return 0;
}

// GetEventSize site 1 (EDI = event type): answer the size in EDX/EBP and resume
// at the instruction that consumes it.  Engine types (0x00-0x2E) return 0 and
// keep the vanilla table path; every other number is answered here, so the
// engine's out-of-bounds table read can never run for an unregistered type.
DEFINE_HOOK(0x64B6FE, Venture_Event_GetSize1, 0x6)
{
	// C-06: a refused platform (lineage conflict) keeps its hooks installed but
	// must not act on the engine.
	if (!venture::IsStarted())
		return 0;

	const std::uint8_t type = static_cast<std::uint8_t>(R->EDI() & 0xFFu);
	const venture::EventChannel::WireSize wire = venture::EventChannel::GetWireSize(type);

	if (wire.policy == venture::EventChannel::WireSizePolicy::Vanilla)
		return 0;

	R->EDX(wire.size);
	R->EBP(wire.size);
	return kReturnGetSize1;
}

// GetEventSize site 2 (EDI = event type): the original block also stores the
// size in the caller's stack frame at [esp+0x20]; that slot must be written as
// well, exactly like the reference implementation does.
DEFINE_HOOK(0x64BE7D, Venture_Event_GetSize2, 0x6)
{
	// C-06: a refused platform (lineage conflict) keeps its hooks installed but
	// must not act on the engine.
	if (!venture::IsStarted())
		return 0;

	const std::uint8_t type = static_cast<std::uint8_t>(R->EDI() & 0xFFu);
	const venture::EventChannel::WireSize wire = venture::EventChannel::GetWireSize(type);

	if (wire.policy == venture::EventChannel::WireSizePolicy::Vanilla)
		return 0;

	REF_STACK(size_t, sizeInStack, STACK_OFFSET(0xAC, -0x8C));
	sizeInStack = wire.size;
	R->ECX(wire.size);
	R->EBP(wire.size);
	return kReturnGetSize2;
}

// GetEventSize site 3 (ESI = event type).
DEFINE_HOOK(0x64C30E, Venture_Event_GetSize3, 0x6)
{
	// C-06: a refused platform (lineage conflict) keeps its hooks installed but
	// must not act on the engine.
	if (!venture::IsStarted())
		return 0;

	const std::uint8_t type = static_cast<std::uint8_t>(R->ESI() & 0xFFu);
	const venture::EventChannel::WireSize wire = venture::EventChannel::GetWireSize(type);

	if (wire.policy == venture::EventChannel::WireSizePolicy::Vanilla)
		return 0;

	R->ECX(wire.size);
	R->EBP(wire.size);
	return kReturnGetSize3;
}
