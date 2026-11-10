# Event Channel

The platform's own multiplayer event types and payloads: 16 fixed-size event
numbers in `0x70-0x7F`, a registration table with one responder per number, and
a frame codec. The engine-facing half binds the channel into the three engine
`GetEventSize` sites and the event dispatch function, so an event raised by this
platform is sized and executed correctly on every machine that runs the same
platform build.

The size binding is fail-closed: only the engine's own event numbers are allowed
to reach the engine's payload-size table. Every unregistered number above that
table's range is consumed as one byte, so a mismatched or hostile peer cannot
make the engine read past the table and copy an attacker-controlled length into
its event buffer (C-01).

Related documents: `docs/HookCatalogue.md` (the four engine hooks this channel
installs), `docs/EngineBindings.md` (the binding layers), `docs/HostMatrix.md`
(which hosts may be installed next to the platform).

## Why `0x70-0x7F`

The engine event type is one byte. The vanilla game uses `0x00-0x2E` only
(`EventType::LAST_EVENT = 47`), so its payload-size table has exactly 47 entries
and `0x2F` is the first free number. Everything above that is claimed by the
DLLs that inject into the game, so a new platform has to pick an unused block:

| Range | Owner | Notes |
| --- | --- | --- |
| `0x00-0x2E` | vanilla engine | `EventType` enum, 47 values; each one indexes a byte of the engine's payload-size table at `0x8208EC` |
| `0x2F` | - | first free number; it is *not* a table entry (the table ends at `0x2E`) |
| `0x30-0x3F` | CnCNet | reserved for the CnCNet transport |
| `0x40-0x45` | Phobos lineage | e.g. `ApproachObject = 0x40` ... `SpecialActionReturn = 0x45` |
| `0x50-0x51` | Kratos | |
| `0x60-0x61` | Ares | |
| **`0x70-0x7F`** | **this platform** | 16 types, fixed payload each |

`0x70-0x7F` is the highest contiguous free block below the end of the byte
range. It is also above Ares' numbers, so the two platforms can be installed
together without an event-number collision (see the mixed-client warning below
for what still has to match).

## Fail-closed size policy

The engine sizes an event *before* it reads the payload: the three
`GetEventSize` sites index the engine's 47-entry table at `0x8208EC + type`.
For a number above `0x2E` that read is out of bounds - it picks up the bytes of
`EventClass::EventNames[]` that follow the table, and the multiplayer decoder
then copies that many bytes into the 104-byte `DataBuffer` of its stack event
record. For the `0x70-0x7F` bytes this garbage size is up to 130; for unclaimed
foreign ranges it can be 232, which overwrites the return address.

The channel therefore never lets a number it has not answered reach that table.
`EventChannel::GetWireSize(type)` is the engine-free policy the three size hooks
share:

| Case | Numbers | Answer | Effect |
| --- | --- | --- | --- |
| `Vanilla` | `0x00-0x2E` | `0` | the hook returns 0 and the engine's own table path runs (always in bounds) |
| `Registered` | `0x70-0x7F` with a registration | the registered payload size, 1-104 | the hook writes the size into the registers the vanilla block uses and resumes at the fixed return address |
| `FailClosed` | any other number above `0x2E` - unregistered `0x70-0x7F`, `0x2F`, every foreign range | `1` | the hook writes size 1 and resumes; the decoder copies one byte and cannot overrun |

Consuming one byte guarantees memory safety, not stream alignment: when a peer
sent a longer payload, the rest of it is read as the next event header, so the
event stream is misparsed and the game desynchronises. That is what the
mixed-client warning is about; there is no length on the wire that would let the
receiver skip a payload whose type it does not know.

### Order contract

Engine hooks run in DLL load order, and the first handler that returns a
non-zero resume address short-circuits the handlers chained behind it at the
same site. This platform's DLL name (`VentureCore.dll`) sorts after the known
platforms (`Ares.dll`, `Kratos.dll`, `Phobos.dll`), so its hooks sit at the end
of the chain: a foreign number only reaches them when no earlier platform
claimed it - and answering it with the fail-closed size is then the safe
behaviour, because the alternative is the engine's out-of-bounds table read.

**Any platform whose handler runs after VentureCore's must handle unregistered
numbers itself.** VentureCore answers every number above `0x2E` that it does not
own, and that non-zero answer short-circuits the rest of the chain, so a
platform loaded after VentureCore cannot use these three sites to define new
event numbers. Such a platform has to answer unregistered numbers itself - for
example by loading before VentureCore, by claiming its numbers through an
earlier platform, or by negotiating them with the peer - and it must not rely on
the engine's size table for any number above `0x2E`. A platform loaded before
VentureCore keeps this property automatically: its own size hook runs first and
claims its numbers before the fail-closed answer is reached.

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

The record is a process-internal event record, not a wire record: the engine's
multiplayer encoder serializes `type` plus the payload bytes, and the size of
that payload comes from the local size policy above (the wire carries no
length). `frameNumber`/`houseIndex`/`isExecuted` are engine bookkeeping and are
not sent per event.

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

