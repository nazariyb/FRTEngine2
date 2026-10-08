#include <gtest/gtest.h>

#include "TestCommon.h"

#include <algorithm>
#include <array>
#include <compare>
#include <filesystem>
#include <functional>
#include <iterator>
#include <limits>
#include <random>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "Containers/Array.h"
#include "Containers/String.h"
#include "Containers/StringView.h"

using namespace frt;
using namespace frt::literals;


namespace
{
using IndexStrategy = math::SIndexStrategy;

// Section 9 fixture texts: 2-byte (with every Ukrainian-specific letter), 3-byte, 4-byte plus a skin-tone
// modifier, and all of them in a row (78 bytes, 38 code points).
constexpr FStringLiteral Uk = "Привіт, світе! Ґанок, їжак, є";
constexpr FStringLiteral Cjk = "漢字テスト";
constexpr FStringLiteral Emoji = "😀👍🏽";

FString MakeMixed ()
{
	return FString("a") + Uk + Cjk + Emoji;
}

// Shared by every case that checks the IgnoreCase order: sign of a three-way result.
int32 Sign (int32 Value)
{
	return (Value > 0) - (Value < 0);
}

// Fills a byte string with the lead/continuation mix the boundary rules care about: plain random bytes
// almost never form a valid multi-byte sequence, so the interesting cases would hardly be exercised.
std::string MakeRandomBytes (std::mt19937& Random)
{
	static constexpr std::array<std::pair<int32, int32>, 12> ByteRanges =
	{{
		{ 0x00, 0x7F }, { 0x80, 0x8F }, { 0x90, 0x9F }, { 0xA0, 0xBF }, { 0xC0, 0xC1 }, { 0xC2, 0xDF },
		{ 0xE0, 0xE0 }, { 0xE1, 0xEC }, { 0xED, 0xED }, { 0xEE, 0xEF }, { 0xF0, 0xF4 }, { 0xF5, 0xFF },
	}};

	std::uniform_int_distribution<int32> lengthDistribution(0, 12);
	std::uniform_int_distribution<size_t> rangeDistribution(0, ByteRanges.size() - 1);

	std::string bytes;
	const int32 length = lengthDistribution(Random);
	for (int32 i = 0; i < length; ++i)
	{
		const std::pair<int32, int32> range = ByteRanges[rangeDistribution(Random)];
		std::uniform_int_distribution<int32> byteDistribution(range.first, range.second);
		bytes.push_back(static_cast<char>(byteDistribution(Random)));
	}
	return bytes;
}

// Byte offsets CodePoints() reports, plus Len(): by 4.1 exactly the code-point boundaries.
std::vector<bool> GetIteratorBoundaries (FStringView Text)
{
	std::vector<bool> bIsBoundary(Text.Len() + 1, false);
	const FCodePointRange range = Text.CodePoints();
	for (FCodePointIterator it = range.begin(); it != range.end(); ++it)
	{
		bIsBoundary[it.GetByteIndex()] = true;
	}
	bIsBoundary[Text.Len()] = true;
	return bIsBoundary;
}

uint32 LiteralLength (FStringLiteral Name)
{
	return Name.Len();
}

// Negative compile checks go through concepts over a template parameter (section 9). A char32_t must never
// reach an ASCII-character overload as its truncated low byte.
template <typename TText, typename TValue>
concept CanStartsWith = requires (const TText& Text, TValue Value) { Text.StartsWith(Value); };

template <typename TText, typename TValue>
concept CanStartsWithIgnoreCase = requires (const TText& Text, TValue Value) { Text.template StartsWith<ESearchCase::IgnoreCase>(Value); };

template <typename TText, typename TValue>
concept CanEndsWith = requires (const TText& Text, TValue Value) { Text.EndsWith(Value); };

template <typename TText, typename TValue>
concept CanSplitFirst = requires (const TText& Text, TValue Value) { Text.SplitFirst(Value); };

template <typename TText, typename TValue>
concept CanSplitLast = requires (const TText& Text, TValue Value) { Text.SplitLast(Value); };

template <typename TText, typename TValue>
concept CanSplit = requires (const TText& Text, TValue Value) { Text.Split(Value); };

template <typename TText, typename TValue>
concept CanFind = requires (const TText& Text, TValue Value) { Text.Find(Value); };

template <typename TValue>
concept CanConsumeChar = requires (FStringView& Cursor, TValue Value) { Cursor.ConsumeChar(Value); };

template <typename TValue>
concept CanConsumeToken = requires (FStringView& Cursor, TValue Value) { Cursor.ConsumeToken(Value); };
}


// ---------------------------------------------------------------------------------
// Compile-time contracts (6.5, 6.6, 6.9)
// ---------------------------------------------------------------------------------

static_assert(sizeof(FStringView) == 16);
static_assert(sizeof(FStringLiteral) == 16);

static_assert(FStringView("abc").Len() == 3);
static_assert(FStringView("abc").Get<IndexStrategy::IS_Circular>(-1) == 'c');
static_assert(FStringView("abcdef").Left(2) == "ab");
static_assert(FStringView("abc").Find("c") == 2);
static_assert(FStringView("abc").StartsWith("ab"));
static_assert(FStringView("a,b").SplitFirst(',').Right == "b");
static_assert(FStringView("ab").IsCodePointBoundary(1));
static_assert(FStringView("ґ").FloorCodePointBoundary(1) == 0);
static_assert(FStringView("_id1").IsIdentifier());
static_assert(FStringLiteral("abc").Len() == 3);
static_assert("a\0b"_sv.Len() == 3);

static_assert(!std::is_constructible_v<FStringView, std::nullptr_t>);
static_assert(std::is_nothrow_default_constructible_v<FStringView>);

// Raw byte containers convert only explicitly: a char[64] turned std::array would otherwise become a
// 64-byte view including its trailing NULs.
static_assert(!std::is_convertible_v<std::array<char, 8>, FStringView>);
static_assert(!std::is_convertible_v<std::array<char, 8>&, FStringView>);
static_assert(std::is_constructible_v<FStringView, std::array<char, 8>&>);
static_assert(!std::is_convertible_v<std::vector<char>, FStringView>);
static_assert(!std::is_convertible_v<std::vector<char>&, FStringView>);
static_assert(std::is_constructible_v<FStringView, std::vector<char>&>);
static_assert(!std::is_convertible_v<std::span<const char>, FStringView>);
static_assert(std::is_constructible_v<FStringView, std::span<const char>>);
// Text types convert implicitly.
static_assert(std::is_convertible_v<std::string, FStringView>);
static_assert(std::is_convertible_v<const std::string&, FStringView>);
static_assert(std::is_convertible_v<std::string_view, FStringView>);
static_assert(std::is_convertible_v<FString&, FStringView>);
static_assert(std::is_convertible_v<FStringLiteral, FStringView>);
static_assert(std::is_convertible_v<const char*, FStringView>);
static_assert(std::is_convertible_v<const char8_t*, FStringView>);
static_assert(!std::is_convertible_v<const wchar_t*, FStringView>);
static_assert(!std::is_constructible_v<FStringView, std::wstring>);

// A view must not be assigned an owning temporary (it would dangle at the ';').
static_assert(!std::is_assignable_v<FStringView&, FString>);
static_assert(!std::is_assignable_v<FStringView&, std::string>);
static_assert(std::is_assignable_v<FStringView&, FString&>);
static_assert(std::is_assignable_v<FStringView&, const FString&>);
static_assert(std::is_assignable_v<FStringView&, std::string_view>);
static_assert(std::is_assignable_v<FStringView&, const char*>);
static_assert(std::is_assignable_v<FStringView&, FStringLiteral>);

// FStringLiteral is immutable by type: not a view (a base would let a view reference rebind it) and only
// constructible from constant arrays.
static_assert(!std::is_base_of_v<FStringView, FStringLiteral>);
static_assert(!std::is_convertible_v<FStringLiteral*, FStringView*>);
static_assert(!std::is_constructible_v<FStringLiteral, const char*>);
static_assert(!std::is_constructible_v<FStringLiteral, FStringView>);
static_assert(!std::is_assignable_v<FStringLiteral&, FStringView>);

static_assert(std::ranges::contiguous_range<FStringView>);
static_assert(std::ranges::borrowed_range<FStringView>);
static_assert(std::ranges::view<FStringView>);
static_assert(std::ranges::borrowed_range<FStringLiteral>);

static_assert(std::forward_iterator<FCodePointIterator>);
static_assert(std::forward_iterator<FStringSplitRange::FIterator>);
static_assert(std::sentinel_for<std::default_sentinel_t, FStringSplitRange::FIterator>);
static_assert(std::forward_iterator<FStringLineRange::FIterator>);
static_assert(std::sentinel_for<std::default_sentinel_t, FStringLineRange::FIterator>);

static_assert(FStringView::NotFound == 0xFFFFFFFFu);
static_assert(FStringView::npos == FStringView::NotFound);
static_assert(FStringView::MaxLength == 0x7FFFFFFFu);

// ASCII characters and text work; a code point only where a char32_t overload encodes it (Find, FindLast,
// Contains), never truncated to a byte ('W' for U'ї').
static_assert(CanStartsWith<FStringView, char> && CanStartsWith<FStringView, const char*>);
static_assert(!CanStartsWith<FStringView, char32_t>);
static_assert(!CanStartsWith<FString, char32_t>);
static_assert(!CanStartsWith<FStringLiteral, char32_t>);
static_assert(CanStartsWithIgnoreCase<FStringView, char> && !CanStartsWithIgnoreCase<FStringView, char32_t>);
static_assert(CanEndsWith<FStringView, char> && !CanEndsWith<FStringView, char32_t>);
static_assert(CanSplitFirst<FStringView, char> && !CanSplitFirst<FStringView, char32_t>);
static_assert(CanSplitLast<FStringView, char> && !CanSplitLast<FStringView, char32_t>);
static_assert(CanSplit<FStringView, char> && !CanSplit<FStringView, char32_t>);
static_assert(CanSplit<FString, char> && !CanSplit<FString, char32_t>);
static_assert(CanFind<FStringView, char> && CanFind<FStringView, char32_t>);
static_assert(CanConsumeChar<char> && !CanConsumeChar<char32_t>);
static_assert(CanConsumeToken<char> && !CanConsumeToken<char32_t>);


// ---------------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------------

