#include <gtest/gtest.h>

#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <system_error>

#include "Containers/String.h"
#include "Containers/WideString.h"
#include "Unicode/Unicode.h"

#include <Windows.h>

using namespace frt;


namespace
{
constexpr FStringLiteral Uk = "Привіт, світе! Ґанок, їжак, є";
constexpr FStringLiteral Cjk = "漢字テスト";
constexpr FStringLiteral Emoji = "😀👍🏽";

FString MakeMixed ()
{
	return FString("a") + Uk + Cjk + Emoji;
}

uint64 GetAllocations ()
{
	return string_detail::GetThreadHeapAllocationCount();
}

// Ill-formed units the strict decoder reports, left to right: one U+FFFD each.
uint32 CountReplacements (FStringView Bytes)
{
	uint32 count = 0;
	for (uint32 at = 0; at < Bytes.Len();)
	{
		const unicode::SDecodedCodePoint decoded = unicode::DecodeUtf8(Bytes.GetData() + at, Bytes.Len() - at);
		count += decoded.bWellFormed ? 0u : 1u;
		at += decoded.Length;
	}
	return count;
}

// Whether the text lives in the object's own inline buffer (FWideString re-points Data on copy and move).
bool PointsIntoItself (const FWideString& Text)
{
	const uintptr_t begin = reinterpret_cast<uintptr_t>(&Text);
	const uintptr_t data = reinterpret_cast<uintptr_t>(Text.CStr());
	return data >= begin && data < begin + sizeof(FWideString);
}

/**
 * A directory under the system temp directory whose name is not ASCII (every test in it exercises the
 * path conversions), removed afterwards.
 * Its parent is this process's own, like PlatformFileTest's: test runs of several configurations may
 * share the temp directory, and with a shared parent one run deleted - or removed the parent of -
 * what another had just created. The parent goes once it is empty.
 */
class CScratchDirectory
{
public:
	explicit CScratchDirectory (std::wstring_view Name)
		: Root(std::filesystem::temp_directory_path() / FString::Format("FRT String Тест 漢字 😀 {}", GetCurrentProcessId()).ToPath())
		, Path(Root / std::filesystem::path(Name))
	{
		std::error_code error;
		std::filesystem::remove_all(Path, error);
		std::filesystem::create_directories(Path, error);
	}

	~CScratchDirectory ()
	{
		std::error_code error;
		std::filesystem::remove_all(Path, error);
		std::filesystem::remove(Root, error);
	}

	CScratchDirectory (const CScratchDirectory&) = delete;
	CScratchDirectory& operator= (const CScratchDirectory&) = delete;

	const std::filesystem::path& Get () const
	{
		return Path;
	}

private:
	std::filesystem::path Root;
	std::filesystem::path Path;
};
}


// ---------------------------------------------------------------------------------
// UTF-16 <-> WTF-8 (FromWide / ToWide)
// ---------------------------------------------------------------------------------

TEST(StringConversion, LoneSurrogatesRoundTripAsWtf8)
{
	const std::wstring_view wide(L"a\xD800" L"b");
	const FString text = FString::FromWide(wide);
	EXPECT_EQ(text, "a\xED\xA0\x80" "b");
	EXPECT_FALSE(text.IsValidUtf8());
	EXPECT_TRUE(text.IsValidWtf8());
	EXPECT_EQ(ToWide(text).View(), wide);
	EXPECT_EQ(FWideString(text.View()).View(), wide);

	const FString trail = FString::FromWide(std::wstring_view(L"\xDC00"));
	EXPECT_EQ(trail, "\xED\xB0\x80");
	EXPECT_EQ(ToWide(trail).View(), std::wstring_view(L"\xDC00"));

	const FString pair = FString::FromWide(L"\xD83D\xDE00");
	EXPECT_EQ(pair, "\xF0\x9F\x98\x80");
	EXPECT_EQ(pair, "😀");
	EXPECT_TRUE(pair.IsValidUtf8());
}

