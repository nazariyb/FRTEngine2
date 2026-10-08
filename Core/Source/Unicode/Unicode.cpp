#include "Unicode/Unicode.h"

#include <cstring>
#include <limits>

#include "Unicode/UnicodeTablesInternal.h"

// The one definition of the tables in Core (generated; see Tools/Unicode/README.md).
#include "Unicode/UnicodeTables.generated.inl"


namespace frt::unicode
{
static_assert(static_cast<uint32>(EGeneralCategory::Count) == 30u, "the generator emits the 30 UCD categories in this order");
static_assert(tables::TotalBytes < 48u * 1024u, "the generator enforces this budget; a hand edit would not");
static_assert(tables::UnassignedRecord < tables::RecordCount);

namespace
{
constexpr uint64 AsciiMask = 0x8080808080808080ull;

// The first index >= Index that is not ASCII, or Len. Engine text is mostly ASCII: a run costs one
// load and one test per 8 bytes instead of a decode per byte.
uint32 SkipAscii (const char* Data, uint32 Index, uint32 Len) noexcept
{
	while (Len - Index >= 8u)
	{
		uint64 word;
		std::memcpy(&word, Data + Index, sizeof(word));
		if ((word & AsciiMask) != 0u)
		{
			break;
		}
		Index += 8u;
	}

	while (Index < Len && static_cast<unsigned char>(Data[Index]) < 0x80u)
	{
		++Index;
	}
	return Index;
}

// Offset of the first unit the decoder calls ill-formed, or Len.
template <bool bWtf8>
uint32 FindInvalid (const char* Data, uint32 Len) noexcept
{
	uint32 index = 0u;
	while (true)
	{
		index = SkipAscii(Data, index, Len);
		if (index == Len)
		{
			return Len;
		}

		const SDecodedCodePoint decoded = detail::DecodeUtf8Unit<bWtf8>(Data + index, Len - index);
		if (!decoded.bWellFormed)
		{
			return index;
		}
		index += decoded.Length;
	}
}
}


bool IsValidUtf8 (const char* Data, uint32 Len) noexcept
{
	return FindInvalid<false>(Data, Len) == Len;
}

bool IsValidWtf8 (const char* Data, uint32 Len) noexcept
{
	return FindInvalid<true>(Data, Len) == Len;
}

uint32 FindInvalidUtf8 (const char* Data, uint32 Len) noexcept
{
	return FindInvalid<false>(Data, Len);
}

uint32 CountCodePoints (const char* Data, uint32 Len) noexcept
{
	uint32 count = 0u;
	uint32 index = 0u;
	while (true)
	{
		const uint32 asciiEnd = SkipAscii(Data, index, Len);
		count += asciiEnd - index;
		if (asciiEnd == Len)
		{
			return count;
		}

		index = asciiEnd + DecodeUtf8(Data + asciiEnd, Len - asciiEnd).Length;
		++count;
	}
}

bool IsAscii (const char* Data, uint32 Len) noexcept
{
	return SkipAscii(Data, 0u, Len) == Len;
}

uint64 Utf16LengthOfWtf8 (const char* Src, uint32 SrcLen) noexcept
{
	uint64 length = 0u;
	uint32 index = 0u;
	while (true)
	{
		const uint32 asciiEnd = SkipAscii(Src, index, SrcLen);
		length += asciiEnd - index;
		if (asciiEnd == SrcLen)
		{
			return length;
		}

		// An ill-formed unit becomes one U+FFFD; only a supplementary code point needs a pair.
		const SDecodedCodePoint decoded = DecodeWtf8(Src + asciiEnd, SrcLen - asciiEnd);
		length += (decoded.bWellFormed && decoded.CodePoint > 0xFFFFu) ? 2u : 1u;
		index = asciiEnd + decoded.Length;
	}
}

uint32 ConvertWtf8ToUtf16 (const char* Src, uint32 SrcLen, wchar_t* Dst) noexcept
{
	uint32 written = 0u;
	uint32 index = 0u;
	while (true)
	{
		const uint32 asciiEnd = SkipAscii(Src, index, SrcLen);
		for (; index < asciiEnd; ++index)
		{
			Dst[written++] = static_cast<wchar_t>(Src[index]);
		}
		if (index == SrcLen)
		{
			return written;
		}

		const SDecodedCodePoint decoded = DecodeWtf8(Src + index, SrcLen - index);
		// A WTF-8 surrogate form decodes well-formed to the lone surrogate itself, which
		// EncodeUtf16 writes as one unit: the Windows name it came from, unchanged.
		written += EncodeUtf16(decoded.bWellFormed ? decoded.CodePoint : ReplacementCharacter, Dst + written);
		index += decoded.Length;
	}
}

uint64 Wtf8LengthOfUtf16 (const wchar_t* Src, size_t SrcLen) noexcept
{
	uint64 length = 0u;
	size_t index = 0u;
	while (index < SrcLen)
	{
		const uint16 unit = static_cast<uint16>(Src[index]);
		if (unit < 0x80u)
		{
			++length;
			++index;
			continue;
		}

		// Never more than 2 units are read, so the uint32 Available cannot truncate anything.
		const uint32 available = SrcLen - index >= 2u ? 2u : 1u;
		const SDecodedCodePoint decoded = DecodeUtf16(Src + index, available);
		// A pair is one supplementary code point (4 bytes); a lone surrogate (not well-formed) is kept
		// as its 3-byte form, which is what Utf8Length answers for it too.
		length += Utf8Length(decoded.CodePoint);
		index += decoded.Length;
	}
	return length;
}

uint32 ConvertUtf16ToWtf8 (const wchar_t* Src, size_t SrcLen, char* Dst) noexcept
{
	size_t written = 0u;
	size_t index = 0u;
	while (index < SrcLen)
	{
		const uint16 unit = static_cast<uint16>(Src[index]);
		if (unit < 0x80u)
		{
			Dst[written++] = static_cast<char>(unit);
			++index;
			continue;
		}

		const uint32 available = SrcLen - index >= 2u ? 2u : 1u;
		const SDecodedCodePoint decoded = DecodeUtf16(Src + index, available);
		// Pairs come back as one supplementary code point (4 bytes); lone surrogates as themselves
		// (3 bytes): canonical WTF-8 either way.
		written += EncodeWtf8(decoded.CodePoint, Dst + written);
		index += decoded.Length;
	}

	frt_assert(written <= (std::numeric_limits<uint32>::max)());
	return static_cast<uint32>(written);
}


namespace detail
{
const SCharRecord& GetCharRecord (char32_t C) noexcept
{
	return tables::LookupRecord(C);
}
}

const char* GetUnicodeVersion () noexcept
{
	return tables::Version;
}
}
