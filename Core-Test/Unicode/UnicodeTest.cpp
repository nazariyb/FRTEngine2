#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "Unicode/Ascii.h"
#include "Unicode/Unicode.h"
// Only for tables::TotalBytes and tables::Version: the arrays it declares are Core's and not exported.
#include "Unicode/UnicodeTables.generated.h"

#include "UnicodeTableChecksum.generated.inl"

using namespace frt;

using unicode::EGeneralCategory;
using unicode::SDecodedCodePoint;


namespace
{
constexpr uint64 FnvOffsetBasis = 0xCBF29CE484222325ull;
constexpr uint64 FnvPrime = 0x100000001B3ull;

uint64 HashFnv1a (uint64 Hash, const uint8* Bytes, size_t Size)
{
	for (size_t i = 0; i < Size; ++i)
	{
		Hash = (Hash ^ Bytes[i]) * FnvPrime;
	}
	return Hash;
}

void StoreLittleEndian (uint8* Out, uint32 Value)
{
	Out[0] = static_cast<uint8>(Value);
	Out[1] = static_cast<uint8>(Value >> 8);
	Out[2] = static_cast<uint8>(Value >> 16);
	Out[3] = static_cast<uint8>(Value >> 24);
}


// One DecodeUtf8 / DecodeWtf8 unit of a byte string, found by walking it forwards: the reference
// segmentation every other walk (backwards, bulk, counting) must agree with.
struct SUnit
{
	uint32 Start = 0;
	SDecodedCodePoint Decoded;
};

template <bool bWtf8>
std::vector<SUnit> Segment (std::string_view Bytes)
{
	std::vector<SUnit> units;
	const uint32 len = static_cast<uint32>(Bytes.size());
	uint32 index = 0;
	while (index < len)
	{
		SUnit unit;
		unit.Start = index;
		if constexpr (bWtf8)
		{
			unit.Decoded = unicode::DecodeWtf8(Bytes.data() + index, len - index);
		}
		else
		{
			unit.Decoded = unicode::DecodeUtf8(Bytes.data() + index, len - index);
		}
		index += unit.Decoded.Length;
		units.push_back(unit);
	}
	return units;
}

// Every unit's code point, U+FFFD for the ill-formed ones: what a lossy decode shows.
template <bool bWtf8>
std::u32string DecodeAll (std::string_view Bytes)
{
	std::u32string decoded;
	for (const SUnit& unit : Segment<bWtf8>(Bytes))
	{
		decoded.push_back(unit.Decoded.bWellFormed ? unit.Decoded.CodePoint : unicode::ReplacementCharacter);
	}
	return decoded;
}

// Byte strings built to hit every decoder branch: leads of every class, both ends of every
// continuation range, invalid bytes, and ASCII runs long enough for the 8-byte fast paths.
std::string MakeRandomBytes (std::mt19937& Random)
{
	static constexpr unsigned char Palette[] =
	{
		0x00, 'a', 'Z', '~', 0x7F,
		0x80, 0x8F, 0x90, 0x9F, 0xA0, 0xAF, 0xB0, 0xBF,
		0xC0, 0xC1, 0xC2, 0xDF, 0xE0, 0xE1, 0xEC, 0xED, 0xEE, 0xEF,
		0xF0, 0xF1, 0xF3, 0xF4, 0xF5, 0xF8, 0xFF,
	};

	std::uniform_int_distribution<uint32> partDistribution(0u, 24u);
	std::uniform_int_distribution<uint32> paletteDistribution(0u, static_cast<uint32>(std::size(Palette) - 1u));
	std::uniform_int_distribution<uint32> kindDistribution(0u, 4u);
	std::uniform_int_distribution<uint32> asciiRunDistribution(1u, 12u);

	std::string bytes;
	const uint32 parts = partDistribution(Random);
	for (uint32 i = 0; i < parts; ++i)
	{
		if (kindDistribution(Random) == 0u)
		{
			bytes.append(asciiRunDistribution(Random), 'x');
		}
		else
		{
			bytes.push_back(static_cast<char>(Palette[paletteDistribution(Random)]));
		}
	}
	return bytes;
}

// UTF-16 built from ASCII, BMP, both surrogate halves and valid pairs: lone halves in every position.
std::wstring MakeRandomUtf16 (std::mt19937& Random)
{
	static constexpr uint16 Palette[] =
	{
		'a', 0x7F, 0x80, 0x7FF, 0x800, 0x0457, 0x6F22, 0xD7FF, 0xE000, 0xFFFD, 0xFFFF,
		0xD800, 0xD83D, 0xDBFF, 0xDC00, 0xDE00, 0xDFFF,
	};

	std::uniform_int_distribution<uint32> lengthDistribution(0u, 16u);
	std::uniform_int_distribution<uint32> paletteDistribution(0u, static_cast<uint32>(std::size(Palette) - 1u));

	std::wstring units;
	const uint32 length = lengthDistribution(Random);
	for (uint32 i = 0; i < length; ++i)
	{
		units.push_back(static_cast<wchar_t>(Palette[paletteDistribution(Random)]));
	}
	return units;
}

std::wstring ToUtf16 (std::string_view Wtf8)
{
	const uint32 len = static_cast<uint32>(Wtf8.size());
	std::wstring utf16(static_cast<size_t>(unicode::Utf16LengthOfWtf8(Wtf8.data(), len)), L'\0');
	const uint32 written = unicode::ConvertWtf8ToUtf16(Wtf8.data(), len, utf16.data());
	EXPECT_EQ(written, utf16.size());
	return utf16;
}

std::string ToWtf8 (std::wstring_view Utf16)
{
	std::string wtf8(static_cast<size_t>(unicode::Wtf8LengthOfUtf16(Utf16.data(), Utf16.size())), '\0');
	const uint32 written = unicode::ConvertUtf16ToWtf8(Utf16.data(), Utf16.size(), wtf8.data());
	EXPECT_EQ(written, wtf8.size());
	return wtf8;
}
}


// ---- frt::ascii ----

