#include <gtest/gtest.h>

#include <cstdio>
#include <iterator>

#include "Unicode/Unicode.h"

#include <Windows.h>

using namespace frt;

using unicode::EGeneralCategory;


/**
 * An independent check of the generator's UCD parsing (UnicodeTest.cpp's checksum only proves that
 * the C++ lookup reproduces whatever the generator emitted): Windows' own ICU (icu.dll, Windows 10
 * 1903+) is asked the same questions for every code point both sides have assigned - case mappings
 * and folding, White_Space, the general category, and the Alphabetic, Uppercase and Lowercase flags.
 *
 * Its Unicode version is older than ours. Unicode's stability policies keep the answers comparable
 * anyway: case pairs and case folding never change once both characters exist. What a newer version
 * can add is a mapping whose target is new (U+019B -> U+A7DC in Unicode 16), so a difference is
 * skipped only when one side's result is a character the other version does not have yet. Such
 * skips are few and bounded; everything else, a result no version assigns included, is a mismatch.
 */
namespace
{
// ICU's C API as icu.dll exports it: unversioned names, UChar32 = int32, UBool one byte,
// UVersionInfo = uint8[4].
using CharTypeFunction = int8 (*) (int32);
using CaseFunction = int32 (*) (int32);
using FoldFunction = int32 (*) (int32, uint32);
using PropertyFunction = int8 (*) (int32);
using BinaryPropertyFunction = int8 (*) (int32, int32);
using VersionFunction = void (*) (uint8*);

constexpr int8 IcuUnassigned = 0;          // U_UNASSIGNED (UCharCategory)
constexpr uint32 IcuFoldCaseDefault = 0u;  // U_FOLD_CASE_DEFAULT
// UProperty values (uchar.h).
constexpr int32 IcuAlphabetic = 0;         // UCHAR_ALPHABETIC
constexpr int32 IcuLowercase = 22;         // UCHAR_LOWERCASE
constexpr int32 IcuUppercase = 30;         // UCHAR_UPPERCASE

// UCharCategory, in ICU's order (uchar.h), as EGeneralCategory.
constexpr EGeneralCategory IcuCategories[] = {
	EGeneralCategory::Unassigned, EGeneralCategory::UppercaseLetter, EGeneralCategory::LowercaseLetter,
	EGeneralCategory::TitlecaseLetter, EGeneralCategory::ModifierLetter, EGeneralCategory::OtherLetter,
	EGeneralCategory::NonspacingMark, EGeneralCategory::EnclosingMark, EGeneralCategory::SpacingMark,
	EGeneralCategory::DecimalNumber, EGeneralCategory::LetterNumber, EGeneralCategory::OtherNumber,
	EGeneralCategory::SpaceSeparator, EGeneralCategory::LineSeparator, EGeneralCategory::ParagraphSeparator,
	EGeneralCategory::Control, EGeneralCategory::Format, EGeneralCategory::PrivateUse, EGeneralCategory::Surrogate,
	EGeneralCategory::DashPunctuation, EGeneralCategory::OpenPunctuation, EGeneralCategory::ClosePunctuation,
	EGeneralCategory::ConnectorPunctuation, EGeneralCategory::OtherPunctuation, EGeneralCategory::MathSymbol,
	EGeneralCategory::CurrencySymbol, EGeneralCategory::ModifierSymbol, EGeneralCategory::OtherSymbol,
	EGeneralCategory::InitialPunctuation, EGeneralCategory::FinalPunctuation,
};
static_assert(std::size(IcuCategories) == static_cast<size_t>(EGeneralCategory::Count));

enum class EProperty : uint8
{
	Category,
	Alphabetic,
	Uppercase,
	Lowercase,
};

// What Unicode itself changed, after the Unicode 15.1 of the icu.dll these were found with, for
// characters both versions have: a newer icu.dll agrees with the tables on them, an older one does
// not. Short by design - every other difference is a parsing bug - and growing only with a Unicode
// version bump that changes a property on purpose (UAX #44's change history says which).
struct SKnownChange
{
	EProperty Property;
	char32_t First;
	char32_t Last;
};

constexpr SKnownChange KnownChanges[] = {
	{ EProperty::Category, 0x0295, 0x0295 },     // Ll -> Lo,
	{ EProperty::Lowercase, 0x0295, 0x0295 },    // and so no longer Lowercase
	{ EProperty::Category, 0x1171E, 0x1171E },   // Mn -> Mc
	{ EProperty::Alphabetic, 0x0363, 0x036F },   // combining Latin small letters: Other_Alphabetic
	{ EProperty::Alphabetic, 0x1DD3, 0x1DE6 },
};

constexpr uint32 CountKnownChanges ()
{
	uint32 count = 0;
	for (const SKnownChange& change : KnownChanges)
	{
		count += static_cast<uint32>(change.Last - change.First) + 1u;
	}
	return count;
}
// A long list would hide real differences: narrow an entry rather than widening the bound.
static_assert(CountKnownChanges() <= 48u);

bool IsKnownChange (EProperty Property, char32_t C)
{
	for (const SKnownChange& change : KnownChanges)
	{
		if (change.Property == Property && C >= change.First && C <= change.Last)
		{
			return true;
		}
	}
	return false;
}

struct SIcu
{
	HMODULE Module = nullptr;
	CharTypeFunction CharType = nullptr;
	CaseFunction ToUpper = nullptr;
	CaseFunction ToLower = nullptr;
	FoldFunction FoldCase = nullptr;
	PropertyFunction IsWhiteSpace = nullptr;
	BinaryPropertyFunction HasBinaryProperty = nullptr;
	VersionFunction GetUnicodeVersion = nullptr;
	VersionFunction GetVersion = nullptr;   // optional: only reported

