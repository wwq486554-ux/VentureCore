// Unit test: platform event channel (registry, routing, frame codec, Send,
// engine size policy).
//
// The channel unit is engine-free by design (src/Event/EventChannel.h), so the
// test links EventChannel.cpp alone and replaces the engine sender with a
// capture function: everything a machine does with the 0x70-0x7F event numbers
// except the engine call itself is exercised here.
//
// The wire-size matrix is the unit-testable half of C-01: the three engine
// GetEventSize hooks answer the registered payload size for the channel types,
// let the engine's own 0x00-0x2E types fall through to the vanilla size table,
// and answer every unregistered number above 0x2E with one byte (fail-closed)
// instead of an out-of-bounds table read.  The dispatch tests cover C-02: the
// responder size is checked against the real available length.
//
// TEST_DEPS: src/Event/EventChannel.cpp

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "Event/EventChannel.h"

namespace
{
using Channel = venture::EventChannel;
using RegisterResult = Channel::RegisterResult;
using WireSizePolicy = Channel::WireSizePolicy;

int g_failures = 0;

void Check(const bool ok, const char* const what)
{
	if (ok)
	{
		std::printf("ok    %s\n", what);
	}
	else
	{
		++g_failures;
		std::printf("FAIL  %s\n", what);
	}
}

// --- responders ------------------------------------------------------------
int g_responderACalls = 0;
std::uint8_t g_responderALast[venture::EventChannelMaxPayload] = {};
std::uint32_t g_responderALastSize = 0;

void ResponderA(const std::uint8_t* const data, const std::uint32_t size)
{
	++g_responderACalls;
	g_responderALastSize = size;
	std::memcpy(g_responderALast, data, size);
}

int g_responderBCalls = 0;
std::uint32_t g_responderBLastSize = 0;

void ResponderB(const std::uint8_t* const data, const std::uint32_t size)
{
	(void)data;
	g_responderBLastSize = size;
	++g_responderBCalls;
}

int g_responderCCalls = 0;

void ResponderC(const std::uint8_t* const data, const std::uint32_t size)
{
	(void)data;
	(void)size;
	++g_responderCCalls;
}

// --- fake engine sender ----------------------------------------------------
int g_sendCalls = 0;
std::uint8_t g_sentFrame[Channel::FrameSize] = {};
std::uint32_t g_sentFrameSize = 0;

bool CaptureSender(const std::uint8_t* const frame, const std::uint32_t frameSize)
{
	++g_sendCalls;
	g_sentFrameSize = frameSize;
	std::memcpy(g_sentFrame, frame, frameSize);
	return true;
}

bool RefusingSender(const std::uint8_t* const frame, const std::uint32_t frameSize)
{
	(void)frame;
	(void)frameSize;
	return false;
}

std::uint32_t ReadU32At(const std::uint8_t* const data)
{
	return static_cast<std::uint32_t>(data[0])
		| (static_cast<std::uint32_t>(data[1]) << 8)
		| (static_cast<std::uint32_t>(data[2]) << 16)
		| (static_cast<std::uint32_t>(data[3]) << 24);
}
}