TEST(StringView, ConstructsFromEveryTextSource)
{
	const char bytes[] = { 'a', '\0', 'b' };
	const FStringView withNul(bytes, 3);
	EXPECT_EQ(withNul.Len(), 3u);
	EXPECT_EQ(withNul[1], '\0');
	EXPECT_EQ(withNul.GetData(), bytes);

	const std::string stdString = "std::string";
	const FStringView fromStdString = stdString;
	EXPECT_EQ(fromStdString.Len(), stdString.size());
	EXPECT_EQ(fromStdString.GetData(), stdString.data());

	const std::string_view stdView = "string_view";
	const FStringView fromStdView = stdView;
	EXPECT_EQ(fromStdView, "string_view");

	const FString owned = "owned text that is longer than the inline buffer";
	const FStringView fromOwned = owned;
	EXPECT_EQ(fromOwned.GetData(), owned.GetData());
	EXPECT_EQ(fromOwned.Len(), owned.Len());
	EXPECT_EQ(owned.View(), fromOwned);

	const FStringView fromChar8 = u8"їжак";
	EXPECT_EQ(fromChar8, "їжак");
	EXPECT_EQ(fromChar8.Len(), 8u);
	const FStringView fromChar8Length(u8"їжак", 4);
	EXPECT_EQ(fromChar8Length, "їж");

	EXPECT_EQ("a\0b"_sv.Len(), 3u);
	EXPECT_EQ("a\0b"_sv, withNul);

	// A C array decays: the view stops at the first NUL, not at the array's end.
	const char array[8] = "ab";
	EXPECT_EQ(FStringView(array).Len(), 2u);

	// A null pointer value (not the literal nullptr, which is deleted) is an empty view.
	const char* nullText = nullptr;
	EXPECT_TRUE(FStringView(nullText).IsEmpty());
}

TEST(StringView, RawByteContainersConvertExplicitly)
{
	const std::array<char, 4> array = { 'a', 'b', '\0', '\0' };
	const FStringView arrayView(array);
	EXPECT_EQ(arrayView.Len(), 4u);

	const std::vector<char> vector = { 'x', 'y' };
	const FStringView vectorView(vector);
	EXPECT_EQ(vectorView, "xy");

	const std::span<const char> span(vector);
	EXPECT_EQ(FStringView(span), "xy");
}

TEST(StringView, EmptyViewsStoreNullButNeverReturnIt)
{
	// 4.4: an empty view keeps no pointer (so one made in a game module never points into its image), and
	// GetData()/data() still return a valid, empty C string.
	const char text[] = "abc";
	const FStringView pointerAndZero(text, 0);
	const FStringView atTerminator(text + 3);
	const FStringView fromEmptyLiteral("");
	const FStringView defaulted;

	for (const FStringView& empty : { pointerAndZero, atTerminator, fromEmptyLiteral, defaulted })
	{
		EXPECT_TRUE(empty.IsEmpty());
		EXPECT_TRUE(empty.empty());
		EXPECT_EQ(empty.Len(), 0u);
		ASSERT_NE(empty.GetData(), nullptr);
		ASSERT_NE(empty.data(), nullptr);
		EXPECT_EQ(empty.GetData()[0], '\0');
		EXPECT_EQ(empty.begin(), empty.end());
		EXPECT_EQ(empty, defaulted);
	}

	// Observable consequence of storing nullptr: the caller's pointer is not kept.
	EXPECT_NE(pointerAndZero.GetData(), text);
	EXPECT_NE(atTerminator.GetData(), text + 3);

	const FStringView whole = text;
	EXPECT_NE(whole.Left(0).GetData(), text);
	EXPECT_NE(whole.RightChop(3).GetData(), text + 3);
}


// ---------------------------------------------------------------------------------
// Element access, index strategies, STL surface
// ---------------------------------------------------------------------------------

TEST(StringView, IndexStrategies)
{
	const FStringView text = "abc";

	EXPECT_EQ(text[1], 'b');
	EXPECT_EQ(text.First(), 'a');
	EXPECT_EQ(text.Last(), 'c');

	EXPECT_EQ(text.Get(0), 'a');
	EXPECT_EQ(text.Get<IndexStrategy::IS_Circular>(-1), 'c');
	EXPECT_EQ(text.Get<IndexStrategy::IS_Circular>(3), 'a');
	EXPECT_EQ(text.Get<IndexStrategy::IS_CircularClamped>(-3), 'a');
	EXPECT_EQ(text.Get<IndexStrategy::IS_CircularClamped>(-1), 'c');

	EXPECT_TRUE(text.IsIndexValid(0));
	EXPECT_TRUE(text.IsIndexValid(2));
	EXPECT_FALSE(text.IsIndexValid(3));
	EXPECT_FALSE(text.IsIndexValid(-1));

	EXPECT_TRUE(text.IsIndexValid<IndexStrategy::IS_CircularClamped>(-3));
	EXPECT_TRUE(text.IsIndexValid<IndexStrategy::IS_CircularClamped>(2));
	EXPECT_FALSE(text.IsIndexValid<IndexStrategy::IS_CircularClamped>(-4));
	EXPECT_FALSE(text.IsIndexValid<IndexStrategy::IS_CircularClamped>(3));

	EXPECT_TRUE(text.IsIndexValid<IndexStrategy::IS_Circular>(100));
	EXPECT_TRUE(text.IsIndexValid<IndexStrategy::IS_Circular>(-100));
	EXPECT_FALSE(FStringView().IsIndexValid<IndexStrategy::IS_Circular>(0));
}

TEST(StringView, StlSurface)
{
	const FStringView text = "Ґанок";

	EXPECT_EQ(text.data(), text.GetData());
	EXPECT_EQ(text.size(), 10u);
	EXPECT_EQ(text.length(), 10u);
	EXPECT_FALSE(text.empty());
	EXPECT_EQ(std::string(text.begin(), text.end()), "Ґанок");
	EXPECT_EQ(text.end() - text.begin(), 10);

	uint32 byteCount = 0;
	for (const char byte : text)
	{
		byteCount += byte != '\0' ? 1u : 0u;
	}
	EXPECT_EQ(byteCount, 10u);

	EXPECT_TRUE(std::ranges::equal(text, std::string_view("Ґанок")));
	EXPECT_EQ(text.ToStdString(), std::string("Ґанок"));
	EXPECT_EQ(static_cast<std::string_view>(text), std::string_view("Ґанок"));
	EXPECT_EQ(text.View(), text);
}


// ---------------------------------------------------------------------------------
// Substrings
// ---------------------------------------------------------------------------------

TEST(StringView, SubstringsClamp)
{
	const FStringView text = "abcdef";

	EXPECT_EQ(text.Left(2), "ab");
	EXPECT_EQ(text.Left(100), "abcdef");
	EXPECT_EQ(text.Left(FStringView::NotFound), "abcdef");
	EXPECT_EQ(text.LeftChop(2), "abcd");
	EXPECT_TRUE(text.LeftChop(100).IsEmpty());
	EXPECT_EQ(text.Right(2), "ef");
	EXPECT_EQ(text.Right(100), "abcdef");
	EXPECT_EQ(text.RightChop(2), "cdef");
	EXPECT_TRUE(text.RightChop(100).IsEmpty());
	EXPECT_EQ(text.Mid(2), "cdef");
	EXPECT_EQ(text.Mid(2, 2), "cd");
	EXPECT_EQ(text.Mid(2, 100), "cdef");
	EXPECT_TRUE(text.Mid(10).IsEmpty());
	EXPECT_TRUE(text.Mid(FStringView::NotFound).IsEmpty());
	EXPECT_TRUE(text.Mid(6).IsEmpty());
}

TEST(StringView, SubstringsOnCodePointBoundaries)
{
	const FStringView text = "їжак";

	EXPECT_EQ(text.Left(4), "їж");
	EXPECT_EQ(text.LeftChop(2), "їжа");
	EXPECT_EQ(text.Right(2), "к");
	EXPECT_EQ(text.RightChop(6), "к");
	EXPECT_EQ(text.Mid(2, 4), "жа");
	EXPECT_EQ(text.Slice(-4), "ак");
}

TEST(StringView, SlicePythonStyle)
{
	const FStringView text = "abcdef";

	EXPECT_EQ(text.Slice(-3), "def");
	EXPECT_EQ(text.Slice(1, -1), "bcde");
	EXPECT_EQ(text.Slice(-100, 2), "ab");
	EXPECT_EQ(text.Slice(0, 100), "abcdef");
	EXPECT_EQ(text.Slice(2), "cdef");
	EXPECT_EQ(text.Slice(2, FStringView::SliceEnd), "cdef");
	EXPECT_TRUE(text.Slice(5, 2).IsEmpty());
	EXPECT_TRUE(text.Slice(-1, -3).IsEmpty());
	EXPECT_TRUE(text.Slice(10, 20).IsEmpty());
	EXPECT_TRUE(text.Slice(6).IsEmpty());
	EXPECT_EQ(text.Slice((std::numeric_limits<int64>::min)(), (std::numeric_limits<int64>::max)()), "abcdef");
}

TEST(StringView, InPlaceSubstrings)
{
	FStringView text = "abcdefgh";
	EXPECT_EQ(text.LeftInline(7), "abcdefg");
	EXPECT_EQ(text.LeftChopInline(1), "abcdef");
	EXPECT_EQ(text.RightChopInline(1), "bcdef");
	EXPECT_EQ(text.RightInline(4), "cdef");
	EXPECT_EQ(text.MidInline(1, 2), "de");
	EXPECT_EQ(text, "de");

	FStringView sliced = "abcdef";
	EXPECT_EQ(sliced.SliceInline(1, -1), "bcde");
	EXPECT_EQ(sliced.SliceInline(-2), "de");

	// Chains, like TArray-style mutators returning *this.
	FStringView chained = "[value]";
	chained.RemovePrefix(1).RemoveSuffix(1);
	EXPECT_EQ(chained, "value");

	FStringView clamped = "ab";
	EXPECT_TRUE(clamped.RemovePrefix(10).IsEmpty());
	clamped = "ab";
	EXPECT_TRUE(clamped.RemoveSuffix(10).IsEmpty());

	FStringView truncated = "ґґґ";
	EXPECT_EQ(truncated.TruncateInline(5), "ґґ");
}

TEST(StringView, RemoveFromStartAndEnd)
{
	FStringView text = "\xEF\xBB\xBF" "key=value;";
	EXPECT_TRUE(text.RemoveFromStart(Utf8Bom));
	EXPECT_EQ(text, "key=value;");
	EXPECT_FALSE(text.RemoveFromStart("value"));
	EXPECT_EQ(text, "key=value;");
	EXPECT_TRUE(text.RemoveFromEnd(";"));
	EXPECT_EQ(text, "key=value");
	EXPECT_FALSE(text.RemoveFromEnd("key"));

	// IgnoreCase removes the matched haystack bytes (3 for U+212A KELVIN SIGN), not the needle's length.
	FStringView kelvin = "\u212Aey";
	EXPECT_TRUE(kelvin.RemoveFromStart<ESearchCase::IgnoreCase>("k"));
	EXPECT_EQ(kelvin, "ey");

	FStringView longS = "STOPſ";
	EXPECT_TRUE(longS.RemoveFromEnd<ESearchCase::IgnoreCase>("s"));
	EXPECT_EQ(longS, "STOP");
	EXPECT_TRUE(longS.RemoveFromStart<ESearchCase::IgnoreCaseAscii>("st"));
	EXPECT_EQ(longS, "OP");
}


// ---------------------------------------------------------------------------------
// Search
// ---------------------------------------------------------------------------------

