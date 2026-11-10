#pragma once

#include <cstddef>
#include <cstdint>

// ---------------------------------------------------------------------------
// Platform event channel: the 16 fixed-size engine event types 0x70-0x7F.
//
// The engine's own event types are 0x00-0x2E: 47 numbers, each indexing one
// byte of the engine's payload-size table.  0x2F is the first free number; the
// rest of the byte range is carved up between the DLLs that inject into the
// game (CnCNet 0x30-0x3F, Phobos 0x40-0x45, Kratos 0x50-0x51, Ares
// 0x60-0x61).  This platform owns 0x70-0x7F: 16 event types with a fixed
// payload size each, registered at load time together with the responder that
// handles them on every machine.
//
// The size hooks are fail-closed above the vanilla range: a number greater
// than 0x2E that no registered platform claimed is consumed as one byte
// instead of falling through to the engine's 47-entry table (an out-of-bounds
// read there feeds the multiplayer decoder a garbage size).  See GetWireSize()
// and the order contract documented in docs/EventChannel.md.
//
// This unit is pure (no engine headers, no platform logger) so the registry,
// routing and the frame codec are unit-testable.  The engine side lives in
// src/Engine/Hooks.Events.cpp:
//   * it installs the engine sender with SetSender() at load time;
//   * it defines LogStartup();
//   * the four engine hooks (dispatch + three GetEventSize sites) feed
//     GetWireSize()/Dispatch() and take their sizes from the registry.
//
// See docs/EventChannel.md for the wire format, the number-range argument,
// the fail-closed policy and the mixed-client warning.
// ---------------------------------------------------------------------------

namespace venture
{
inline constexpr std::uint8_t EventChannelFirstType = 0x70;
inline constexpr std::uint8_t EventChannelLastType = 0x7F;
inline constexpr std::uint32_t EventChannelSlotCount = 16;

// The engine's EventClass union is 104 bytes; a payload can never be larger.
inline constexpr std::uint32_t EventChannelMaxPayload = 104;

// The engine numbers its own event types 0x00-0x2E: 47 numbers, each indexing
// one byte of the engine's payload-size table.  0x2F is the first free number;
// the engine table read must never see a number above EventChannelLastVanillaType.
inline constexpr std::uint8_t EventChannelLastVanillaType = 0x2E;

// Payload size the GetEventSize hooks answer for an unregistered number above
// the vanilla range: consume exactly one byte.  Memory-safe, not stream-aligned
// (a peer that sent a longer payload desynchronises, but cannot overrun).
inline constexpr std::uint32_t EventChannelFailClosedSize = 1;

class EventChannel
{
public:
	using Type = std::uint8_t;

	// Runs on every machine that receives the event.  `data` points at the
	// payload inside the engine event (or inside the frame handed to the
	// codec); `size` is the registered payload size.
	using Responder = void (*)(const std::uint8_t* data, std::uint32_t size);

	// Engine adapter hook point.  Installed once by the engine-facing unit
	// (SetSender).  It receives one serialized channel frame and queues it as
	// an engine event; it returns false when the engine refused the event.
	using EngineSender = bool (*)(const std::uint8_t* frame, std::uint32_t frameSize);

	// One engine event record.  Byte for byte the layout of the engine's
	// EventClass (111 bytes): the frame is what travels event transport, so
	// keeping the layouts identical means Send() needs no re-packing.
#pragma pack(push, 1)
	struct Frame
	{
		std::uint8_t type;         // 0x70-0x7F
		std::uint8_t isExecuted;   // 0 on the wire; the engine sets it when it runs
		std::int8_t houseIndex;    // player house that raised it; -1 = none
		std::uint32_t frameNumber; // engine frame the event is for
		std::uint8_t payload[EventChannelMaxPayload]; // zero-padded
	};
#pragma pack(pop)

	static constexpr std::uint32_t FrameSize = sizeof(Frame); // 111
	static constexpr std::uint32_t FramePayloadOffset = offsetof(Frame, payload); // 7

	static_assert(FrameSize == 111, "the channel frame must match the engine's 111-byte event record");
	static_assert(FramePayloadOffset == 7, "the engine's EventClass::DataBuffer sits at offset 7");

	struct Entry
	{
		Type type;
		const char* name;
		std::uint32_t payloadSize;
		Responder responder;
	};

	enum class RegisterResult : std::uint8_t
	{
		Ok = 0,
		InvalidType,       // outside 0x70-0x7F
		InvalidName,       // null or empty
		InvalidPayloadSize, // 0, or larger than the engine's 104-byte union
		InvalidResponder,  // null
		AlreadyRegistered, // the type already has a handler
	};

	// --- registration ------------------------------------------------------
	// Registers one event type.  The type must be in the channel range, unused,
	// the name non-empty and the payload size in 1..104 bytes.  Registration
	// happens at load time (before the engine can raise or dispatch an event),
	// so Dispatch() can be lock-free.
	static RegisterResult Register(
		Type type,
		const char* name,
		std::uint32_t payloadSize,
		Responder responder);