static_assert(ascii::IsWhitespace('\v') && !ascii::IsWhitespace('\0') && !ascii::IsWhitespace('\xA0'));
static_assert(ascii::ToUpper('a') == 'A' && ascii::ToUpper('A') == 'A' && ascii::ToUpper('\xD1') == '\xD1');
static_assert(ascii::ToLower('Z') == 'z' && ascii::ToLower('[') == '[' && ascii::ToLower('@') == '@');
static_assert(ascii::HexValue('f') == 15 && ascii::HexValue('F') == 15 && ascii::HexValue('g') == -1);
static_assert(ascii::HexDigit(10) == 'A' && ascii::HexDigit(10, false) == 'a' && ascii::HexDigit(0x1F) == 'F');
static_assert(ascii::IsIdentifierStart('_') && !ascii::IsIdentifierStart('1') && ascii::IsIdentifierChar('1'));

TEST(AsciiTest, ClassesOfEveryByte)
{
	for (uint32 value = 0; value < 256u; ++value)
	{
		const char c = static_cast<char>(value);
		const bool bUpper = value >= 'A' && value <= 'Z';
		const bool bLower = value >= 'a' && value <= 'z';
		const bool bDigit = value >= '0' && value <= '9';

		EXPECT_EQ(ascii::IsAscii(c), value < 0x80u) << value;
		EXPECT_EQ(ascii::IsWhitespace(c), value == ' ' || (value >= 0x09u && value <= 0x0Du)) << value;
		EXPECT_EQ(ascii::IsDigit(c), bDigit) << value;
		EXPECT_EQ(ascii::IsHexDigit(c), bDigit || (value >= 'a' && value <= 'f') || (value >= 'A' && value <= 'F')) << value;
		EXPECT_EQ(ascii::IsUpper(c), bUpper) << value;
		EXPECT_EQ(ascii::IsLower(c), bLower) << value;
		EXPECT_EQ(ascii::IsAlpha(c), bUpper || bLower) << value;
		EXPECT_EQ(ascii::IsAlnum(c), bUpper || bLower || bDigit) << value;
		EXPECT_EQ(ascii::IsControl(c), value < 0x20u || value == 0x7Fu) << value;
		EXPECT_EQ(ascii::IsPrintable(c), value >= 0x20u && value < 0x7Fu) << value;
		EXPECT_EQ(ascii::IsIdentifierStart(c), bUpper || bLower || value == '_') << value;
		EXPECT_EQ(ascii::IsIdentifierChar(c), bUpper || bLower || bDigit || value == '_') << value;

		// Bytes of UTF-8 sequences pass through the case mappings untouched.
		EXPECT_EQ(ascii::ToUpper(c), bLower ? static_cast<char>(value - 32u) : c) << value;
		EXPECT_EQ(ascii::ToLower(c), bUpper ? static_cast<char>(value + 32u) : c) << value;
	}
}

TEST(AsciiTest, HexDigitsRoundTrip)
{
	for (uint32 nibble = 0; nibble < 16u; ++nibble)
	{
		EXPECT_EQ(ascii::HexValue(ascii::HexDigit(nibble)), static_cast<int32>(nibble));
		EXPECT_EQ(ascii::HexValue(ascii::HexDigit(nibble, false)), static_cast<int32>(nibble));
	}
	EXPECT_EQ(ascii::HexValue('/'), -1);
	EXPECT_EQ(ascii::HexValue(':'), -1);
	EXPECT_EQ(ascii::HexValue('@'), -1);
	EXPECT_EQ(ascii::HexValue('G'), -1);
	EXPECT_EQ(ascii::HexValue('`'), -1);
	EXPECT_EQ(ascii::HexValue('\xC6'), -1);
}


// ---- Generated tables ----

static_assert(unicode::tables::TotalBytes < 48u * 1024u);
static_assert(std::size(unicode::checksum::CategoryCounts) == static_cast<size_t>(EGeneralCategory::Count));

TEST(UnicodeTablesTest, PublicApiReproducesGeneratedChecksum)
{
	// Everything goes through the public functions, ASCII branches included, so this pins those
	// branches to the generated data as well as the trie lookup.
	uint64 hash = FnvOffsetBasis;
	uint32 counts[static_cast<size_t>(EGeneralCategory::Count)] = {};
	for (char32_t c = 0; c <= unicode::MaxCodePoint; ++c)
	{
		const EGeneralCategory category = unicode::GetGeneralCategory(c);
		ASSERT_LT(static_cast<uint32>(category), static_cast<uint32>(EGeneralCategory::Count));
		++counts[static_cast<size_t>(category)];

		const uint32 flags = (unicode::IsWhitespace(c) ? unicode::detail::WhiteSpace : 0u)
			| (unicode::IsAlpha(c) ? unicode::detail::Alphabetic : 0u)
			| (unicode::IsUpper(c) ? unicode::detail::Uppercase : 0u)
			| (unicode::IsLower(c) ? unicode::detail::Lowercase : 0u);

		uint8 bytes[18];
		StoreLittleEndian(bytes, static_cast<uint32>(c));
		bytes[4] = static_cast<uint8>(category);
		bytes[5] = static_cast<uint8>(flags);
		StoreLittleEndian(bytes + 6, static_cast<uint32>(unicode::ToUpper(c)));
		StoreLittleEndian(bytes + 10, static_cast<uint32>(unicode::ToLower(c)));
		StoreLittleEndian(bytes + 14, static_cast<uint32>(unicode::FoldCase(c)));
		hash = HashFnv1a(hash, bytes, sizeof(bytes));
	}

	EXPECT_EQ(hash, unicode::checksum::Fnv1a64);
	for (size_t i = 0; i < std::size(counts); ++i)
	{
		EXPECT_EQ(counts[i], unicode::checksum::CategoryCounts[i]) << "category " << i;
	}
}

TEST(UnicodeTablesTest, Version)
{
	EXPECT_STREQ(unicode::GetUnicodeVersion(), unicode::checksum::Version);
	EXPECT_STREQ(unicode::tables::Version, unicode::checksum::Version);
}