int __cdecl main(void)
{
	std::uint8_t payload4[4] = {0xDE, 0xAD, 0xBE, 0xEF};
	std::uint8_t payload2[2] = {0x11, 0x22};
	std::uint8_t payload1[1] = {0x5A};
	std::uint8_t payload8[8] = {1, 2, 3, 4, 5, 6, 7, 8};
	std::uint8_t payload104[venture::EventChannelMaxPayload] = {};
	for (std::uint32_t i = 0; i < venture::EventChannelMaxPayload; ++i)
		payload104[i] = static_cast<std::uint8_t>(i);
	(void)payload1;

	// --- range --------------------------------------------------------------
	std::printf("-- range --\n");
	Check(!Channel::IsValidEventType(0x00), "0x00 is not a channel type");
	Check(!Channel::IsValidEventType(0x2E), "0x2E (last vanilla type) is not a channel type");
	Check(!Channel::IsValidEventType(0x2F), "0x2F (first free number) is not a channel type");
	Check(!Channel::IsValidEventType(0x40), "0x40 (Phobos range) is not a channel type");
	Check(!Channel::IsValidEventType(0x6F), "0x6F is not a channel type");
	Check(Channel::IsValidEventType(0x70), "0x70 is the first channel type");
	Check(Channel::IsValidEventType(0x7F), "0x7F is the last channel type");
	Check(!Channel::IsValidEventType(0x80), "0x80 is not a channel type");

	Check(Channel::Count() == 0, "registry starts empty");
	Check(Channel::GetDataSize(0x70) == 0, "unregistered channel type reports no data size");
	Check(Channel::GetDataSize(0x40) == 0, "foreign type reports no data size");
	Check(!Channel::IsOwnEventType(0x70), "unregistered channel type is not ours");
	Check(Channel::Name(0x70) == nullptr, "unregistered channel type has no name");
	Check(Channel::EntryAt(0) == nullptr, "no entry at index 0 of the empty registry");

	// --- wire size policy (fail-closed) -------------------------------------
	// The engine's payload-size table has 47 entries (0x00-0x2E); 0x2F is the
	// first free number.  Only the engine types may fall through to that table -
	// every other number has to be answered here, otherwise the engine reads
	// past the table and the multiplayer decoder copies a garbage size into its
	// 104-byte event buffer (C-01).
	std::printf("-- wire size policy --\n");
	{
		const Channel::WireSize wire00 = Channel::GetWireSize(0x00);
		Check(wire00.policy == WireSizePolicy::Vanilla && wire00.size == 0,
			"0x00: vanilla policy, engine answers");

		const Channel::WireSize wire2E = Channel::GetWireSize(0x2E);
		Check(wire2E.policy == WireSizePolicy::Vanilla && wire2E.size == 0,
			"0x2E (last engine type): vanilla policy, engine answers");

		const Channel::WireSize wire2F = Channel::GetWireSize(0x2F);
		Check(wire2F.policy == WireSizePolicy::FailClosed
				&& wire2F.size == venture::EventChannelFailClosedSize,
			"0x2F (first free number): one-byte fail-closed");

		const Channel::WireSize wire30 = Channel::GetWireSize(0x30);
		Check(wire30.policy == WireSizePolicy::FailClosed && wire30.size == 1,
			"0x30 (unregistered foreign range): one-byte fail-closed");

		const Channel::WireSize wire40 = Channel::GetWireSize(0x40);
		Check(wire40.policy == WireSizePolicy::FailClosed && wire40.size == 1,
			"0x40 (unregistered Phobos range): one-byte fail-closed");

		const Channel::WireSize wire6F = Channel::GetWireSize(0x6F);
		Check(wire6F.policy == WireSizePolicy::FailClosed && wire6F.size == 1,
			"0x6F (unregistered): one-byte fail-closed");

		const Channel::WireSize wire70 = Channel::GetWireSize(0x70);
		Check(wire70.policy == WireSizePolicy::FailClosed && wire70.size == 1,
			"0x70 while unregistered: one-byte fail-closed");

		const Channel::WireSize wire72 = Channel::GetWireSize(0x72);
		Check(wire72.policy == WireSizePolicy::FailClosed && wire72.size == 1,
			"0x72 while unregistered: one-byte fail-closed");

		const Channel::WireSize wire7F = Channel::GetWireSize(0x7F);
		Check(wire7F.policy == WireSizePolicy::FailClosed && wire7F.size == 1,
			"0x7F while unregistered: one-byte fail-closed");

		const Channel::WireSize wireFF = Channel::GetWireSize(0xFF);
		Check(wireFF.policy == WireSizePolicy::FailClosed && wireFF.size == 1,
			"0xFF (unregistered): one-byte fail-closed");
	}

	// --- registration validation -------------------------------------------
	std::printf("-- registration validation --\n");
	Check(Channel::Register(0x40, "bad_low", 4, &ResponderA) == RegisterResult::InvalidType,
		"0x40 rejected as outside the channel range");
	Check(Channel::Register(0x80, "bad_high", 4, &ResponderA) == RegisterResult::InvalidType,
		"0x80 rejected as outside the channel range");
	Check(Channel::Register(0x70, nullptr, 4, &ResponderA) == RegisterResult::InvalidName,
		"null name rejected");
	Check(Channel::Register(0x70, "", 4, &ResponderA) == RegisterResult::InvalidName,
		"empty name rejected");
	Check(Channel::Register(0x70, "test", 0, &ResponderA) == RegisterResult::InvalidPayloadSize,
		"payload size 0 rejected");
	Check(Channel::Register(0x70, "test", venture::EventChannelMaxPayload + 1, &ResponderA)
			== RegisterResult::InvalidPayloadSize,
		"payload larger than 104 bytes rejected");
	Check(Channel::Register(0x70, "test", 4, nullptr) == RegisterResult::InvalidResponder,
		"null responder rejected");

	Check(Channel::Register(0x70, "Venture_Test_Primary", 4, &ResponderA) == RegisterResult::Ok,
		"0x70 registers");
	Check(Channel::Count() == 1, "one handler registered");
	Check(Channel::Register(0x70, "Venture_Test_Primary", 4, &ResponderA)
			== RegisterResult::AlreadyRegistered,
		"re-registering the same type rejected");
	Check(Channel::Count() == 1, "duplicate does not change the count");

	Check(Channel::IsOwnEventType(0x70), "0x70 is ours after registration");
	Check(!Channel::IsOwnEventType(0x71), "0x71 is not ours yet");
	Check(Channel::GetDataSize(0x70) == 4, "0x70 payload size is 4");
	Check(Channel::GetDataSize(0x71) == 0, "0x71 has no payload size");
	Check(Channel::Name(0x70) != nullptr && std::strcmp(Channel::Name(0x70), "Venture_Test_Primary") == 0,
		"registered name is readable");
	Check(Channel::EntryAt(0) != nullptr && Channel::EntryAt(0)->type == 0x70,
		"entry 0 is 0x70");
	Check(Channel::EntryAt(0) != nullptr && Channel::EntryAt(0)->payloadSize == 4,
		"entry 0 carries the payload size");
	Check(Channel::EntryAt(1) == nullptr, "no second entry yet");

	{
		const Channel::WireSize wire70 = Channel::GetWireSize(0x70);
		Check(wire70.policy == WireSizePolicy::Registered && wire70.size == 4,
			"registered 0x70: registered policy with its payload size");
		const Channel::WireSize wire71 = Channel::GetWireSize(0x71);
		Check(wire71.policy == WireSizePolicy::FailClosed && wire71.size == 1,
			"unregistered 0x71 stays fail-closed next to a registered type");
	}

	Check(Channel::Register(0x71, "Venture_Test_Secondary", 2, &ResponderB) == RegisterResult::Ok,
		"0x71 registers");
	Check(Channel::Register(0x7F, "Venture_Test_Last", 8, &ResponderC) == RegisterResult::Ok,
		"0x7F registers");
	Check(Channel::Count() == 3, "three handlers registered");

	{
		const Channel::WireSize wire71 = Channel::GetWireSize(0x71);
		Check(wire71.policy == WireSizePolicy::Registered && wire71.size == 2,
			"registered 0x71: registered policy with its payload size");
		const Channel::WireSize wire7F = Channel::GetWireSize(0x7F);
		Check(wire7F.policy == WireSizePolicy::Registered && wire7F.size == 8,
			"registered 0x7F: registered policy with its payload size");
	}

	// --- iteration ----------------------------------------------------------
	std::printf("-- iteration --\n");
	Check(Channel::EntryAt(0) != nullptr && Channel::EntryAt(0)->type == 0x70, "iteration: 0x70 first");
	Check(Channel::EntryAt(1) != nullptr && Channel::EntryAt(1)->type == 0x71, "iteration: 0x71 second");
	Check(Channel::EntryAt(2) != nullptr && Channel::EntryAt(2)->type == 0x7F, "iteration: 0x7F third");
	Check(Channel::EntryAt(3) == nullptr, "iteration ends after the last entry");

	// --- dispatch -----------------------------------------------------------
	std::printf("-- dispatch --\n");
	g_responderACalls = 0;
	g_responderBCalls = 0;
	g_responderCCalls = 0;

	Check(Channel::Dispatch(0x70, payload4, 4), "dispatch routes 0x70");
	Check(g_responderACalls == 1, "0x70 responder called once");
	Check(g_responderALastSize == 4 && std::memcmp(g_responderALast, payload4, 4) == 0,
		"0x70 responder received the payload");
	Check(g_responderBCalls == 0, "other responders not called");

	// C-02: `availableSize` is the real length the caller can vouch for.  More
	// available bytes than the registered size is fine - the responder is still
	// told the registered size; fewer is rejected before the responder runs.
	Check(Channel::Dispatch(0x70, payload4, 104), "dispatch accepts a larger available length");
	Check(g_responderACalls == 2, "responder called for the larger available length");
	Check(g_responderALastSize == 4, "responder receives the registered size, not the available length");

	Check(!Channel::Dispatch(0x70, payload4, 3), "available length below the registered size rejected");
	Check(!Channel::Dispatch(0x70, payload4, 0), "zero available length rejected");
	Check(g_responderACalls == 2, "rejected available lengths did not call the responder");
	Check(!Channel::Dispatch(0x70, nullptr, 4), "null payload rejected");
	Check(!Channel::Dispatch(0x71, payload4, 1), "0x71 (registered size 2) rejects a 1-byte available length");
	Check(g_responderBCalls == 0, "short available length did not call the 0x71 responder");
	Check(!Channel::Dispatch(0x72, payload4, 4), "unregistered type in range not dispatched");
	Check(!Channel::Dispatch(0x40, payload4, 4), "foreign type not dispatched");
	Check(g_responderACalls == 2 && g_responderBCalls == 0 && g_responderCCalls == 0,
		"no responder called by the rejected dispatches");

	Check(Channel::Dispatch(0x71, payload4, 4), "dispatch routes 0x71 with a larger available length");
	Check(g_responderBCalls == 1, "0x71 responder called");
	Check(g_responderBLastSize == 2, "0x71 responder receives its registered 2-byte size");
	Check(Channel::Dispatch(0x71, payload2, 2), "dispatch routes 0x71 with exactly its size");
	Check(g_responderBCalls == 2, "0x71 responder called for the exact size");
	Check(Channel::Dispatch(0x7F, payload8, 8), "dispatch routes 0x7F");
	Check(g_responderCCalls == 1, "0x7F responder called");

	// --- frame codec --------------------------------------------------------
	std::printf("-- frame codec --\n");
	std::uint8_t frame[Channel::FrameSize] = {};
	Check(Channel::WriteFrame(frame, Channel::FrameSize, 0x70, 3, 0x11223344u, payload4, 4)
			== Channel::FrameSize,
		"WriteFrame writes one full frame");
	Check(Channel::FrameSize == 111, "frame size is 111 bytes");
	Check(frame[0] == 0x70, "frame byte 0 is the event type");
	Check(frame[1] == 0, "wire frames are not marked executed");
	Check(frame[2] == 3, "frame byte 2 is the house index");
	Check(ReadU32At(&frame[3]) == 0x11223344u, "frame bytes 3-6 are the frame number (LE)");
	Check(std::memcmp(&frame[7], payload4, 4) == 0, "frame bytes 7.. are the payload");
	Check(frame[11] == 0, "payload area is zero-padded");

	Channel::FrameView view{};
	Check(Channel::ReadFrame(frame, Channel::FrameSize, view), "ReadFrame decodes the frame");
	Check(view.type == 0x70, "decoded type");
	Check(view.houseIndex == 3, "decoded house index");
	Check(view.frameNumber == 0x11223344u, "decoded frame number");
	Check(view.payloadSize == 4, "decoded payload size comes from the registry");
	Check(view.payload != nullptr && std::memcmp(view.payload, payload4, 4) == 0,
		"decoded payload points into the buffer");

	Check(!Channel::ReadFrame(frame, Channel::FrameSize - 1, view), "truncated frame rejected");
	Check(!Channel::ReadFrame(frame, Channel::FrameSize + 1, view), "oversized frame rejected");
	Check(!Channel::ReadFrame(nullptr, Channel::FrameSize, view), "null frame rejected");

	std::uint8_t foreignFrame[Channel::FrameSize] = {};
	Check(Channel::WriteFrame(foreignFrame, Channel::FrameSize, 0x70, 0, 0, payload4, 4)
			== Channel::FrameSize,
		"helper frame for the foreign-type test");
	foreignFrame[0] = 0x72;
	Check(!Channel::ReadFrame(foreignFrame, Channel::FrameSize, view),
		"frame of an unregistered type rejected");

	Check(Channel::WriteFrame(frame, Channel::FrameSize - 1, 0x70, 0, 0, payload4, 4) == 0,
		"too-small buffer rejected");
	Check(Channel::WriteFrame(frame, Channel::FrameSize, 0x70, 0, 0, payload4, 3) == 0,
		"payload size mismatch rejected");
	Check(Channel::WriteFrame(frame, Channel::FrameSize, 0x70, 0, 0, nullptr, 4) == 0,
		"null payload rejected");
	Check(Channel::WriteFrame(frame, Channel::FrameSize, 0x72, 0, 0, payload4, 4) == 0,
		"unregistered type rejected");

	Check(Channel::WriteFrame(frame, Channel::FrameSize, 0x71, -1, 0, payload2, 2)
			== Channel::FrameSize,
		"negative house index encodes");
	Check(Channel::ReadFrame(frame, Channel::FrameSize, view), "frame with house -1 decodes");
	Check(view.type == 0x71 && view.houseIndex == -1 && view.frameNumber == 0,
		"house -1 round-trips");

	// --- sending ------------------------------------------------------------
	std::printf("-- send --\n");
	Check(Channel::Sender() == nullptr, "no engine sender installed initially");
	Check(!Channel::Send(0x70, 0, 10, payload4, 4), "Send without a sender fails");

	Channel::SetSender(&CaptureSender);
	Check(Channel::Sender() == &CaptureSender, "sender is installed");
	g_sendCalls = 0;
	Check(Channel::Send(0x70, 2, 77, payload4, 4), "Send accepts a registered event");
	Check(g_sendCalls == 1, "sender called once");
	Check(g_sentFrameSize == Channel::FrameSize, "sender receives one full frame");
	Check(g_sentFrame[0] == 0x70 && g_sentFrame[1] == 0 && g_sentFrame[2] == 2,
		"sent frame carries type and house index");
	Check(ReadU32At(&g_sentFrame[3]) == 77u, "sent frame carries the frame number");
	Check(std::memcmp(&g_sentFrame[7], payload4, 4) == 0, "sent frame carries the payload");

	Check(!Channel::Send(0x70, 2, 77, payload4, 3), "Send rejects a wrong payload size");
	Check(!Channel::Send(0x72, 2, 77, payload4, 4), "Send rejects an unregistered type");
	Check(!Channel::Send(0x70, 2, 77, nullptr, 4), "Send rejects a null payload");
	Check(g_sendCalls == 1, "rejected sends do not reach the sender");

	Channel::SetSender(&RefusingSender);
	Check(!Channel::Send(0x70, 2, 77, payload4, 4), "a refusing sender fails the Send");
	Channel::SetSender(nullptr);
	Check(Channel::Sender() == nullptr, "sender can be removed");

	// --- every slot ---------------------------------------------------------
	std::printf("-- all 16 slots --\n");
	for (std::uint8_t type = 0x72; type < 0x7F; ++type)
	{
		const std::uint32_t size = (type == 0x7E) ? venture::EventChannelMaxPayload : 1u;
		Check(Channel::Register(type, "Venture_Test_Slot", size, &ResponderA) == RegisterResult::Ok,
			"remaining slot registers");
	}

	Check(Channel::Count() == venture::EventChannelSlotCount, "all 16 channel slots are registered");
	Check(Channel::GetDataSize(0x7E) == venture::EventChannelMaxPayload,
		"maximum 104-byte payload accepted");
	Check(Channel::GetDataSize(0x72) == 1, "minimum 1-byte payload accepted");
	Check(Channel::EntryAt(0) != nullptr && Channel::EntryAt(0)->type == 0x70, "full iteration: first slot");
	Check(Channel::EntryAt(15) != nullptr && Channel::EntryAt(15)->type == 0x7F, "full iteration: last slot");
	Check(Channel::EntryAt(16) == nullptr, "full iteration: end");
	Check(Channel::Register(0x7D, "Venture_Overflow", 1, &ResponderA)
			== RegisterResult::AlreadyRegistered,
		"no 17th handler can be registered");

	g_responderACalls = 0;
	Check(Channel::Dispatch(0x7E, payload104, venture::EventChannelMaxPayload),
		"dispatch of the maximum 104-byte payload");
	Check(g_responderACalls == 1 && g_responderALastSize == venture::EventChannelMaxPayload,
		"maximum payload reaches the responder unmodified");

	// --- whole byte range ---------------------------------------------------
	// Exhaustive C-01 policy check: all 256 values must land in exactly one of
	// the three cases - a registered type answers its payload size, the engine's
	// own 47 types (0x00-0x2E) fall through to the vanilla table, and every other
	// value is fail-closed.  No value may reach the engine's out-of-bounds read.
	std::printf("-- whole byte range --\n");
	{
		int policyErrors = 0;
		int registeredSeen = 0;
		int vanillaSeen = 0;
		int failClosedSeen = 0;

		for (unsigned value = 0; value <= 0xFF; ++value)
		{
			const std::uint8_t type = static_cast<std::uint8_t>(value);
			const Channel::WireSize wire = Channel::GetWireSize(type);

			if (Channel::IsOwnEventType(type))
			{
				++registeredSeen;
				if (wire.policy != WireSizePolicy::Registered
					|| wire.size != Channel::GetDataSize(type))
				{
					++policyErrors;
				}
			}
			else if (value <= 0x2E)
			{
				++vanillaSeen;
				if (wire.policy != WireSizePolicy::Vanilla || wire.size != 0)
					++policyErrors;
			}
			else
			{
				++failClosedSeen;
				if (wire.policy != WireSizePolicy::FailClosed
					|| wire.size != venture::EventChannelFailClosedSize)
				{
					++policyErrors;
				}
			}
		}

		Check(policyErrors == 0, "every byte value maps to the expected wire-size policy");
		Check(registeredSeen == static_cast<int>(venture::EventChannelSlotCount),
			"exhaustive check saw all 16 registered channel types");
		Check(vanillaSeen == 0x2F, "exhaustive check saw exactly 47 vanilla types (0x00-0x2E)");
		Check(failClosedSeen == 256 - 0x2F - static_cast<int>(venture::EventChannelSlotCount),
			"exhaustive check saw fail-closed for every other byte value");
	}

	std::printf("\n");
	if (g_failures == 0)
	{
		std::printf("test_event_channel: all checks passed\n");
		return 0;
	}

	std::printf("test_event_channel: %d check(s) FAILED\n", g_failures);
	return 1;
}