	SIcu () = default;
	SIcu (const SIcu&) = delete;
	SIcu& operator= (const SIcu&) = delete;

	~SIcu ()
	{
		if (Module != nullptr)
		{
			FreeLibrary(Module);
		}
	}

	// System32 only: a stray icu.dll next to the test must not become the oracle.
	bool Load ()
	{
		Module = LoadLibraryExW(L"icu.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
		if (Module == nullptr)
		{
			return false;
		}

		CharType = reinterpret_cast<CharTypeFunction>(GetProcAddress(Module, "u_charType"));
		ToUpper = reinterpret_cast<CaseFunction>(GetProcAddress(Module, "u_toupper"));
		ToLower = reinterpret_cast<CaseFunction>(GetProcAddress(Module, "u_tolower"));
		FoldCase = reinterpret_cast<FoldFunction>(GetProcAddress(Module, "u_foldCase"));
		IsWhiteSpace = reinterpret_cast<PropertyFunction>(GetProcAddress(Module, "u_isUWhiteSpace"));
		HasBinaryProperty = reinterpret_cast<BinaryPropertyFunction>(GetProcAddress(Module, "u_hasBinaryProperty"));
		GetUnicodeVersion = reinterpret_cast<VersionFunction>(GetProcAddress(Module, "u_getUnicodeVersion"));
		GetVersion = reinterpret_cast<VersionFunction>(GetProcAddress(Module, "u_getVersion"));

		return CharType != nullptr && ToUpper != nullptr && ToLower != nullptr && FoldCase != nullptr
			&& IsWhiteSpace != nullptr && HasBinaryProperty != nullptr && GetUnicodeVersion != nullptr;
	}

	bool IsAssigned (char32_t C) const
	{
		return CharType(static_cast<int32>(C)) != IcuUnassigned;
	}
};

bool IsAssignedInOurs (char32_t C)
{
	return unicode::GetGeneralCategory(C) != EGeneralCategory::Unassigned;
}

// Whether two different results are explained by the versions: one of them is a character only that
// side has. A result neither side assigns is no character at all - a broken mapping, e.g. a delta
// with the wrong sign landing in a gap - and must not be explained away.
bool IsNewInOneVersion (const SIcu& Icu, char32_t Ours, char32_t Theirs)
{
	return (IsAssignedInOurs(Ours) && !Icu.IsAssigned(Ours)) || (Icu.IsAssigned(Theirs) && !IsAssignedInOurs(Theirs));
}

struct SComparison
{
	const char* Name = nullptr;
	uint32 Mismatches = 0;
	uint32 Skipped = 0;      // results new in one of the two versions