TEST(UnicodeTablesTest, AsciiBranchesMatchTheRecords)
{
	// The inline ASCII answers never read the tables; the tables have ASCII records all the same.
	for (char32_t c = 0; c < 0x80u; ++c)
	{
		const unicode::detail::SCharRecord& record = unicode::detail::GetCharRecord(c);
		EXPECT_EQ(unicode::GetGeneralCategory(c), record.Category) << static_cast<uint32>(c);
		EXPECT_EQ(unicode::IsWhitespace(c), (record.Flags & unicode::detail::WhiteSpace) != 0) << static_cast<uint32>(c);
		EXPECT_EQ(unicode::IsAlpha(c), (record.Flags & unicode::detail::Alphabetic) != 0) << static_cast<uint32>(c);
		EXPECT_EQ(unicode::IsUpper(c), (record.Flags & unicode::detail::Uppercase) != 0) << static_cast<uint32>(c);
		EXPECT_EQ(unicode::IsLower(c), (record.Flags & unicode::detail::Lowercase) != 0) << static_cast<uint32>(c);
		EXPECT_EQ(unicode::ToUpper(c), static_cast<char32_t>(c + static_cast<uint32>(record.UpperDelta))) << static_cast<uint32>(c);
		EXPECT_EQ(unicode::ToLower(c), static_cast<char32_t>(c + static_cast<uint32>(record.LowerDelta))) << static_cast<uint32>(c);
		EXPECT_EQ(unicode::FoldCase(c), static_cast<char32_t>(c + static_cast<uint32>(record.FoldDelta))) << static_cast<uint32>(c);
	}
}

TEST(UnicodeTablesTest, AboveMaxCodePointIsUnassigned)
{
	for (const char32_t c : { char32_t(0x110000u), char32_t(0x7FFFFFFFu), char32_t(0xFFFFFFFFu) })
	{
		EXPECT_EQ(unicode::GetGeneralCategory(c), EGeneralCategory::Unassigned);
		EXPECT_EQ(unicode::ToUpper(c), c);
		EXPECT_EQ(unicode::ToLower(c), c);
		EXPECT_EQ(unicode::FoldCase(c), c);
		EXPECT_FALSE(unicode::IsWhitespace(c));
		EXPECT_FALSE(unicode::IsAlpha(c));
		EXPECT_FALSE(unicode::IsPrintable(c));
		EXPECT_FALSE(unicode::ExtendsPrevious(c));
	}
}


// ---- Properties and case mapping ----

TEST(UnicodePropertiesTest, CaseMappingSpotChecks)
{
	// CYRILLIC A / a, GHE WITH UPTURN, YI.
	EXPECT_EQ(unicode::ToLower(U'А'), U'а');
	EXPECT_EQ(unicode::ToUpper(U'а'), U'А');
	EXPECT_EQ(unicode::ToUpper(U'ґ'), U'Ґ');
	EXPECT_EQ(unicode::ToLower(U'Ґ'), U'ґ');
	EXPECT_EQ(unicode::FoldCase(U'Ґ'), U'ґ');
	EXPECT_EQ(unicode::ToUpper(U'ї'), U'Ї');
	EXPECT_EQ(unicode::ToLower(U'Ї'), U'ї');

	// GREEK SMALL LETTER FINAL SIGMA folds to the ordinary sigma; no final-sigma rule when lowering.
	EXPECT_EQ(unicode::FoldCase(U'ς'), U'σ');
	EXPECT_EQ(unicode::FoldCase(U'Σ'), U'σ');
	EXPECT_EQ(unicode::ToLower(U'Σ'), U'σ');
	EXPECT_EQ(unicode::ToUpper(U'ς'), U'Σ');

	// Turkish dotted / dotless i: simple, locale-free mappings only.
	EXPECT_EQ(unicode::FoldCase(U'İ'), U'İ');   // its folding is full (i + U+0307) or Turkic: neither is ours
	EXPECT_EQ(unicode::ToLower(U'İ'), U'i');
	EXPECT_EQ(unicode::FoldCase(U'ı'), U'ı');
	EXPECT_EQ(unicode::ToUpper(U'ı'), U'I');

	// Sharp s: the capital folds to the small one; the small one has no simple uppercase.
	EXPECT_EQ(unicode::FoldCase(U'ẞ'), U'ß');
	EXPECT_EQ(unicode::ToLower(U'ẞ'), U'ß');
	EXPECT_EQ(unicode::ToUpper(U'ß'), U'ß');

	// The two non-ASCII code points that fold into ASCII.
	EXPECT_EQ(unicode::FoldCase(U'K'), U'k');   // KELVIN SIGN
	EXPECT_EQ(unicode::FoldCase(U'ſ'), U's');   // LATIN SMALL LETTER LONG S
	EXPECT_EQ(unicode::ToUpper(U'ſ'), U'S');

	// DZ digraphs: upper, title and lower forms.
	EXPECT_EQ(unicode::ToUpper(U'ǆ'), U'Ǆ');
	EXPECT_EQ(unicode::ToUpper(U'ǅ'), U'Ǆ');
	EXPECT_EQ(unicode::ToLower(U'ǅ'), U'ǆ');
	EXPECT_EQ(unicode::ToLower(U'Ǆ'), U'ǆ');
	EXPECT_EQ(unicode::FoldCase(U'ǅ'), U'ǆ');
	EXPECT_EQ(unicode::GetGeneralCategory(U'ǅ'), EGeneralCategory::TitlecaseLetter);

	// Cherokee small letters fold to the uppercase ones (stability of folding beats "fold = lower").
	EXPECT_EQ(unicode::FoldCase(U'ꭰ'), U'Ꭰ');
	EXPECT_EQ(unicode::ToUpper(U'ꭰ'), U'Ꭰ');
	EXPECT_EQ(unicode::ToLower(U'ꭰ'), U'ꭰ');
	EXPECT_EQ(unicode::FoldCase(U'Ꭰ'), U'Ꭰ');

	// Mappings that change the UTF-8 length: 2 -> 3 and 3 -> 2 bytes.
	EXPECT_EQ(unicode::ToUpper(U'ɐ'), U'Ɐ');
	EXPECT_EQ(unicode::FoldCase(U'ᲀ'), U'в');

	// Supplementary plane: DESERET CAPITAL LETTER LONG I / small.
	EXPECT_EQ(unicode::ToLower(U'\U00010400'), U'\U00010428');
	EXPECT_EQ(unicode::FoldCase(U'\U00010400'), U'\U00010428');
	EXPECT_EQ(unicode::ToUpper(U'\U00010428'), U'\U00010400');

	// MapCase is the three of them.
	EXPECT_EQ(unicode::MapCase(U'ґ', unicode::ECaseMapping::Upper), U'Ґ');
	EXPECT_EQ(unicode::MapCase(U'Ґ', unicode::ECaseMapping::Lower), U'ґ');
	EXPECT_EQ(unicode::MapCase(U'K', unicode::ECaseMapping::Fold), U'k');
	EXPECT_EQ(unicode::MapCase(U'k', unicode::ECaseMapping::Upper), U'K');

	// No mapping: identity.
	EXPECT_EQ(unicode::ToUpper(U'漢'), U'漢');
	EXPECT_EQ(unicode::FoldCase(U'1'), U'1');
	EXPECT_EQ(unicode::ToUpper(0xD800u), char32_t(0xD800u));
}