TEST(StringConversion, FromWideOverloads)
{
	const wchar_t* nullWide = nullptr;
	EXPECT_TRUE(FString::FromWide(nullWide).IsEmpty());
	EXPECT_EQ(FString::FromWide(L"Тест"), "Тест");
	EXPECT_EQ(FString::FromWide(L"Тест", 2), "Те");
	EXPECT_EQ(FString::FromWide(L"a\0b", 3).Len(), 3u);
	EXPECT_EQ(FString::FromWide(std::wstring_view(L"漢字")), "漢字");

	const std::wstring mixedWide = L"a" L"Привіт, світе! Ґанок, їжак, є" L"漢字テスト" L"😀👍🏽";
	EXPECT_EQ(FString::FromWide(mixedWide), MakeMixed());

	FString appended = "x";
	appended.AppendWide(L"ї").AppendWide(std::wstring_view());
	EXPECT_EQ(appended, "xї");
}

TEST(StringConversion, Wtf8IsCanonical)
{
	// A pair spelled as two 3-byte surrogates is not canonical WTF-8.
	const FStringView splitPair = "\xED\xA0\xBD\xED\xB8\x80";
	EXPECT_FALSE(splitPair.IsValidWtf8());
	EXPECT_FALSE(splitPair.IsValidUtf8());
	EXPECT_FALSE(splitPair.TryToPath().has_value());

	const unicode::SDecodedCodePoint first = unicode::DecodeWtf8(splitPair.GetData(), splitPair.Len());
	EXPECT_EQ(first.CodePoint, unicode::ReplacementCharacter);
	EXPECT_EQ(first.Length, 3u);
	EXPECT_FALSE(first.bWellFormed);

	// UTF-16 appended to a trailing lead surrogate is merged into one 4-byte sequence.
	FString merged = FString::FromWide(L"\xD83D");
	merged.AppendWide(L"\xDE00");
	EXPECT_EQ(merged, "😀");
	EXPECT_TRUE(merged.IsValidUtf8());

	FString mergedPath = FString::FromWide(L"\xD83D");
	mergedPath.AppendPath(std::filesystem::path(L"\xDE00"));
	EXPECT_EQ(mergedPath, "😀");

	// Append of UTF-8 text stays a byte concatenation (path keys come only from FromPath).
	FString concatenated = FString::FromWide(L"\xD83D");
	concatenated.Append(FString::FromWide(L"\xDE00"));
	EXPECT_EQ(concatenated, splitPair);

	const FString mixed = MakeMixed();
	for (const FStringView canonical : { mixed.View(), FStringView("a\xED\xA0\x80" "b"), FStringView("x\xED\xB0\x80"),
		FStringView("C:/Тест/файл.txt"), FStringView() })
	{
		EXPECT_TRUE(canonical.IsValidWtf8());
		EXPECT_EQ(FString::FromPath(canonical.ToPath()), canonical);
		ASSERT_TRUE(canonical.TryToPath().has_value());
		EXPECT_EQ(*canonical.TryToPath(), canonical.ToPath());
	}
}

