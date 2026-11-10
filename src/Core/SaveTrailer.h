#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

struct IStream;

namespace venture::SaveTrailer
{
// ---------------------------------------------------------------------------
// Platform-owned savegame tail block.
//
// Every platform in the ecosystem appends its own opaque block to the engine's
// savegame stream.  This platform appends one **anchored trailer** after all of
// them; the reader finds it by scanning backwards from the end of the stream, so
// the reading side does not depend on which other platforms wrote blocks before
// us, and a save from a game without this platform simply has no trailer
// (self-healing for old saves).
//
// On-stream layout:
//
//   [payload][magic "VNC1"][formatVersion][payloadSize][crc32][magic "VNC1"]
//
// `payload` is the extension payload from Ext/ExtSave.h and `crc32` covers it
// (standard IEEE 802.3 CRC-32).  The 20-byte suffix is fixed size, so the frame
// can be validated with a single backwards read.  The writer appends after every
// other platform's block because this DLL loads after the known platforms
// (file-name order: Ares/Kratos/Kaleido/Mix/Phobos/RA2YRLWResourceReader before
// VentureCore; a future platform whose DLL name sorts after this one would break
// the "last block" assumption - the reader then reports a corrupt trailer and
// keeps the game running).
//
// Host compatibility: block reading is order-independent (backwards scan +
// position restore), but only the party that wrote the save can read a save that
// contains our trailer.  Removing our DLL later still loads old saves normally
// (no trailer found).
// ---------------------------------------------------------------------------
constexpr std::uint32_t Magic = 0x31434E56u;  // bytes 'V' 'N' 'C' '1'
constexpr std::uint32_t FormatVersion = 1;
constexpr std::size_t SuffixSize = 20;        // 5 x u32
constexpr std::uint32_t MaxPayloadSize = 16u * 1024u * 1024u;

enum class ParseResult
{
	NoTrailer,  // no trailer (or somebody else's tail) - old save, nothing to do
	Corrupt,    // our magic is there but the frame is unusable
	Ok,
};

// Pure framing helpers (unit-tested without the engine).
std::vector<std::uint8_t> WrapPayload(const std::vector<std::uint8_t>& payload);
ParseResult Parse(const std::uint8_t* data, std::size_t size, std::size_t& payloadOffset, std::size_t& payloadSize);

// Engine driver, called from src/Engine/Hooks.Save.cpp.
void Write(IStream* stream);
void Read(IStream* stream);
void FinishLoad();
void EnterLoadWindow();
void ExitLoadWindow();
}