TEST(UnicodePropertiesTest, ClassificationSpotChecks)
{
	EXPECT_TRUE(unicode::IsDigit(U'١'));    // ARABIC-INDIC DIGIT ONE
	EXPECT_FALSE(unicode::IsDigit(U'²'));   // SUPERSCRIPT TWO is No, not Nd
	EXPECT_TRUE(unicode::IsAlnum(U'١'));
	EXPECT_TRUE(unicode::IsAlnum(U'ї'));

	EXPECT_TRUE(unicode::IsWhitespace(U' '));    // NO-BREAK SPACE
	EXPECT_TRUE(unicode::IsWhitespace(U'　'));    // IDEOGRAPHIC SPACE
	EXPECT_TRUE(unicode::IsWhitespace(U'\u0085'));    // NEXT LINE
	EXPECT_TRUE(unicode::IsWhitespace(U' '));    // LINE SEPARATOR
	EXPECT_FALSE(unicode::IsWhitespace(U'​'));   // ZERO WIDTH SPACE is a format character
	EXPECT_FALSE(unicode::IsWhitespace(U'\u001C'));   // FILE SEPARATOR: isspace-like in some libraries, not White_Space

	uint32 whiteSpaceCount = 0;
	for (char32_t c = 0; c <= unicode::MaxCodePoint; ++c)
	{
		whiteSpaceCount += unicode::IsWhitespace(c) ? 1u : 0u;
	}
	EXPECT_EQ(whiteSpaceCount, 25u);

	EXPECT_EQ(unicode::GetGeneralCategory(U'漢'), EGeneralCategory::OtherLetter);         // CJK ideograph
	EXPECT_EQ(unicode::GetGeneralCategory(U'\U0001F600'), EGeneralCategory::OtherSymbol);     // GRINNING FACE
	EXPECT_EQ(unicode::GetGeneralCategory(0xD800u), EGeneralCategory::Surrogate);
	EXPECT_EQ(unicode::GetGeneralCategory(0xDFFFu), EGeneralCategory::Surrogate);
	EXPECT_EQ(unicode::GetGeneralCategory(0xE000u), EGeneralCategory::PrivateUse);
	EXPECT_EQ(unicode::GetGeneralCategory(0x10FFFFu), EGeneralCategory::Unassigned);
	EXPECT_EQ(unicode::GetGeneralCategory(0x110000u), EGeneralCategory::Unassigned);
	EXPECT_EQ(unicode::GetGeneralCategory(U'́'), EGeneralCategory::NonspacingMark);
	EXPECT_EQ(unicode::GetGeneralCategory(U'₴'), EGeneralCategory::CurrencySymbol);       // HRYVNIA SIGN
	EXPECT_EQ(unicode::GetGeneralCategory(U'䶿'), EGeneralCategory::OtherLetter);          // inside a First/Last range
	EXPECT_EQ(unicode::GetGeneralCategory(U'\U00033479'), EGeneralCategory::OtherLetter);      // end of CJK Ext. J (Unicode 17)

	EXPECT_TRUE(unicode::ExtendsPrevious(U'́'));       // COMBINING ACUTE ACCENT
	EXPECT_TRUE(unicode::ExtendsPrevious(U'ः'));       // DEVANAGARI SIGN VISARGA (Mc)
	EXPECT_TRUE(unicode::ExtendsPrevious(U'⃝'));       // COMBINING ENCLOSING CIRCLE (Me)
	EXPECT_TRUE(unicode::ExtendsPrevious(U'‍'));       // ZERO WIDTH JOINER
	EXPECT_TRUE(unicode::ExtendsPrevious(U'️'));       // VARIATION SELECTOR-16
	EXPECT_TRUE(unicode::ExtendsPrevious(U'\U0001F3FD'));   // EMOJI MODIFIER FITZPATRICK TYPE-4
	EXPECT_TRUE(unicode::ExtendsPrevious(U'\U000E0067'));   // TAG LATIN SMALL LETTER G
	EXPECT_TRUE(unicode::ExtendsPrevious(U'\U000E0100'));   // VARIATION SELECTOR-17
	EXPECT_FALSE(unicode::ExtendsPrevious(U'a'));
	EXPECT_FALSE(unicode::ExtendsPrevious(U'‌'));      // ZERO WIDTH NON-JOINER
	EXPECT_FALSE(unicode::ExtendsPrevious(U'\U0001F1E6')); // a regional indicator pairs, it does not extend

	EXPECT_TRUE(unicode::IsUpper(U'Ⓐ'));    // CIRCLED LATIN CAPITAL LETTER A: Other_Uppercase, category So
	EXPECT_TRUE(unicode::IsLower(U'ª'));    // FEMININE ORDINAL INDICATOR: Other_Lowercase, category Lo
	EXPECT_FALSE(unicode::IsUpper(U'ǅ'));   // titlecase is neither
	EXPECT_FALSE(unicode::IsLower(U'ǅ'));
	EXPECT_TRUE(unicode::IsAlpha(U'ї'));
	EXPECT_TRUE(unicode::IsAlpha(U'漢'));
	EXPECT_FALSE(unicode::IsAlpha(U'١'));

	EXPECT_TRUE(unicode::IsPunctuation(U'!'));
	EXPECT_TRUE(unicode::IsPunctuation(U'_'));
	EXPECT_TRUE(unicode::IsPunctuation(U'«'));   // LEFT-POINTING DOUBLE ANGLE QUOTATION MARK (Pi)
	EXPECT_FALSE(unicode::IsPunctuation(U'$'));       // Sc
	EXPECT_FALSE(unicode::IsPunctuation(U'+'));       // Sm

	EXPECT_TRUE(unicode::IsControl(U'\u0085'));
	EXPECT_FALSE(unicode::IsControl(U' '));
	EXPECT_TRUE(unicode::IsPrintable(U' '));
	EXPECT_TRUE(unicode::IsPrintable(U' '));
	EXPECT_TRUE(unicode::IsPrintable(U'漢'));
	EXPECT_FALSE(unicode::IsPrintable(U'​'));    // Cf
	EXPECT_FALSE(unicode::IsPrintable(U' '));    // Zl
	EXPECT_FALSE(unicode::IsPrintable(U''));    // Co
	EXPECT_FALSE(unicode::IsPrintable(U'͸'));    // Cn
	EXPECT_FALSE(unicode::IsPrintable(U'\x7F'));
}