TEST(StringConversion, IllFormedSequencesGiveOneReplacementPerMaximalSubpart)
{
	struct SCase
	{
		FStringView Bytes;
		uint32 Replacements;          // strict UTF-8 (DecodeUtf8, FromUtf8Lossy)
		std::wstring_view Utf16;      // ToWide decodes WTF-8: surrogate forms are kept
	};
	const SCase cases[] =
	{
		{ "\x80", 1u, L"\xFFFD" },
		{ "\xE2\x82", 1u, L"\xFFFD" },
		{ "\xC0\xAF", 2u, L"\xFFFD\xFFFD" },
		{ "\xED\xA0\x80", 3u, L"\xD800" },
		{ "\xF4\x90\x80\x80", 4u, L"\xFFFD\xFFFD\xFFFD\xFFFD" },
		{ "\xF0\x80\x80", 3u, L"\xFFFD\xFFFD\xFFFD" },
		{ "\xFF", 1u, L"\xFFFD" },
		{ "\xED\xA0", 2u, L"\xFFFD" },
	};

	for (const SCase& item : cases)
	{
		const std::string printable = ::testing::PrintToString(item.Bytes);
		EXPECT_EQ(CountReplacements(item.Bytes), item.Replacements) << printable;
		EXPECT_EQ(item.Bytes.CountCodePoints(), item.Replacements) << printable;
		EXPECT_EQ(FString::FromUtf8Lossy(item.Bytes), FString::Repeat("\xEF\xBF\xBD", item.Replacements)) << printable;
		EXPECT_EQ(ToWide(item.Bytes).View(), item.Utf16) << printable;
		EXPECT_FALSE(item.Bytes.IsValidUtf8()) << printable;
	}

	const unicode::SDecodedCodePoint truncated = unicode::DecodeUtf8("\xE2\x82", 2);
	EXPECT_EQ(truncated.CodePoint, unicode::ReplacementCharacter);
	EXPECT_EQ(truncated.Length, 2u);
	EXPECT_FALSE(truncated.bWellFormed);

	const unicode::SDecodedCodePoint truncatedSurrogate = unicode::DecodeWtf8("\xED\xA0", 2);
	EXPECT_EQ(truncatedSurrogate.Length, 2u);
	EXPECT_FALSE(truncatedSurrogate.bWellFormed);

	// Valid text passes through unchanged.
	const FString mixed = MakeMixed();
	EXPECT_EQ(FString::FromUtf8Lossy(mixed), mixed);
	EXPECT_EQ(FString::FromUtf8Lossy("a\xFF" "ї"), "a\uFFFDї");
}


// ---------------------------------------------------------------------------------
// FWideString (Containers/WideString.h)
// ---------------------------------------------------------------------------------

TEST(WideString, EmptyIsAValidCString)
{
	const FWideString empty;
	ASSERT_NE(empty.CStr(), nullptr);
	EXPECT_STREQ(empty.CStr(), L"");
	EXPECT_EQ(empty.Len(), 0u);
	EXPECT_TRUE(empty.IsEmpty());
	EXPECT_TRUE(empty.View().empty());
	EXPECT_TRUE(empty.ToUtf8().IsEmpty());

	EXPECT_TRUE(FWideString(FStringView()).IsEmpty());
	EXPECT_STREQ(ToWide(FStringView()).CStr(), L"");
}

TEST(WideString, InlineCapacityIs127Units)
{
	const std::wstring units127(127, L'x');
	uint64 before = GetAllocations();
	const FWideString inlineWide{ std::wstring_view(units127) };
	EXPECT_EQ(GetAllocations(), before);
	EXPECT_EQ(inlineWide.Len(), 127u);
	EXPECT_EQ(inlineWide.CStr()[127], L'\0');
	EXPECT_TRUE(PointsIntoItself(inlineWide));

	const std::wstring units128(128, L'x');
	before = GetAllocations();
	const FWideString heapWide{ std::wstring_view(units128) };
	EXPECT_EQ(GetAllocations(), before + 1u);
	EXPECT_EQ(heapWide.View(), units128);
	EXPECT_FALSE(PointsIntoItself(heapWide));

	// From UTF-8 the count is in UTF-16 units: 128 two-byte letters are 256 bytes but 128 units.
	const std::string ascii127(127, 'y');
	before = GetAllocations();
	const FWideString fromAscii{ FStringView(ascii127) };
	EXPECT_EQ(GetAllocations(), before);
	EXPECT_EQ(fromAscii.Len(), 127u);

	const FString letters128 = FString::Repeat("ї", 128);
	const FWideString fromLetters(letters128.View());
	EXPECT_EQ(fromLetters.Len(), 128u);
	EXPECT_FALSE(PointsIntoItself(fromLetters));
}

