#include "Assets/AssetId.h"

#include <cstring>
#include <initializer_list>

#include "Hash/StableHash.h"
#include "Platform/Random.h"


namespace frt::assets
{
namespace
{
// derive_key contexts (Hash/StableHash.h): one per derivation, versioned.
constexpr FStringLiteral DerivedGuidContext = "FRT derived GUID v1";
constexpr FStringLiteral AutoGuidContext = "FRT auto GUID v1";

constexpr char HexDigits[] = "0123456789abcdef";

// RFC 9562: the version in the high nibble of byte 6, the variant 10 in the two high bits of byte 8.
void SetVersionAndVariant (SGuid& Guid, uint8 Version) noexcept
{
	Guid.Bytes[6] = static_cast<uint8>((Guid.Bytes[6] & 0x0Fu) | (Version << 4u));
	Guid.Bytes[8] = static_cast<uint8>((Guid.Bytes[8] & 0x3Fu) | 0x80u);
}

void AppendLe64 (hash::CContentHasher& Hasher, uint64 Value) noexcept
{
	uint8 bytes[8];
	for (uint32 i = 0u; i < 8u; ++i)
	{
		bytes[i] = static_cast<uint8>(Value >> (8u * i));
	}
	Hasher.Update(std::span<const uint8>(bytes));
}

// A variable-length field of a derivation: its length first, so consecutive fields cannot trade bytes.
void AppendField (hash::CContentHasher& Hasher, std::span<const uint8> Bytes) noexcept
{
	AppendLe64(Hasher, Bytes.size());
	Hasher.Update(Bytes);
}

SGuid FinalizeGuid (const hash::CContentHasher& Hasher, uint8 Version) noexcept
{
	SGuid guid;
	Hasher.FinalizeTo(std::span<uint8>(guid.Bytes));
	SetVersionAndVariant(guid, Version);
	return guid;
}
}


SGuid SGuid::NewRandom () noexcept
{
	SGuid guid;
	do
	{
		platform::FillRandomBytes(std::span<uint8>(guid.Bytes));
		SetVersionAndVariant(guid, 4u);
	}
	while (guid.HasNumericText());
	return guid;
}

std::optional<SGuid> SGuid::Parse (FStringView Text) noexcept
{
	// Bytes, not substrings: the checks below never split a code point, whatever the text holds.
	const char* data = Text.GetData();
	uint32 length = Text.Len();
	if (length >= 2u && data[0] == '{' && data[length - 1u] == '}')
	{
		++data;
		length -= 2u;
	}

	char digits[TextLength];
	if (length == TextLength)
	{
		std::memcpy(digits, data, TextLength);
	}
	else if (length == TextLength + 4u)
	{
		// 8-4-4-4-12.
		uint32 count = 0u;
		for (uint32 i = 0u; i < length; ++i)
		{
			const bool bDashPosition = (i == 8u || i == 13u || i == 18u || i == 23u);
			if (bDashPosition != (data[i] == '-'))
			{
				return std::nullopt;
			}
			if (!bDashPosition)
			{
				digits[count++] = data[i];
			}
		}
	}
	else
	{
		return std::nullopt;
	}

	SGuid guid;
	if (!FStringView(digits, TextLength).DecodeHex(std::span<uint8>(guid.Bytes)))
	{
		return std::nullopt;
	}
	return guid;
}

void SGuid::AppendText (FString& Out) const noexcept
{
	char text[TextLength];
	for (uint32 i = 0u; i < Size; ++i)
	{
		text[2u * i] = HexDigits[Bytes[i] >> 4u];
		text[2u * i + 1u] = HexDigits[Bytes[i] & 0x0Fu];
	}
	Out.Append(FStringView(text, TextLength));
}

FString SGuid::ToString () const noexcept
{
	FString text;
	AppendText(text);
	return text;
}

bool SGuid::HasNumericText () const noexcept
{
	uint32 letters = 0u;
	bool bOnlyE = true;
	for (const uint8 byte : Bytes)
	{
		for (const uint32 nibble : { static_cast<uint32>(byte >> 4u), static_cast<uint32>(byte & 0x0Fu) })
		{
			if (nibble >= 10u)
			{
				++letters;
				bOnlyE = bOnlyE && nibble == 0xEu;
			}
		}
	}
	return letters == 0u || (letters == 1u && bOnlyE);
}


SGuid MakeDerivedGuid (const SGuid& Parent, FStringView Generator, FStringView Label) noexcept
{
	hash::CContentHasher hasher = hash::CContentHasher::ForDeriveKey(DerivedGuidContext);
	hasher.Update(std::span<const uint8>(Parent.Bytes));
	AppendField(hasher, hash::TextBytes(Generator));
	AppendField(hasher, hash::TextBytes(Label));
	return FinalizeGuid(hasher, 8u);
}

SGuid MakeAutoGuidCandidate (FStringView Salt, FStringView Generator, std::span<const uint8> Input, uint32 Attempt) noexcept
{
	hash::CContentHasher hasher = hash::CContentHasher::ForDeriveKey(AutoGuidContext);
	AppendField(hasher, hash::TextBytes(Salt));
	AppendField(hasher, hash::TextBytes(Generator));
	AppendField(hasher, Input);
	if (Attempt > 0u)
	{
		const uint8 counter[4] = { static_cast<uint8>(Attempt), static_cast<uint8>(Attempt >> 8u),
			static_cast<uint8>(Attempt >> 16u), static_cast<uint8>(Attempt >> 24u) };
		hasher.Update(std::span<const uint8>(counter));
	}
	return FinalizeGuid(hasher, 4u);
}

SGuid MakeAutoGuid (FStringView Salt, FStringView Generator, std::span<const uint8> Input) noexcept
{
	return MakeAutoGuid(Salt, Generator, Input, [] (const SGuid&) { return false; });
}


SAssetId64 MakeAssetId64 (const SAssetId& Id) noexcept
{
	uint8 bytes[SGuid::Size + 8u];
	std::memcpy(bytes, Id.Guid.Bytes, SGuid::Size);
	for (uint32 i = 0u; i < 8u; ++i)
	{
		bytes[SGuid::Size + i] = static_cast<uint8>(Id.SubId >> (8u * i));
	}
	const uint64 value = hash::Xxh3_64(std::span<const uint8>(bytes), AssetId64Seed);
	return SAssetId64 { value != 0u ? value : 1u };
}

uint64 MakeSubId (FStringView Label) noexcept
{
	const uint64 value = hash::Xxh3_64(Label);
	return value != 0u ? value : 1u;
}
}
