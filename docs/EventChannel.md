# Event Channel

The platform's own multiplayer event types and payloads: 16 fixed-size event
numbers in `0x70-0x7F`, a registration table with one responder per number, and
a frame codec. The engine-facing half binds the channel into the three engine
`GetEventSize` sites and the event dispatch function, so an event raised by this
platform is sized and executed correctly on every machine that runs the same
platform build.

Related documents: `docs/HookCatalogue.md` (the four engine hooks this channel
installs), `docs/EngineBindings.md` (the binding layers), `docs/HostMatrix.md`
(which hosts may be installed next to the platform).

## Why `0x70-0x7F`

The engine event type is one byte and the vanilla game uses `0x00-0x2F` only
(`EventType::LAST_EVENT = 47`). Everything above that is claimed by the DLLs
that inject into the game, so a new platform has to pick an unused block:

| Range | Owner | Notes |
| --- | --- | --- |
| `0x00-0x2F` | vanilla engine | `EventType` enum; the engine has a 0x2F-entry payload-size table the `GetEventSize` sites read |
| `0x30-0x3F` | CnCNet | reserved for the CnCNet transport |
| `0x40-0x45` | Phobos lineage | e.g. `ApproachObject = 0x40` ... `SpecialActionReturn = 0x45` |
| `0x50-0x51` | Kratos | |
| `0x60-0x61` | Ares | |
| **`0x70-0x7F`** | **this platform** | 16 types, fixed payload each |

`0x70-0x7F` is the highest contiguous free block below the end of the byte
range. It is also above Ares' numbers, so the two platforms can be installed
together without an event-number collision (see the mixed-client warning below
for what still has to match).

## The frame

One event is one 111-byte record, byte-for-byte identical to the engine's
`EventClass` (`sizeof(EventClass) == 111` in the pinned YRpp):

| Offset | Size | Field | Meaning |
| --- | --- | --- | --- |
| 0 | 1 | `type` | event number, `0x70-0x7F` |
| 1 | 1 | `isExecuted` | `0` on the wire; the engine sets it when it runs the event |
| 2 | 1 | `houseIndex` | player house that raised the event, `-1` = no house |
| 3 | 4 | `frameNumber` | engine frame the event should execute on (little endian) |
| 7 | 104 | `payload` | the registered payload, zero-padded to 104 bytes |

The frame layout is enforced at compile time in
`src/Engine/Hooks.Events.cpp` with `static_assert`s on `sizeof` and `offsetof`
against the pinned YRpp `EventClass`; a YRpp update that moves a field fails the
build instead of corrupting the engine event queue.

Limits: at most 16 event types; payload size 1-104 bytes (`0` is reserved for
"not ours"); the payload size is fixed per type at registration and both ends
must agree on it.

## Registration API

The channel logic (`src/Event/EventChannel.h`) is engine-free, so registering an
event and the responder that handles it takes no engine header:

```cpp
#include "Event/EventChannel.h"

struct MyPayload
{
	std::uint32_t value;
};

void RespondMyEvent(const std::uint8_t* data, std::uint32_t size)
{
	MyPayload payload{};
	if (size != sizeof(payload))
		return;
	std::memcpy(&payload, data, sizeof(payload));
	// Runs on every machine that receives the event, including the one that
	// raised it. Touch only per-machine state that every peer can derive the
	// same way, and never raise the same event again from inside a responder.
}

constexpr std::uint8_t MyEventType = 0x70;

// Load time (static initializer of the owning module).
const venture::EventChannel::RegisterResult registered = venture::EventChannel::Register(
	MyEventType,
	"My_Event",              // name for the startup log
	sizeof(MyPayload),       // 1..104
	&RespondMyEvent);
```

`Register` returns one of `Ok`, `InvalidType` (outside `0x70-0x7F`),
`InvalidName`, `InvalidPayloadSize` (0 or > 104), `InvalidResponder`,
`AlreadyRegistered`. Registration happens at load time, before the engine can
raise or dispatch an event, so dispatch is a plain table lookup.

Raising an event from game code:

