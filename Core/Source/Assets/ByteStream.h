#pragma once

#include <cstring>
#include <span>
#include <vector>

#include "CoreTypes.h"
#include "Containers/String.h"
#include "Hash/StableHash.h"


/**
 * Little-endian integers, LEB128 varints and length-prefixed bytes for the asset system's binary
 * encodings: KeyBlobs (CKeyWriter), the derived-data cache's manifests and records, the build state
 * log. One encoder for all of them, so the normative key bytes and the cache files never drift apart.
 * Internal to Core/Source/Assets: header-only, not exported.
 *
 * The reader never trusts a length it reads: every count is checked against the bytes that remain
 * before anything is sized from it, so a damaged or hostile file can make a parse fail, never make
 * it allocate gigabytes or read past the end. std::vector only (CRT heap): used on worker threads.
 */
namespace frt::assets::binary
{
// Byte order (memcmp), a prefix before what extends it: -1, 0 or 1. The order of KeyBlob field
// names, of discovered inputs in DDC keys and manifests, and of steps in the build state log.
[[nodiscard]] inline int32 CompareBytes (FStringView A, FStringView B) noexcept
{
	const uint32 common = A.Len() < B.Len() ? A.Len() : B.Len();
	const int32 order = common > 0u ? std::memcmp(A.GetData(), B.GetData(), common) : 0;
	if (order != 0)
	{
		return order < 0 ? -1 : 1;
	}
	return A.Len() == B.Len() ? 0 : (A.Len() < B.Len() ? -1 : 1);
}

// Unsigned LEB128, shortest form.
inline void AppendVarint (std::vector<uint8>& Out, uint64 Value)
{
	while (Value >= 0x80u)
	{
		Out.push_back(static_cast<uint8>(Value | 0x80u));
		Value >>= 7u;
	}
	Out.push_back(static_cast<uint8>(Value));
}

// Bytes AppendVarint writes for Value.
[[nodiscard]] constexpr uint64 GetVarintSize (uint64 Value) noexcept
{
	uint64 size = 1u;
	while (Value >= 0x80u)
	{
		Value >>= 7u;
		++size;
	}
	return size;
}

inline void AppendLittleEndian (std::vector<uint8>& Out, uint64 Value, uint32 ByteCount)
{
	for (uint32 i = 0u; i < ByteCount; ++i)
	{
		Out.push_back(static_cast<uint8>(Value >> (8u * i)));
	}
}

inline void AppendRaw (std::vector<uint8>& Out, std::span<const uint8> Bytes)
{
	Out.insert(Out.end(), Bytes.begin(), Bytes.end());
}

// varint length, then the bytes.
inline void AppendSized (std::vector<uint8>& Out, std::span<const uint8> Bytes)
{
	AppendVarint(Out, Bytes.size());
	AppendRaw(Out, Bytes);
}

inline void AppendText (std::vector<uint8>& Out, FStringView Text)
{
	AppendSized(Out, Text.AsBytes());
}

inline void AppendHash (std::vector<uint8>& Out, const hash::SContentHash& Hash)
{
	AppendRaw(Out, Hash.AsBytes());
}

inline void WriteLittleEndianAt (std::vector<uint8>& Out, size_t Offset, uint64 Value, uint32 ByteCount)
{
	for (uint32 i = 0u; i < ByteCount; ++i)
	{
		Out[Offset + i] = static_cast<uint8>(Value >> (8u * i));
	}
}

[[nodiscard]] inline uint64 ReadLittleEndianAt (std::span<const uint8> Bytes, size_t Offset, uint32 ByteCount) noexcept
{
	uint64 value = 0u;
	for (uint32 i = 0u; i < ByteCount; ++i)
	{
		value |= static_cast<uint64>(Bytes[Offset + i]) << (8u * i);
	}
	return value;
}


// A cursor over bytes. Every Read returns false, leaving the output unspecified, once the bytes run
// out or a value is malformed; after a failure every further Read fails too.
class CByteReader
{
public:
	explicit CByteReader (std::span<const uint8> InBytes) noexcept
		: Bytes(InBytes)
	{}

	[[nodiscard]] size_t GetPosition () const noexcept { return Position; }
	[[nodiscard]] size_t GetRemaining () const noexcept { return Bytes.size() - Position; }
	[[nodiscard]] bool IsAtEnd () const noexcept { return Position == Bytes.size(); }
	[[nodiscard]] bool HasFailed () const noexcept { return bFailed; }

	bool ReadU8 (uint8& Out) noexcept
	{
		if (!Require(1u))
		{
			return false;
		}
		Out = Bytes[Position++];
		return true;
	}

	bool ReadLittleEndian (uint64& Out, uint32 ByteCount) noexcept
	{
		if (!Require(ByteCount))
		{
			return false;
		}
		Out = ReadLittleEndianAt(Bytes, Position, ByteCount);
		Position += ByteCount;
		return true;
	}

	// At most 10 bytes; a value past 64 bits fails.
	bool ReadVarint (uint64& Out) noexcept
	{
		uint64 value = 0u;
		for (uint32 shift = 0u; shift < 64u; shift += 7u)
		{
			uint8 byte = 0u;
			if (!ReadU8(byte))
			{
				return false;
			}
			if (shift == 63u && byte > 1u)
			{
				return Fail();
			}
			value |= static_cast<uint64>(byte & 0x7Fu) << shift;
			if ((byte & 0x80u) == 0u)
			{
				Out = value;
				return true;
			}
		}
		return Fail();
	}

	bool ReadSpan (uint64 Count, std::span<const uint8>& Out) noexcept
	{
		if (!Require(Count))
		{
			return false;
		}
		Out = Bytes.subspan(Position, static_cast<size_t>(Count));
		Position += static_cast<size_t>(Count);
		return true;
	}

	// A varint length, then that many bytes.
	bool ReadSized (std::span<const uint8>& Out) noexcept
	{
		uint64 count = 0u;
		return ReadVarint(count) && ReadSpan(count, Out);
	}

	bool ReadText (FString& Out)
	{
		std::span<const uint8> bytes;
		if (!ReadSized(bytes))
		{
			return false;
		}
		Out = FStringView(reinterpret_cast<const char*>(bytes.data()), bytes.size());
		return true;
	}

	bool ReadHash (hash::SContentHash& Out) noexcept
	{
		std::span<const uint8> bytes;
		if (!ReadSpan(hash::SContentHash::Size, bytes))
		{
			return false;
		}
		std::memcpy(Out.Bytes, bytes.data(), hash::SContentHash::Size);
		return true;
	}

	// Skips the bytes if they are exactly Expected (a magic number); fails otherwise.
	bool Expect (std::span<const uint8> Expected) noexcept
	{
		std::span<const uint8> bytes;
		if (!ReadSpan(Expected.size(), bytes))
		{
			return false;
		}
		return std::memcmp(bytes.data(), Expected.data(), Expected.size()) == 0 ? true : Fail();
	}

private:
	std::span<const uint8> Bytes;
	size_t Position = 0u;
	bool bFailed = false;

	bool Require (uint64 Count) noexcept
	{
		if (bFailed || Count > Bytes.size() - Position)
		{
			return Fail();
		}
		return true;
	}

	bool Fail () noexcept
	{
		bFailed = true;
		return false;
	}
};
}