TEST(StringView, FindSemantics)
{
	const FStringView text = "abcabc";

	EXPECT_EQ(text.Find("bc"), 1u);
	EXPECT_EQ(text.Find("bc", 2), 4u);
	EXPECT_EQ(text.Find("x"), FStringView::NotFound);
	EXPECT_EQ(text.Find("a", 10), FStringView::NotFound);
	EXPECT_EQ(text.Find('c', 3), 5u);
	EXPECT_EQ(text.Find('c', 10), FStringView::NotFound);

	// Empty needle: Start while Start <= Len().
	EXPECT_EQ(text.Find(""), 0u);
	EXPECT_EQ(text.Find("", 4), 4u);
	EXPECT_EQ(text.Find("", 6), 6u);
	EXPECT_EQ(text.Find("", 7), FStringView::NotFound);

	EXPECT_EQ(FStringView().Find("a"), FStringView::NotFound);
	EXPECT_EQ(FStringView().Find(""), 0u);
}

TEST(StringView, FindLastSemantics)
{
	// UE's FindLast misses a match that starts at 0; ours does not.
	EXPECT_EQ(FStringView("X").FindLast("X"), 0u);
	EXPECT_EQ(FStringView("X").FindLast('X'), 0u);

	const FStringView text = "abcabc";
	EXPECT_EQ(text.FindLast("abc"), 3u);
	// The occurrence must lie entirely inside [0, End).
	EXPECT_EQ(text.FindLast("abc", 6), 3u);
	EXPECT_EQ(text.FindLast("abc", 5), 0u);
	EXPECT_EQ(text.FindLast("abc", 2), FStringView::NotFound);
	EXPECT_EQ(text.FindLast("abc", 100), 3u);
	EXPECT_EQ(text.FindLast('a'), 3u);
	EXPECT_EQ(text.FindLast('a', 3), 0u);
	EXPECT_EQ(text.FindLast('z'), FStringView::NotFound);

	// Empty needle: min(End, Len()).
	EXPECT_EQ(text.FindLast(""), 6u);
	EXPECT_EQ(text.FindLast("", 4), 4u);
	EXPECT_EQ(text.FindLast("", 100), 6u);
}

TEST(StringView, FindCodePoints)
{
	const FString mixed = MakeMixed();

	EXPECT_EQ(Uk.Find(U'ї'), 38u);
	EXPECT_EQ(Uk.Find(U'Ґ'), 26u);
	EXPECT_EQ(Uk.FindLast(U'є'), 48u);
	EXPECT_EQ(Uk.Find(U'ґ'), FStringView::NotFound);
	EXPECT_EQ(mixed.Find(U'😀'), 66u);
	EXPECT_EQ(mixed.FindLast(U'👍'), 70u);
	EXPECT_EQ(mixed.FindLast(U'👍', 70), FStringView::NotFound);
	EXPECT_TRUE(mixed.Contains(U'漢'));
	EXPECT_FALSE(mixed.Contains(U'中'));

	// The IgnoreCase overloads behave like the one-unit view overload.
	EXPECT_EQ(Uk.Find<ESearchCase::IgnoreCase>(U'ґ'), 26u);
	EXPECT_TRUE(Uk.Contains<ESearchCase::IgnoreCase>(U'ґ'));
	EXPECT_EQ(Uk.FindLast<ESearchCase::IgnoreCase>(U'Є'), 48u);
}

TEST(StringView, FindCharacterSets)
{
	EXPECT_EQ(FStringView("hello, world; bye").FindFirstOf(",;"), 5u);
	EXPECT_EQ(FStringView("hello, world; bye").FindLastOf(",;"), 12u);
	EXPECT_EQ(FStringView("hello, world; bye").FindLastOf(",;", 12), 5u);
	EXPECT_EQ(FStringView("hello").FindFirstOf(",;"), FStringView::NotFound);
	EXPECT_EQ(FStringView("  ab  ").FindFirstNotOf(" "), 2u);
	EXPECT_EQ(FStringView("  ab  ").FindLastNotOf(" "), 3u);
	EXPECT_EQ(FStringView("    ").FindFirstNotOf(" "), FStringView::NotFound);

	// Non-ASCII sets are decoded into code points; sets are case-sensitive, so 'Ґ' is not 'ґ'.
	EXPECT_EQ(Uk.FindFirstOf("ґї"), 38u);
	EXPECT_EQ(Uk.FindFirstOf("Ґї"), 26u);
	EXPECT_EQ(Uk.FindLastOf("Ґї"), 38u);
	EXPECT_EQ(Uk.FindFirstOf("ґї", 40), FStringView::NotFound);
	EXPECT_EQ(FStringView("abїї").FindLastNotOf("ї"), 1u);
	EXPECT_EQ(FStringView("їїab").FindFirstNotOf("ї"), 4u);

	// An ill-formed unit matches only a byte-identical ill-formed unit; U+FFFD matches only EF BF BD.
	EXPECT_EQ(FStringView("a\xFF" "b").FindFirstOf("\xFF"), 1u);
	EXPECT_EQ(FStringView("a\xFF" "b").FindFirstOf("\uFFFD"), FStringView::NotFound);
	EXPECT_EQ(FStringView("a\uFFFD").FindFirstOf("\uFFFD"), 1u);
}

TEST(StringView, FindWithPredicates)
{
	const FString mixed = MakeMixed();
	const auto isIdeograph = [](char32_t C) { return C >= 0x4E00 && C <= 0x9FFF; };

	EXPECT_EQ(mixed.FindIf(isIdeograph), 51u);
	EXPECT_EQ(mixed.FindIf(isIdeograph, 54), 54u);
	EXPECT_EQ(mixed.FindIf(isIdeograph, 57), FStringView::NotFound);
	EXPECT_EQ(mixed.FindLastIf(isIdeograph), 54u);
	EXPECT_EQ(mixed.FindLastIf(isIdeograph, 54), 51u);
	EXPECT_EQ(Uk.FindIf(isIdeograph), FStringView::NotFound);

	// An ill-formed unit reaches the predicate as U+FFFD.
	EXPECT_EQ(FStringView("a\xFF" "b").FindIf([](char32_t C) { return C == 0xFFFD; }), 1u);
}

TEST(StringView, FindAnyOfSeveralNeedles)
{
	const FStringView text = "xxabcd";
	uint32 needleIndex = 99u;

	EXPECT_EQ(text.FindAny({ "cd", "ab" }, 0, &needleIndex), 2u);
	EXPECT_EQ(needleIndex, 1u);

	// Ties go to the lower needle index.
	EXPECT_EQ(text.FindAny({ "abc", "ab" }, 0, &needleIndex), 2u);
	EXPECT_EQ(needleIndex, 0u);

	// Empty needles take no part.
	EXPECT_EQ(text.FindAny({ "", "cd" }, 0, &needleIndex), 4u);
	EXPECT_EQ(needleIndex, 1u);

	EXPECT_EQ(text.FindAny({ "ab" }, 3), FStringView::NotFound);
	EXPECT_EQ(text.FindAny({ "zz", "yy" }), FStringView::NotFound);

	const std::array<FStringView, 2> needles = { "ЖА", "b" };
	EXPECT_EQ(FStringView("ажаb").FindAny<ESearchCase::IgnoreCase>(std::span<const FStringView>(needles), 0, &needleIndex), 2u);
	EXPECT_EQ(needleIndex, 0u);
}

TEST(StringView, ContainsStartsEndsCount)
{
	const FStringView text = "abcabc";

	EXPECT_TRUE(text.Contains(""));
	EXPECT_TRUE(FStringView().Contains(""));
	EXPECT_TRUE(text.Contains("cab"));
	EXPECT_FALSE(text.Contains("cba"));
	EXPECT_TRUE(text.Contains('b'));
	EXPECT_FALSE(text.Contains('z'));

	EXPECT_TRUE(text.StartsWith(""));
	EXPECT_TRUE(text.StartsWith("abc"));
	EXPECT_FALSE(text.StartsWith("bc"));
	EXPECT_TRUE(text.StartsWith('a'));
	EXPECT_FALSE(FStringView().StartsWith('a'));
	EXPECT_TRUE(text.EndsWith(""));
	EXPECT_TRUE(text.EndsWith("bc"));
	EXPECT_TRUE(text.EndsWith('c'));
	EXPECT_FALSE(text.EndsWith('a'));
	EXPECT_FALSE(FStringView("ab").StartsWith("abc"));

	// Non-overlapping, left to right.
	EXPECT_EQ(FStringView("aaaa").CountOccurrences("aa"), 2u);
	EXPECT_EQ(FStringView("aaaaa").CountOccurrences("aa"), 2u);
	EXPECT_EQ(text.CountOccurrences(""), 0u);
	EXPECT_EQ(text.CountOccurrences("abc"), 2u);
	EXPECT_EQ(FStringView("ЇїЇ").CountOccurrences<ESearchCase::IgnoreCase>("ї"), 3u);
}

TEST(StringView, FindMatchReportsMatchedHaystackLength)
{
	const FStringView text = "a\u212Ab";

	const SStringMatch match = text.FindMatch<ESearchCase::IgnoreCase>("k");
	EXPECT_TRUE(static_cast<bool>(match));
	EXPECT_EQ(match.Index, 1u);
	EXPECT_EQ(match.Len, 3u);
	EXPECT_EQ(text.Mid(match.Index + match.Len), "b");

	const SStringMatch caseSensitive = text.FindMatch("b");
	EXPECT_EQ(caseSensitive.Index, 4u);
	EXPECT_EQ(caseSensitive.Len, 1u);

	const SStringMatch missing = text.FindMatch("z");
	EXPECT_FALSE(static_cast<bool>(missing));
	EXPECT_EQ(missing.Index, FStringView::NotFound);

	const FStringView kelvins = "k\u212Ak";
	const SStringMatch last = kelvins.FindLastMatch<ESearchCase::IgnoreCase>("K");
	EXPECT_EQ(last.Index, 4u);
	EXPECT_EQ(last.Len, 1u);
	const SStringMatch beforeEnd = kelvins.FindLastMatch<ESearchCase::IgnoreCase>("K", 4);
	EXPECT_EQ(beforeEnd.Index, 1u);
	EXPECT_EQ(beforeEnd.Len, 3u);

	const SStringSplit split = text.SplitFirst<ESearchCase::IgnoreCase>("K");
	EXPECT_TRUE(split.bFound);
	EXPECT_EQ(split.Left, "a");
	EXPECT_EQ(split.Right, "b");

	const SStringSplit splitLast = kelvins.SplitLast<ESearchCase::IgnoreCase>("K");
	EXPECT_EQ(splitLast.Left, "k\u212A");
	EXPECT_TRUE(splitLast.Right.IsEmpty());
}


// ---------------------------------------------------------------------------------
// Case-insensitive comparison and search (6.4)
// ---------------------------------------------------------------------------------

