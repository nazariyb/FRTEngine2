#pragma once

#include <span>
#include <vector>

#include "CoreTypes.h"
#include "Assets/AssetId.h"
#include "Assets/ByteStream.h"
#include "Assets/Registry/RegistryTypes.h"
#include "Containers/String.h"
#include "Diagnostics/Diagnostics.h"
#include "Platform/FileSystem.h"


/**
 * The few encodings the registry snapshot (Snapshot.cpp) and journal (Journal.cpp) share, over
 * assets::binary (ByteStream.h): diagnostics, GUIDs, file identities, signed varints and range-checked
 * enums. Internal to Core/Source/Assets/Registry. Every Read checks what it reads - an enum past its last
 * value fails like a short read - so a damaged file makes a decode fail, never produce a value the store
 * would trip over. std::vector and FString only (both run on workers).
 */
namespace frt::assets::registry_binary
{
using binary::CByteReader;

// Signed values as zigzag varints (mtimes are 100 ns ticks: small magnitudes stay short either side of 0).
inline void AppendSignedVarint (std::vector<uint8>& Out, int64 Value)
{
	binary::AppendVarint(Out, (static_cast<uint64>(Value) << 1u) ^ static_cast<uint64>(Value >> 63));
}

inline bool ReadSignedVarint (CByteReader& Reader, int64& Out) noexcept
{
	uint64 value = 0u;
	if (!Reader.ReadVarint(value))
	{
		return false;
	}
	Out = static_cast<int64>((value >> 1u) ^ (~(value & 1u) + 1u));
	return true;
}

inline bool ReadU32Varint (CByteReader& Reader, uint32& Out) noexcept
{
	uint64 value = 0u;
	if (!Reader.ReadVarint(value) || value > 0xFFFFFFFFull)
	{
		return false;
	}
	Out = static_cast<uint32>(value);
	return true;
}

inline bool ReadU16Varint (CByteReader& Reader, uint16& Out) noexcept
{
	uint64 value = 0u;
	if (!Reader.ReadVarint(value) || value > 0xFFFFull)
	{
		return false;
	}
	Out = static_cast<uint16>(value);
	return true;
}

inline bool ReadU64 (CByteReader& Reader, uint64& Out) noexcept
{
	return Reader.ReadLittleEndian(Out, 8u);
}

// An index stored as Index + 1, with 0 for ~0u ("none").
inline void AppendOptionalIndex (std::vector<uint8>& Out, uint32 Index)
{
	binary::AppendVarint(Out, Index == ~0u ? 0u : static_cast<uint64>(Index) + 1u);
}

inline bool ReadOptionalIndex (CByteReader& Reader, uint32& Out) noexcept
{
	uint64 value = 0u;
	if (!Reader.ReadVarint(value) || value > 0xFFFFFFFFull)
	{
		return false;
	}
	Out = value == 0u ? ~0u : static_cast<uint32>(value - 1u);
	return true;
}

// An enum stored as one byte, at most Last.
template <typename TEnum>
inline void AppendEnum (std::vector<uint8>& Out, TEnum Value)
{
	Out.push_back(static_cast<uint8>(Value));
}

template <typename TEnum>
inline bool ReadEnum (CByteReader& Reader, TEnum Last, TEnum& Out) noexcept
{
	uint8 value = 0u;
	if (!Reader.ReadU8(value) || value > static_cast<uint8>(Last))
	{
		return false;
	}
	Out = static_cast<TEnum>(value);
	return true;
}

// Flag enums as varints of their bits.
template <typename TEnum>
inline void AppendFlags (std::vector<uint8>& Out, TEnum Value)
{
	binary::AppendVarint(Out, static_cast<uint64>(Value));
}

template <typename TEnum>
inline bool ReadFlags (CByteReader& Reader, TEnum& Out) noexcept
{
	uint64 value = 0u;
	using TUnderlying = std::underlying_type_t<TEnum>;
	if (!Reader.ReadVarint(value) || value > static_cast<uint64>(static_cast<TUnderlying>(~TUnderlying(0))))
	{
		return false;
	}
	Out = static_cast<TEnum>(static_cast<TUnderlying>(value));
	return true;
}

inline void AppendGuid (std::vector<uint8>& Out, const SGuid& Guid)
{
	binary::AppendRaw(Out, std::span<const uint8>(Guid.Bytes, SGuid::Size));
}

inline bool ReadGuid (CByteReader& Reader, SGuid& Out) noexcept
{
	std::span<const uint8> bytes;
	if (!Reader.ReadSpan(SGuid::Size, bytes))
	{
		return false;
	}
	std::memcpy(Out.Bytes, bytes.data(), SGuid::Size);
	return true;
}

inline void AppendIdentity (std::vector<uint8>& Out, const platform::SFileIdentity& Identity)
{
	binary::AppendRaw(Out, std::span<const uint8>(Identity.Bytes, sizeof(Identity.Bytes)));
}

inline bool ReadIdentity (CByteReader& Reader, platform::SFileIdentity& Out) noexcept
{
	std::span<const uint8> bytes;
	if (!Reader.ReadSpan(sizeof(Out.Bytes), bytes))
	{
		return false;
	}
	std::memcpy(Out.Bytes, bytes.data(), sizeof(Out.Bytes));
	return true;
}

inline void AppendDiagnostic (std::vector<uint8>& Out, const SDiagnostic& Diagnostic)
{
	AppendEnum(Out, Diagnostic.Severity);
	binary::AppendVarint(Out, Diagnostic.Code);
	binary::AppendText(Out, Diagnostic.Subject);
	binary::AppendText(Out, Diagnostic.File);
	binary::AppendVarint(Out, Diagnostic.Line);
	binary::AppendVarint(Out, Diagnostic.Column);
	binary::AppendText(Out, Diagnostic.FieldPath);
	binary::AppendText(Out, Diagnostic.Message);
}

inline bool ReadDiagnostic (CByteReader& Reader, SDiagnostic& Out)
{
	return ReadEnum(Reader, ESeverity::Error, Out.Severity) && ReadU32Varint(Reader, Out.Code) && Reader.ReadText(Out.Subject)
		&& Reader.ReadText(Out.File) && ReadU32Varint(Reader, Out.Line) && ReadU32Varint(Reader, Out.Column)
		&& Reader.ReadText(Out.FieldPath) && Reader.ReadText(Out.Message);
}

// A diagnostic list: a count, then the diagnostics. Each takes at least 8 bytes, which bounds the count.
inline void AppendDiagnostics (std::vector<uint8>& Out, std::span<const SDiagnostic> Diagnostics)
{
	binary::AppendVarint(Out, Diagnostics.size());
	for (const SDiagnostic& diagnostic : Diagnostics)
	{
		AppendDiagnostic(Out, diagnostic);
	}
}

inline bool ReadDiagnostics (CByteReader& Reader, std::vector<SDiagnostic>& Out)
{
	uint64 count = 0u;
	if (!Reader.ReadVarint(count) || count > Reader.GetRemaining() / 8u)
	{
		return false;
	}
	Out.resize(static_cast<size_t>(count));
	for (SDiagnostic& diagnostic : Out)
	{
		if (!ReadDiagnostic(Reader, diagnostic))
		{
			return false;
		}
	}
	return true;
}

// A count of items of at least MinItemBytes each: false when the bytes left cannot hold that many.
inline bool ReadCount (CByteReader& Reader, uint64 MinItemBytes, uint32& Out) noexcept
{
	uint64 count = 0u;
	if (!Reader.ReadVarint(count) || count > 0xFFFFFFFFull || count > Reader.GetRemaining() / (MinItemBytes == 0u ? 1u : MinItemBytes))
	{
		return false;
	}
	Out = static_cast<uint32>(count);
	return true;
}

/**
 * The mount a mount path, or a problem subject, belongs to: its first segment, which ends at the first '/'
 * or '#' (a subject may be "/Mount#label" in principle; mount names never hold '#'). Empty when Path names
 * no mount.
 */
[[nodiscard]] inline FStringView GetMountNameOf (FStringView Path) noexcept
{
	FStringView mountName;
	FStringView relative;
	if (!SplitMountPath(Path, mountName, relative))
	{
		return FStringView();
	}
	const uint32 hash = mountName.Find('#');
	return hash == FStringView::NotFound ? mountName : mountName.Left(hash);
}
}