TEST(UnicodePropertiesTest, TableFreeAnswersAgreeWithTheCategories)
{
	for (char32_t c = 0; c <= unicode::MaxCodePoint; ++c)
	{
		const EGeneralCategory category = unicode::GetGeneralCategory(c);
		ASSERT_EQ(unicode::IsControl(c), category == EGeneralCategory::Control) << static_cast<uint32>(c);
		ASSERT_EQ(unicode::IsDigit(c), category == EGeneralCategory::DecimalNumber) << static_cast<uint32>(c);
		ASSERT_EQ(unicode::IsSurrogate(c), category == EGeneralCategory::Surrogate) << static_cast<uint32>(c);
		if (unicode::IsNoncharacter(c))
		{
			ASSERT_EQ(category, EGeneralCategory::Unassigned) << static_cast<uint32>(c);
		}
	}
}

TEST(UnicodePropertiesTest, ExactlyTwoNonAsciiCodePointsFoldToAscii)
{
	// What lets IgnoreCase comparison and search take byte shortcuts when the ASCII operand has no s S k K.
	std::vector<char32_t> foldsToAscii;
	for (char32_t c = 0x80u; c <= unicode::MaxCodePoint; ++c)
	{
		if (unicode::FoldCase(c) < 0x80u)
		{
			foldsToAscii.push_back(c);
		}
	}
	EXPECT_EQ(foldsToAscii, (std::vector<char32_t>{ U'ſ', U'K' }));
}

TEST(UnicodePropertiesTest, CodePointPredicates)
{
	EXPECT_TRUE(unicode::IsSurrogate(0xD800u));
	EXPECT_TRUE(unicode::IsSurrogate(0xDFFFu));
	EXPECT_FALSE(unicode::IsSurrogate(0xD7FFu));
	EXPECT_FALSE(unicode::IsSurrogate(0xE000u));
	EXPECT_TRUE(unicode::IsHighSurrogate(0xDBFFu));
	EXPECT_FALSE(unicode::IsHighSurrogate(0xDC00u));
	EXPECT_TRUE(unicode::IsLowSurrogate(0xDC00u));
	EXPECT_FALSE(unicode::IsLowSurrogate(0xDBFFu));

	EXPECT_TRUE(unicode::IsScalarValue(0u));
	EXPECT_TRUE(unicode::IsScalarValue(0xD7FFu));
	EXPECT_FALSE(unicode::IsScalarValue(0xD800u));
	EXPECT_TRUE(unicode::IsScalarValue(0x10FFFFu));
	EXPECT_FALSE(unicode::IsScalarValue(0x110000u));

	uint32 noncharacters = 0;
	for (char32_t c = 0; c <= 0x110001u; ++c)
	{
		noncharacters += unicode::IsNoncharacter(c) ? 1u : 0u;
	}
	EXPECT_EQ(noncharacters, 66u);   // 32 in FDD0..FDEF + 2 in each of the 17 planes
	EXPECT_TRUE(unicode::IsNoncharacter(0xFFFEu));
	EXPECT_TRUE(unicode::IsNoncharacter(0x10FFFFu));
	EXPECT_FALSE(unicode::IsNoncharacter(0xFFFDu));

	EXPECT_TRUE(unicode::IsRegionalIndicator(0x1F1E6u));
	EXPECT_TRUE(unicode::IsRegionalIndicator(0x1F1FFu));
	EXPECT_FALSE(unicode::IsRegionalIndicator(0x1F1E5u));
	EXPECT_FALSE(unicode::IsRegionalIndicator(0x1F200u));

	for (uint32 value = 0; value < 256u; ++value)
	{
		EXPECT_EQ(unicode::IsContinuationByte(static_cast<char>(value)), value >= 0x80u && value <= 0xBFu) << value;
	}
}


// ---- Codecs ----

static_assert(unicode::DecodeUtf8("A", 1).CodePoint == U'A');
static_assert(unicode::DecodeUtf8("A", 1).Length == 1 && unicode::DecodeUtf8("A", 1).bWellFormed);
static_assert(unicode::DecodeUtf8("\xD1\x97", 2).CodePoint == U'ї');
static_assert(unicode::DecodeUtf8("\xF0\x9F\x98\x80", 4).CodePoint == U'\U0001F600');
static_assert(!unicode::DecodeUtf8("\x80", 1).bWellFormed);
static_assert(unicode::DecodeWtf8("\xED\xA0\x80", 3).CodePoint == 0xD800u);
static_assert([]
{
	// Both pointers into one array: pointers into two equal literals need not compare in a constant expression.
	constexpr const char* Text = "ab";
	return unicode::DecodeUtf8Backward(Text, Text + 2).CodePoint == U'b';
}());
static_assert(unicode::Utf8Length(U'ї') == 2 && unicode::Utf8Length(0xD800u) == 3 && unicode::Utf8Length(0x110000u) == 3);
static_assert([]
{
	char bytes[4] = {};
	return unicode::EncodeUtf8(U'z', bytes) == 1u && bytes[0] == 'z';
}());
static_assert([]
{
	char bytes[4] = {};
	return unicode::EncodeUtf8(U'\U0001F600', bytes) == 4u && bytes[0] == '\xF0' && bytes[3] == '\x80';
}());
static_assert([]
{
	wchar_t units[2] = {};
	return unicode::EncodeUtf16(U'\U0001F600', units) == 2u && units[0] == 0xD83D && units[1] == 0xDE00;
}());