TEST(WideString, CopyAndMoveRepointInlineData)
{
	auto source = std::make_unique<FWideString>(FStringView("Тест"));
	FWideString moved(std::move(*source));
	source.reset();
	EXPECT_EQ(moved.View(), L"Тест");
	EXPECT_TRUE(PointsIntoItself(moved));

	const FWideString copy(moved);
	EXPECT_EQ(copy.View(), L"Тест");
	EXPECT_TRUE(PointsIntoItself(copy));

	FWideString assigned;
	assigned = copy;
	EXPECT_EQ(assigned.View(), L"Тест");
	EXPECT_TRUE(PointsIntoItself(assigned));

	FWideString moveAssigned;
	moveAssigned = std::move(moved);
	EXPECT_EQ(moveAssigned.View(), L"Тест");
	EXPECT_TRUE(PointsIntoItself(moveAssigned));

	// A heap block is stolen, not copied.
	const std::wstring longText(200, L'ж');
	FWideString heapSource{ std::wstring_view(longText) };
	const wchar_t* block = heapSource.CStr();
	const uint64 before = GetAllocations();
	const FWideString heapMoved(std::move(heapSource));
	EXPECT_EQ(GetAllocations(), before);
	EXPECT_EQ(heapMoved.CStr(), block);
	EXPECT_EQ(heapMoved.View(), longText);
}

TEST(WideString, ConvertsBothWays)
{
	const FString mixed = MakeMixed();
	const FWideString wide = ToWide(mixed);
	EXPECT_EQ(wide.View(), std::wstring_view(L"a" L"Привіт, світе! Ґанок, їжак, є" L"漢字テスト" L"😀👍🏽"));
	EXPECT_EQ(wide.Len(), 41u);
	EXPECT_EQ(wide.ToUtf8(), mixed);
	EXPECT_EQ(FWideString(mixed.View()).View(), wide.View());

	FWideString writable{ FStringView("abc") };
	writable.GetData()[0] = L'X';
	EXPECT_EQ(writable.View(), L"Xbc");
	EXPECT_EQ(writable.ToUtf8(), "Xbc");

	const FWideString lone{ std::wstring_view(L"a\xDC00") };
	EXPECT_EQ(lone.ToUtf8(), "a\xED\xB0\x80");
}


// ---------------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------------

TEST(StringPath, FromPathStyles)
{
	const std::filesystem::path path(L"C:\\Тест\\文件.txt");
	EXPECT_EQ(FString::FromPath(path), "C:\\Тест\\文件.txt");
	EXPECT_TRUE(FString::FromPath(path).IsValidUtf8());

	const std::filesystem::path mixedSeparators(L"C:/a\\b/c.txt");
	EXPECT_EQ(FString::FromPath(mixedSeparators, EPathStyle::Native), "C:/a\\b/c.txt");
	EXPECT_EQ(FString::FromPath(mixedSeparators, EPathStyle::Preferred), "C:\\a\\b\\c.txt");
	EXPECT_EQ(FString::FromPath(mixedSeparators, EPathStyle::Generic), "C:/a/b/c.txt");
	EXPECT_TRUE(FString::FromPath(std::filesystem::path()).IsEmpty());

	FString appended = "root: ";
	appended.AppendPath(path, EPathStyle::Generic);
	EXPECT_EQ(appended, "root: C:/Тест/文件.txt");
}

TEST(StringPath, ToPathRoundTrips)
{
	const FString mixed = MakeMixed();
	const std::filesystem::path path = mixed.ToPath();
	EXPECT_EQ(path.native(), ToWide(mixed).View());
	EXPECT_EQ(FString::FromPath(path), mixed);
	EXPECT_EQ(mixed.TryToPath(), path);

	// Separators are left alone.
	EXPECT_EQ(FStringView("a/b\\c").ToPath().native(), L"a/b\\c");

	// Latin-1 bytes are not UTF-8: no path from them, and the lossy conversion shows U+FFFD.
	const FStringView latin1 = "C:\\Users\\J\xF6rg";
	EXPECT_FALSE(latin1.TryToPath().has_value());
	const std::wstring native = latin1.ToPath().native();
	EXPECT_NE(native.find(L'\xFFFD'), std::wstring::npos);
	EXPECT_EQ(native, L"C:\\Users\\J\xFFFD" L"rg");
}

