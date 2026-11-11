#include "Event/EventChannel.h"

#include "Core/PlatformGate.h"

#include <cstring>

// ---------------------------------------------------------------------------
// Pure implementation of the event channel: registry, routing and frame codec.
// Nothing in this file may include an engine header or the platform logger -
// tests/test_event_channel.cpp links this unit on its own.
// ---------------------------------------------------------------------------

namespace
{
using Channel = venture::EventChannel;

struct Registry
{
	Channel::Entry entries[venture::EventChannelSlotCount] = {};
	bool used[venture::EventChannelSlotCount] = {};
	std::size_t count = 0;
	Channel::EngineSender sender = nullptr;
};

// Function-local static: safe from the static initializers of any translation
// unit, and never destroyed at unload (the game process owns the DLL).
Registry& State()
{
	static Registry registry;
	return registry;
}

std::size_t SlotIndex(const venture::EventChannel::Type type)
{
	return static_cast<std::size_t>(type - venture::EventChannelFirstType);
}

const Channel::Entry* FindEntry(const venture::EventChannel::Type type)
{
	if (!Channel::IsValidEventType(type))
		return nullptr;

	Registry& state = State();
	const std::size_t index = SlotIndex(type);
	return state.used[index] ? &state.entries[index] : nullptr;
}
}

namespace venture
{
EventChannel::RegisterResult EventChannel::Register(
	const Type type,
	const char* const name,
	const std::uint32_t payloadSize,
	const Responder responder)
{
	// A refused platform keeps no registration path to the engine (C-06).
	if (PlatformGate::IsRefused())
		return RegisterResult::PlatformRefused;

	if (!IsValidEventType(type))
		return RegisterResult::InvalidType;
	if (!name || !*name)
		return RegisterResult::InvalidName;
	if (payloadSize == 0 || payloadSize > EventChannelMaxPayload)
		return RegisterResult::InvalidPayloadSize;
	if (!responder)
		return RegisterResult::InvalidResponder;

	Registry& state = State();
	const std::size_t index = SlotIndex(type);
	if (state.used[index])
		return RegisterResult::AlreadyRegistered;

	state.entries[index] = Entry{type, name, payloadSize, responder};
	state.used[index] = true;
	++state.count;
	return RegisterResult::Ok;
}

bool EventChannel::IsValidEventType(const Type type)
{
	return type >= EventChannelFirstType && type <= EventChannelLastType;
}

bool EventChannel::IsOwnEventType(const Type type)
{
	return FindEntry(type) != nullptr;
}

std::uint32_t EventChannel::GetDataSize(const Type type)
{
	const Entry* const entry = FindEntry(type);
	return entry ? entry->payloadSize : 0u;
}

EventChannel::WireSize EventChannel::GetWireSize(const Type type)
{
	const Entry* const entry = FindEntry(type);
	if (entry)
		return WireSize{WireSizePolicy::Registered, entry->payloadSize};

	// Only the engine's own numbers may fall through to the engine's table; its
	// table has no entry above 0x2E, so any other number would be an
	// out-of-bounds read that feeds the decoder a garbage size.  Answer it with
	// the fixed one-byte size instead (memory-safe, may desynchronise).
	if (type <= EventChannelLastVanillaType)
		return WireSize{WireSizePolicy::Vanilla, 0u};

	return WireSize{WireSizePolicy::FailClosed, EventChannelFailClosedSize};
}

const char* EventChannel::Name(const Type type)
{
	const Entry* const entry = FindEntry(type);
	return entry ? entry->name : nullptr;
}

std::size_t EventChannel::Count()
{
	return State().count;
}

const EventChannel::Entry* EventChannel::EntryAt(const std::size_t index)
{
	const Registry& state = State();
	std::size_t seen = 0;
	for (std::size_t slot = 0; slot < EventChannelSlotCount; ++slot)
	{
		if (!state.used[slot])
			continue;
		if (seen == index)
			return &state.entries[slot];
		++seen;
	}
	return nullptr;
}

bool EventChannel::Dispatch(
	const Type type,
	const std::uint8_t* const data,
	const std::uint32_t availableSize)
{
	const Entry* const entry = FindEntry(type);
	if (!entry || !entry->responder)
		return false;

	// `availableSize` is the number of payload bytes the caller can vouch for,
	// not the length the registry expects: reject instead of letting the
	// responder read past the caller's buffer.  This is what makes the check
	// real - the old signature compared the registered size with itself.
	if (!data || entry->payloadSize > availableSize)
		return false;

	entry->responder(data, entry->payloadSize);
	return true;
}

std::uint32_t EventChannel::WriteFrame(
	std::uint8_t* const buffer,
	const std::uint32_t bufferSize,
	const Type type,
	const std::int8_t houseIndex,
	const std::uint32_t frameNumber,
	const std::uint8_t* const payload,
	const std::uint32_t payloadSize)
{
	const Entry* const entry = FindEntry(type);
	if (!entry || !buffer || bufferSize < FrameSize || !payload)
		return 0u;
	if (payloadSize != entry->payloadSize)
		return 0u;

	Frame frame{};
	frame.type = type;
	frame.isExecuted = 0;
	frame.houseIndex = houseIndex;
	frame.frameNumber = frameNumber;
	std::memcpy(frame.payload, payload, entry->payloadSize);

	std::memcpy(buffer, &frame, FrameSize);
	return FrameSize;
}

bool EventChannel::ReadFrame(
	const std::uint8_t* const frame,
	const std::uint32_t frameSize,
	FrameView& out)
{
	if (!frame || frameSize != FrameSize)
		return false;

	Frame decoded{};
	std::memcpy(&decoded, frame, FrameSize);

	const Entry* const entry = FindEntry(decoded.type);
	if (!entry)
		return false;

	out.type = decoded.type;
	out.houseIndex = decoded.houseIndex;
	out.frameNumber = decoded.frameNumber;
	out.payloadSize = entry->payloadSize;
	out.payload = frame + FramePayloadOffset;
	return true;
}

void EventChannel::SetSender(const EngineSender sender)
{
	State().sender = sender;
}

EventChannel::EngineSender EventChannel::Sender()
{
	return State().sender;
}

bool EventChannel::Send(
	const Type type,
	const std::int8_t houseIndex,
	const std::uint32_t frameNumber,
	const std::uint8_t* const payload,
	const std::uint32_t payloadSize)
{
	const Entry* const entry = FindEntry(type);
	if (!entry || !payload || payloadSize != entry->payloadSize)
		return false;

	const EngineSender sender = State().sender;
	if (!sender)
		return false;

	std::uint8_t frame[FrameSize] = {};
	const std::uint32_t frameSize = WriteFrame(
		frame, FrameSize, type, houseIndex, frameNumber, payload, payloadSize);
	if (frameSize == 0u)
		return false;

	return sender(frame, frameSize);
}
}