	void Compare (const SIcu& Icu, char32_t C, char32_t Ours, char32_t Theirs)
	{
		if (Ours == Theirs)
		{
			return;
		}
		if (IsNewInOneVersion(Icu, Ours, Theirs))
		{
			++Skipped;
			return;
		}

		// The first few are enough to see the pattern; the count says how bad it is.
		if (Mismatches++ < 16u)
		{
			ADD_FAILURE() << Name << "(U+" << std::hex << std::uppercase << static_cast<uint32>(C) << "): ours U+"
				<< static_cast<uint32>(Ours) << ", icu.dll U+" << static_cast<uint32>(Theirs);
		}
	}
};
}


TEST(UnicodeOracleTest, CaseMappingsAndWhiteSpaceMatchWindowsIcu)
{
	SIcu icu;
	if (!icu.Load())
	{
		GTEST_SKIP() << "icu.dll (System32, Windows 10 1903+) or one of its functions is not available";
	}

	uint8 unicodeVersion[4] = {};
	icu.GetUnicodeVersion(unicodeVersion);
	uint8 icuVersion[4] = {};
	if (icu.GetVersion != nullptr)
	{
		icu.GetVersion(icuVersion);
	}
	std::printf("[   INFO   ] icu.dll: ICU %u.%u, Unicode %u.%u.%u; tables: Unicode %s\n",
		icuVersion[0], icuVersion[1], unicodeVersion[0], unicodeVersion[1], unicodeVersion[2],
		unicode::GetUnicodeVersion());

	// The skip rule itself: U+FDD0, a noncharacter, is unassigned in every version, so a mapping into it
	// must count as a mismatch.
	constexpr char32_t Noncharacter = 0xFDD0;
	ASSERT_FALSE(icu.IsAssigned(Noncharacter) || IsAssignedInOurs(Noncharacter));
	EXPECT_FALSE(IsNewInOneVersion(icu, Noncharacter, U'A'));
	EXPECT_FALSE(IsNewInOneVersion(icu, U'A', Noncharacter));

	SComparison upper{ "ToUpper" };
	SComparison lower{ "ToLower" };
	SComparison fold{ "FoldCase" };
	uint32 whiteSpaceMismatches = 0;
	uint32 compared = 0;

	for (char32_t c = 0; c <= unicode::MaxCodePoint; ++c)
	{
		if (!icu.IsAssigned(c) || !IsAssignedInOurs(c))
		{
			continue;
		}
		++compared;

		const int32 icuC = static_cast<int32>(c);
		upper.Compare(icu, c, unicode::ToUpper(c), static_cast<char32_t>(icu.ToUpper(icuC)));
		lower.Compare(icu, c, unicode::ToLower(c), static_cast<char32_t>(icu.ToLower(icuC)));
		fold.Compare(icu, c, unicode::FoldCase(c), static_cast<char32_t>(icu.FoldCase(icuC, IcuFoldCaseDefault)));

		const bool bIcuWhiteSpace = icu.IsWhiteSpace(icuC) != 0;
		if (unicode::IsWhitespace(c) != bIcuWhiteSpace && whiteSpaceMismatches++ < 16u)
		{
			ADD_FAILURE() << "IsWhitespace(U+" << std::hex << std::uppercase << static_cast<uint32>(c) << "): ours "
				<< unicode::IsWhitespace(c) << ", icu.dll " << bIcuWhiteSpace;
		}
	}

	std::printf("[   INFO   ] %u code points compared; skipped as new in one version: upper %u, lower %u, fold %u\n",
		compared, upper.Skipped, lower.Skipped, fold.Skipped);

	// Unicode 10 already had ~137 000 assigned code points; anything far below means a broken lookup.
	EXPECT_GT(compared, 250000u);
	// A handful of old characters gained a mapping to a new one (Unicode 17 against a Unicode 15.1
	// icu.dll: 4). Many more means the skip rule is hiding real differences.
	EXPECT_LE(upper.Skipped + lower.Skipped + fold.Skipped, 16u);
	EXPECT_EQ(upper.Mismatches, 0u);
	EXPECT_EQ(lower.Mismatches, 0u);
	EXPECT_EQ(fold.Mismatches, 0u);
	EXPECT_EQ(whiteSpaceMismatches, 0u);
}

// The rest of what the generator parses (UnicodeData.txt's field 2, DerivedCoreProperties.txt): what
// IsAlpha / IsUpper / IsLower answer, Truncate's mark detection and Escape's control set rest on them.
TEST(UnicodeOracleTest, CategoriesAndDerivedPropertiesMatchWindowsIcu)
{
	SIcu icu;
	if (!icu.Load())
	{
		GTEST_SKIP() << "icu.dll (System32, Windows 10 1903+) or one of its functions is not available";
	}

	uint32 compared = 0;
	uint32 known = 0;
	uint32 mismatches = 0;
	const auto check = [&] (EProperty Property, const char* Name, char32_t C, auto Ours, auto Theirs)
	{
		if (Ours == Theirs)
		{
			return;
		}
		if (IsKnownChange(Property, C))
		{
			++known;
			return;
		}
		if (mismatches++ < 16u)
		{
			ADD_FAILURE() << Name << "(U+" << std::hex << std::uppercase << static_cast<uint32>(C) << "): ours "
				<< std::dec << static_cast<int32>(Ours) << ", icu.dll " << static_cast<int32>(Theirs);
		}
	};

	for (char32_t c = 0; c <= unicode::MaxCodePoint; ++c)
	{
		const int8 icuType = icu.CharType(static_cast<int32>(c));
		if (icuType == IcuUnassigned || !IsAssignedInOurs(c))
		{
			continue;
		}
		ASSERT_LT(static_cast<size_t>(icuType), std::size(IcuCategories));
		++compared;

		const int32 icuC = static_cast<int32>(c);
		check(EProperty::Category, "GetGeneralCategory", c, unicode::GetGeneralCategory(c), IcuCategories[icuType]);
		check(EProperty::Alphabetic, "IsAlpha", c, unicode::IsAlpha(c), icu.HasBinaryProperty(icuC, IcuAlphabetic) != 0);
		check(EProperty::Uppercase, "IsUpper", c, unicode::IsUpper(c), icu.HasBinaryProperty(icuC, IcuUppercase) != 0);
		check(EProperty::Lowercase, "IsLower", c, unicode::IsLower(c), icu.HasBinaryProperty(icuC, IcuLowercase) != 0);
	}

	std::printf("[   INFO   ] %u code points compared; known Unicode changes since icu.dll's version: %u\n", compared, known);
	EXPECT_GT(compared, 250000u);
	EXPECT_EQ(mismatches, 0u);
}
