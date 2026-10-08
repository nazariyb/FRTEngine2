#pragma once

#include <cstddef>
#include <type_traits>

#include "Core.h"
#include "CoreTypes.h"
#include "Unicode/Ascii.h"


/**
 * Code points, UTF-8 / WTF-8 / UTF-16 codecs, character properties and simple case mapping.
 *
 * Text in the engine is UTF-8 by contract, not by invariant (Containers/String.h): any byte
 * sequence may arrive, so every decoder here is total. An ill-formed sequence decodes to U+FFFD
 * and consumes its maximal subpart (Unicode Standard ch. 3, "U+FFFD Substitution of Maximal
 * Subparts"), which is what makes forward iteration, backward iteration and boundary tests agree
 * on where each unit starts.
 *
 * WTF-8 is UTF-8 plus the 3-byte forms of lone surrogates (ED A0..BF 80..BF). Windows file names
 * and other UTF-16 OS strings may hold unpaired surrogates; storing them that way is what lets a
 * path survive path -> FString -> path. Canonical WTF-8 never holds a lead-surrogate form directly
 * followed by a trail-surrogate form: a pair is always its 4-byte UTF-8 sequence.
 *
 * Properties and case mappings come from tables generated out of the Unicode Character Database
 * (Tools/Unicode/GenerateUnicodeTables.py; the version is GetUnicodeVersion()). They are the simple,
 * locale-free mappings: no Turkish dotless i, no final sigma, no 1:n expansions such as German
 * sharp s -> "SS". ASCII is answered inline without touching the tables.
 */