TEST(StringViewIgnoreCase, SimpleCaseFolding)
{
	EXPECT_TRUE(FStringView("ПРИВІТ").Equals<ESearchCase::IgnoreCase>("привіт"));
	EXPECT_EQ(FStringView("ПРИВІТ").Compare<ESearchCase::IgnoreCase>("привіт"), 0);
	EXPECT_TRUE(FStringView("Ґанок, Їжак, Єнот, Іволга").Equals<ESearchCase::IgnoreCase>("ґАНОК, їЖАК, єНОТ, іВОЛГА"));
	EXPECT_FALSE(FStringView("Ґ").Equals<ESearchCase::IgnoreCase>("Г"));

	// Simple folding only: ß does not become "ss".
	EXPECT_FALSE(FStringView("Straße").Equals<ESearchCase::IgnoreCase>("STRASSE"));

	// Different byte lengths can still be equal: no length shortcut.
	EXPECT_TRUE(FStringView("\u212A").Equals<ESearchCase::IgnoreCase>("K"));
	EXPECT_TRUE(FStringView("\u212A").Equals<ESearchCase::IgnoreCase>("k"));
	EXPECT_TRUE(FStringView("ſ").Equals<ESearchCase::IgnoreCase>("S"));

	// Greek: Σ, σ and final ς all fold to σ.
	EXPECT_TRUE(FStringView("Σ").Equals<ESearchCase::IgnoreCase>("σ"));
	EXPECT_TRUE(FStringView("ς").Equals<ESearchCase::IgnoreCase>("σ"));
	EXPECT_TRUE(FStringView("ς").Equals<ESearchCase::IgnoreCase>("Σ"));
	EXPECT_TRUE(FStringView("ΟΔΟΣ").Equals<ESearchCase::IgnoreCase>("οδος"));

	EXPECT_FALSE(FStringView("ПРИВІТ").Equals("привіт"));
}

TEST(StringViewIgnoreCase, SearchMatchesWholeUnits)
{
	const FString mixed = MakeMixed();

	EXPECT_EQ(mixed.Find<ESearchCase::IgnoreCase>("ҐАНОК"), 27u);
	EXPECT_EQ(mixed.Find<ESearchCase::IgnoreCase>("ЇЖАК"), 39u);
	EXPECT_EQ(mixed.Find<ESearchCase::IgnoreCase>("漢字"), 51u);
	EXPECT_EQ(mixed.FindLast<ESearchCase::IgnoreCase>("A"), 0u);
	// U+0410 CYRILLIC CAPITAL LETTER A, not the Latin one above.
	EXPECT_EQ(mixed.Find<ESearchCase::IgnoreCase>("\u0410"), 29u);
	EXPECT_EQ(mixed.FindLast<ESearchCase::IgnoreCase>("\u0410"), 43u);
	EXPECT_TRUE(mixed.Contains<ESearchCase::IgnoreCase>("СВІТЕ"));

	// The two non-ASCII code points that fold to ASCII.
	EXPECT_EQ(FStringView("ſ").Find<ESearchCase::IgnoreCase>('s'), 0u);
	EXPECT_EQ(FStringView("ſ").Find<ESearchCase::IgnoreCase>('S'), 0u);
	EXPECT_EQ(FStringView("ſ").Find<ESearchCase::IgnoreCaseAscii>('s'), FStringView::NotFound);
	EXPECT_EQ(FStringView("a\u212Ab k").Find<ESearchCase::IgnoreCase>("k", 2), 6u);

	EXPECT_TRUE(FStringView("ſtop").StartsWith<ESearchCase::IgnoreCase>("STOP"));
	EXPECT_TRUE(FStringView("STOP").StartsWith<ESearchCase::IgnoreCase>("ſt"));
	EXPECT_TRUE(FStringView("buſ").EndsWith<ESearchCase::IgnoreCase>("S"));
	EXPECT_TRUE(FStringView("Abc").StartsWith<ESearchCase::IgnoreCase>('a'));
	EXPECT_TRUE(FStringView("abC").EndsWith<ESearchCase::IgnoreCase>('c'));

	// Candidates are unit starts: the trailing byte of a well-formed "ї" is not a stray continuation.
	EXPECT_EQ(FStringView("ї").Find("\x97"), 1u);
	EXPECT_EQ(FStringView("ї").Find<ESearchCase::IgnoreCase>("\x97"), FStringView::NotFound);
	// Needles cut mid-code-point: IgnoreCase is not a superset of CaseSensitive.
	EXPECT_TRUE(FStringView("ab€").StartsWith("ab\xE2"));
	EXPECT_FALSE(FStringView("ab€").StartsWith<ESearchCase::IgnoreCase>("ab\xE2"));
}

TEST(StringViewIgnoreCase, IllFormedUnitsCompareExactly)
{
	EXPECT_TRUE(FStringView("a\x80").Equals<ESearchCase::IgnoreCase>("A\x80"));
	EXPECT_TRUE(FStringView("\xE2\x82").Equals<ESearchCase::IgnoreCase>("\xE2\x82"));
	EXPECT_FALSE(FStringView("\xFF").Equals<ESearchCase::IgnoreCase>("\xFE"));
	EXPECT_FALSE(FStringView("\xC3").Equals<ESearchCase::IgnoreCase>("\xC3\xA9"));
	EXPECT_FALSE(FStringView("\xFF").Equals<ESearchCase::IgnoreCase>("\uFFFD"));

	// Ill-formed keys sort after every code point.
	EXPECT_GT(FStringView("\xFF").Compare<ESearchCase::IgnoreCase>("a"), 0);
	EXPECT_GT(FStringView("\x80").Compare<ESearchCase::IgnoreCase>("\U0010FFFF"), 0);
	EXPECT_EQ(FStringView("a\x80" "b").Find<ESearchCase::IgnoreCase>("\x80" "b"), 1u);
}

TEST(StringViewIgnoreCase, CompareModes)
{
	EXPECT_EQ(FStringView("a").Compare("b"), -1);
	EXPECT_EQ(FStringView("b").Compare("a"), 1);
	EXPECT_EQ(FStringView("a").Compare("a"), 0);
	EXPECT_EQ(FStringView("ab").Compare("abc"), -1);
	EXPECT_EQ(FStringView("").Compare(""), 0);

	// 'a' (0x61) sorts after 'B' (0x42) by bytes, before it once folded.
	EXPECT_EQ(FStringView("a").Compare("B"), 1);
	EXPECT_EQ(FStringView("a").Compare<ESearchCase::IgnoreCase>("B"), -1);
	EXPECT_EQ(FStringView("a").Compare<ESearchCase::IgnoreCaseAscii>("B"), -1);
	EXPECT_EQ(FStringView("ab").Compare<ESearchCase::IgnoreCase>("AB"), 0);
	EXPECT_EQ(FStringView("ab").Compare<ESearchCase::IgnoreCase>("ABC"), -1);
}

TEST(StringViewIgnoreCase, OrderIsAStrictWeakOrderConsistentWithEqualsAndHash)
{
	const std::vector<FStringView> items =
	{
		"k", "K", "\xC3", "\xC3\xA9", "\u212A", "é", "É", "\xFF", "a", "",
	};

	const SStringLessIgnoreCase less;
	const SStringEqualIgnoreCase equal;
	const SStringHashIgnoreCase hash;

	for (const FStringView a : items)
	{
		EXPECT_FALSE(less(a, a)) << ::testing::PrintToString(a);

		for (const FStringView b : items)
		{
			if (less(a, b))
			{
				EXPECT_FALSE(less(b, a));
			}

			const bool bEquivalent = !less(a, b) && !less(b, a);
			EXPECT_EQ(bEquivalent, equal(a, b)) << ::testing::PrintToString(a) << " vs " << ::testing::PrintToString(b);
			EXPECT_EQ(equal(a, b), a.Equals<ESearchCase::IgnoreCase>(b));
			EXPECT_EQ(less(a, b), a.Compare<ESearchCase::IgnoreCase>(b) < 0);
			EXPECT_EQ(Sign(a.Compare<ESearchCase::IgnoreCase>(b)), -Sign(b.Compare<ESearchCase::IgnoreCase>(a)));
			if (bEquivalent)
			{
				EXPECT_EQ(hash(a), hash(b));
				EXPECT_EQ(a.GetHash<ESearchCase::IgnoreCase>(), b.GetHash<ESearchCase::IgnoreCase>());
			}

			for (const FStringView c : items)
			{
				if (less(a, b) && less(b, c))
				{
					EXPECT_TRUE(less(a, c));
				}
			}
		}
	}

	std::vector<FStringView> sorted = items;
	std::sort(sorted.begin(), sorted.end(), less);
	EXPECT_TRUE(std::is_sorted(sorted.begin(), sorted.end(), less));

	// Folded code-point order first, ill-formed bytes last.
	EXPECT_TRUE(less("", "a"));
	EXPECT_TRUE(less("a", "\u212A"));
	EXPECT_TRUE(less("K", "É"));
	EXPECT_TRUE(less("é", "\xC3"));
	EXPECT_TRUE(less("\xC3", "\xFF"));
	EXPECT_TRUE(equal("k", "\u212A"));
	EXPECT_TRUE(equal("\xC3\xA9", "É"));
}

TEST(StringViewIgnoreCase, AsciiOnlyMode)
{
	EXPECT_TRUE(FStringView("KEY").Equals<ESearchCase::IgnoreCaseAscii>("key"));
	EXPECT_FALSE(FStringView("КЛЮЧ").Equals<ESearchCase::IgnoreCaseAscii>("ключ"));
	EXPECT_FALSE(FStringView("\u212A").Equals<ESearchCase::IgnoreCaseAscii>("k"));
	EXPECT_TRUE(FStringView("КЛЮЧ").Equals<ESearchCase::IgnoreCaseAscii>("КЛЮЧ"));

	EXPECT_EQ(FStringView("a key").Find<ESearchCase::IgnoreCaseAscii>("KEY"), 2u);
	EXPECT_TRUE(FStringView("Section").StartsWith<ESearchCase::IgnoreCaseAscii>("SECT"));

	EXPECT_EQ(FStringView("ABC").GetHash<ESearchCase::IgnoreCaseAscii>(), FStringView("abc").GetHash<ESearchCase::IgnoreCaseAscii>());
}


// ---------------------------------------------------------------------------------
// Trimming
// ---------------------------------------------------------------------------------

TEST(StringView, TrimUnicodeWhitespace)
{
	EXPECT_EQ(FStringView(" \t\r\n abc \n\v\f").TrimStartAndEnd(), "abc");
	EXPECT_EQ(FStringView("  abc  ").TrimStart(), "abc  ");
	EXPECT_EQ(FStringView("  abc  ").TrimEnd(), "  abc");

	// NBSP and U+3000 are White_Space; U+200B ZERO WIDTH SPACE is not.
	EXPECT_EQ(FStringView("\u00A0abc\u3000").TrimStartAndEnd(), "abc");
	EXPECT_EQ(FStringView("\u2028\xC2\x85" "x\u205F").TrimStartAndEnd(), "x");
	EXPECT_EQ(FStringView("\u200Babc\u200B").TrimStartAndEnd(), "\u200Babc\u200B");

	EXPECT_TRUE(FStringView(" \u3000 ").TrimStartAndEnd().IsEmpty());
	EXPECT_TRUE(FStringView().TrimStartAndEnd().IsEmpty());

	// Ill-formed bytes are kept.
	EXPECT_EQ(FStringView(" \xFF ").TrimStartAndEnd(), "\xFF");
}