TEST(UnicodeCodecTest, Utf8RoundTripsEveryScalarValue)
{
	uint32 failures = 0;
	for (char32_t c = 0; c <= unicode::MaxCodePoint; ++c)
	{
		if (unicode::IsSurrogate(c))
		{
			continue;
		}

		char bytes[4] = {};
		const uint32 length = unicode::EncodeUtf8(c, bytes);
		const SDecodedCodePoint forward = unicode::DecodeUtf8(bytes, length);
		const SDecodedCodePoint backward = unicode::DecodeUtf8Backward(bytes, bytes + length);
		const SDecodedCodePoint wtf8 = unicode::DecodeWtf8(bytes, length);
		char wtf8Bytes[4] = {};
		const uint32 wtf8Length = unicode::EncodeWtf8(c, wtf8Bytes);

		const bool bOk = length == unicode::Utf8Length(c)
			&& forward.CodePoint == c && forward.Length == length && forward.bWellFormed
			&& backward.CodePoint == c && backward.Length == length && backward.bWellFormed
			&& wtf8.CodePoint == c && wtf8.Length == length && wtf8.bWellFormed
			&& wtf8Length == length && std::string_view(bytes, length) == std::string_view(wtf8Bytes, wtf8Length);
		if (!bOk && failures++ < 10u)
		{
			ADD_FAILURE() << "U+" << std::hex << static_cast<uint32>(c);
		}
	}
	EXPECT_EQ(failures, 0u);
}

TEST(UnicodeCodecTest, Utf16RoundTripsEveryScalarValue)
{
	uint32 failures = 0;
	for (char32_t c = 0; c <= unicode::MaxCodePoint; ++c)
	{
		if (unicode::IsSurrogate(c))
		{
			continue;
		}

		wchar_t units[2] = {};
		const uint32 length = unicode::EncodeUtf16(c, units);
		const SDecodedCodePoint decoded = unicode::DecodeUtf16(units, length);
		const bool bOk = length == (c > 0xFFFFu ? 2u : 1u) && decoded.CodePoint == c && decoded.Length == length
			&& decoded.bWellFormed;
		if (!bOk && failures++ < 10u)
		{
			ADD_FAILURE() << "U+" << std::hex << static_cast<uint32>(c);
		}
	}
	EXPECT_EQ(failures, 0u);
}

TEST(UnicodeCodecTest, SurrogatesAreWtf8Only)
{
	for (char32_t c = 0xD800u; c <= 0xDFFFu; ++c)
	{
		char bytes[4] = {};
		ASSERT_EQ(unicode::EncodeWtf8(c, bytes), 3u);
		ASSERT_EQ(static_cast<uint8>(bytes[0]), 0xEDu);
		ASSERT_GE(static_cast<uint8>(bytes[1]), 0xA0u);

		const SDecodedCodePoint wtf8 = unicode::DecodeWtf8(bytes, 3);
		ASSERT_EQ(wtf8.CodePoint, c);
		ASSERT_EQ(wtf8.Length, 3u);
		ASSERT_TRUE(wtf8.bWellFormed);

		// Strict UTF-8: ED cannot be followed by A0..BF, so the lead alone is the maximal subpart.
		const SDecodedCodePoint utf8 = unicode::DecodeUtf8(bytes, 3);
		ASSERT_EQ(utf8.CodePoint, unicode::ReplacementCharacter);
		ASSERT_EQ(utf8.Length, 1u);
		ASSERT_FALSE(utf8.bWellFormed);

		char replaced[4] = {};
		ASSERT_EQ(unicode::EncodeUtf8(c, replaced), 3u);
		ASSERT_EQ(std::string_view(replaced, 3), "\xEF\xBF\xBD");

		wchar_t unit[2] = {};
		ASSERT_EQ(unicode::EncodeUtf16(c, unit), 1u);
		ASSERT_EQ(static_cast<char32_t>(unit[0]), c);
		const SDecodedCodePoint lone = unicode::DecodeUtf16(unit, 1);
		ASSERT_EQ(lone.CodePoint, c);
		ASSERT_EQ(lone.Length, 1u);
		ASSERT_FALSE(lone.bWellFormed);
	}

	char bytes[4] = {};
	EXPECT_EQ(unicode::EncodeWtf8(0x110000u, bytes), 3u);
	EXPECT_EQ(std::string_view(bytes, 3), "\xEF\xBF\xBD");
	wchar_t units[2] = {};
	EXPECT_EQ(unicode::EncodeUtf16(0x110000u, units), 1u);
	EXPECT_EQ(units[0], L'\xFFFD');
}

TEST(UnicodeCodecTest, MaximalSubparts)
{
	// U+FFFD count of a lossy decode: one per maximal subpart (Unicode Standard, Table 3-8 practice).
	auto replacements = [] (std::string_view Bytes, bool bWtf8)
	{
		const std::u32string decoded = bWtf8 ? DecodeAll<true>(Bytes) : DecodeAll<false>(Bytes);
		return static_cast<uint32>(std::count(decoded.begin(), decoded.end(), unicode::ReplacementCharacter));
	};

	EXPECT_EQ(replacements("\x80", false), 1u);                 // stray continuation byte
	EXPECT_EQ(replacements("\xE2\x82", false), 1u);             // truncated, one subpart
	EXPECT_EQ(replacements("\xC0\xAF", false), 2u);             // C0 is never a lead
	EXPECT_EQ(replacements("\xED\xA0\x80", false), 3u);         // a surrogate form is not UTF-8
	EXPECT_EQ(replacements("\xF4\x90\x80\x80", false), 4u);     // above 10FFFF
	EXPECT_EQ(replacements("\xF0\x80\x80", false), 3u);         // overlong
	EXPECT_EQ(replacements("\xFF", false), 1u);

	// WTF-8: the same, except that the surrogate forms (and their truncations) are one unit.
	EXPECT_EQ(replacements("\x80", true), 1u);
	EXPECT_EQ(replacements("\xE2\x82", true), 1u);
	EXPECT_EQ(replacements("\xC0\xAF", true), 2u);
	EXPECT_EQ(replacements("\xED\xA0\x80", true), 0u);
	EXPECT_EQ(DecodeAll<true>("\xED\xA0\x80"), std::u32string(1, char32_t(0xD800u)));
	EXPECT_EQ(replacements("\xED\xA0", true), 1u);
	EXPECT_EQ(replacements("\xF4\x90\x80\x80", true), 4u);
	EXPECT_EQ(replacements("\xF0\x80\x80", true), 3u);
	EXPECT_EQ(replacements("\xFF", true), 1u);

	// The Unicode Standard's worked example: 61 F1 80 80 E1 80 C2 62 80 63 80 BF 64.
	EXPECT_EQ(DecodeAll<false>("a\xF1\x80\x80\xE1\x80\xC2" "b\x80" "c\x80\xBF" "d"),
		std::u32string(U"a���b�c��d"));

	const SDecodedCodePoint truncated = unicode::DecodeUtf8("\xF0\x9F\x98", 3);
	EXPECT_EQ(truncated.CodePoint, unicode::ReplacementCharacter);
	EXPECT_EQ(truncated.Length, 3u);
	EXPECT_FALSE(truncated.bWellFormed);

	// Available limits the read even when more bytes follow in memory.
	const SDecodedCodePoint limited = unicode::DecodeUtf8("\xD1\x97", 1);
	EXPECT_EQ(limited.Length, 1u);
	EXPECT_FALSE(limited.bWellFormed);
}