TEST(StringPath, LoneSurrogateFileNamesRoundTrip)
{
	const CScratchDirectory scratch(L"LoneSurrogate");
	ASSERT_TRUE(std::filesystem::is_directory(scratch.Get()));

	const std::filesystem::path fileName(L"x\xD800" L"y.txt");
	{
		std::ofstream file(scratch.Get() / fileName);
		ASSERT_TRUE(file.good());
		file << "content";
	}

	bool bFound = false;
	for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(scratch.Get()))
	{
		const FString name = FString::FromPath(entry.path().filename());
		if (name != "x\xED\xA0\x80" "y.txt")
		{
			continue;
		}

		bFound = true;
		EXPECT_TRUE(name.IsValidWtf8());
		EXPECT_FALSE(name.IsValidUtf8());

		const FString full = FString::FromPath(entry.path());
		const std::filesystem::path back = full.ToPath();
		EXPECT_EQ(back, entry.path());
		EXPECT_TRUE(std::filesystem::exists(back));
		EXPECT_EQ(full.TryToPath(), entry.path());
	}
	EXPECT_TRUE(bFound);
}

TEST(StringPath, ProcessCodePageIsUtf8)
{
	// The UTF-8 process code page: narrow text reaching path(const char*) in third-party code decodes as UTF-8.
	EXPECT_EQ(std::filesystem::path(reinterpret_cast<const char*>(u8"Тест")), std::filesystem::path(L"Тест"))
		<< "Is Core/Resources/Utf8CodePage.manifest embedded in Core-Test?";
}


// ---------------------------------------------------------------------------------
// Numbers (shortest round trip, fixed notation, integer limits)
// ---------------------------------------------------------------------------------

TEST(StringNumbers, FloatingPointIsShortestRoundTrip)
{
	EXPECT_EQ(FString::FromNumber(0.1f), "0.1");
	EXPECT_EQ(FString::FromNumber(0.1), "0.1");
	EXPECT_EQ(FString::FromNumber(1e-7), "1e-07");
	EXPECT_EQ(FString::FromNumber(1.5f), "1.5");
	EXPECT_EQ(FString::FromNumber(100.0), "100");
	EXPECT_EQ(FString::FromNumber(-0.0), "-0");
	EXPECT_EQ(FString::FromNumber((std::numeric_limits<double>::infinity)()), "inf");
	EXPECT_EQ(FString::FromNumber(-(std::numeric_limits<float>::infinity)()), "-inf");
	EXPECT_EQ(FString::FromNumber((std::numeric_limits<double>::quiet_NaN)()), "nan");

	for (const float value : { (std::numeric_limits<float>::min)(), (std::numeric_limits<float>::max)(),
		(std::numeric_limits<float>::lowest)(), (std::numeric_limits<float>::denorm_min)(),
		(std::numeric_limits<float>::epsilon)(), 1.0f / 3.0f, -0.0f, 16777217.0f })
	{
		const FString text = FString::FromNumber(value);
		const std::optional<float> parsed = text.ToFloat();
		ASSERT_TRUE(parsed.has_value()) << text;
		EXPECT_EQ(std::bit_cast<uint32>(*parsed), std::bit_cast<uint32>(value)) << text;
	}

	for (const double value : { (std::numeric_limits<double>::min)(), (std::numeric_limits<double>::max)(),
		(std::numeric_limits<double>::lowest)(), (std::numeric_limits<double>::denorm_min)(),
		(std::numeric_limits<double>::epsilon)(), 1.0 / 3.0, -0.0, 0.1 + 0.2 })
	{
		const FString text = FString::FromNumber(value);
		const std::optional<double> parsed = text.ToDouble();
		ASSERT_TRUE(parsed.has_value()) << text;
		EXPECT_EQ(std::bit_cast<uint64>(*parsed), std::bit_cast<uint64>(value)) << text;
	}
}