TEST(StringView, TrimCharacterSets)
{
	EXPECT_EQ(FStringView("xyabcyx").TrimStartAndEnd("xy"), "abc");
	EXPECT_EQ(FStringView("xyabcyx").TrimStart("xy"), "abcyx");
	EXPECT_EQ(FStringView("xyabcyx").TrimEnd("xy"), "xyabc");
	EXPECT_EQ(FStringView("ґґabcїґ").TrimStartAndEnd("ґї"), "abc");

	// The INI set: ASCII whitespace only, so a value's NBSP survives.
	EXPECT_EQ(FStringView(" \u00A0a\u00A0 ").TrimStartAndEnd(AsciiWhitespace), "\u00A0a\u00A0");
	EXPECT_EQ(AsciiWhitespace, " \t\r\n\f\v");

	EXPECT_EQ(FStringView("\xFF" "a" "\xFF").TrimStartAndEnd("\xFF"), "a");
	EXPECT_EQ(FStringView("\xFF" "a").TrimStart("\uFFFD"), "\xFF" "a");
}

TEST(StringView, TrimInPlace)
{
	FStringView text = "  abc  ";
	EXPECT_EQ(text.TrimStartInline(), "abc  ");
	EXPECT_EQ(text.TrimEndInline(), "abc");

	text = "\u3000x\u00A0";
	EXPECT_EQ(text.TrimStartAndEndInline(), "x");

	text = "--x--";
	EXPECT_EQ(text.TrimStartAndEndInline("-"), "x");
	text = "--x--";
	EXPECT_EQ(text.TrimStartInline("-"), "x--");
	EXPECT_EQ(text.TrimEndInline("-"), "x");
}

TEST(StringView, TrimQuotesRemovesOnePair)
{
	EXPECT_EQ(FStringView("\"a\"").TrimQuotes(), "a");
	EXPECT_EQ(FStringView("\"\"a\"\"").TrimQuotes(), "\"a\"");
	EXPECT_EQ(FStringView("'a'").TrimQuotes("'\""), "a");
	EXPECT_EQ(FStringView("'a'").TrimQuotes(), "'a'");
	EXPECT_EQ(FStringView("\"a'").TrimQuotes("'\""), "\"a'");
	EXPECT_EQ(FStringView("\"").TrimQuotes(), "\"");
	EXPECT_TRUE(FStringView("\"\"").TrimQuotes().IsEmpty());
	EXPECT_EQ(FStringView("\"ґ\"").TrimQuotes(), "ґ");
}


// ---------------------------------------------------------------------------------
// Truncate / CopyTo (label-safe, 6.5)
// ---------------------------------------------------------------------------------

TEST(StringView, CopyToCutsOnCodePoints)
{
	const FStringView text = "ґґґ";

	char buffer[5];
	EXPECT_EQ(text.CopyTo(buffer, 5), 4u);
	EXPECT_STREQ(buffer, "ґґ");

	char big[16];
	EXPECT_EQ(text.CopyTo(big, 16), 6u);
	EXPECT_STREQ(big, "ґґґ");

	char untouched[2] = { 'x', 'y' };
	EXPECT_EQ(text.CopyTo(untouched, 0), 0u);
	EXPECT_EQ(untouched[0], 'x');
	EXPECT_EQ(text.CopyTo(untouched, 1), 0u);
	EXPECT_EQ(untouched[0], '\0');
}

TEST(StringView, TruncateBacksOffOverVisibleSequences)
{
	EXPECT_EQ(FStringView("ab€\x80").Truncate(5), "ab€");
	EXPECT_EQ(FStringView("abc").Truncate(100), "abc");
	EXPECT_TRUE(FStringView("abc").Truncate(0).IsEmpty());

	// Base and combining marks go together.
	const FStringView marks = "e\u0301\u0301x";
	ASSERT_EQ(marks.Len(), 6u);
	EXPECT_TRUE(marks.Truncate(4).IsEmpty());
	EXPECT_EQ(marks.Truncate(5), "e\u0301\u0301");

	// Emoji modifier and variation selector extend the previous code point.
	EXPECT_EQ(FStringView("a👍🏽").Truncate(5), "a");
	EXPECT_EQ(FStringView("a❤\uFE0F").Truncate(4), "a");

	// A ZWJ sequence is kept whole or dropped whole.
	const FStringView family = "ab👨\u200D👩\u200D👧\u200D👦";
	ASSERT_EQ(family.Len(), 27u);
	for (uint32 maxBytes = 2; maxBytes < family.Len(); ++maxBytes)
	{
		EXPECT_EQ(family.Truncate(maxBytes), "ab") << "MaxBytes " << maxBytes;
	}
	EXPECT_EQ(family.Truncate(family.Len()), family);

	// Never half a flag.
	const FStringView flags = "\U0001F1FA\U0001F1E6\U0001F1FA\U0001F1E6";
	EXPECT_EQ(flags.Truncate(12), "\U0001F1FA\U0001F1E6");
	EXPECT_EQ(flags.Truncate(15), "\U0001F1FA\U0001F1E6");
	EXPECT_EQ(flags.Truncate(8), "\U0001F1FA\U0001F1E6");
	EXPECT_TRUE(flags.Truncate(4).IsEmpty());

	// A Cyrillic name over the 31-byte Comp_Name budget.
	const FStringView name = "Олександр Петренко-Ковальчук";
	EXPECT_EQ(name.Truncate(31), "Олександр Петрен");
	EXPECT_EQ(name.Truncate(30), "Олександр Петре");
	EXPECT_TRUE(name.Truncate(30).IsValidUtf8());
}


// ---------------------------------------------------------------------------------
// Code points and boundaries (4.1)
// ---------------------------------------------------------------------------------

TEST(StringViewCodePoints, BoundariesInsideMultiByteCharacters)
{
	const FStringView text = "aґ漢😀";
	ASSERT_EQ(text.Len(), 10u);

	const std::array<bool, 11> bExpected = { true, true, false, true, false, false, true, false, false, false, true };
	for (uint32 i = 0; i < bExpected.size(); ++i)
	{
		EXPECT_EQ(text.IsCodePointBoundary(i), bExpected[i]) << "index " << i;
	}
	EXPECT_FALSE(text.IsCodePointBoundary(11));

	EXPECT_EQ(text.FloorCodePointBoundary(2), 1u);
	EXPECT_EQ(text.CeilCodePointBoundary(2), 3u);
	EXPECT_EQ(text.FloorCodePointBoundary(5), 3u);
	EXPECT_EQ(text.CeilCodePointBoundary(4), 6u);
	EXPECT_EQ(text.FloorCodePointBoundary(9), 6u);
	EXPECT_EQ(text.CeilCodePointBoundary(7), 10u);
	EXPECT_EQ(text.FloorCodePointBoundary(6), 6u);
	EXPECT_EQ(text.CeilCodePointBoundary(6), 6u);
	EXPECT_EQ(text.FloorCodePointBoundary(100), 10u);
	EXPECT_EQ(text.CeilCodePointBoundary(100), 10u);
}

TEST(StringViewCodePoints, BoundariesInIllFormedText)
{
	// A truncated sequence is one unit.
	const FStringView truncated = "a\xE2\x82" "b";
	EXPECT_TRUE(truncated.IsCodePointBoundary(0));
	EXPECT_TRUE(truncated.IsCodePointBoundary(1));
	EXPECT_FALSE(truncated.IsCodePointBoundary(2));
	EXPECT_TRUE(truncated.IsCodePointBoundary(3));
	EXPECT_TRUE(truncated.IsCodePointBoundary(4));

	// Stray continuation bytes are each their own unit, however long the run.
	FString strays = "a";
	strays.AppendChars('\x80', 1000);
	EXPECT_EQ(strays.FloorCodePointBoundary(900), 900u);
	EXPECT_EQ(strays.CeilCodePointBoundary(900), 900u);
	EXPECT_EQ(strays.CountCodePoints(), 1001u);
}

TEST(StringViewCodePoints, IteratorYieldsCodePointsAndByteIndices)
{
	const FStringView text = "aґ漢😀" "\xFF";

	struct SExpected
	{
		char32_t CodePoint;
		uint32 ByteIndex;
		uint32 ByteLength;
		bool bWellFormed;
	};
	const std::array<SExpected, 5> expected =
	{{
		{ U'a', 0u, 1u, true },
		{ U'ґ', 1u, 2u, true },
		{ U'漢', 3u, 3u, true },
		{ U'😀', 6u, 4u, true },
		{ 0xFFFD, 10u, 1u, false },
	}};

	const FCodePointRange range = text.CodePoints();
	FCodePointIterator it = range.begin();
	for (const SExpected& item : expected)
	{
		ASSERT_NE(it, range.end());
		EXPECT_EQ(*it, item.CodePoint);
		EXPECT_EQ(it.GetByteIndex(), item.ByteIndex);
		EXPECT_EQ(it.GetByteLength(), item.ByteLength);
		EXPECT_EQ(it.IsWellFormed(), item.bWellFormed);
		const FCodePointIterator previous = it++;
		EXPECT_EQ(previous.GetByteIndex(), item.ByteIndex);
	}
	EXPECT_EQ(it, range.end());

	const FStringView twoLetters = "їж";
	std::vector<char32_t> codePoints;
	for (const char32_t codePoint : twoLetters.CodePoints())
	{
		codePoints.push_back(codePoint);
	}
	EXPECT_EQ(codePoints, (std::vector<char32_t>{ U'ї', U'ж' }));
}

TEST(StringViewCodePoints, CountAndValidity)
{
	const FString mixed = MakeMixed();
	EXPECT_EQ(mixed.Len(), 78u);
	EXPECT_EQ(mixed.CountCodePoints(), 38u);
	EXPECT_EQ(Uk.CountCodePoints(), 29u);
	EXPECT_EQ(FStringView().CountCodePoints(), 0u);
	EXPECT_EQ(FStringView("\xC0\xAF").CountCodePoints(), 2u);

	EXPECT_TRUE(FStringView("abc").IsAscii());
	EXPECT_TRUE(FStringView().IsAscii());
	EXPECT_FALSE(Uk.IsAscii());

	EXPECT_TRUE(mixed.IsValidUtf8());
	EXPECT_TRUE(mixed.IsValidWtf8());
	EXPECT_FALSE(FStringView("\xC0\xAF").IsValidUtf8());
	EXPECT_FALSE(FStringView("a\xED\xA0\x80").IsValidUtf8());
	EXPECT_TRUE(FStringView("a\xED\xA0\x80").IsValidWtf8());
}

TEST(StringViewCodePoints, IteratorIndicesAreExactlyTheBoundaries)
{
	const FString mixed = MakeMixed();
	const std::vector<bool> bMixedBoundaries = GetIteratorBoundaries(mixed);
	for (uint32 i = 0; i <= mixed.Len(); ++i)
	{
		ASSERT_EQ(mixed.IsCodePointBoundary(i), bMixedBoundaries[i]) << "Mixed, index " << i;
	}

	std::mt19937 random(20261008u);
	for (uint32 sample = 0; sample < 10000u; ++sample)
	{
		const std::string bytes = MakeRandomBytes(random);
		const FStringView text = bytes;
		const std::vector<bool> bIsBoundary = GetIteratorBoundaries(text);

		uint32 unitCount = 0;
		for (uint32 i = 0; i <= text.Len(); ++i)
		{
			ASSERT_EQ(text.IsCodePointBoundary(i), bIsBoundary[i]) << ::testing::PrintToString(text) << " index " << i;

			const uint32 floor = text.FloorCodePointBoundary(i);
			const uint32 ceil = text.CeilCodePointBoundary(i);
			ASSERT_TRUE(bIsBoundary[floor]);
			ASSERT_TRUE(bIsBoundary[ceil]);
			ASSERT_LE(floor, i);
			ASSERT_GE(ceil, i);
			for (uint32 j = floor + 1; j < ceil; ++j)
			{
				ASSERT_FALSE(bIsBoundary[j]) << ::testing::PrintToString(text) << " index " << i;
			}
			unitCount += (i < text.Len() && bIsBoundary[i]) ? 1u : 0u;
		}
		ASSERT_EQ(text.CountCodePoints(), unitCount) << ::testing::PrintToString(text);
	}
}