```cpp
MyPayload payload{ 42 };
venture::EventChannel::Send(
	MyEventType,
	static_cast<std::int8_t>(houseIndex), // -1 when there is no house
	frameNumber,                          // e.g. Unsorted::CurrentFrame
	reinterpret_cast<const std::uint8_t*>(&payload),
	sizeof(payload));
```

`Send` validates the type and payload size, serializes the 111-byte frame and
hands it to the engine sender installed at load time by
`src/Engine/Hooks.Events.cpp`. That sender queues the frame as a regular engine
event with `EventClass::OutList.Add(...)`, which is how the reference ecosystem
raises the custom events it defines: the event then travels the normal event
transport and is executed on every machine, including the sender's own.

Helpers for a transport layer: `WriteFrame`/`ReadFrame` encode and decode one
frame in a caller buffer; `GetDataSize(type)` returns the registered payload
size (`0` = not ours); `IsValidEventType(type)` checks the range and
`IsOwnEventType(type)` additionally requires a registered handler; `Name(type)`,
`Count()` and `EntryAt(index)` iterate the table for logs.

## Engine binding

Four hooks, all at the sites the wider ecosystem (Phobos, Kratos, Mix) also
uses; exact windows and objdump evidence are in `docs/HookCatalogue.md`.

| Address | Size | Handler | Semantics |
| --- | --- | --- | --- |
| `0x4C6CC8` | 5 | `Venture_Event_Dispatch` | `EventClass*` in ESI. If the channel owns the type, dispatch `pEvent->DataBuffer` to the registered responder with the registered size. Always returns 0, so the engine's own event switch (and the other platforms chained at this address) still runs |
| `0x64B6FE` | 6 | `Venture_Event_GetSize1` | type in EDI: for our types set EDX/EBP = payload size and resume at `0x64B71D`; otherwise return 0 and let the vanilla size-table path run |
| `0x64BE7D` | 6 | `Venture_Event_GetSize2` | type in EDI: same as above plus the size copy into the caller frame at `[esp+0x20]` (`STACK_OFFSET(0xAC, -0x8C)`), resume at `0x64BE97` |
| `0x64C30E` | 6 | `Venture_Event_GetSize3` | type in ESI: same as site 1, resume at `0x64C321` |

The three `GetEventSize` hooks exist because the engine sizes an event by
indexing its own 0x2F-entry payload-size table; an event number above that
table's range would read unrelated memory. Our numbers return their registered
size instead, and every other number falls through unchanged to the engine.

The engine sender is installed at DLL load time with
`EventChannel::SetSender(...)`; the channel unit itself never includes an engine
header. The startup log is hooked in from `src/Core/Platform.cpp`, right after
the host-detection line, and prints the channel range, the occupied numbers and
the mixed-client warning.

## Startup log

```
event channel: 0x70-0x7F, 1 registered handler(s)
event channel:   0x70 Venture_Event_Test        8 byte(s)
event channel: mixed-client risk - a multiplayer peer that does not run this platform
does not know event types 0x70-0x7F; ...
```

## Mixed-client warning

The event number and payload size are **not self-describing on the wire**: the
engine already knows how many bytes an event has before it reads the payload,
and it is the `GetEventSize` hook of the receiving platform that answers that
question. Consequences:

* a peer that does **not** run this platform (or runs a build that registered a
  different event set) answers the engine with the vanilla size: the payload is
  misparsed and the game desynchronises;
* a peer that runs this platform but registered a different payload size for
  the same number has the same problem.

There is **no automatic peer detection** in this increment: the channel cannot
know which events a remote peer understands. Avoiding a mixed client is the
user's responsibility - every player in a multiplayer game has to run a platform
build that registers the same event types and payload sizes. This is documented
here and listed as a T2/T3 (multiplayer) test for the user; the single-player
path (raise an event, the local dispatch hook runs the responder) can be tested
by hand with the example command described in `docs/Commands.md`.

## What is not done

* No automatic peer detection or negotiation - see the mixed-client warning.
* No payload versioning: the meaning of a payload is a contract between
  platform builds of the same version, not encoded in the frame.
* No event persistence: events are transient, they are not written to a
  savegame.
* No queueing/retry layer: an event is queued through the engine's own
  `OutList`, exactly like a vanilla event.
* No integration with the CnCNet `0x30-0x3F` transport; the channel only uses
  the engine's normal event queue.
