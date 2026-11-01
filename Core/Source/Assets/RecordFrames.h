#pragma once

#include <cstring>
#include <optional>
#include <span>
#include <vector>

#include "CoreTypes.h"
#include "Assets/ByteStream.h"
#include "Hash/StableHash.h"


/**
 * Length-prefixed, checksummed records for the asset system's append-only logs: the build state log
 * (BuildState.cpp) and the registry journal (Registry/Journal.cpp). A log is appended one record (or one
 * batch of records) per write, so a crash tears at most its end; a reader checks every record before it
 * trusts a byte of it and either skips to the next magic number (the build state log, whose later records
 * stand alone) or stops there (the journal, whose deltas build on each other).
 *
 * A frame is: magic (4 bytes), payload size (u32 LE), kind (1 byte), the payload, then the XXH3-64 (LE) of
 * everything after the magic. These are the build state log's bytes, unchanged since M1 (its known-answer
 * test pins them); a format of its own would only add a second reader to keep right. Internal to
 * Core/Source/Assets: header-only, not exported, std::vector only (used on worker threads).
 */
namespace frt::assets::record_frames
{
// 0xFE never occurs in UTF-8, so text in payloads cannot fake it; the checksum rejects the rest.
inline constexpr uint8 RecordMagic[4] = { 0xFEu, 0x42u, 0x53u, 0xA7u };
inline constexpr size_t FrameHeaderBytes = 4u + 4u + 1u;     // magic, payload size, kind
inline constexpr size_t FrameBytes = FrameHeaderBytes + 8u;  // ... and the trailing checksum
// No record comes near this: a size beyond it is garbage.
inline constexpr uint32 MaxPayloadBytes = 16u * 1024u * 1024u;

// One framed record: magic, size, kind, payload, checksum of everything after the magic.
inline void AppendFrame (std::vector<uint8>& Out, uint8 Kind, std::span<const uint8> Payload)
{
	const size_t start = Out.size();
	binary::AppendRaw(Out, RecordMagic);
	binary::AppendLittleEndian(Out, Payload.size(), 4u);
	Out.push_back(Kind);
	binary::AppendRaw(Out, Payload);
	const uint64 checksum = hash::Xxh3_64(std::span<const uint8>(Out).subspan(start + 4u));
	binary::AppendLittleEndian(Out, checksum, 8u);
}

// The checked record at Offset of Bytes: its kind and payload, or nullopt when there is none there
// (no magic, a size past the end or beyond reason, a checksum that fails).
struct SFrame
{
	uint8 Kind = 0u;
	std::span<const uint8> Payload;
	size_t Size = 0u;
};

inline std::optional<SFrame> ReadFrame (std::span<const uint8> Bytes, size_t Offset)
{
	if (Bytes.size() - Offset < FrameBytes || std::memcmp(Bytes.data() + Offset, RecordMagic, sizeof(RecordMagic)) != 0)
	{
		return std::nullopt;
	}
	const uint64 payloadSize = binary::ReadLittleEndianAt(Bytes, Offset + 4u, 4u);
	if (payloadSize > MaxPayloadBytes || payloadSize > Bytes.size() - Offset - FrameBytes)
	{
		return std::nullopt;
	}
	const size_t checksumOffset = Offset + FrameHeaderBytes + static_cast<size_t>(payloadSize);
	const uint64 stored = binary::ReadLittleEndianAt(Bytes, checksumOffset, 8u);
	if (stored != hash::Xxh3_64(Bytes.subspan(Offset + 4u, checksumOffset - Offset - 4u)))
	{
		return std::nullopt;
	}
	SFrame frame;
	frame.Kind = Bytes[Offset + 8u];
	frame.Payload = Bytes.subspan(Offset + FrameHeaderBytes, static_cast<size_t>(payloadSize));
	frame.Size = FrameBytes + static_cast<size_t>(payloadSize);
	return frame;
}

// Where the next magic number starts after Offset, or Bytes.size().
inline size_t FindNextMagic (std::span<const uint8> Bytes, size_t Offset)
{
	for (size_t at = Offset + 1u; at + sizeof(RecordMagic) <= Bytes.size(); ++at)
	{
		const void* found = std::memchr(Bytes.data() + at, RecordMagic[0], Bytes.size() - at);
		if (found == nullptr)
		{
			break;
		}
		at = static_cast<size_t>(static_cast<const uint8*>(found) - Bytes.data());
		if (at + sizeof(RecordMagic) <= Bytes.size() && std::memcmp(Bytes.data() + at, RecordMagic, sizeof(RecordMagic)) == 0)
		{
			return at;
		}
	}
	return Bytes.size();
}
}