#if FRT_ASSERTIONS_ENABLED
TEST(StringViewDeathTest, SubstringInsideACodePointAsserts)
{
	const FStringView text = "ґ";
	EXPECT_DEATH((void)text.Left(1), "ASSERT FAILED");
}
#endif


// ---------------------------------------------------------------------------------
// Split, lines, cursor
// ---------------------------------------------------------------------------------

TEST(StringViewSplit, SingleCharacterDelimiter)
{
	const FStringView text = "a,b,,c";
	EXPECT_EQ(text.Split(',').Collect(), (std::vector<FStringView>{ "a", "b", "", "c" }));
	EXPECT_EQ(text.Split(',', EStringSplit::SkipEmpty).Collect(), (std::vector<FStringView>{ "a", "b", "c" }));
	EXPECT_EQ(text.Split(',').Count(), 4u);

	const FStringView spaced = " a , b ,, c ";
	EXPECT_EQ(spaced.Split(',', EStringSplit::Trim).Collect(), (std::vector<FStringView>{ "a", "b", "", "c" }));
	const FStringView blanks = " a , , b";
	EXPECT_EQ(blanks.Split(',', EStringSplit::Trim | EStringSplit::SkipEmpty).Collect(), (std::vector<FStringView>{ "a", "b" }));
	EXPECT_EQ(blanks.Split(',', EStringSplit::SkipEmpty).Collect(), (std::vector<FStringView>{ " a ", " ", " b" }));

	const FStringView empty;
	EXPECT_EQ(empty.Split(',').Collect(), (std::vector<FStringView>{ "" }));
	EXPECT_TRUE(empty.Split(',', EStringSplit::SkipEmpty).Collect().empty());
	EXPECT_EQ(empty.Split(',', EStringSplit::SkipEmpty).Count(), 0u);

	const FStringView trailing = "a,b,";
	EXPECT_EQ(trailing.Split(',').Collect(), (std::vector<FStringView>{ "a", "b", "" }));

	EXPECT_TRUE(HasFlag(EStringSplit::Trim | EStringSplit::SkipEmpty, EStringSplit::Trim));
	EXPECT_FALSE(HasFlag(EStringSplit::Trim, EStringSplit::SkipEmpty));
	EXPECT_EQ((EStringSplit::Trim | EStringSplit::SkipEmpty) & EStringSplit::SkipEmpty, EStringSplit::SkipEmpty);
}

TEST(StringViewSplit, ViewDelimitersAndSets)
{
	const FStringView text = "a::b::::c";
	EXPECT_EQ(text.Split("::").Collect(), (std::vector<FStringView>{ "a", "b", "", "c" }));
	EXPECT_EQ(text.Split("::", EStringSplit::SkipEmpty).Count(), 3u);

	const FStringView mixedSeparators = "a,b;;c";
	EXPECT_EQ(mixedSeparators.SplitAny(",;").Collect(), (std::vector<FStringView>{ "a", "b", "", "c" }));
	EXPECT_EQ(mixedSeparators.SplitAny(",;", EStringSplit::SkipEmpty).Collect(), (std::vector<FStringView>{ "a", "b", "c" }));

	const FStringView cjk = "甲，乙、丙";
	EXPECT_EQ(cjk.SplitAny("，、").Collect(), (std::vector<FStringView>{ "甲", "乙", "丙" }));
	EXPECT_EQ(cjk.Split("，").Collect(), (std::vector<FStringView>{ "甲", "乙、丙" }));
}

TEST(StringViewSplit, Whitespace)
{
	const FStringView text = " a \t b\u3000c\u00A0 ";
	EXPECT_EQ(text.SplitWhitespace().Collect(), (std::vector<FStringView>{ "a", "b", "c" }));

	const FStringView empty;
	EXPECT_EQ(empty.SplitWhitespace().Count(), 0u);
	const FStringView blanks = " \t\u3000 ";
	EXPECT_EQ(blanks.SplitWhitespace().Count(), 0u);
	const FStringView single = "abc";
	EXPECT_EQ(single.SplitWhitespace().Collect(), (std::vector<FStringView>{ "abc" }));
	// U+200B is not White_Space.
	const FStringView zeroWidth = "a\u200Bb c";
	EXPECT_EQ(zeroWidth.SplitWhitespace().Collect(), (std::vector<FStringView>{ "a\u200Bb", "c" }));
}

TEST(StringViewSplit, Lines)
{
	const FStringView text = "a\r\nb\nc\rd\n";
	EXPECT_EQ(text.Lines().Collect(), (std::vector<FStringView>{ "a", "b", "c", "d" }));
	EXPECT_EQ(text.Lines().Count(), 4u);

	const FStringView empty;
	EXPECT_EQ(empty.Lines().Count(), 0u);
	const FStringView newline = "\n";
	EXPECT_EQ(newline.Lines().Collect(), (std::vector<FStringView>{ "" }));
	const FStringView noTerminator = "a";
	EXPECT_EQ(noTerminator.Lines().Collect(), (std::vector<FStringView>{ "a" }));
	const FStringView blankLine = "a\n\nb";
	EXPECT_EQ(blankLine.Lines().Collect(), (std::vector<FStringView>{ "a", "", "b" }));
	EXPECT_EQ(blankLine.Lines(EStringSplit::SkipEmpty).Collect(), (std::vector<FStringView>{ "a", "b" }));
	const FStringView windows = "a\r\n\r\nb\r\n";
	EXPECT_EQ(windows.Lines().Collect(), (std::vector<FStringView>{ "a", "", "b" }));
	const FStringView padded = " a \n b";
	EXPECT_EQ(padded.Lines(EStringSplit::Trim).Collect(), (std::vector<FStringView>{ "a", "b" }));
}

TEST(StringViewSplit, IteratorsAndCollect)
{
	const FStringView text = "x,ї,z";
	const FStringSplitRange range = text.Split(',');

	FStringSplitRange::FIterator it = range.begin();
	ASSERT_FALSE(it == range.end());
	EXPECT_EQ(*it, "x");
	++it;
	EXPECT_EQ(*it, "ї");
	const FStringSplitRange::FIterator previous = it++;
	EXPECT_EQ(*previous, "ї");
	EXPECT_EQ(*it, "z");
	++it;
	EXPECT_TRUE(it == std::default_sentinel);

	uint32 count = 0;
	for (const FStringView token : range)
	{
		EXPECT_FALSE(token.IsEmpty());
		++count;
	}
	EXPECT_EQ(count, 3u);
	EXPECT_EQ(std::ranges::distance(range.begin(), range.end()), 3);

	const std::vector<FStringView> views = range.Collect<std::vector<FStringView>>();
	EXPECT_EQ(views.size(), 3u);
	const std::vector<FString> owned = range.Collect<std::vector<FString>>();
	ASSERT_EQ(owned.size(), 3u);
	EXPECT_EQ(owned[1], "ї");

	const FStringView lines = "1\n2";
	const FStringLineRange lineRange = lines.Lines();
	FStringLineRange::FIterator lineIt = lineRange.begin();
	EXPECT_EQ(*lineIt, "1");
	++lineIt;
	EXPECT_EQ(*lineIt, "2");
	++lineIt;
	EXPECT_TRUE(lineIt == lineRange.end());
	EXPECT_EQ(lineRange.Collect<std::vector<FString>>().size(), 2u);
}

TEST(StringViewSplit, CollectIntoTArray)
{
	FRT_TEST_MEMORY_POOL();

	const FStringView text = "alpha,бета,a token that is longer than the inline buffer";
	const TArray<FString> tokens = text.Split(',').Collect<TArray<FString>>();
	ASSERT_EQ(tokens.Count(), 3u);
	EXPECT_EQ(tokens[0], "alpha");
	EXPECT_EQ(tokens[1], "бета");
	EXPECT_EQ(tokens[2], "a token that is longer than the inline buffer");
}

TEST(StringViewSplit, SplitFirstAndLast)
{
	const FStringView text = "key=value=more";

	const SStringSplit first = text.SplitFirst('=');
	EXPECT_TRUE(first.bFound);
	EXPECT_TRUE(static_cast<bool>(first));
	EXPECT_EQ(first.Left, "key");
	EXPECT_EQ(first.Right, "value=more");

	const SStringSplit last = text.SplitLast('=');
	EXPECT_EQ(last.Left, "key=value");
	EXPECT_EQ(last.Right, "more");

	const SStringSplit missing = FStringView("abc").SplitFirst('=');
	EXPECT_FALSE(missing.bFound);
	EXPECT_FALSE(static_cast<bool>(missing));
	EXPECT_EQ(missing.Left, "abc");
	EXPECT_TRUE(missing.Right.IsEmpty());
	EXPECT_FALSE(FStringView("abc").SplitLast('=').bFound);
	EXPECT_EQ(FStringView("abc").SplitLast('=').Left, "abc");

	const FStringView scoped = "a::b::c";
	EXPECT_EQ(scoped.SplitFirst("::").Left, "a");
	EXPECT_EQ(scoped.SplitFirst("::").Right, "b::c");
	EXPECT_EQ(scoped.SplitLast("::").Left, "a::b");
	EXPECT_EQ(scoped.SplitLast("::").Right, "c");

	// The documented usage.
	const FStringView line = "Name = Ґанок";
	if (const SStringSplit keyValue = line.SplitFirst('='))
	{
		EXPECT_EQ(keyValue.Left.TrimEnd(), "Name");
		EXPECT_EQ(keyValue.Right.TrimStart(), "Ґанок");
	}
	else
	{
		ADD_FAILURE() << "SplitFirst did not find '='";
	}
}

TEST(StringViewCursor, ConsumeTokensAndCharacters)
{
	FStringView cursor = "a,b,,c";
	EXPECT_EQ(cursor.ConsumeToken(','), "a");
	EXPECT_EQ(cursor, "b,,c");
	EXPECT_EQ(cursor.ConsumeToken(','), "b");
	EXPECT_TRUE(cursor.ConsumeToken(',').IsEmpty());
	EXPECT_EQ(cursor.ConsumeToken(','), "c");
	EXPECT_TRUE(cursor.IsEmpty());

	FStringView noDelimiter = "abc";
	EXPECT_EQ(noDelimiter.ConsumeToken(','), "abc");
	EXPECT_TRUE(noDelimiter.IsEmpty());

	FStringView scoped = "Їжак::Ґанок";
	EXPECT_EQ(scoped.ConsumeToken("::"), "Їжак");
	EXPECT_EQ(scoped, "Ґанок");

	FStringView call = "(x)";
	EXPECT_TRUE(call.ConsumeChar('('));
	EXPECT_EQ(call, "x)");
	EXPECT_FALSE(call.ConsumeChar('('));
	EXPECT_EQ(call, "x)");
}