TEST(StringNumbers, RandomBitPatternsRoundTrip)
{
	std::mt19937_64 random(20261008u);
	for (uint32 i = 0; i < 2000u; ++i)
	{
		const uint64 bits = random();

		const double doubleValue = std::bit_cast<double>(bits);
		if (std::isfinite(doubleValue))
		{
			const FString text = FString::FromNumber(doubleValue);
			const std::optional<double> parsed = text.ToDouble();
			ASSERT_TRUE(parsed.has_value()) << text;
			ASSERT_EQ(std::bit_cast<uint64>(*parsed), bits) << text;
		}

		const float floatValue = std::bit_cast<float>(static_cast<uint32>(bits >> 32));
		if (std::isfinite(floatValue))
		{
			const FString text = FString::FromNumber(floatValue);
			const std::optional<float> parsed = text.ToFloat();
			ASSERT_TRUE(parsed.has_value()) << text;
			ASSERT_EQ(std::bit_cast<uint32>(*parsed), std::bit_cast<uint32>(floatValue)) << text;
		}
	}
}

TEST(StringNumbers, SanitizeFloatIsFixedNotation)
{
	EXPECT_EQ(FString::SanitizeFloat(1.0), "1.0");
	EXPECT_EQ(FString::SanitizeFloat(1e-7), "0.0000001");
	EXPECT_EQ(FString::SanitizeFloat(-0.0), "0.0");
	EXPECT_EQ(FString::SanitizeFloat(2.5, 3), "2.500");
	EXPECT_EQ(FString::SanitizeFloat(0.1f), "0.1");
	EXPECT_EQ(FString::SanitizeFloat(-1.5), "-1.5");
	EXPECT_EQ(FString::SanitizeFloat(123.456), "123.456");
	EXPECT_EQ(FString::SanitizeFloat(1e21), "1000000000000000000000.0");
	EXPECT_EQ(FString::SanitizeFloat((std::numeric_limits<double>::quiet_NaN)()), "nan");
	EXPECT_EQ(FString::SanitizeFloat((std::numeric_limits<float>::infinity)()), "inf");
	EXPECT_EQ(FString::SanitizeFloat(-(std::numeric_limits<double>::infinity)()), "-inf");
}

TEST(StringNumbers, IntegerLimitsAndBooleans)
{
	const uint64 before = GetAllocations();
	const FString minimum = FString::FromNumber((std::numeric_limits<int64>::min)());
	const FString maximum = FString::FromNumber((std::numeric_limits<uint64>::max)());
	// Twenty digits still fit the inline buffer.
	EXPECT_EQ(GetAllocations(), before);

	EXPECT_EQ(minimum, "-9223372036854775808");
	EXPECT_EQ(maximum, "18446744073709551615");
	EXPECT_EQ(minimum.ToInt64(), (std::numeric_limits<int64>::min)());
	EXPECT_EQ(maximum.ToUInt64(), (std::numeric_limits<uint64>::max)());

	EXPECT_EQ(FString::FromNumber(static_cast<int8>(-5)), "-5");
	EXPECT_EQ(FString::FromNumber(static_cast<uint8>(255)), "255");
	EXPECT_EQ(FString::FromNumber(static_cast<int16>(-32768)), "-32768");
	EXPECT_EQ(FString::FromNumber(0), "0");
	EXPECT_EQ(FString::FromNumber(4294967295u), "4294967295");

	EXPECT_EQ(FString::FromBool(true), "true");
	EXPECT_EQ(FString::FromBool(false), "false");
	EXPECT_EQ(FString::FromBool(true).TryParseBool(), true);
}