TEST(UnicodeCodecTest, CanonicalWtf8RejectsSplitPairs)
{
	// U+1F600 as two 3-byte surrogate forms instead of one 4-byte sequence.
	const std::string_view split = "\xED\xA0\xBD\xED\xB8\x80";
	const SDecodedCodePoint lead = unicode::DecodeWtf8(split.data(), static_cast<uint32>(split.size()));
	EXPECT_EQ(lead.CodePoint, unicode::ReplacementCharacter);
	EXPECT_EQ(lead.Length, 3u);
	EXPECT_FALSE(lead.bWellFormed);

	// The trail after it decodes on its own.
	const SDecodedCodePoint trail = unicode::DecodeWtf8(split.data() + 3, 3);
	EXPECT_EQ(trail.CodePoint, char32_t(0xDE00u));
	EXPECT_TRUE(trail.bWellFormed);

	EXPECT_FALSE(unicode::IsValidWtf8(split.data(), static_cast<uint32>(split.size())));
	EXPECT_FALSE(unicode::IsValidUtf8(split.data(), static_cast<uint32>(split.size())));

	// Lone halves that do not form a pair are canonical: trail then lead, or separated by anything.
	for (const std::string_view canonical : { std::string_view("\xED\xA0\x80"), std::string_view("\xED\xB8\x80\xED\xA0\xBD"),
		std::string_view("\xED\xA0\xBD" "a" "\xED\xB8\x80") })
	{
		EXPECT_TRUE(unicode::IsValidWtf8(canonical.data(), static_cast<uint32>(canonical.size())));
		EXPECT_FALSE(unicode::IsValidUtf8(canonical.data(), static_cast<uint32>(canonical.size())));
	}

	// A lead followed by a truncated trail is a lone lead plus an ill-formed tail, not a split pair.
	const std::string_view truncatedTrail = "\xED\xA0\xBD\xED\xB8";
	const SDecodedCodePoint loneLead = unicode::DecodeWtf8(truncatedTrail.data(), 5);
	EXPECT_EQ(loneLead.CodePoint, char32_t(0xD83Du));
	EXPECT_TRUE(loneLead.bWellFormed);
	EXPECT_FALSE(unicode::IsValidWtf8(truncatedTrail.data(), 5));
	EXPECT_EQ(ToUtf16(truncatedTrail), std::wstring(L"\xD83D\xFFFD"));
}

TEST(UnicodeCodecTest, BackwardDecodingAgreesWithForwardSegmentation)
{
	// Stray continuation runs longer than any sequence: every byte is its own unit.
	{
		const std::string_view bytes = "a\x80\x80\x80\x80\x80";
		const char* end = bytes.data() + bytes.size();
		for (uint32 i = 0; i < 5u; ++i)
		{
			const SDecodedCodePoint unit = unicode::DecodeUtf8Backward(bytes.data(), end);
			EXPECT_EQ(unit.Length, 1u);
			EXPECT_FALSE(unit.bWellFormed);
			end -= unit.Length;
		}
		EXPECT_EQ(unicode::DecodeUtf8Backward(bytes.data(), end).CodePoint, U'a');
	}

	// A well-formed sequence followed by a stray continuation byte.
	{
		const std::string_view bytes = "\xE2\x82\xAC\x80";
		const SDecodedCodePoint stray = unicode::DecodeUtf8Backward(bytes.data(), bytes.data() + 4);
		EXPECT_EQ(stray.Length, 1u);
		EXPECT_FALSE(stray.bWellFormed);
		const SDecodedCodePoint euro = unicode::DecodeUtf8Backward(bytes.data(), bytes.data() + 3);
		EXPECT_EQ(euro.CodePoint, U'€');
		EXPECT_EQ(euro.Length, 3u);
	}

	// A truncated sequence is one unit backwards too.
	{
		const std::string_view bytes = "a\xE2\x82";
		const SDecodedCodePoint truncated = unicode::DecodeUtf8Backward(bytes.data(), bytes.data() + 3);
		EXPECT_EQ(truncated.Length, 2u);
		EXPECT_FALSE(truncated.bWellFormed);
	}

	// Random byte strings: walking back from the end visits exactly the forward units.
	std::mt19937 random(20261008u);
	for (uint32 iteration = 0; iteration < 10000u; ++iteration)
	{
		const std::string bytes = MakeRandomBytes(random);
		const std::vector<SUnit> forward = Segment<false>(bytes);

		const char* end = bytes.data() + bytes.size();
		for (auto unit = forward.rbegin(); unit != forward.rend(); ++unit)
		{
			const SDecodedCodePoint backward = unicode::DecodeUtf8Backward(bytes.data(), end);
			ASSERT_EQ(backward.Length, unit->Decoded.Length) << iteration;
			ASSERT_EQ(backward.CodePoint, unit->Decoded.CodePoint) << iteration;
			ASSERT_EQ(backward.bWellFormed, unit->Decoded.bWellFormed) << iteration;
			end -= backward.Length;
			ASSERT_EQ(static_cast<uint32>(end - bytes.data()), unit->Start) << iteration;
		}
	}
}