TEST(StringViewCursor, ConsumeNumbers)
{
	FStringView cursor = "1.5, -2, 3e2)";

	EXPECT_EQ(cursor.ConsumeNumber<float>(), 1.5f);
	EXPECT_TRUE(cursor.ConsumeChar(','));
	cursor.TrimStartInline();
	EXPECT_EQ(cursor.ConsumeNumber<float>(), -2.0f);
	EXPECT_TRUE(cursor.ConsumeChar(','));
	cursor.TrimStartInline();
	EXPECT_EQ(cursor.ConsumeNumber<float>(), 300.0f);
	EXPECT_TRUE(cursor.ConsumeChar(')'));
	EXPECT_TRUE(cursor.IsEmpty());

	FStringView hex = "ffz";
	EXPECT_EQ(hex.ConsumeNumber<int32>(16), 255);
	EXPECT_EQ(hex, "z");

	FStringView plus = "+5x";
	EXPECT_EQ(plus.ConsumeNumber<int32>(), 5);
	EXPECT_EQ(plus, "x");

	FStringView scientific = "+2.5e1;";
	EXPECT_EQ(scientific.ConsumeNumber<double>(), 25.0);
	EXPECT_EQ(scientific, ";");

	// Failure leaves the cursor where it was.
	FStringView notANumber = "x1";
	EXPECT_FALSE(notANumber.ConsumeNumber<int32>().has_value());
	EXPECT_EQ(notANumber, "x1");

	FStringView overflow = "200";
	EXPECT_FALSE(overflow.ConsumeNumber<int8>().has_value());
	EXPECT_EQ(overflow, "200");

	FStringView negativeUnsigned = "-1";
	EXPECT_FALSE(negativeUnsigned.ConsumeNumber<uint32>().has_value());
	EXPECT_EQ(negativeUnsigned, "-1");
}


// ---------------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------------

TEST(StringViewParse, Integers)
{
	EXPECT_EQ(FStringView("2147483647").ToInt32(), 2147483647);
	EXPECT_EQ(FStringView("-2147483648").ToInt32(), (std::numeric_limits<int32>::min)());
	EXPECT_FALSE(FStringView("2147483648").ToInt32().has_value());
	EXPECT_FALSE(FStringView("-2147483649").ToInt32().has_value());
	EXPECT_EQ(FStringView("-0").ToInt32(), 0);
	EXPECT_EQ(FStringView("+5").ToInt32(), 5);
	EXPECT_EQ(FStringView("007").ToInt32(), 7);

	// Whole text, no whitespace, no prefixes, no doubled signs.
	for (const FStringView bad : { " 5", "5 ", "", "+", "-", "5x", "0x10", "++5", "+-5", "1.0", "1e3", "١٢" })
	{
		EXPECT_FALSE(bad.ToInt32().has_value()) << ::testing::PrintToString(bad);
	}

	EXPECT_EQ(FStringView("10").ToInt32(16), 16);
	EXPECT_EQ(FStringView("Ff").ToInt32(16), 255);
	EXPECT_EQ(FStringView("-ff").ToInt32(16), -255);
	EXPECT_EQ(FStringView("101").ToInt32(2), 5);
	EXPECT_FALSE(FStringView("2").ToInt32(2).has_value());
	EXPECT_EQ(FStringView("z").ToInt32(36), 35);

	EXPECT_EQ(FStringView("+7").ToUInt32(), 7u);
	EXPECT_FALSE(FStringView("-1").ToUInt32().has_value());
	EXPECT_FALSE(FStringView("-0").ToUInt32().has_value());
	EXPECT_EQ(FStringView("4294967295").ToUInt32(), 4294967295u);
	EXPECT_FALSE(FStringView("4294967296").ToUInt32().has_value());

	EXPECT_EQ(FStringView("-9223372036854775808").ToInt64(), (std::numeric_limits<int64>::min)());
	EXPECT_FALSE(FStringView("9223372036854775808").ToInt64().has_value());
	EXPECT_EQ(FStringView("18446744073709551615").ToUInt64(), (std::numeric_limits<uint64>::max)());
	EXPECT_FALSE(FStringView("18446744073709551616").ToUInt64().has_value());

	EXPECT_EQ(FStringView("-128").TryParse<int8>(), static_cast<int8>(-128));
	EXPECT_FALSE(FStringView("128").TryParse<int8>().has_value());
	EXPECT_EQ(FStringView("255").TryParse<uint8>(), static_cast<uint8>(255));
	EXPECT_FALSE(FStringView("256").TryParse<uint8>().has_value());
	EXPECT_EQ(FStringView("65535").TryParse<uint16>(), static_cast<uint16>(65535));
	EXPECT_FALSE(FStringView("-32769").TryParse<int16>().has_value());
	EXPECT_EQ(FStringView("ff").TryParse<uint64>(16), 255u);
}

TEST(StringViewParse, FloatingPoint)
{
	EXPECT_EQ(FStringView("1e-7").ToFloat(), 1e-7f);
	EXPECT_EQ(FStringView("1.5").ToFloat(), 1.5f);
	EXPECT_EQ(FStringView("+2.5").ToFloat(), 2.5f);
	EXPECT_EQ(FStringView(".5").ToFloat(), 0.5f);
	EXPECT_EQ(FStringView("2.5").TryParse<double>(), 2.5);

	const std::optional<float> infinity = FStringView("inf").ToFloat();
	ASSERT_TRUE(infinity.has_value());
	EXPECT_TRUE(std::isinf(*infinity));
	EXPECT_GT(*infinity, 0.0f);
	const std::optional<double> negativeInfinity = FStringView("-inf").ToDouble();
	ASSERT_TRUE(negativeInfinity.has_value());
	EXPECT_TRUE(std::isinf(*negativeInfinity));
	EXPECT_LT(*negativeInfinity, 0.0);
	const std::optional<float> notANumber = FStringView("nan").ToFloat();
	ASSERT_TRUE(notANumber.has_value());
	EXPECT_TRUE(std::isnan(*notANumber));

	const std::optional<double> negativeZero = FStringView("-0").ToDouble();
	ASSERT_TRUE(negativeZero.has_value());
	EXPECT_EQ(*negativeZero, 0.0);
	EXPECT_TRUE(std::signbit(*negativeZero));

	// Overflow is an error, not infinity.
	EXPECT_FALSE(FStringView("1e39").ToFloat().has_value());
	EXPECT_FALSE(FStringView("1e309").ToDouble().has_value());

	for (const FStringView bad : { "1,5", "1.5f", " 1.5", "1.5 ", "", ".", "0x1p3", "1..5", "--1" })
	{
		EXPECT_FALSE(bad.ToFloat().has_value()) << ::testing::PrintToString(bad);
	}
}

TEST(StringViewParse, Booleans)
{
	for (const FStringView yes : { "Yes", "true", "TRUE", "1", "on", "ON" })
	{
		EXPECT_EQ(yes.TryParseBool(), true) << ::testing::PrintToString(yes);
	}
	for (const FStringView no : { "No", "false", "False", "0", "off", "OFF" })
	{
		EXPECT_EQ(no.TryParseBool(), false) << ::testing::PrintToString(no);
	}
	for (const FStringView neither : { "maybe", "", " true", "y", "2" })
	{
		EXPECT_FALSE(neither.TryParseBool().has_value()) << ::testing::PrintToString(neither);
	}

	EXPECT_TRUE(FStringView("maybe").ToBool(true));
	EXPECT_FALSE(FStringView("maybe").ToBool(false));
	EXPECT_FALSE(FStringView("off").ToBool(true));
	EXPECT_TRUE(FStringView("on").ToBool(false));
}

TEST(StringViewParse, NumericIdentifierHex)
{
	for (const FStringView numeric : { "123", "-1.5", "+0.5", "1.", ".5", "0" })
	{
		EXPECT_TRUE(numeric.IsNumeric()) << ::testing::PrintToString(numeric);
	}
	for (const FStringView notNumeric : { "", ".", "+", "-", "1.2.3", "1e5", " 1", "١", "0x1", "1 " })
	{
		EXPECT_FALSE(notNumeric.IsNumeric()) << ::testing::PrintToString(notNumeric);
	}

	EXPECT_TRUE(FStringView("_a1").IsIdentifier());
	EXPECT_TRUE(FStringView("A").IsIdentifier());
	EXPECT_TRUE(FStringView("_").IsIdentifier());
	EXPECT_FALSE(FStringView("1a").IsIdentifier());
	EXPECT_FALSE(FStringView("").IsIdentifier());
	EXPECT_FALSE(FStringView("a-b").IsIdentifier());
	EXPECT_FALSE(FStringView("їжак").IsIdentifier());

	std::array<uint8, 4> bytes = {};
	EXPECT_TRUE(FStringView("00ff10Ab").DecodeHex(bytes));
	EXPECT_EQ(bytes, (std::array<uint8, 4>{ 0x00, 0xFF, 0x10, 0xAB }));
	EXPECT_FALSE(FStringView("00ff10A").DecodeHex(bytes));
	EXPECT_FALSE(FStringView("00ff10Ag").DecodeHex(bytes));
	EXPECT_FALSE(FStringView("00ff10Ab00").DecodeHex(bytes));
	EXPECT_TRUE(FStringView().DecodeHex(std::span<uint8>()));
}

TEST(StringViewParse, EscapeRoundTripsThroughUnescape)
{
	const FString mixed = MakeMixed();
	// Named first: MSVC 14.38 rejects the consteval literal operator inside a braced list (C7595).
	constexpr FStringView withNul = "a\0b"_sv;
	const std::vector<FStringView> texts =
	{
		"", "plain", "tab\there", "quote\"back\\slash", withNul, "\x01\x1F\x7F", "\r\n",
		Uk, mixed, "\xFF\xC0\xAF", "\xE2\x82", "\u2028\u202E\xC2\x85", "a\xED\xA0\x80" "b",
	};

	for (const FStringView text : texts)
	{
		const FString escaped = FString::Escape(text);
		const std::optional<FString> unescaped = FString::Unescape(escaped);
		ASSERT_TRUE(unescaped.has_value()) << ::testing::PrintToString(escaped);
		EXPECT_EQ(*unescaped, text);
	}

	EXPECT_EQ(FString::Unescape("\\'"), FString("'"));
	EXPECT_EQ(FString::Unescape("a\\tb\\n"), FString("a\tb\n"));
	EXPECT_EQ(FString::Unescape("\\x41"), FString("A"));
	EXPECT_EQ(FString::Unescape("\\xc3\\xA9"), FString("é"));
	EXPECT_EQ(FString::Unescape("\\0")->Len(), 1u);

	for (const FStringView malformed : { "\\q", "\\x4", "\\xZZ", "\\x4g", "abc\\" })
	{
		EXPECT_FALSE(FString::Unescape(malformed).has_value()) << ::testing::PrintToString(malformed);
	}
}


// ---------------------------------------------------------------------------------
// Wildcards
// ---------------------------------------------------------------------------------