Helpers: `WriteFrame` serializes one event into a caller buffer and is the codec
`Send` uses to build the engine event record; `ReadFrame` validates and decodes
one record from a caller buffer and is currently only exercised by
`tests/test_event_channel.cpp` (there is no production consumer - the engine owns
the received record).  `GetDataSize(type)` returns the registered payload size
(`0` = not ours); `GetWireSize(type)` is the engine-size policy above (`policy`
plus `size`, never fails); `IsValidEventType(type)` checks the range and
`IsOwnEventType(type)` additionally requires a registered handler;
`Name(type)`, `Count()` and `EntryAt(index)` iterate the table for logs.

## Dispatch and payload validation

`Dispatch(type, data, availableSize)` routes one received event to its
responder. `availableSize` is the number of payload bytes the caller can
actually vouch for: the engine dispatch hook (`0x4C6CC8`) passes the payload
capacity of its fixed event record (104 bytes). Dispatch calls the responder
with the *registered* payload size and rejects the event (returns false, calls
nothing) when the type is not ours, `data` is null, or the registered size is
larger than `availableSize`; the engine adapter logs that last case as a
warning. This check is against the real buffer length - it replaced an older one
that compared the registered size with itself (C-02).

Dispatch validates the length, not the contents: the wire format is not
self-describing, so every responder has to check its own magic/size/version
contract. The example responder in `src/Engine/Hooks.Commands.cpp` does exactly
that (it checks a `'VENT'` magic and the payload size).

## Engine binding

Four hooks, all at the sites the wider ecosystem (Phobos, Kratos, Mix) also
uses; exact windows and objdump evidence are in `docs/HookCatalogue.md`.

| Address | Size | Handler | Semantics |
| --- | --- | --- | --- |
| `0x4C6CC8` | 5 | `Venture_Event_Dispatch` | `EventClass*` in ESI. Calls `Dispatch(type, pEvent->DataBuffer, 104)`; always returns 0, so the engine's own event switch (and the other platforms chained at this address) still runs |
| `0x64B6FE` | 6 | `Venture_Event_GetSize1` | type in EDI. Registered: EDX/EBP = payload size, resume at `0x64B71D`. Unregistered above `0x2E`: EDX/EBP = 1 (fail-closed), same resume. Vanilla `0x00-0x2E`: return 0 and let the engine's table path run |
| `0x64BE7D` | 6 | `Venture_Event_GetSize2` | type in EDI: same as above plus the size copy into the caller frame at `[esp+0x20]` (`STACK_OFFSET(0xAC, -0x8C)`), resume at `0x64BE97` |
| `0x64C30E` | 6 | `Venture_Event_GetSize3` | type in ESI: same as site 1, resume at `0x64C321` |

The three `GetEventSize` hooks exist because the engine sizes an event by
indexing its own 47-entry payload-size table (`0x00-0x2E`); an event number
above that table's range would read unrelated memory. The hooks answer the
registered size for the channel's own types, answer one byte fail-closed for
every other unregistered number above `0x2E`, and only let the engine's own
range fall through - see the fail-closed size policy above.

The engine sender is installed at DLL load time with
`EventChannel::SetSender(...)`; the channel unit itself never includes an engine
header. The startup log is hooked in from `src/Core/Platform.cpp`, right after
the host-detection line, and prints the channel range, every registered event
type with its payload size, the fail-closed policy and the mixed-client warning.

## Startup log

```
event channel: 0x70-0x7F, 1 registered handler(s)
event channel:   0x70 Venture_Event_Test        8 byte(s)
event channel: unregistered types 0x2F-0xFF are answered with 1 byte(s) fail-closed (engine types 0x00-0x2E fall through to the vanilla table)
event channel: mixed-client risk - ...
```

## Mixed-client warning

The event number and payload size are **not self-describing on the wire**: the
engine already knows how many bytes an event has before it reads the payload,
and it is the `GetEventSize` hook of the receiving platform that answers that
question. Consequences:

* a peer that does **not** run this platform does not know the `0x70-0x7F`
  numbers. On a build without the fail-closed policy the engine reads its size
  table out of bounds, gets a garbage size and copies that many bytes into the
  104-byte event buffer (stack corruption); on a build with the policy the
  number is consumed as 1 byte fail-closed - memory-safe, but the payload is
  misparsed and the game desynchronises;
* a peer that runs this platform but registered a different payload size for the
  same number has the same parse problem: the receiver consumes its own size,
  not the sender's.

There is **no automatic peer detection** in this increment: the channel cannot
know which events a remote peer understands. Avoiding a mixed client is the
user's responsibility - every player in a multiplayer game has to run a platform
build that registers the same event types and payload sizes. This is documented
here and listed as a T2/T3 (multiplayer) test for the user; the single-player
path (raise an event, the local dispatch hook runs the responder) can be tested
by hand with the example command described in `docs/Commands.md`.

## What is not done

* No automatic peer detection or negotiation - see the mixed-client warning.
* No cross-platform event-number negotiation: the fail-closed policy treats a
  number it does not own as an opaque one-byte payload, and a platform loaded
  after VentureCore cannot claim new numbers (see the order contract).
* No payload versioning: the meaning of a payload is a contract between
  platform builds of the same version, not encoded in the frame.
* No event persistence: events are transient, they are not written to a
  savegame.
* No queueing/retry layer: an event is queued through the engine's own
  `OutList`, exactly like a vanilla event.
* No integration with the CnCNet `0x30-0x3F` transport; the channel only uses
  the engine's normal event queue.
