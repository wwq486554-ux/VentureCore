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
// savegame stream.  This platform appends one **framed trailer** after all of
// them.  The engine's savegame IStream is a streaming wrapper around the OLE
// CONTENTS stream and **does not implement Seek**, so both sides of this layer are
// strictly sequential:
//   * the writer appends at the current stream position - which is the end of the
//     save data when our save handler runs, because it runs after every ecosystem
//     writer (they write inside the engine's object-state writer or at 0x67E42E
//     inside it);
//   * the reader reads the frame where the previous sequential readers left the
//     stream - our load handler runs last in the ecosystem order, so that position
//     is the start of our frame.
// There is no backwards scan and no position restore.  The only Seek call in this
// layer is an optional, failure-tolerant position probe used for one log line.
//
// On-stream layout (header first, 16-byte header):
//
//   [magic "VNC1"][formatVersion][payloadSize][crc32][payload]
//
// `payload` is the extension payload from Ext/ExtSave.h and `crc32` covers it
// (standard IEEE 802.3 CRC-32).  The header is fixed size, so the reader validates
// the frame before reading the payload.
//
// Order contract: at save our handler at 0x67D1B4 runs after every ecosystem
// writer, so a sequential append makes our block the last one.  At load our
// handler at 0x67F7C8 must run after every ecosystem reader for the same reason;
// that holds because `VentureCore.dll` sorts after the known platform DLLs
// (Ares/Kratos/Kaleidoscope/Mix/Phobos).  A future platform whose DLL name sorts
// after this one would gain a reader after ours and break the alignment - see
// docs/SaveFormat.md.
//
// Host compatibility: a game running without this platform ignores the block
// (nothing reads it; it stays unread at the end of the stream), and a save that
// carries no frame simply has no extension data (old save, or a save made without
// this platform).
// ---------------------------------------------------------------------------
constexpr std::uint32_t Magic = 0x31434E56u;  // bytes 'V' 'N' 'C' '1'
constexpr std::uint32_t FormatVersion = 2;    // header-first sequential frame
constexpr std::size_t HeaderSize = 16;        // 4 x u32
constexpr std::uint32_t MaxPayloadSize = 16u * 1024u * 1024u;

enum class ParseResult
{
	NoTrailer,  // buffer too small to hold the fixed header (parser-only state)
	Corrupt,    // a full frame is present but magic/version/size/CRC is wrong
	Ok,
};

// Pure framing helpers (unit-tested without the engine).
std::vector<std::uint8_t> WrapPayload(const std::vector<std::uint8_t>& payload);

// Strict frame validation: magic, format version, payloadSize must equal
// `size - HeaderSize` exactly, and the CRC must cover the payload.  Only a buffer
// smaller than the header is reported as NoTrailer; everything else is Corrupt.
ParseResult Parse(const std::uint8_t* data, std::size_t size, std::size_t& payloadOffset, std::size_t& payloadSize);

// Engine driver, called from src/Engine/Hooks.Save.cpp.  Fully sequential: the
// savegame stream is not seekable and neither function depends on Seek.
void Write(IStream* stream);
void Read(IStream* stream);
void FinishLoad();
void EnterLoadWindow();
void ExitLoadWindow();
}