TEST(StringView, MatchesWildcard)
{
	EXPECT_TRUE(FStringView("Brick.frtmat.yml").MatchesWildcard("*.frtmat.yml"));
	EXPECT_FALSE(FStringView("Brick.frtmat.yaml").MatchesWildcard("*.frtmat.yml"));

	// '?' is one unit, however many bytes it has.
	EXPECT_TRUE(FStringView("aґbc").MatchesWildcard("?ґ*"));
	EXPECT_TRUE(FStringView("ґґ").MatchesWildcard("?ґ*"));
	EXPECT_FALSE(FStringView("abґ").MatchesWildcard("?ґ*"));
	EXPECT_TRUE(FStringView("😀").MatchesWildcard("?"));
	EXPECT_FALSE(FStringView("ґ").MatchesWildcard("??"));

	EXPECT_TRUE(FStringView("").MatchesWildcard("*"));
	EXPECT_TRUE(FStringView("").MatchesWildcard(""));
	EXPECT_FALSE(FStringView("").MatchesWildcard("?"));
	EXPECT_FALSE(FStringView("a").MatchesWildcard(""));
	EXPECT_TRUE(FStringView("aXbYc").MatchesWildcard("a*b*c"));
	EXPECT_TRUE(FStringView("abc").MatchesWildcard("a*b*c"));
	EXPECT_FALSE(FStringView("acb").MatchesWildcard("a*b*c"));

	EXPECT_TRUE(FStringView("BRICK.PNG").MatchesWildcard<ESearchCase::IgnoreCase>("*.png"));
	EXPECT_FALSE(FStringView("BRICK.PNG").MatchesWildcard("*.png"));
	EXPECT_TRUE(FStringView("ФАЙЛ.txt").MatchesWildcard<ESearchCase::IgnoreCase>("файл*"));
	EXPECT_FALSE(FStringView("ФАЙЛ.txt").MatchesWildcard<ESearchCase::IgnoreCaseAscii>("файл*"));
	EXPECT_TRUE(FStringView("\u212A.png").MatchesWildcard<ESearchCase::IgnoreCase>("k.*"));
}


// ---------------------------------------------------------------------------------
// Comparison operators and hashing
// ---------------------------------------------------------------------------------

TEST(StringView, EqualityAcrossOperandTypes)
{
	const FString owned = "abc";
	const FStringView view = "abc";
	constexpr FStringLiteral literal = "abc";
	const std::string stdString = "abc";
	const char* pointer = "abc";

	EXPECT_TRUE(owned == view);
	EXPECT_TRUE(view == owned);
	EXPECT_TRUE(owned == literal);
	EXPECT_TRUE(literal == owned);
	EXPECT_TRUE(view == literal);
	EXPECT_TRUE(literal == view);
	EXPECT_TRUE(owned == "abc");
	EXPECT_TRUE("abc" == owned);
	EXPECT_TRUE(view == "abc");
	EXPECT_TRUE("abc" == view);
	EXPECT_TRUE(literal == "abc");
	EXPECT_TRUE(owned == pointer);
	EXPECT_TRUE(pointer == view);
	EXPECT_TRUE(owned == stdString);
	EXPECT_TRUE(stdString == owned);
	EXPECT_TRUE(view == stdString);
	EXPECT_TRUE(stdString == view);

	EXPECT_TRUE(owned != "abd");
	EXPECT_TRUE(view != literal.View().Left(2));
	EXPECT_FALSE(view == "ab");

	EXPECT_TRUE(view < "abd");
	EXPECT_TRUE((owned <=> literal) == std::strong_ordering::equal);
	EXPECT_TRUE((view <=> "abd") == std::strong_ordering::less);
	EXPECT_TRUE((stdString <=> view) == std::strong_ordering::equal);

#if FRT_STRING_STD_COMPAT
	// While the views convert to std::string_view, this pair would be ambiguous without the exact-match templates.
	const std::string_view stdView = "abc";
	EXPECT_TRUE(view == stdView);
	EXPECT_TRUE(stdView == view);
	EXPECT_FALSE(stdView != view);
	EXPECT_TRUE((view <=> stdView) == std::strong_ordering::equal);
	EXPECT_TRUE((stdView <=> FStringView("abd")) == std::strong_ordering::less);
#endif
}

TEST(StringView, ByteOrderIsCodePointOrder)
{
	EXPECT_TRUE(FStringView("z") < FStringView("é"));
	EXPECT_TRUE(FStringView("é") < FStringView("😀"));
	EXPECT_TRUE(FStringView("Я") < FStringView("ґ"));
	EXPECT_TRUE(FStringView("\x7F") < FStringView("\x80"));
	EXPECT_TRUE(FStringView("ab") < FStringView("abc"));
	EXPECT_TRUE(FStringView("") < FStringView("a"));
	EXPECT_TRUE((FStringView("😀") <=> FStringView("\uFFFD")) == std::strong_ordering::greater);
}

TEST(StringView, HashIsTheSameForEveryTextType)
{
	const FString owned = "a key that is longer than the inline buffer";
	const FStringView view = owned;
	constexpr FStringLiteral literal = "a key that is longer than the inline buffer";

	EXPECT_EQ(owned.GetHash(), view.GetHash());
	EXPECT_EQ(literal.GetHash(), view.GetHash());
	EXPECT_EQ(view.GetHash(), hash::HashBytes(view.GetData(), view.Len()));

	EXPECT_EQ(std::hash<FString>{}(owned), std::hash<FStringView>{}(view));
	EXPECT_EQ(std::hash<FStringLiteral>{}(literal), std::hash<FStringView>{}(view));
	EXPECT_EQ(SStringHash{}(owned), std::hash<FStringView>{}(view));
	EXPECT_EQ(SStringHash{}("a key that is longer than the inline buffer"), SStringHash{}(literal));

	EXPECT_EQ(FStringView("ABC").GetHash<ESearchCase::IgnoreCase>(), FStringView("abc").GetHash<ESearchCase::IgnoreCase>());
	EXPECT_EQ(FStringView("ПРИВІТ").GetHash<ESearchCase::IgnoreCase>(), FStringView("привіт").GetHash<ESearchCase::IgnoreCase>());
	EXPECT_EQ(FStringView("\u212A").GetHash<ESearchCase::IgnoreCase>(), FStringView("k").GetHash<ESearchCase::IgnoreCase>());
	EXPECT_EQ(FStringView("ΟΔΟΣ").GetHash<ESearchCase::IgnoreCase>(), FStringView("οδος").GetHash<ESearchCase::IgnoreCase>());
	EXPECT_EQ(FStringView("ABC").GetHash<ESearchCase::IgnoreCaseAscii>(), FStringView("abc").GetHash<ESearchCase::IgnoreCaseAscii>());
	EXPECT_EQ(SStringHashIgnoreCase{}("Ґанок"), SStringHashIgnoreCase{}("ґАНОК"));
	EXPECT_TRUE(SStringEqualIgnoreCase{}("Ґанок", "ґАНОК"));
	EXPECT_TRUE(SStringEqual{}(owned, literal));
	EXPECT_TRUE(SStringLess{}("a", "b"));
	EXPECT_FALSE(SStringLess{}("b", "a"));
}

TEST(StringView, ConvertsToPaths)
{
	EXPECT_EQ(FStringView("Тест/a.txt").ToPath(), std::filesystem::path(L"Тест/a.txt"));
	EXPECT_EQ(FStringView("Тест/a.txt").TryToPath(), std::filesystem::path(L"Тест/a.txt"));
	EXPECT_FALSE(FStringView("\xFF").TryToPath().has_value());
}


// ---------------------------------------------------------------------------------
// FStringLiteral
// ---------------------------------------------------------------------------------

TEST(StringLiteral, IsAStaticNulTerminatedView)
{
	constexpr FStringLiteral literal = "abc";
	static_assert(literal.Len() == 3);

	EXPECT_EQ(literal.Len(), 3u);
	EXPECT_FALSE(literal.IsEmpty());
	EXPECT_EQ(literal[1], 'b');
	EXPECT_STREQ(literal.CStr(), "abc");
	EXPECT_EQ(literal.CStr()[3], '\0');
	EXPECT_EQ(literal.GetData(), literal.CStr());
	EXPECT_EQ(literal.data(), literal.CStr());
	EXPECT_EQ(literal.size(), 3u);
	EXPECT_FALSE(literal.empty());
	EXPECT_EQ(std::string(literal.begin(), literal.end()), "abc");
	EXPECT_EQ(literal.View().Left(2), "ab");

	const FStringView view = literal;
	EXPECT_EQ(view.GetData(), literal.GetData());

	// Read operations come from the shared base, with the same signatures.
	EXPECT_EQ(literal.Find('c'), 2u);
	EXPECT_TRUE(literal.EndsWith("bc"));
	EXPECT_EQ(literal.SplitFirst('b').Right, "c");
}

TEST(StringLiteral, EmptyAndEmbeddedNul)
{
	constexpr FStringLiteral empty = "";
	constexpr FStringLiteral defaulted;

	for (const FStringLiteral& literal : { empty, defaulted })
	{
		EXPECT_TRUE(literal.IsEmpty());
		EXPECT_EQ(literal.Len(), 0u);
		ASSERT_NE(literal.CStr(), nullptr);
		EXPECT_EQ(literal.CStr()[0], '\0');
		EXPECT_EQ(literal, FStringView());
	}

	constexpr FStringLiteral withNul = "a\0b";
	EXPECT_EQ(withNul.Len(), 3u);
}

TEST(StringLiteral, TablesParametersAndConstants)
{
	constexpr FStringLiteral table[] = { "Zero", "One", "Два" };
	EXPECT_EQ(table[2].Len(), 6u);
	EXPECT_EQ(table[2], "Два");

	static constexpr char staticArray[] = "xyz";
	constexpr FStringLiteral fromArray = staticArray;
	EXPECT_EQ(fromArray, "xyz");

	EXPECT_EQ(LiteralLength("abcd"), 4u);

	EXPECT_EQ(Utf8Bom.Len(), 3u);
	EXPECT_EQ(Utf8Bom, "\xEF\xBB\xBF");
	EXPECT_EQ(AsciiWhitespace.Len(), 6u);
}

// The shapes the constructor's comment gives for what MSVC 14.38 rejects (C7595): a conditional operator
// between two constructions, braced lists of literals. The constants are made first, then chosen or listed.
TEST(StringLiteral, ChoosingAndListingThroughConstants)
{
	constexpr FStringLiteral yes = "так";
	constexpr FStringLiteral no = "ні";
	for (const bool bValue : { true, false })
	{
		const FStringLiteral chosen = bValue ? yes : no;
		EXPECT_EQ(chosen, bValue ? FStringView("так") : FStringView("ні"));
		EXPECT_EQ(chosen.CStr(), (bValue ? yes : no).CStr());
	}

	const std::vector<FStringLiteral> listed = { yes, no };
	ASSERT_EQ(listed.size(), 2u);
	EXPECT_EQ(listed[1], "ні");

	uint32 total = 0;
	for (const FStringLiteral literal : { yes, no })
	{
		total += literal.Len();
	}
	EXPECT_EQ(total, yes.Len() + no.Len());
}
