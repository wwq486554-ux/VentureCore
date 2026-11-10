#include "Core/SaveTrailer.h"

#include "Core/Log.h"
#include "Core/PlatformConfig.h"
#include "Ext/ExtSave.h"
#include "Ext/ExtStream.h"

#include <Objidl.h>
#include <windows.h>

#include <cstring>

namespace
{
std::uint32_t LoadU32(const std::uint8_t* const data)
{
	std::uint32_t value = 0;
	std::memcpy(&value, data, sizeof(value));
	return value;
}

// Writes the whole buffer sequentially.  IStream::Write may legally accept fewer
// bytes than requested, so loop until everything is out.  A failed write or a
// write that reports no progress aborts (do not spin on a broken stream).
bool WriteAll(IStream* const stream, const std::uint8_t* const data, const std::size_t size)
{
	std::size_t totalWritten = 0;
	while (totalWritten < size)
	{
		ULONG written = 0;
		const HRESULT result = stream->Write(data + totalWritten,
			static_cast<ULONG>(size - totalWritten), &written);
		if (FAILED(result) || written == 0)
			return false;
		totalWritten += written;
	}
	return true;
}

// Reads exactly `size` bytes sequentially, tolerating legal COM short reads.
// `totalRead` reports the progress made; false means a failed read or EOF / no
// progress (with totalRead == 0 that is "nothing at this position").
bool ReadAll(IStream* const stream, std::uint8_t* const data, const std::size_t size,
	std::size_t& totalRead)
{
	totalRead = 0;
	while (totalRead < size)
	{
		ULONG chunk = 0;
		const HRESULT result = stream->Read(data + totalRead,
			static_cast<ULONG>(size - totalRead), &chunk);
		if (FAILED(result) || chunk == 0)
			return false;
		totalRead += chunk;
	}
	return true;
}
}

