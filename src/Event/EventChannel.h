#pragma once

#include <cstddef>
#include <cstdint>

// ---------------------------------------------------------------------------
// Platform event channel: the 16 fixed-size engine event types 0x70-0x7F.
//
// The engine's own event types end at 0x2F; the rest of the byte range is
// carved up between the DLLs that inject into the game (CnCNet 0x30-0x3F,
// Phobos 0x40-0x45, Kratos 0x50-0x51, Ares 0x60-0x61).  This platform owns
// 0x70-0x7F: 16 event types with a fixed payload size each, registered at
// load time together with the responder that handles them on every machine.
//
// This unit is pure (no engine headers, no platform logger) so the registry,
// routing and the frame codec are unit-testable.  The engine side lives in
// src/Engine/Hooks.Events.cpp:
//   * it installs the engine sender with SetSender() at load time;
//   * it defines LogStartup();
//   * the four engine hooks (dispatch + three GetEventSize sites) feed
//     GetDataSize()/Dispatch() and take their sizes from the registry.
//
// See docs/EventChannel.md for the wire format, the number-range argument and
// the mixed-client warning.
// ---------------------------------------------------------------------------

namespace venture
{
inline constexpr std::uint8_t EventChannelFirstType = 0x70;
inline constexpr std::uint8_t EventChannelLastType = 0x7F;
inline constexpr std::uint32_t EventChannelSlotCount = 16;

// The engine's EventClass union is 104 bytes; a payload can never be larger.
inline constexpr std::uint32_t EventChannelMaxPayload = 104;

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

	// Registered name, or nullptr when the type is not ours.
	static const char* Name(Type type);

	static std::size_t Count();

	// Iteration in ascending type order (0x70 first).  Returns nullptr for an
	// index >= Count(); used for the startup log.
	static const Entry* EntryAt(std::size_t index);

	// --- dispatch ----------------------------------------------------------
	// Routes one received event to its responder.  Returns false (and calls
	// nothing) for a type that is not ours or for a payload size that does not
	// match the registered size.
	static bool Dispatch(Type type, const std::uint8_t* data, std::uint32_t size);

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