	// Type is inside the channel range 0x70-0x7F.
	static bool IsValidEventType(Type type);

	// Type is inside the range and a handler was registered for it.
	static bool IsOwnEventType(Type type);

	// Payload size of a registered type; 0 means "not ours".
	static std::uint32_t GetDataSize(Type type);

	// --- engine size policy ------------------------------------------------
	// The three engine GetEventSize hooks size an event before the engine reads
	// it.  The engine's own payload-size table only covers 0x00-0x2E, so the
	// hooks have to answer for every other number themselves: a number above
	// the vanilla range that our registry does not own is consumed as one fixed
	// byte (fail-closed) instead of falling through to the engine's
	// out-of-bounds table read.  Only the engine's own types fall through.
	//
	// Order contract: engine hooks run in DLL load order, and this platform
	// loads after the other known platforms (Ares/Kratos/Phobos/Mix; see
	// docs/EventChannel.md).  A foreign number therefore only reaches our hooks
	// when no earlier platform claimed it - and then answering it ourselves is
	// the safe behaviour.  Any platform that loads *after* this one has to
	// answer unclaimed numbers itself: it cannot rely on the engine's table.
	enum class WireSizePolicy : std::uint8_t
	{
		Vanilla = 0,    // 0x00-0x2E: return 0 so the engine answers
		Registered = 1, // a registered channel type: answer its payload size
		FailClosed = 2, // unregistered number above 0x2E: answer one byte
	};

	struct WireSize
	{
		WireSizePolicy policy = WireSizePolicy::FailClosed;
		std::uint32_t size = EventChannelFailClosedSize;
	};

	// Engine-free size policy used by the three GetEventSize hooks (and by the
	// unit test).  Total: every byte value maps to exactly one of the cases.
	static WireSize GetWireSize(Type type);

	// Registered name, or nullptr when the type is not ours.
	static const char* Name(Type type);

	static std::size_t Count();

	// Iteration in ascending type order (0x70 first).  Returns nullptr for an
	// index >= Count(); used for the startup log.
	static const Entry* EntryAt(std::size_t index);

	// --- dispatch ----------------------------------------------------------
	// Routes one received event to its responder.  `availableSize` is how many
	// payload bytes the caller can actually vouch for (the engine dispatch hook
	// passes the payload capacity of its fixed event record); the responder is
	// always called with the *registered* payload size.  Returns false (and
	// calls nothing) when the type is not ours, `data` is null, or the
	// registered size is larger than `availableSize`; the engine adapter logs
	// that last case as a warning.
	//
	// Dispatch cannot validate the contents of a payload: the wire format is
	// not self-describing, so every responder has to check its own
	// magic/version/length contract (see docs/EventChannel.md).
	static bool Dispatch(Type type, const std::uint8_t* data, std::uint32_t availableSize);

	// --- frame codec -------------------------------------------------------
	// Serializes one event into `buffer`; returns the number of bytes written
	// (FrameSize) or 0 when the arguments are invalid (buffer too small,
	// unregistered type, payload size mismatch, null payload).
	static std::uint32_t WriteFrame(
		std::uint8_t* buffer,
		std::uint32_t bufferSize,
		Type type,
		std::int8_t houseIndex,
		std::uint32_t frameNumber,
		const std::uint8_t* payload,
		std::uint32_t payloadSize);

	struct FrameView
	{
		Type type = 0;
		std::int8_t houseIndex = -1;
		std::uint32_t frameNumber = 0;
		std::uint32_t payloadSize = 0;
		const std::uint8_t* payload = nullptr; // points into the caller's buffer
	};

	// Validates and decodes one serialized frame.  The type must be registered;
	// `payload` points into `frame` and stays valid as long as the buffer does.
	static bool ReadFrame(const std::uint8_t* frame, std::uint32_t frameSize, FrameView& out);

	// --- engine adapter ----------------------------------------------------
	// Installs the engine-side sender.  The logic above never calls the engine
	// directly, so the unit test replaces the sender with a capture function.
	static void SetSender(EngineSender sender);
	static EngineSender Sender();

	// Serializes and queues one event through the installed sender.  Returns
	// false when the event is not ours, the payload size is wrong, or no
	// sender is installed.
	static bool Send(
		Type type,
		std::int8_t houseIndex,
		std::uint32_t frameNumber,
		const std::uint8_t* payload,
		std::uint32_t payloadSize);

	// --- startup logging ---------------------------------------------------
	// Writes the occupied event numbers and the mixed-client warning to the
	// platform log.  Defined in src/Engine/Hooks.Events.cpp so this unit stays
	// free of the platform logger (and of <windows.h>).
	static void LogStartup();
};
}
