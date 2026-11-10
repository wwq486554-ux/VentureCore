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
void RestorePosition(IStream* const stream, const ULARGE_INTEGER& position)
{
	LARGE_INTEGER move{};
	move.QuadPart = static_cast<LONGLONG>(position.QuadPart);
	stream->Seek(move, STREAM_SEEK_SET, nullptr);
}

const char* ParseResultName(const venture::SaveTrailer::ParseResult result)
{
	switch (result)
	{
	case venture::SaveTrailer::ParseResult::NoTrailer: return "no trailer";
	case venture::SaveTrailer::ParseResult::Corrupt:   return "corrupt trailer";
	case venture::SaveTrailer::ParseResult::Ok:        return "ok";
	default:                                           return "?";
	}
}
}

namespace venture::SaveTrailer
{
std::vector<std::uint8_t> WrapPayload(const std::vector<std::uint8_t>& payload)
{
	std::vector<std::uint8_t> out;
	out.reserve(payload.size() + SuffixSize);
	out.insert(out.end(), payload.begin(), payload.end());

	ExtByteStreamWriter writerSuffix;
	writerSuffix.WriteU32(Magic);
	writerSuffix.WriteU32(FormatVersion);
	writerSuffix.WriteU32(static_cast<std::uint32_t>(payload.size()));
	writerSuffix.WriteU32(ExtCrc32(payload.empty() ? nullptr : payload.data(), payload.size()));
	writerSuffix.WriteU32(Magic);

	const auto& suffix = writerSuffix.Data();
	out.insert(out.end(), suffix.begin(), suffix.end());
	return out;
}

ParseResult Parse(const std::uint8_t* const data, const std::size_t size,
	std::size_t& payloadOffset, std::size_t& payloadSize)
{
	payloadOffset = 0;
	payloadSize = 0;

	if (!data || size < SuffixSize)
		return ParseResult::NoTrailer;

	const std::uint8_t* const suffix = data + size - SuffixSize;

	std::uint32_t magicBefore = 0;
	std::uint32_t formatVersion = 0;
	std::uint32_t storedPayloadSize = 0;
	std::uint32_t storedCrc = 0;
	std::uint32_t magicAfter = 0;

	std::memcpy(&magicBefore, suffix, sizeof(magicBefore));
	std::memcpy(&formatVersion, suffix + 4, sizeof(formatVersion));
	std::memcpy(&storedPayloadSize, suffix + 8, sizeof(storedPayloadSize));
	std::memcpy(&storedCrc, suffix + 12, sizeof(storedCrc));
	std::memcpy(&magicAfter, suffix + 16, sizeof(magicAfter));

	if (magicBefore != Magic || magicAfter != Magic)
		return ParseResult::NoTrailer;

	if (formatVersion != FormatVersion)
		return ParseResult::Corrupt;

	if (storedPayloadSize > MaxPayloadSize || storedPayloadSize > size - SuffixSize)
		return ParseResult::Corrupt;

	payloadOffset = size - SuffixSize - storedPayloadSize;
	payloadSize = storedPayloadSize;

	const std::uint32_t actualCrc = ExtCrc32(
		payloadSize == 0 ? nullptr : data + payloadOffset, payloadSize);
	if (actualCrc != storedCrc)
		return ParseResult::Corrupt;

	return ParseResult::Ok;
}

void Write(IStream* const stream)
{
	if (!stream)
		return;

	std::vector<std::uint8_t> payload;
	std::uint32_t storeBlocks = 0;
	if (!ExtSave::WritePayload(VC_SAVEGAME_ID, payload, &storeBlocks))
	{
		Log::Debug("savegame trailer: no saveable extension data - nothing appended");
		return;
	}

	if (payload.size() > MaxPayloadSize)
	{
		Log::Error("savegame trailer: payload of %u bytes exceeds the %u byte limit - not appended",
			static_cast<unsigned>(payload.size()), static_cast<unsigned>(MaxPayloadSize));
		return;
	}

	const std::vector<std::uint8_t> bytes = WrapPayload(payload);

	// Append at the end of the stream: the engine's save data ends where its
	// object-state writer stopped, but another platform may have moved the stream
	// position, and writing at the current position would overwrite its bytes.
	LARGE_INTEGER zero{};
	zero.QuadPart = 0;
	ULARGE_INTEGER position{};
	if (FAILED(stream->Seek(zero, STREAM_SEEK_END, &position)))
	{
		Log::Warn("savegame trailer: the stream is not seekable - not appended");
		return;
	}

	ULONG written = 0;
	const HRESULT result = stream->Write(bytes.data(), static_cast<ULONG>(bytes.size()), &written);
	if (FAILED(result) || written != bytes.size())
	{
		Log::Error("savegame trailer: could not write %u bytes (hr=0x%08X, written=%u)",
			static_cast<unsigned>(bytes.size()), static_cast<unsigned>(result), static_cast<unsigned>(written));
		return;
	}

	Log::Info("savegame trailer: offset=0x%08X%08X size=%u payload=%u store-block(s)=%u",
		static_cast<unsigned>(position.QuadPart >> 32), static_cast<unsigned>(position.QuadPart & 0xFFFFFFFFu),
		static_cast<unsigned>(bytes.size()), static_cast<unsigned>(payload.size()), static_cast<unsigned>(storeBlocks));
}

void Read(IStream* const stream)
{
	if (!stream)
		return;

	LARGE_INTEGER zero{};
	zero.QuadPart = 0;
	ULARGE_INTEGER current{};
	if (FAILED(stream->Seek(zero, STREAM_SEEK_CUR, &current)))
	{
		Log::Warn("savegame: the stream is not seekable - extension state not restored");
		return;
	}

	ULARGE_INTEGER end{};
	if (FAILED(stream->Seek(zero, STREAM_SEEK_END, &end)))
	{
		RestorePosition(stream, current);
		return;
	}

	if (end.QuadPart < SuffixSize)
	{
		RestorePosition(stream, current);
		Log::Info("savegame: no extension trailer (save written without this platform)");
		return;
	}

	// Read the largest possible trailer region (payload limit + suffix) from the
	// end of the stream and parse it in memory; the stream position is restored
	// afterwards so the other platforms' sequential readers are not disturbed.
	const std::size_t available =
		end.QuadPart > static_cast<unsigned long long>(MaxPayloadSize) + SuffixSize
			? static_cast<std::size_t>(MaxPayloadSize) + SuffixSize
			: static_cast<std::size_t>(end.QuadPart);

	LARGE_INTEGER back{};
	back.QuadPart = -static_cast<LONGLONG>(available);
	if (FAILED(stream->Seek(back, STREAM_SEEK_END, nullptr)))
	{
		RestorePosition(stream, current);
		return;
	}

	std::vector<std::uint8_t> buffer(available);
	ULONG read = 0;
	const HRESULT readResult = stream->Read(buffer.data(), static_cast<ULONG>(available), &read);

	RestorePosition(stream, current);

	if (FAILED(readResult) || read != available)
	{
		Log::Warn("savegame: could not read the trailer region (hr=0x%08X, read=%u)",
			static_cast<unsigned>(readResult), static_cast<unsigned>(read));
		return;
	}

	std::size_t payloadOffset = 0;
	std::size_t payloadSize = 0;
	const ParseResult parsed = Parse(buffer.data(), buffer.size(), payloadOffset, payloadSize);

	if (parsed == ParseResult::NoTrailer)
	{
		Log::Info("savegame: no extension trailer (save written without this platform)");
		return;
	}

	if (parsed == ParseResult::Corrupt)
	{
		Log::Warn("savegame: extension trailer rejected (%s) - extension state starts empty",
			ParseResultName(parsed));
		return;
	}

	const ExtSave::LoadSummary summary =
		ExtSave::ReadPayload(VC_SAVEGAME_ID, buffer.data() + payloadOffset, payloadSize);

	switch (summary.result)
	{
	case ExtSave::LoadSummary::Result::NoData:
		Log::Info("savegame: trailer carries no extension data");
		break;

	case ExtSave::LoadSummary::Result::Invalid:
		Log::Warn("savegame: extension payload rejected - extension state starts empty");
		break;

	case ExtSave::LoadSummary::Result::Loaded:
		Log::Info("savegame: extension payload read (%u store block(s), %u skipped, %u instance(s))",
			static_cast<unsigned>(summary.storeBlocks), static_cast<unsigned>(summary.skippedBlocks),
			static_cast<unsigned>(summary.created));
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