// ---- Bulk ----

TEST(UnicodeBulkTest, AgreesWithTheSingleCodePointDecoders)
{
	std::mt19937 random(7u);
	for (uint32 iteration = 0; iteration < 10000u; ++iteration)
	{
		const std::string bytes = MakeRandomBytes(random);
		const uint32 len = static_cast<uint32>(bytes.size());
		const std::vector<SUnit> utf8 = Segment<false>(bytes);
		const std::vector<SUnit> wtf8 = Segment<true>(bytes);

		uint32 firstInvalid = len;
		for (const SUnit& unit : utf8)
		{
			if (!unit.Decoded.bWellFormed)
			{
				firstInvalid = unit.Start;
				break;
			}
		}
		const bool bValidWtf8 = std::all_of(wtf8.begin(), wtf8.end(), [] (const SUnit& Unit) { return Unit.Decoded.bWellFormed; });
		const bool bAscii = std::all_of(bytes.begin(), bytes.end(), [] (char C) { return ascii::IsAscii(C); });

		ASSERT_EQ(unicode::CountCodePoints(bytes.data(), len), static_cast<uint32>(utf8.size())) << iteration;
		ASSERT_EQ(unicode::FindInvalidUtf8(bytes.data(), len), firstInvalid) << iteration;
		ASSERT_EQ(unicode::IsValidUtf8(bytes.data(), len), firstInvalid == len) << iteration;
		ASSERT_EQ(unicode::IsValidWtf8(bytes.data(), len), bValidWtf8) << iteration;
		ASSERT_EQ(unicode::IsAscii(bytes.data(), len), bAscii) << iteration;

		// WTF-8 -> UTF-16 is the WTF-8 units, U+FFFD for the ill-formed ones.
		std::wstring expected;
		for (const SUnit& unit : wtf8)
		{
			wchar_t units[2] = {};
			const uint32 count = unicode::EncodeUtf16(unit.Decoded.bWellFormed ? unit.Decoded.CodePoint : unicode::ReplacementCharacter, units);
			expected.append(units, count);
		}
		ASSERT_EQ(ToUtf16(bytes), expected) << iteration;
	}
}

TEST(UnicodeBulkTest, EmptyAndAsciiRuns)
{
	EXPECT_TRUE(unicode::IsValidUtf8(nullptr, 0));
	EXPECT_TRUE(unicode::IsValidWtf8(nullptr, 0));
	EXPECT_TRUE(unicode::IsAscii(nullptr, 0));
	EXPECT_EQ(unicode::CountCodePoints(nullptr, 0), 0u);
	EXPECT_EQ(unicode::FindInvalidUtf8(nullptr, 0), 0u);
	EXPECT_EQ(unicode::Utf16LengthOfWtf8(nullptr, 0), 0u);
	EXPECT_EQ(unicode::Wtf8LengthOfUtf16(nullptr, 0), 0u);

	// The 8-byte fast path, with the non-ASCII byte at every offset of a word and past it.
	for (uint32 position = 0; position < 40u; ++position)
	{
		std::string bytes(40, 'x');
		bytes[position] = '\xFF';
		EXPECT_FALSE(unicode::IsAscii(bytes.data(), 40)) << position;
		EXPECT_EQ(unicode::FindInvalidUtf8(bytes.data(), 40), position);
		EXPECT_EQ(unicode::CountCodePoints(bytes.data(), 40), 40u);

		bytes.replace(position, 1, "\xD1\x97");
		EXPECT_TRUE(unicode::IsValidUtf8(bytes.data(), 41)) << position;
		EXPECT_EQ(unicode::CountCodePoints(bytes.data(), 41), 40u);
		EXPECT_EQ(unicode::Utf16LengthOfWtf8(bytes.data(), 41), 40u);
	}
}

TEST(UnicodeBulkTest, TranscodingByHand)
{
	EXPECT_EQ(ToUtf16("a\xED\xA0\x80" "b"), std::wstring(L"a\xD800" L"b"));
	EXPECT_EQ(ToUtf16("\xF0\x9F\x98\x80"), std::wstring(L"\xD83D\xDE00"));
	EXPECT_EQ(ToUtf16("\xED\xA0\xBD\xED\xB8\x80"), std::wstring(L"\xFFFD\xDE00"));
	EXPECT_EQ(ToUtf16("\xED\xA0"), std::wstring(L"\xFFFD"));
	EXPECT_EQ(ToUtf16("\xC0\xAF"), std::wstring(L"\xFFFD\xFFFD"));
	EXPECT_EQ(ToUtf16("\xD0\x9F\xD1\x80\xD0\xB8"), std::wstring(L"\x041F\x0440\x0438"));

	EXPECT_EQ(ToWtf8(L"a\xD800" L"b"), std::string("a\xED\xA0\x80" "b"));
	EXPECT_EQ(ToWtf8(L"\xD83D\xDE00"), std::string("\xF0\x9F\x98\x80"));
	EXPECT_EQ(ToWtf8(L"\xDE00\xD83D"), std::string("\xED\xB8\x80\xED\xA0\xBD"));
	EXPECT_EQ(ToWtf8(L"\xD83D"), std::string("\xED\xA0\xBD"));
	EXPECT_EQ(ToWtf8(L"\x041F\x0440\x0438 \x6F22"), std::string("\xD0\x9F\xD1\x80\xD0\xB8 \xE6\xBC\xA2"));
}

TEST(UnicodeBulkTest, Utf16ToWtf8IsLossless)
{
	std::mt19937 random(1903u);
	for (uint32 iteration = 0; iteration < 10000u; ++iteration)
	{
		const std::wstring utf16 = MakeRandomUtf16(random);
		const std::string wtf8 = ToWtf8(utf16);

		// Canonical by construction: a pair is never written as two surrogate forms.
		ASSERT_TRUE(unicode::IsValidWtf8(wtf8.data(), static_cast<uint32>(wtf8.size()))) << iteration;
		ASSERT_EQ(ToUtf16(wtf8), utf16) << iteration;
	}
}