namespace venture::SaveTrailer
{
// The frame limit and the payload builder limit are the same 16 MiB cap; keep the
// two public constants from drifting apart.
static_assert(MaxPayloadSize == ExtSave::MaxPayloadSize,
	"the trailer frame limit and the payload builder limit must agree");

std::vector<std::uint8_t> WrapPayload(const std::vector<std::uint8_t>& payload)
{
	std::vector<std::uint8_t> out;
	out.reserve(HeaderSize + payload.size());

	ExtByteStreamWriter writerHeader;
	writerHeader.WriteU32(Magic);
	writerHeader.WriteU32(FormatVersion);
	writerHeader.WriteU32(static_cast<std::uint32_t>(payload.size()));
	writerHeader.WriteU32(ExtCrc32(payload.empty() ? nullptr : payload.data(), payload.size()));

	const auto& header = writerHeader.Data();
	out.insert(out.end(), header.begin(), header.end());
	out.insert(out.end(), payload.begin(), payload.end());
	return out;
}

ParseResult Parse(const std::uint8_t* const data, const std::size_t size,
	std::size_t& payloadOffset, std::size_t& payloadSize)
{
	payloadOffset = 0;
	payloadSize = 0;

	if (!data || size < HeaderSize)
		return ParseResult::NoTrailer;

	const std::uint32_t magic = LoadU32(data);
	const std::uint32_t formatVersion = LoadU32(data + 4);
	const std::uint32_t storedPayloadSize = LoadU32(data + 8);
	const std::uint32_t storedCrc = LoadU32(data + 12);

	if (magic != Magic || formatVersion != FormatVersion)
		return ParseResult::Corrupt;

	if (storedPayloadSize > MaxPayloadSize || storedPayloadSize != size - HeaderSize)
		return ParseResult::Corrupt;

	const std::uint32_t actualCrc = ExtCrc32(
		storedPayloadSize == 0 ? nullptr : data + HeaderSize, storedPayloadSize);
	if (actualCrc != storedCrc)
		return ParseResult::Corrupt;

	payloadOffset = HeaderSize;
	payloadSize = storedPayloadSize;
	return ParseResult::Ok;
}

void Write(IStream* const stream)
{
	if (!stream)
		return;

	// The payload is assembled and size-checked against MaxPayloadSize while it is
	// built (A-02/A-11); an oversized payload fails here without being appended.
	std::vector<std::uint8_t> payload;
	std::uint32_t storeBlocks = 0;
	bool sizeExceeded = false;
	if (!ExtSave::WritePayload(VC_SAVEGAME_ID, payload, &storeBlocks, &sizeExceeded))
	{
		if (sizeExceeded)
		{
			Log::Error("savegame trailer: extension payload exceeds the %u byte limit - not appended",
				static_cast<unsigned>(MaxPayloadSize));
		}
		else
		{
			Log::Debug("savegame trailer: no saveable extension data - nothing appended");
		}
		return;
	}

	if (payload.size() > MaxPayloadSize)
	{
		// Defensive: ExtSave::WritePayload already enforces the cap while building.
		Log::Error("savegame trailer: payload of %u bytes exceeds the %u byte limit - not appended",
			static_cast<unsigned>(payload.size()), static_cast<unsigned>(MaxPayloadSize));
		return;
	}

	const std::vector<std::uint8_t> bytes = WrapPayload(payload);

	// Sequential append: the engine's savegame stream does not support Seek, and
	// this handler runs after every ecosystem writer, so the current position is the
	// end of the save data.  No Seek is needed (or possible) to append.
	if (!WriteAll(stream, bytes.data(), bytes.size()))
	{
		Log::Error("savegame trailer: could not write the %u byte trailer - save written without extension state",
			static_cast<unsigned>(bytes.size()));
		return;
	}

	// Optional position probe for the log only.  The engine's stream is a
	// non-seekable streaming wrapper, so this call normally fails; the trailer has
	// already been appended and that is not an error.
	LARGE_INTEGER zero{};
	zero.QuadPart = 0;
	ULARGE_INTEGER position{};
	if (SUCCEEDED(stream->Seek(zero, STREAM_SEEK_CUR, &position)))
	{
		Log::Info("savegame trailer: offset=0x%08X%08X size=%u payload=%u store-block(s)=%u",
			static_cast<unsigned>(position.QuadPart >> 32),
			static_cast<unsigned>(position.QuadPart & 0xFFFFFFFFu),
			static_cast<unsigned>(bytes.size()), static_cast<unsigned>(payload.size()),
			static_cast<unsigned>(storeBlocks));
	}
	else
	{
		Log::Info("savegame trailer: offset=unknown size=%u payload=%u store-block(s)=%u",
			static_cast<unsigned>(bytes.size()), static_cast<unsigned>(payload.size()),
			static_cast<unsigned>(storeBlocks));
	}
}

void Read(IStream* const stream)
{
	if (!stream)
		return;

	// The frame is read from the current position - our load handler runs after
	// every ecosystem reader, so this is exactly where their sequential reads left
	// the stream, i.e. the start of our block (if the save carries one).
	std::uint8_t header[HeaderSize] = {};
	std::size_t headerRead = 0;
	if (!ReadAll(stream, header, sizeof(header), headerRead))
	{
		if (headerRead == 0)
			Log::Info("savegame: no extension trailer (save written without this platform)");
		else
			Log::Warn("savegame: truncated extension trailer header (%u of %u bytes) - extension state starts empty",
				static_cast<unsigned>(headerRead), static_cast<unsigned>(sizeof(header)));
		return;
	}

	const std::uint32_t magic = LoadU32(header);
	if (magic != Magic)
	{
		// The next block belongs to another platform (or there is no block).  This
		// is not an error: a save written without this platform has no frame here.
		Log::Info("savegame: no extension trailer (the stream continues with 0x%08X, not our block)",
			static_cast<unsigned>(magic));
		return;
	}

	const std::uint32_t formatVersion = LoadU32(header + 4);
	if (formatVersion != FormatVersion)
	{
		Log::Warn("savegame: extension trailer format version %u is not supported (expected %u) - "
			"extension state dropped",
			static_cast<unsigned>(formatVersion), static_cast<unsigned>(FormatVersion));
		return;
	}

	const std::uint32_t storedPayloadSize = LoadU32(header + 8);
	if (storedPayloadSize > MaxPayloadSize)
	{
		Log::Warn("savegame: extension trailer payload size %u exceeds the %u byte limit - "
			"extension state dropped",
			static_cast<unsigned>(storedPayloadSize), static_cast<unsigned>(MaxPayloadSize));
		return;
	}

	std::vector<std::uint8_t> payload(storedPayloadSize);
	if (storedPayloadSize != 0)
	{
		std::size_t payloadRead = 0;
		if (!ReadAll(stream, payload.data(), storedPayloadSize, payloadRead))
		{
			Log::Warn("savegame: truncated extension trailer payload (%u of %u bytes) - "
				"extension state starts empty",
				static_cast<unsigned>(payloadRead), static_cast<unsigned>(storedPayloadSize));
			return;
		}
	}

	const std::uint32_t storedCrc = LoadU32(header + 12);
	const std::uint32_t actualCrc = ExtCrc32(
		storedPayloadSize == 0 ? nullptr : payload.data(), storedPayloadSize);
	if (actualCrc != storedCrc)
	{
		Log::Warn("savegame: extension trailer CRC mismatch (stored 0x%08X, computed 0x%08X) - "
			"extension state starts empty",
			static_cast<unsigned>(storedCrc), static_cast<unsigned>(actualCrc));
		return;
	}

	const ExtSave::LoadSummary summary =
		ExtSave::ReadPayload(VC_SAVEGAME_ID, payload.data(), payload.size());

	switch (summary.result)
	{
	case ExtSave::LoadSummary::Result::NoData:
		Log::Info("savegame: trailer carries no extension data");
		break;

	case ExtSave::LoadSummary::Result::Invalid:
		Log::Warn("savegame: extension payload rejected (malformed) - extension state starts empty");
		break;

	case ExtSave::LoadSummary::Result::Incompatible:
		// The engine still loads its own objects; only the extension state is
		// dropped.  Both cases are deliberate compatibility gates (no migration).
		if (summary.incompatibility == ExtSave::LoadSummary::Incompatibility::PayloadVersion)
		{
			Log::Warn("savegame: extension payload version %u is not supported (expected %u) - "
				"extension state dropped, no migration",
				static_cast<unsigned>(summary.payloadVersion),
				static_cast<unsigned>(ExtSave::PayloadVersion));
		}
		else
		{
			Log::Warn("savegame: store set changed - extension state dropped");
		}
		break;

	case ExtSave::LoadSummary::Result::Loaded:
		Log::Info("savegame: extension payload read (%u store block(s), %u instance(s))",
			static_cast<unsigned>(summary.storeBlocks), static_cast<unsigned>(summary.created));
		break;
	}
}

void FinishLoad()
{
	const ExtSave::LoadSummary summary = ExtSave::FinishLoad();

	if (summary.adopted != 0 || summary.deferred != 0 || summary.failed != 0)
	{
		Log::Info("savegame: extension state restored (%u adopted, %u created during load, %u failed)",
			static_cast<unsigned>(summary.adopted), static_cast<unsigned>(summary.deferred),
			static_cast<unsigned>(summary.failed));
	}

	if (summary.failed != 0)
		Log::Warn("savegame: %u extension instance(s) could not be restored", static_cast<unsigned>(summary.failed));
}

void EnterLoadWindow()
{
	ExtSave::EnterLoadWindow();
	Log::Debug("savegame: load window opened");
}

void ExitLoadWindow()
{
	ExtSave::ExitLoadWindow();
	Log::Debug("savegame: load window closed");
}
}