namespace frt::unicode
{
static_assert(sizeof(wchar_t) == 2, "UTF-16 code units are wchar_t: Windows only");

inline constexpr char32_t ReplacementCharacter = 0xFFFDu;
inline constexpr char32_t MaxCodePoint = 0x10FFFFu;

// D800..DFFF
[[nodiscard]] constexpr bool IsSurrogate (char32_t C) noexcept
{
	return C >= 0xD800u && C <= 0xDFFFu;
}

// D800..DBFF: the first unit of a UTF-16 pair.
[[nodiscard]] constexpr bool IsHighSurrogate (char32_t C) noexcept
{
	return C >= 0xD800u && C <= 0xDBFFu;
}

// DC00..DFFF: the second unit of a UTF-16 pair.
[[nodiscard]] constexpr bool IsLowSurrogate (char32_t C) noexcept
{
	return C >= 0xDC00u && C <= 0xDFFFu;
}

// What UTF-8 and UTF-32 may encode: <= 10FFFF and not a surrogate.
[[nodiscard]] constexpr bool IsScalarValue (char32_t C) noexcept
{
	return C <= MaxCodePoint && !IsSurrogate(C);
}

// FDD0..FDEF, and the last two code points of every plane (xxFFFE, xxFFFF). Permanently reserved
// for internal use: valid in text, never assigned.
[[nodiscard]] constexpr bool IsNoncharacter (char32_t C) noexcept
{
	return (C >= 0xFDD0u && C <= 0xFDEFu) || (C <= MaxCodePoint && (C & 0xFFFEu) == 0xFFFEu);
}

// 10xxxxxx: never the first byte of a well-formed UTF-8 sequence.
[[nodiscard]] constexpr bool IsContinuationByte (char B) noexcept
{
	return (static_cast<unsigned char>(B) & 0xC0u) == 0x80u;
}

// 1F1E6..1F1FF: two of them make a flag.
[[nodiscard]] constexpr bool IsRegionalIndicator (char32_t C) noexcept
{
	return C >= 0x1F1E6u && C <= 0x1F1FFu;
}

enum class ECaseMapping : uint8
{
	Upper,
	Lower,
	Fold,
};


// ---- Single code point codecs ----

struct SDecodedCodePoint
{
	char32_t CodePoint = ReplacementCharacter;
	uint32 Length = 1;          // code units consumed, >= 1
	bool bWellFormed = false;
};


namespace detail
{
// The UTF-8 decoder behind DecodeUtf8 and DecodeWtf8: one Table 3-7 walk, where WTF-8 only widens
// the second byte after ED from 80..9F to 80..BF (the surrogate forms). Every byte that cannot
// continue the sequence ends the maximal subpart before it.
template <bool bWtf8>
constexpr SDecodedCodePoint DecodeUtf8Unit (const char* Data, uint32 Available) noexcept
{
	if (!std::is_constant_evaluated())
	{
		frt_assert(Available >= 1u);
	}

	const uint32 lead = static_cast<unsigned char>(Data[0]);
	if (lead < 0x80u)
	{
		return { static_cast<char32_t>(lead), 1u, true };
	}

	constexpr uint32 SurrogateSecondMax = bWtf8 ? 0xBFu : 0x9Fu;

	uint32 continuationCount = 0u;
	uint32 secondMin = 0x80u;
	uint32 secondMax = 0xBFu;
	char32_t codePoint = 0u;
	if (lead >= 0xC2u && lead <= 0xDFu)
	{
		continuationCount = 1u;
		codePoint = lead & 0x1Fu;
	}
	else if (lead >= 0xE0u && lead <= 0xEFu)
	{
		continuationCount = 2u;
		codePoint = lead & 0x0Fu;
		// E0 80..9F would be overlong; ED A0..BF are the surrogates.
		secondMin = lead == 0xE0u ? 0xA0u : 0x80u;
		secondMax = lead == 0xEDu ? SurrogateSecondMax : 0xBFu;
	}
	else if (lead >= 0xF0u && lead <= 0xF4u)
	{
		continuationCount = 3u;
		codePoint = lead & 0x07u;
		// F0 80..8F would be overlong; F4 90..BF would be above 10FFFF.
		secondMin = lead == 0xF0u ? 0x90u : 0x80u;
		secondMax = lead == 0xF4u ? 0x8Fu : 0xBFu;
	}
	else
	{
		// 80..BF (a stray continuation byte), C0/C1 (always overlong) and F5..FF (above 10FFFF).
		return {};
	}

	for (uint32 i = 1u; i <= continuationCount; ++i)
	{
		if (i >= Available)
		{
			return { ReplacementCharacter, i, false };
		}

		const uint32 byte = static_cast<unsigned char>(Data[i]);
		const uint32 byteMin = i == 1u ? secondMin : 0x80u;
		const uint32 byteMax = i == 1u ? secondMax : 0xBFu;
		if (byte < byteMin || byte > byteMax)
		{
			return { ReplacementCharacter, i, false };
		}
		codePoint = (codePoint << 6) | (byte & 0x3Fu);
	}

	if constexpr (bWtf8)
	{
		// Canonical WTF-8: a lead surrogate directly followed by a trail surrogate must have been
		// written as one 4-byte pair. The lead alone is the ill-formed unit; the trail that follows
		// decodes on its own.
		if (IsHighSurrogate(codePoint) && Available >= 6u
			&& static_cast<unsigned char>(Data[3]) == 0xEDu
			&& static_cast<unsigned char>(Data[4]) >= 0xB0u && static_cast<unsigned char>(Data[4]) <= 0xBFu
			&& IsContinuationByte(Data[5]))
		{
			return { ReplacementCharacter, 3u, false };
		}
	}

	return { codePoint, continuationCount + 1u, true };
}
}


// Strict UTF-8 at Data[0..Available), Available >= 1. Ill-formed: {U+FFFD, maximal subpart length,
// false}: an invalid lead (80..C1, F5..FF) is one byte, otherwise the longest prefix of a
// well-formed sequence (Unicode Table 3-7). WTF-8 surrogate forms are ill-formed here.
[[nodiscard]] constexpr SDecodedCodePoint DecodeUtf8 (const char* Data, uint32 Available) noexcept
{
	return detail::DecodeUtf8Unit<false>(Data, Available);
}

// WTF-8: the table above with ED 80..BF 80..BF; a surrogate decodes to itself with bWellFormed =
// true -- except a lead surrogate (ED A0..AF xx) directly followed by a trail surrogate (ED B0..BF
// xx), which is {U+FFFD, 3, false}. Maximal subparts use this table, so the truncated "\xED\xA0"
// is one unit (one U+FFFD).
[[nodiscard]] constexpr SDecodedCodePoint DecodeWtf8 (const char* Data, uint32 Available) noexcept
{
	return detail::DecodeUtf8Unit<true>(Data, Available);
}

// The DecodeUtf8 unit that ends at End (Begin < End, End a unit boundary of the text starting at
// Begin). Backs up over at most 3 continuation bytes and decodes forward from there; if that unit
// does not end exactly at End, the last byte is a unit of its own (a stray continuation byte).
[[nodiscard]] constexpr SDecodedCodePoint DecodeUtf8Backward (const char* Begin, const char* End) noexcept
{
	if (!std::is_constant_evaluated())
	{
		frt_assert(Begin < End);
	}

	const char* p = End - 1;
	while (p > Begin && End - p < 4 && IsContinuationByte(*p))
	{
		--p;
	}

	const SDecodedCodePoint decoded = DecodeUtf8(p, static_cast<uint32>(End - p));
	return p + decoded.Length == End ? decoded : SDecodedCodePoint{ ReplacementCharacter, 1u, false };
}

// Bytes EncodeUtf8 writes: 1-4, and 3 for anything that is not a scalar value (U+FFFD).
[[nodiscard]] constexpr uint32 Utf8Length (char32_t C) noexcept
{
	if (C < 0x80u)
	{
		return 1u;
	}
	if (C < 0x800u)
	{
		return 2u;
	}
	if (C < 0x10000u || C > MaxCodePoint)
	{
		return 3u;
	}
	return 4u;
}

// WTF-8 encoding of any code point <= 10FFFF, surrogates included (3 bytes); above it, U+FFFD.
// Out must have room for 4 bytes. Returns the bytes written.
constexpr uint32 EncodeWtf8 (char32_t C, char* Out) noexcept
{
	if (C > MaxCodePoint)
	{
		C = ReplacementCharacter;
	}

	if (C < 0x80u)
	{
		Out[0] = static_cast<char>(C);
		return 1u;
	}
	if (C < 0x800u)
	{
		Out[0] = static_cast<char>(0xC0u | (C >> 6));
		Out[1] = static_cast<char>(0x80u | (C & 0x3Fu));
		return 2u;
	}
	if (C < 0x10000u)
	{
		Out[0] = static_cast<char>(0xE0u | (C >> 12));
		Out[1] = static_cast<char>(0x80u | ((C >> 6) & 0x3Fu));
		Out[2] = static_cast<char>(0x80u | (C & 0x3Fu));
		return 3u;
	}
	Out[0] = static_cast<char>(0xF0u | (C >> 18));
	Out[1] = static_cast<char>(0x80u | ((C >> 12) & 0x3Fu));
	Out[2] = static_cast<char>(0x80u | ((C >> 6) & 0x3Fu));
	Out[3] = static_cast<char>(0x80u | (C & 0x3Fu));
	return 4u;
}

// UTF-8 encoding of a scalar value; surrogates and values above 10FFFF become U+FFFD (3 bytes).
// Out must have room for 4 bytes. Returns the bytes written.
constexpr uint32 EncodeUtf8 (char32_t C, char* Out) noexcept
{
	return EncodeWtf8(IsScalarValue(C) ? C : ReplacementCharacter, Out);
}

// UTF-16 at Data[0..Available), Available >= 1. A pair -> its scalar value (Length 2, well-formed);
// a lone surrogate -> {that surrogate, 1, false}: the caller chooses between keeping it (WTF-8)
// and replacing it (U+FFFD).
[[nodiscard]] constexpr SDecodedCodePoint DecodeUtf16 (const wchar_t* Data, uint32 Available) noexcept
{
	if (!std::is_constant_evaluated())
	{
		frt_assert(Available >= 1u);
	}

	const char32_t first = static_cast<uint16>(Data[0]);
	if (!IsSurrogate(first))
	{
		return { first, 1u, true };
	}
	if (IsHighSurrogate(first) && Available >= 2u)
	{
		const char32_t second = static_cast<uint16>(Data[1]);
		if (IsLowSurrogate(second))
		{
			return { 0x10000u + ((first - 0xD800u) << 10) + (second - 0xDC00u), 2u, true };
		}
	}
	return { first, 1u, false };
}

// Out must have room for 2 units. A surrogate code point is written as itself (1 unit: the
// WTF-8 -> UTF-16 direction); above 10FFFF, U+FFFD. Returns the units written.
constexpr uint32 EncodeUtf16 (char32_t C, wchar_t* Out) noexcept
{
	if (C > MaxCodePoint)
	{
		C = ReplacementCharacter;
	}

	if (C < 0x10000u)
	{
		Out[0] = static_cast<wchar_t>(C);
		return 1u;
	}

	const char32_t offset = C - 0x10000u;
	Out[0] = static_cast<wchar_t>(0xD800u + (offset >> 10));
	Out[1] = static_cast<wchar_t>(0xDC00u + (offset & 0x3FFu));
	return 2u;
}


// ---- Bulk (Unicode.cpp): ASCII runs are checked 8 bytes at a time ----

// Strict UTF-8: every DecodeUtf8 unit is well-formed (WTF-8 surrogate forms are not).
[[nodiscard]] FRT_CORE_API bool IsValidUtf8 (const char* Data, uint32 Len) noexcept;
// Canonical WTF-8 (see above): lone surrogate forms allowed, a split pair rejected.
[[nodiscard]] FRT_CORE_API bool IsValidWtf8 (const char* Data, uint32 Len) noexcept;
// Offset of the first ill-formed DecodeUtf8 unit, or Len when there is none.
[[nodiscard]] FRT_CORE_API uint32 FindInvalidUtf8 (const char* Data, uint32 Len) noexcept;
// Number of DecodeUtf8 units (an ill-formed unit counts once, like the U+FFFD it decodes to).
[[nodiscard]] FRT_CORE_API uint32 CountCodePoints (const char* Data, uint32 Len) noexcept;
[[nodiscard]] FRT_CORE_API bool IsAscii (const char* Data, uint32 Len) noexcept;

// WTF-8 -> UTF-16 (DecodeWtf8): ill-formed -> U+FFFD; WTF-8 surrogates -> lone surrogates.
// Lengths in uint64: callers check them against their limit before narrowing.
[[nodiscard]] FRT_CORE_API uint64 Utf16LengthOfWtf8 (const char* Src, uint32 SrcLen) noexcept;
// Dst sized by Utf16LengthOfWtf8. Returns the units written; no terminator.
FRT_CORE_API uint32 ConvertWtf8ToUtf16 (const char* Src, uint32 SrcLen, wchar_t* Dst) noexcept;
// UTF-16 -> canonical WTF-8: pairs -> 4 bytes, lone surrogates -> 3 bytes. One unit can produce 3
// bytes, so the length is uint64 (more than 0x55555555 units would wrap a uint32).
[[nodiscard]] FRT_CORE_API uint64 Wtf8LengthOfUtf16 (const wchar_t* Src, size_t SrcLen) noexcept;
// Dst sized by Wtf8LengthOfUtf16 (which the caller has checked fits uint32). Returns the bytes
// written; no terminator.
FRT_CORE_API uint32 ConvertUtf16ToWtf8 (const wchar_t* Src, size_t SrcLen, char* Dst) noexcept;


// ---- Properties and case mapping ----

// Order is part of the generated-table contract (the generator emits the same order).
enum class EGeneralCategory : uint8
{
	Unassigned = 0,                                                                      // Cn (also the default)
	UppercaseLetter, LowercaseLetter, TitlecaseLetter, ModifierLetter, OtherLetter,      // Lu Ll Lt Lm Lo
	NonspacingMark, SpacingMark, EnclosingMark,                                          // Mn Mc Me
	DecimalNumber, LetterNumber, OtherNumber,                                            // Nd Nl No
	ConnectorPunctuation, DashPunctuation, OpenPunctuation, ClosePunctuation,            // Pc Pd Ps Pe
	InitialPunctuation, FinalPunctuation, OtherPunctuation,                              // Pi Pf Po
	MathSymbol, CurrencySymbol, ModifierSymbol, OtherSymbol,                             // Sm Sc Sk So
	SpaceSeparator, LineSeparator, ParagraphSeparator,                                   // Zs Zl Zp
	Control, Format, Surrogate, PrivateUse,                                              // Cc Cf Cs Co
	Count,
};


namespace detail
{
enum ECharFlags : uint8
{
	WhiteSpace = 1u << 0,   // White_Space (PropList.txt)
	Alphabetic = 1u << 1,   // Alphabetic (DerivedCoreProperties.txt)
	Uppercase = 1u << 2,    // Uppercase
	Lowercase = 1u << 3,    // Lowercase
};

struct SCharRecord
{
	int32 UpperDelta;            // simple uppercase mapping = C + UpperDelta
	int32 LowerDelta;
	int32 FoldDelta;             // simple case folding (C and S status)
	EGeneralCategory Category;
	uint8 Flags;                 // ECharFlags
	uint8 Padding[2];
};
static_assert(sizeof(SCharRecord) == 16);

// Table lookup for any code point (> 10FFFF returns the Cn record). Out-of-line (Unicode.cpp) for
// other modules; Core's own TUs use the inline unicode::tables::LookupRecord
// (UnicodeTablesInternal.h).
[[nodiscard]] FRT_CORE_API const SCharRecord& GetCharRecord (char32_t C) noexcept;

// The general category of an ASCII byte (< 0x80), without the tables. Core-Test's checksum runs
// every code point through the public functions, so this is pinned to the generated data.
[[nodiscard]] constexpr EGeneralCategory GetAsciiCategory (char C) noexcept
{
	if (ascii::IsControl(C))
	{
		return EGeneralCategory::Control;
	}
	if (ascii::IsUpper(C))
	{
		return EGeneralCategory::UppercaseLetter;
	}
	if (ascii::IsLower(C))
	{
		return EGeneralCategory::LowercaseLetter;
	}
	if (ascii::IsDigit(C))
	{
		return EGeneralCategory::DecimalNumber;
	}

	switch (C)
	{
	case ' ':
		return EGeneralCategory::SpaceSeparator;
	case '$':
		return EGeneralCategory::CurrencySymbol;
	case '+': case '<': case '=': case '>': case '|': case '~':
		return EGeneralCategory::MathSymbol;
	case '^': case '`':
		return EGeneralCategory::ModifierSymbol;
	case '(': case '[': case '{':
		return EGeneralCategory::OpenPunctuation;
	case ')': case ']': case '}':
		return EGeneralCategory::ClosePunctuation;
	case '-':
		return EGeneralCategory::DashPunctuation;
	case '_':
		return EGeneralCategory::ConnectorPunctuation;
	default:
		// ! " # % & ' * , . / : ; ? @ backslash
		return EGeneralCategory::OtherPunctuation;
	}
}

[[nodiscard]] constexpr int32 GetCaseDelta (const SCharRecord& Record, ECaseMapping Mapping) noexcept
{
	switch (Mapping)
	{
	case ECaseMapping::Upper:
		return Record.UpperDelta;
	case ECaseMapping::Lower:
		return Record.LowerDelta;
	default:
		return Record.FoldDelta;
	}
}

// C + Delta in modular arithmetic: the deltas are signed, code points unsigned.
[[nodiscard]] constexpr char32_t ApplyCaseDelta (char32_t C, int32 Delta) noexcept
{
	return static_cast<char32_t>(C + static_cast<uint32>(Delta));
}

// ASCII case mapping of a code point < 0x80: folding is lower-casing there.
[[nodiscard]] constexpr char32_t MapAsciiCase (char32_t C, ECaseMapping Mapping) noexcept
{
	const char byte = static_cast<char>(C);
	return static_cast<char32_t>(static_cast<unsigned char>(
		Mapping == ECaseMapping::Upper ? ascii::ToUpper(byte) : ascii::ToLower(byte)));
}

// The members of ExtendsPrevious that are not marks: ZWJ, emoji modifiers, tags. (The variation
// selectors are Mn already; listed anyway, so the rule reads like its definition.)
[[nodiscard]] constexpr bool IsExtendingNonMark (char32_t C) noexcept
{
	return C == 0x200Du                        // ZERO WIDTH JOINER
		|| (C >= 0xFE00u && C <= 0xFE0Fu)      // variation selectors 1-16
		|| (C >= 0x1F3FBu && C <= 0x1F3FFu)    // emoji modifiers (skin tones)
		|| (C >= 0xE0020u && C <= 0xE007Fu)    // tags
		|| (C >= 0xE0100u && C <= 0xE01EFu);   // variation selectors 17-256
}

[[nodiscard]] constexpr bool IsMark (EGeneralCategory Category) noexcept
{
	return Category == EGeneralCategory::NonspacingMark || Category == EGeneralCategory::SpacingMark
		|| Category == EGeneralCategory::EnclosingMark;
}

// Cc, Cf, Cs, Co, Cn, Zl, Zp: nothing to show.
[[nodiscard]] constexpr bool IsPrintableCategory (EGeneralCategory Category) noexcept
{
	switch (Category)
	{
	case EGeneralCategory::Unassigned:
	case EGeneralCategory::Control:
	case EGeneralCategory::Format:
	case EGeneralCategory::Surrogate:
	case EGeneralCategory::PrivateUse:
	case EGeneralCategory::LineSeparator:
	case EGeneralCategory::ParagraphSeparator:
		return false;
	default:
		return true;
	}
}
}


// Each: ASCII answered inline without the tables, everything else through GetCharRecord.

[[nodiscard]] inline EGeneralCategory GetGeneralCategory (char32_t C) noexcept
{
	if (C < 0x80u)
	{
		return detail::GetAsciiCategory(static_cast<char>(C));
	}
	return detail::GetCharRecord(C).Category;
}

// White_Space: 25 code points; includes U+0085, NBSP and U+3000, not ZWSP (U+200B, a format char).
[[nodiscard]] inline bool IsWhitespace (char32_t C) noexcept
{
	if (C < 0x80u)
	{
		return ascii::IsWhitespace(static_cast<char>(C));
	}
	return (detail::GetCharRecord(C).Flags & detail::WhiteSpace) != 0;
}

// Alphabetic (derived): letters, letter numbers and the marks that are part of a letter.
[[nodiscard]] inline bool IsAlpha (char32_t C) noexcept
{
	if (C < 0x80u)
	{
		return ascii::IsAlpha(static_cast<char>(C));
	}
	return (detail::GetCharRecord(C).Flags & detail::Alphabetic) != 0;
}

// Nd -- classification only; no parser accepts non-ASCII digits.
[[nodiscard]] inline bool IsDigit (char32_t C) noexcept
{
	if (C < 0x80u)
	{
		return ascii::IsDigit(static_cast<char>(C));
	}
	return detail::GetCharRecord(C).Category == EGeneralCategory::DecimalNumber;
}

[[nodiscard]] inline bool IsAlnum (char32_t C) noexcept
{
	return IsAlpha(C) || IsDigit(C);
}

// Uppercase (derived): Lu plus Other_Uppercase (e.g. the circled capitals U+24B6..24CF).
[[nodiscard]] inline bool IsUpper (char32_t C) noexcept
{
	if (C < 0x80u)
	{
		return ascii::IsUpper(static_cast<char>(C));
	}
	return (detail::GetCharRecord(C).Flags & detail::Uppercase) != 0;
}

// Lowercase (derived): Ll plus Other_Lowercase (e.g. U+00AA, U+02B0).
[[nodiscard]] inline bool IsLower (char32_t C) noexcept
{
	if (C < 0x80u)
	{
		return ascii::IsLower(static_cast<char>(C));
	}
	return (detail::GetCharRecord(C).Flags & detail::Lowercase) != 0;
}

// Pc Pd Ps Pe Pi Pf Po. Note that ASCII $ + < = > ^ ` | ~ are symbols, not punctuation.
[[nodiscard]] inline bool IsPunctuation (char32_t C) noexcept
{
	const EGeneralCategory category = GetGeneralCategory(C);
	return category >= EGeneralCategory::ConnectorPunctuation && category <= EGeneralCategory::OtherPunctuation;
}

// Cc: U+0000..001F and U+007F..009F, a set Unicode has promised never to change -- no lookup.
[[nodiscard]] constexpr bool IsControl (char32_t C) noexcept
{
	return C < 0x20u || (C >= 0x7Fu && C <= 0x9Fu);
}

// Not Cc, Cf, Cs, Co, Cn, Zl or Zp. Spaces (Zs) print.
[[nodiscard]] inline bool IsPrintable (char32_t C) noexcept
{
	if (C < 0x80u)
	{
		return ascii::IsPrintable(static_cast<char>(C));
	}
	return detail::IsPrintableCategory(detail::GetCharRecord(C).Category);
}

// Truncation helper (FStringView::Truncate): true for what must not be separated from the code
// point before it -- Mn, Mc, Me, U+200D ZWJ, U+FE00..FE0F and U+E0100..E01EF (variation
// selectors), U+1F3FB..1F3FF (emoji modifiers), U+E0020..E007F (tags).
[[nodiscard]] inline bool ExtendsPrevious (char32_t C) noexcept
{
	if (C < 0x80u)
	{
		return false;
	}
	return detail::IsExtendingNonMark(C) || detail::IsMark(detail::GetCharRecord(C).Category);
}

// Simple mappings (one code point to one code point), identity where there is none.
[[nodiscard]] inline char32_t MapCase (char32_t C, ECaseMapping Mapping) noexcept
{
	if (C < 0x80u)
	{
		return detail::MapAsciiCase(C, Mapping);
	}
	return detail::ApplyCaseDelta(C, detail::GetCaseDelta(detail::GetCharRecord(C), Mapping));
}

// Simple uppercase mapping (UnicodeData field 12).
[[nodiscard]] inline char32_t ToUpper (char32_t C) noexcept
{
	return MapCase(C, ECaseMapping::Upper);
}

// Simple lowercase mapping (UnicodeData field 13).
[[nodiscard]] inline char32_t ToLower (char32_t C) noexcept
{
	return MapCase(C, ECaseMapping::Lower);
}

// Simple case folding (CaseFolding.txt, status C and S): the key of caseless comparison. Mostly
// lower case, but not always (Cherokee U+AB70 folds to the uppercase U+13A0).
[[nodiscard]] inline char32_t FoldCase (char32_t C) noexcept
{
	return MapCase(C, ECaseMapping::Fold);
}

// The UCD version the tables were generated from, e.g. "17.0.0".
[[nodiscard]] FRT_CORE_API const char* GetUnicodeVersion () noexcept;
}
