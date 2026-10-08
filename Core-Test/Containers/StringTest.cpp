#include <gtest/gtest.h>

#include "TestCommon.h"

#include <algorithm>
#include <array>
#include <concepts>
#include <filesystem>
#include <format>
#include <iterator>
#include <new>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "Containers/Array.h"
#include "Containers/String.h"
#include "Containers/StringMap.h"
#include "Platform/Platform.h"

using namespace frt;


namespace
{
using IndexStrategy = math::SIndexStrategy;

constexpr FStringLiteral Uk = "Привіт, світе! Ґанок, їжак, є";
constexpr FStringLiteral Alphabet = "abcdefghijklmnopqrstuvwxyz0123456789";

// Constant-initialized: the constexpr default constructor is what keeps namespace-scope strings free of
// static-initialization-order problems.
constinit FString GConstantInitialized;

enum EUnscopedTestEnum { UnscopedTestValue = 65 };
enum class EScopedTestEnum : uint8 { Value = 65 };

uint64 GetAllocations ()
{
	return string_detail::GetThreadHeapAllocationCount();
}

// 5.2: blocks are multiples of 16 bytes, one of which holds the terminator.
uint32 RoundCapacity (uint32 Len)
{
	return (Len + 1u + 15u) / 16u * 16u - 1u;
}

std::string MakeAscii (uint32 Len)
{
	std::string text;
	for (uint32 i = 0; i < Len; ++i)
	{
		text.push_back(Alphabet[i % Alphabet.Len()]);
	}
	return text;
}

bool IsAllZero (const FString& Text)
{
	const unsigned char* bytes = reinterpret_cast<const unsigned char*>(&Text);
	return std::all_of(bytes, bytes + sizeof(FString), [](unsigned char Byte) { return Byte == 0; });
}

std::string ReplaceAll (std::string Text, std::string_view From, std::string_view To)
{
	for (size_t at = Text.find(From); at != std::string::npos; at = Text.find(From, at + To.size()))
	{
		Text.replace(at, From.size(), To);
	}
	return Text;
}

// Escape's \xHH digits may be either case (the spec does not fix it): compare with them lowered.
FString LowerHexEscapes (FStringView Escaped)
{
	FString result;
	for (uint32 i = 0; i < Escaped.Len(); ++i)
	{
		result.AppendChar(Escaped[i]);
		if (Escaped[i] != '\\' || i + 1 >= Escaped.Len())
		{
			continue;
		}

		const char next = Escaped[++i];
		result.AppendChar(next);
		for (uint32 digit = 0; next == 'x' && digit < 2 && i + 1 < Escaped.Len(); ++digit)
		{
			const char hex = Escaped[++i];
			result.AppendChar(hex >= 'A' && hex <= 'F' ? static_cast<char>(hex - 'A' + 'a') : hex);
		}
	}
	return result;
}

// The aliasing cases run on each of these: inline, the two lengths whose doubling lands on and just past
// InlineCapacity, a full inline buffer, a heap block with Len() == GetCapacity(), and one with slack.
constexpr uint32 AliasingTextCount = 6;

FString MakeAliasingText (uint32 Variant)
{
	static constexpr std::array<uint32, 5> Lengths = { 6u, 11u, 12u, 22u, 31u };
	if (Variant < Lengths.size())
	{
		return FString(Alphabet.View().Left(Lengths[Variant]));
	}

	FString withSlack(Alphabet.View());
	withSlack.Reserve(200);
	return withSlack;
}

// Negative compile checks go through concepts over a template parameter: a requires-expression with an
// invalid non-dependent expression outside a template is ill-formed, not false (section 9).
template <typename TLeft, typename TRight>
concept CanConcatenate = requires (TLeft&& Left, TRight&& Right)
{
	{ std::forward<TLeft>(Left) + std::forward<TRight>(Right) } -> std::same_as<FString>;
};

template <typename TString, typename TValue>
concept CanAppendAssign = requires (TString& Text, TValue Value) { Text += Value; };

template <typename TString, typename TValue>
concept CanStream = requires (TString& Text, TValue Value) { Text << Value; };

template <typename TString, typename TValue>
concept CanInsertAt = requires (TString& Text, TValue Value) { Text.InsertAt(0, Value); };

template <typename TString, typename TValue>
concept CanRemoveAll = requires (TString& Text, TValue Value) { Text.RemoveAll(Value); };

template <typename TString, typename TFrom, typename TTo>
concept CanReplaceChar = requires (TString& Text, TFrom From, TTo To) { Text.ReplaceCharInline(From, To); };

template <typename TString>
concept CanMakePathFromNarrowLiteral = requires { TString::FromPath("C:/x"); };

template <typename TString>
concept CanMakePathFromStdString = requires { TString::FromPath(std::string()); };

template <typename TString>
concept CanAppendNarrowPath = requires (TString& Text) { Text.AppendPath("C:/x"); };

template <typename TString>
concept CanReadNarrowPath = requires (const TString& Text) { platform::ReadFileText(Text.CStr()); };

using LiteralRef = const char (&)[2];
}


// ---------------------------------------------------------------------------------
// Compile-time contracts: layout, conversions, deleted overloads (5.1, 6.7, 6.8)
// ---------------------------------------------------------------------------------

static_assert(sizeof(FString) == 24);
static_assert(alignof(FString) == 8);
static_assert(FString::InlineCapacity == 22u);
static_assert(std::is_standard_layout_v<FString>);
static_assert(std::is_nothrow_default_constructible_v<FString>);
static_assert(std::is_nothrow_copy_constructible_v<FString>);
static_assert(std::is_nothrow_move_constructible_v<FString>);
static_assert(std::is_nothrow_move_assignable_v<FString>);

static_assert(std::is_convertible_v<const char*, FString>);
static_assert(std::is_convertible_v<const char8_t*, FString>);
static_assert(std::is_convertible_v<FStringLiteral, FString>);
static_assert(std::is_convertible_v<std::string, FStringView>);
static_assert(std::is_convertible_v<FString, FStringView>);
static_assert(std::is_constructible_v<FString, FStringView>);
static_assert(!std::is_convertible_v<FStringView, FString>);
static_assert(!std::is_convertible_v<std::string_view, FString>);
static_assert(!std::is_convertible_v<std::string, FString>);
static_assert(!std::is_constructible_v<FString, std::nullptr_t>);
static_assert(!std::is_constructible_v<FString, const wchar_t*>);
static_assert(!std::is_constructible_v<FString, std::wstring_view>);
static_assert(!std::is_constructible_v<std::filesystem::path, FString>);
static_assert(!std::is_constructible_v<FStringLiteral, const char*>);
static_assert(!CanMakePathFromNarrowLiteral<FString>);
static_assert(!CanMakePathFromStdString<FString>);
static_assert(!CanAppendNarrowPath<FString>);
static_assert(!CanReadNarrowPath<FString>);
#if !FRT_STRING_STD_COMPAT
static_assert(!std::is_convertible_v<FString, std::string>);
static_assert(!std::is_convertible_v<FStringView, std::string_view>);
#else
// Migration bridge (10.1), removed in B6.
static_assert(std::is_convertible_v<FString, std::string>);
static_assert(std::is_convertible_v<FStringView, std::string_view>);
#endif

static_assert(std::is_assignable_v<FString&, const char*>);
static_assert(std::is_assignable_v<FString&, const char8_t*>);
static_assert(std::is_assignable_v<FString&, FStringLiteral>);
static_assert(std::is_assignable_v<FString&, FStringView>);
static_assert(!std::is_assignable_v<FString&, std::nullptr_t>);

// The 6.8 resolution table: every row compiles and yields an FString (buffer reuse is checked at runtime).
static_assert(CanConcatenate<FString&, FString&>);
static_assert(CanConcatenate<const FString&, const FString&>);
static_assert(CanConcatenate<FString&, LiteralRef>);
static_assert(CanConcatenate<LiteralRef, FString&>);
static_assert(CanConcatenate<FStringView, LiteralRef>);
static_assert(CanConcatenate<FStringLiteral, LiteralRef>);
static_assert(CanConcatenate<LiteralRef, FStringLiteral>);
static_assert(CanConcatenate<FString&, FStringView>);
static_assert(CanConcatenate<FStringLiteral, FString&>);
static_assert(CanConcatenate<FString, FString&>);
static_assert(CanConcatenate<FString, LiteralRef>);
static_assert(CanConcatenate<FString, FStringView>);
static_assert(CanConcatenate<FString, FStringLiteral>);
static_assert(CanConcatenate<FString&, FString>);
static_assert(CanConcatenate<LiteralRef, FString>);
static_assert(CanConcatenate<FStringLiteral, FString>);
static_assert(CanConcatenate<FString, FString>);
static_assert(CanConcatenate<FString&, char>);
static_assert(CanConcatenate<FString, char>);
static_assert(CanConcatenate<char, FString&>);
static_assert(CanConcatenate<char, FString>);
static_assert(CanConcatenate<FStringView, FStringView>);
static_assert(CanConcatenate<FString&, const char*&>);
static_assert(CanConcatenate<const char*&, FString&>);
static_assert(CanConcatenate<FString&, std::string&>);
static_assert(CanConcatenate<std::string&, FString&>);
static_assert(CanConcatenate<FString, std::string&>);
// Numbers, enums and code points would otherwise append one truncated byte.
static_assert(!CanConcatenate<FString&, int>);
static_assert(!CanConcatenate<FString&, double>);
static_assert(!CanConcatenate<FString&, bool>);
static_assert(!CanConcatenate<FString&, EUnscopedTestEnum>);
static_assert(!CanConcatenate<FString&, char32_t>);
static_assert(!CanConcatenate<int, FString&>);
static_assert(!CanConcatenate<FString, int>);
static_assert(!CanConcatenate<FStringView, int>);

static_assert(CanAppendAssign<FString, char>);
static_assert(CanAppendAssign<FString, char32_t>);
static_assert(CanAppendAssign<FString, const char*>);
static_assert(CanAppendAssign<FString, FStringView>);
static_assert(CanAppendAssign<FString, FStringLiteral>);
static_assert(CanAppendAssign<FString, FString>);
static_assert(CanAppendAssign<FString, std::string>);
static_assert(!CanAppendAssign<FString, int>);
static_assert(!CanAppendAssign<FString, double>);
static_assert(!CanAppendAssign<FString, bool>);
static_assert(!CanAppendAssign<FString, uint8>);
static_assert(!CanAppendAssign<FString, char8_t>);
static_assert(!CanAppendAssign<FString, EUnscopedTestEnum>);
// (A scoped enum is not checked here: that concept check crashes MSVC 14.38 with C1001. It cannot convert
// to any text parameter anyway.)

static_assert(CanStream<FString, const char*>);
static_assert(CanStream<FString, const char8_t*>);
static_assert(CanStream<FString, FStringView>);
static_assert(CanStream<FString, FStringLiteral>);
static_assert(CanStream<FString, char>);
static_assert(CanStream<FString, char32_t>);
static_assert(CanStream<FString, bool>);
static_assert(CanStream<FString, int8>);
static_assert(CanStream<FString, double>);
static_assert(!CanStream<FString, const wchar_t*>);
static_assert(!CanStream<FString, std::nullptr_t>);
static_assert(!CanStream<FString, EScopedTestEnum>);
static_assert(!CanStream<FString, EUnscopedTestEnum>);
static_assert(!CanStream<FString, void*>);
static_assert(!CanStream<FString, const void*>);

static_assert(CanInsertAt<FString, char>);
static_assert(CanInsertAt<FString, const char*>);
static_assert(CanInsertAt<FString, FStringView>);
static_assert(!CanInsertAt<FString, int>);
static_assert(!CanInsertAt<FString, double>);
// A code point would insert its low byte ('W' for U'ї'), although += and << append it whole.
static_assert(!CanInsertAt<FString, char32_t>);

static_assert(CanRemoveAll<FString, char>);
static_assert(CanRemoveAll<FString, const char*>);
static_assert(!CanRemoveAll<FString, char32_t>);
static_assert(CanReplaceChar<FString, char, char>);
static_assert(!CanReplaceChar<FString, char32_t, char>);
static_assert(!CanReplaceChar<FString, char, char32_t>);


// ---------------------------------------------------------------------------------
// Layout and the empty state
// ---------------------------------------------------------------------------------

TEST(String, AllZeroBytesAreTheEmptyString)
{
	alignas(FString) unsigned char raw[sizeof(FString)] = {};
	const FString& zero = *std::launder(reinterpret_cast<const FString*>(raw));
	EXPECT_TRUE(zero.IsEmpty());
	EXPECT_EQ(zero.Len(), 0u);
	EXPECT_EQ(zero.CStr()[0], '\0');
	EXPECT_FALSE(zero.IsHeapAllocated());
	EXPECT_EQ(zero.GetCapacity(), FString::InlineCapacity);

	const FString defaulted;
	EXPECT_TRUE(IsAllZero(defaulted));
	EXPECT_NE(defaulted.CStr(), nullptr);
	EXPECT_NE(defaulted.GetData(), nullptr);
	EXPECT_STREQ(defaulted.CStr(), "");

	EXPECT_TRUE(GConstantInitialized.IsEmpty());
}

TEST(String, ConstructsFromEveryTextSource)
{
	const FString fromLiteral = "Ґанок";
	EXPECT_EQ(fromLiteral.Len(), 10u);
	EXPECT_STREQ(fromLiteral.CStr(), "Ґанок");

	const FString withNul("a\0b", 3);
	EXPECT_EQ(withNul.Len(), 3u);
	EXPECT_EQ(withNul[1], '\0');
	EXPECT_EQ(withNul.CStr()[3], '\0');

	const FString fromChar8 = u8"їжак";
	EXPECT_EQ(fromChar8, "їжак");
	const FString fromChar8Length(u8"їжак", 4);
	EXPECT_EQ(fromChar8Length, "їж");

	constexpr FStringLiteral literal = "literal";
	const FString fromFStringLiteral = literal;
	EXPECT_EQ(fromFStringLiteral, "literal");

	const std::string stdString = "from std::string";
	const FString fromView(FStringView(stdString).Left(4));
	EXPECT_EQ(fromView, "from");
	const FString fromStdString{ FStringView(stdString) };
	EXPECT_EQ(fromStdString, stdString);

	const char* nullText = nullptr;
	const FString fromNull(nullText);
	EXPECT_TRUE(fromNull.IsEmpty());
}

TEST(String, AssignsFromEveryTextSource)
{
	FString text;
	text = "first";
	EXPECT_EQ(text, "first");
	text = u8"ї";
	EXPECT_EQ(text, "\xD1\x97");

	constexpr FStringLiteral literal = "literal";
	text = literal;
	EXPECT_EQ(text, "literal");

	const std::string stdString = "std";
	text = FStringView(stdString);
	EXPECT_EQ(text, "std");
}

TEST(String, AccessAndStlSurface)
{
	FString text = "cab";

	EXPECT_EQ(text[0], 'c');
	text[0] = 'C';
	text.GetData()[1] = 'A';
	EXPECT_EQ(text, "CAb");
	EXPECT_EQ(text.First(), 'C');
	EXPECT_EQ(text.Last(), 'b');
	EXPECT_EQ(text.Get<IndexStrategy::IS_Circular>(-1), 'b');

	text = "cba";
	std::ranges::sort(text);
	EXPECT_EQ(text, "abc");

	std::format_to(std::back_inserter(text), "{}", 42);
	text.push_back('!');
	EXPECT_EQ(text, "abc42!");

	const FString& constText = text;
	EXPECT_EQ(constText.data(), constText.CStr());
	EXPECT_EQ(text.c_str(), text.GetData());
	EXPECT_EQ(text.data(), text.GetData());
	EXPECT_EQ(text.size(), 6u);
	EXPECT_EQ(text.length(), 6u);
	EXPECT_EQ(text.capacity(), text.GetCapacity());
	EXPECT_FALSE(text.empty());
	EXPECT_EQ(std::string(constText.begin(), constText.end()), "abc42!");
	EXPECT_EQ(text.end() - text.begin(), 6);

	for (char& byte : text)
	{
		byte = byte == '!' ? '?' : byte;
	}
	EXPECT_EQ(text.ToStdString(), std::string("abc42?"));
	EXPECT_EQ(text.View(), "abc42?");

#if FRT_STRING_STD_COMPAT
	// Migration bridge for consumers not migrated yet (10.1); removed in B6.
	const std::string converted = text;
	EXPECT_EQ(converted, "abc42?");
#endif
}


TEST(String, AllocatorCountsThisThreadOnly)
{
	const uint64 before = GetAllocations();
	void* block = string_detail::Allocate(32);
	ASSERT_NE(block, nullptr);
	block = string_detail::Reallocate(block, 64);
	ASSERT_NE(block, nullptr);
	string_detail::Free(block);
	string_detail::Free(nullptr);
	EXPECT_EQ(GetAllocations(), before + 2u);
	EXPECT_GE(string_detail::GetHeapAllocationCount(), GetAllocations());

	// Strings built on another thread (job workers, async texture decode) do not disturb this thread's count.
	const uint64 beforeThread = GetAllocations();
	std::thread worker([]()
	{
		const FString heap = "built on a worker thread, longer than inline";
		(void)heap.Len();
	});
	worker.join();
	EXPECT_EQ(GetAllocations(), beforeThread);
}


// ---------------------------------------------------------------------------------
// Small-string optimization and growth (5.2)
// ---------------------------------------------------------------------------------

TEST(String, ShortStringsStayInline)
{
	for (const uint32 length : { 0u, 1u, 21u, 22u })
	{
		const std::string ascii = MakeAscii(length);
		const uint64 before = GetAllocations();
		const FString text(ascii.data(), ascii.size());

		EXPECT_EQ(GetAllocations(), before) << "length " << length;
		EXPECT_FALSE(text.IsHeapAllocated());
		EXPECT_EQ(text.GetCapacity(), FString::InlineCapacity);
		EXPECT_EQ(text.Len(), length);
		EXPECT_EQ(text.CStr()[length], '\0');
		EXPECT_EQ(text, ascii);
	}

	const uint64 before = GetAllocations();
	const FString fromLiteral = "twenty-two bytes, ok!!";
	const FString cyrillic = "одинадцять";
	EXPECT_EQ(GetAllocations(), before);
	EXPECT_EQ(fromLiteral.Len(), 22u);
	EXPECT_FALSE(cyrillic.IsHeapAllocated());
}

TEST(String, ConstructionAllocatesExactRoundedCapacity)
{
	for (const uint32 length : { 23u, 31u, 47u, 48u, 1000u })
	{
		const std::string ascii = MakeAscii(length);
		const uint64 before = GetAllocations();
		const FString text(ascii.data(), ascii.size());

		EXPECT_EQ(GetAllocations(), before + 1u) << "length " << length;
		EXPECT_TRUE(text.IsHeapAllocated());
		EXPECT_EQ(text.GetCapacity(), RoundCapacity(length)) << "length " << length;
		EXPECT_EQ(text, ascii);
		EXPECT_EQ(text.CStr()[length], '\0');
	}

	EXPECT_EQ(RoundCapacity(23u), 31u);
	EXPECT_EQ(RoundCapacity(48u), 63u);
}

TEST(String, AppendGrowsGeometricallyAndKeepsContent)
{
	const std::string ascii = MakeAscii(22);
	FString text(ascii.data(), ascii.size());

	// Leaving the inline buffer reserves for 33 bytes at least: capacity 47, not 31.
	const uint64 before = GetAllocations();
	text.AppendChar('!');
	EXPECT_EQ(GetAllocations(), before + 1u);
	EXPECT_EQ(text.GetCapacity(), 47u);
	EXPECT_TRUE(text.IsHeapAllocated());
	EXPECT_EQ(text, ascii + "!");

	// Byte by byte from empty: capacity follows the TArray factor 1.5, rounded to 16-byte blocks.
	FString grown;
	std::string reference;
	uint32 expectedCapacity = FString::InlineCapacity;
	for (uint32 i = 0; i < 600; ++i)
	{
		const char byte = Alphabet[i % Alphabet.Len()];
		grown.AppendChar(byte);
		reference.push_back(byte);

		const uint32 required = static_cast<uint32>(reference.size());
		if (required > expectedCapacity)
		{
			expectedCapacity = expectedCapacity == FString::InlineCapacity
				? RoundCapacity((std::max)(required, 33u))
				: RoundCapacity((std::max)(required, expectedCapacity + expectedCapacity / 2u));
		}
		ASSERT_EQ(grown.GetCapacity(), expectedCapacity) << "length " << required;
		ASSERT_EQ(grown.Len(), required);
		ASSERT_EQ(grown.CStr()[required], '\0');
	}
	EXPECT_EQ(grown, reference);
}

// Growth near MaxLength (5.2) through the helper the growth path uses: reaching it by appending would
// take 2 GB. The 1.5 factor is computed in 64 bits and clamped to MaxLength, never past it.
TEST(String, GrowthNearMaxLengthIsClamped)
{
	constexpr uint32 maxLength = FString::MaxLength;
	EXPECT_EQ(string_detail::RoundCapacity(maxLength), maxLength);
	EXPECT_EQ(string_detail::RoundCapacity(maxLength - 15u), maxLength);
	EXPECT_EQ(string_detail::RoundCapacity(maxLength - 16u), maxLength - 16u);

	const uint32 nearMax = string_detail::GetGrowCapacity(0x70000000u, 0x70000001u, false);
	EXPECT_LE(nearMax, maxLength);
	EXPECT_EQ(nearMax, maxLength);
	EXPECT_EQ(string_detail::GetGrowCapacity(maxLength - 16u, maxLength, false), maxLength);

	// Away from the limit: the factor, and at least 33 bytes when leaving the inline buffer.
	EXPECT_EQ(string_detail::GetGrowCapacity(47u, 48u, false), RoundCapacity(70u));
	EXPECT_EQ(string_detail::GetGrowCapacity(FString::InlineCapacity, 23u, true), 47u);
	EXPECT_EQ(string_detail::GetGrowCapacity(47u, 1000u, false), RoundCapacity(1000u));
}

TEST(String, CapacityManagement)
{
	FString text;
	uint64 before = GetAllocations();
	text.Reserve(10);
	EXPECT_EQ(GetAllocations(), before);
	EXPECT_EQ(text.GetCapacity(), FString::InlineCapacity);

	text.Reserve(100);
	EXPECT_EQ(GetAllocations(), before + 1u);
	EXPECT_EQ(text.GetCapacity(), RoundCapacity(100));
	EXPECT_TRUE(text.IsHeapAllocated());

	// Grow-only, and assignment reuses a block that is big enough.
	before = GetAllocations();
	text.Reserve(50);
	text = "content";
	EXPECT_EQ(GetAllocations(), before);
	EXPECT_EQ(text.GetCapacity(), RoundCapacity(100));
	EXPECT_EQ(text, "content");

	text.Clear();
	EXPECT_TRUE(text.IsEmpty());
	EXPECT_EQ(text.CStr()[0], '\0');
	EXPECT_TRUE(text.IsHeapAllocated());
	EXPECT_EQ(text.GetCapacity(), RoundCapacity(100));

	text.Reset(200);
	EXPECT_TRUE(text.IsEmpty());
	EXPECT_EQ(text.GetCapacity(), RoundCapacity(200));
	text.Reset(10);
	EXPECT_EQ(text.GetCapacity(), RoundCapacity(200));

	text = "kept";
	text.Free();
	EXPECT_TRUE(text.IsEmpty());
	EXPECT_FALSE(text.IsHeapAllocated());
	EXPECT_EQ(text.GetCapacity(), FString::InlineCapacity);
}

TEST(String, ShrinkToFit)
{
	FString shortText = "short";
	shortText.Reserve(100);
	shortText.ShrinkToFit();
	EXPECT_FALSE(shortText.IsHeapAllocated());
	EXPECT_EQ(shortText.GetCapacity(), FString::InlineCapacity);
	EXPECT_EQ(shortText, "short");

	const std::string ascii = MakeAscii(40);
	FString longText(ascii.data(), ascii.size());
	longText.Reserve(500);
	longText.ShrinkToFit();
	EXPECT_EQ(longText.GetCapacity(), RoundCapacity(40));
	EXPECT_EQ(longText, ascii);
}

TEST(String, ResizeAndCApiBuffers)
{
	FString text = "ab";
	text.Resize(5, 'z');
	EXPECT_EQ(text, "abzzz");
	text.Resize(1);
	EXPECT_EQ(text, "a");
	text.Resize(30);
	EXPECT_EQ(text.Len(), 30u);
	EXPECT_EQ(text[29], '\0');
	EXPECT_EQ(text.CStr()[30], '\0');

	FString cyrillic = "їж";
	cyrillic.Resize(2);
	EXPECT_EQ(cyrillic, "ї");

	// A C API writes into the buffer, then the length is taken from its terminator.
	FString buffer;
	buffer.ResizeUninitialized(64);
	EXPECT_EQ(buffer.Len(), 64u);
	EXPECT_EQ(buffer.CStr()[64], '\0');
	const char written[] = "written by C";
	std::copy(std::begin(written), std::end(written), buffer.GetData());
	buffer.TrimToNullTerminator();
	EXPECT_EQ(buffer, "written by C");
	EXPECT_EQ(buffer.Len(), 12u);
}


// ---------------------------------------------------------------------------------
// Copy, move, swap
// ---------------------------------------------------------------------------------

TEST(String, CopyAllocatesExactlyAndOnlyForHeapSources)
{
	FString heap;
	for (uint32 i = 0; i < 100; ++i)
	{
		heap.AppendChar('h');
	}
	ASSERT_GT(heap.GetCapacity(), RoundCapacity(100));

	uint64 before = GetAllocations();
	const FString heapCopy(heap);
	EXPECT_EQ(GetAllocations(), before + 1u);
	EXPECT_EQ(heapCopy.GetCapacity(), RoundCapacity(100));
	EXPECT_EQ(heapCopy, heap);
	EXPECT_NE(heapCopy.GetData(), heap.GetData());

	const FString inlineText = "inline";
	before = GetAllocations();
	const FString inlineCopy(inlineText);
	EXPECT_EQ(GetAllocations(), before);
	EXPECT_EQ(inlineCopy, "inline");
	EXPECT_FALSE(inlineCopy.IsHeapAllocated());

	// Copy assignment from a heap source reuses a big enough destination block.
	FString destination;
	destination.Reserve(200);
	before = GetAllocations();
	destination = heap;
	EXPECT_EQ(GetAllocations(), before);
	EXPECT_EQ(destination, heap);
	EXPECT_EQ(destination.GetCapacity(), RoundCapacity(200));

	destination = inlineText;
	EXPECT_EQ(destination, "inline");
}

TEST(String, MoveStealsAndLeavesAnEmptyInlineSource)
{
	const std::string ascii = MakeAscii(64);
	FString source(ascii.data(), ascii.size());
	const char* block = source.GetData();

	const uint64 before = GetAllocations();
	FString target(std::move(source));
	EXPECT_EQ(GetAllocations(), before);
	EXPECT_EQ(target.GetData(), block);
	EXPECT_EQ(target, ascii);
	EXPECT_TRUE(source.IsEmpty());
	EXPECT_FALSE(source.IsHeapAllocated());
	EXPECT_TRUE(IsAllZero(source));

	FString assigned = "previous heap contents, longer than inline";
	assigned = std::move(target);
	EXPECT_EQ(assigned.GetData(), block);
	EXPECT_TRUE(target.IsEmpty());
	EXPECT_TRUE(IsAllZero(target));

	FString inlineSource = "inline";
	FString inlineTarget(std::move(inlineSource));
	EXPECT_EQ(inlineTarget, "inline");
	EXPECT_TRUE(inlineSource.IsEmpty());
}

TEST(String, SelfCopyAndSelfMoveAreNoOps)
{
	for (const FStringView text : { FStringView("inline"), FStringView("a heap string that is longer than inline") })
	{
		FString value(text);
		FString& alias = value;
		value = alias;
		EXPECT_EQ(value, text);
		value = std::move(alias);
		EXPECT_EQ(value, text);
	}
}

TEST(String, SwapInlineAndHeap)
{
	FString a = "abc";
	FString b = "a heap allocated string that is long";
	const char* block = b.GetData();

	a.Swap(b);
	EXPECT_EQ(a, "a heap allocated string that is long");
	EXPECT_EQ(a.GetData(), block);
	EXPECT_EQ(b, "abc");
	EXPECT_FALSE(b.IsHeapAllocated());

	swap(a, b);
	EXPECT_EQ(a, "abc");
	EXPECT_EQ(b.GetData(), block);

	std::swap(a, b);
	EXPECT_EQ(a.GetData(), block);
	EXPECT_EQ(b, "abc");
}


// ---------------------------------------------------------------------------------
// Aliasing (5.4): every mutator behaves as if its arguments were copied first
// ---------------------------------------------------------------------------------

TEST(StringAliasing, ArgumentsViewingTheStringItself)
{
	struct SAliasCase
	{
		const char* Name;
		void (*Apply) (FString& Text);
		std::string (*Expected) (const std::string& Text);
	};

	const std::array<SAliasCase, 11> cases =
	{{
		{ "Append(S)", [](FString& S) { S.Append(S); }, [](const std::string& R) { return R + R; } },
		{ "Append(S.View().Mid(3))", [](FString& S) { S.Append(S.View().Mid(3)); }, [](const std::string& R) { return R + R.substr(3); } },
		{ "S += S", [](FString& S) { S += S; }, [](const std::string& R) { return R + R; } },
		{ "InsertAt(2, S.View())", [](FString& S) { S.InsertAt(2, S.View()); }, [](const std::string& R) { return R.substr(0, 2) + R + R.substr(2); } },
		{ "InsertAt(0, S.View().Right(3))", [](FString& S) { S.InsertAt(0, S.View().Right(3)); }, [](const std::string& R) { return R.substr(R.size() - 3) + R; } },
		{ "S = S.View().Mid(2)", [](FString& S) { S = S.View().Mid(2); }, [](const std::string& R) { return R.substr(2); } },
		{ "S = S.CStr() + 2", [](FString& S) { S = S.CStr() + 2; }, [](const std::string& R) { return R.substr(2); } },
		{ "ReplaceInline(\"a\", S.View().Left(2))", [](FString& S) { (void)S.ReplaceInline("a", S.View().Left(2)); }, [](const std::string& R) { return ReplaceAll(R, "a", R.substr(0, 2)); } },
		{ "RemoveAll(S.View().Left(1))", [](FString& S) { (void)S.RemoveAll(S.View().Left(1)); }, [](const std::string& R) { return ReplaceAll(R, R.substr(0, 1), ""); } },
		{ "AppendFormat(\"{}{}\", S, S)", [](FString& S) { S.AppendFormat("{}{}", S, S); }, [](const std::string& R) { return R + R + R; } },
		{ "Appendf(\"%s\", S.CStr())", [](FString& S) { S.Appendf("%s", S.CStr()); }, [](const std::string& R) { return R + R; } },
	}};

	for (const SAliasCase& aliasCase : cases)
	{
		for (uint32 variant = 0; variant < AliasingTextCount; ++variant)
		{
			FString text = MakeAliasingText(variant);
			if (variant == 4)
			{
				ASSERT_EQ(text.Len(), text.GetCapacity());
			}
			const std::string reference = text.ToStdString();

			aliasCase.Apply(text);
			EXPECT_EQ(text, aliasCase.Expected(reference)) << aliasCase.Name << " on " << reference;
			EXPECT_EQ(text.CStr()[text.Len()], '\0');
		}
	}
}

// The alphabet texts above hold their needle once, so a rewrite never reaches a second match: here the
// needle viewed from the string occurs again after the bytes the first match moves.
TEST(StringAliasing, ReplaceWithANeedleViewingTheStringItself)
{
	struct SAliasCase
	{
		const char* Name;
		void (*Apply) (FString& Text);
		std::string To;
	};

	const std::array<SAliasCase, 3> cases =
	{{
		{ "RemoveAll(S.View().Left(2))", [](FString& S) { (void)S.RemoveAll(S.View().Left(2)); }, "" },
		{ "ReplaceInline(S.View().Left(2), \"-\")", [](FString& S) { (void)S.ReplaceInline(S.View().Left(2), "-"); }, "-" },
		{ "ReplaceInline(S.View().Left(2), \"<longer>\")", [](FString& S) { (void)S.ReplaceInline(S.View().Left(2), "<longer>"); }, "<longer>" },
	}};

	// Inline, heap, and heap with slack (the rewrite stays in the same block).
	const std::string pattern = "abXabYab";
	const auto makeText = [&pattern](uint32 Variant)
	{
		FString text(Variant == 0 ? pattern : pattern + pattern + pattern + pattern);
		if (Variant == 2)
		{
			text.Reserve(200);
		}
		return text;
	};

	for (const SAliasCase& aliasCase : cases)
	{
		for (uint32 variant = 0; variant < 3; ++variant)
		{
			FString text = makeText(variant);
			const std::string reference = text.ToStdString();

			aliasCase.Apply(text);
			EXPECT_EQ(text, ReplaceAll(reference, reference.substr(0, 2), aliasCase.To)) << aliasCase.Name << " on " << reference;
			EXPECT_EQ(text.CStr()[text.Len()], '\0');
		}
	}
}

TEST(StringAliasing, ConcatenationWithAViewOfTheRvalue)
{
	for (uint32 variant = 0; variant < AliasingTextCount; ++variant)
	{
		FString text = MakeAliasingText(variant);
		const std::string reference = text.ToStdString();

		const FStringView self = text;
		const FString joined = self + std::move(text);
		EXPECT_EQ(joined, reference + reference);
	}
}


// ---------------------------------------------------------------------------------
// Append, insert, remove, replace
// ---------------------------------------------------------------------------------

TEST(String, AppendFamily)
{
	FString text;
	text.Append("a").AppendChar('b').AppendChars('c', 3).AppendCodePoint(U'ж').AppendCodePoint(U'😀');
	EXPECT_EQ(text, "abcccж😀");

	// Non-scalar values become U+FFFD.
	FString replaced;
	replaced.AppendCodePoint(0xD800).AppendCodePoint(0x110000);
	EXPECT_EQ(replaced, "\xEF\xBF\xBD\xEF\xBF\xBD");

	FString numbers;
	numbers.AppendNumber(-42).AppendChar(' ').AppendNumber(42u).AppendChar(' ').AppendNumber(static_cast<int8>(-5))
		.AppendChar(' ').AppendNumber(0.5).AppendChar(' ').AppendBool(true).AppendBool(false);
	EXPECT_EQ(numbers, "-42 42 -5 0.5 truefalse");

	FString chars;
	chars.AppendChars('x', 0);
	EXPECT_TRUE(chars.IsEmpty());
	chars.AppendChars('\x80', 2);
	EXPECT_EQ(chars, "\x80\x80");
}

TEST(String, AppendOperator)
{
	FString text = "a";
	text += U'ж';
	EXPECT_EQ(text.Len(), 3u);
	text += 'b';
	EXPECT_EQ(text.Len(), 4u);
	EXPECT_EQ(text, "aжb");

	text += "cd";
	text += FStringView("e");
	text += std::string("f");
	constexpr FStringLiteral literal = "g";
	text += literal;
	text += FString("h");
	text += U'😀';
	EXPECT_EQ(text, "aжbcdefgh😀");

	FString surrogate;
	surrogate += static_cast<char32_t>(0xDC00);
	EXPECT_EQ(surrogate, "\xEF\xBF\xBD");
}

TEST(String, StreamOperator)
{
	FString text;
	text << "text";
	EXPECT_EQ(text, "text");

	FString utf8;
	utf8 << u8"ї";
	EXPECT_EQ(utf8, "\xD1\x97");
	utf8 << U'ї';
	EXPECT_EQ(utf8, "\xD1\x97\xD1\x97");

	FString values;
	values << true << ' ' << false << ' ' << static_cast<int8>(-5) << ' ' << static_cast<uint8>(200) << ' ' << 42 << ' '
		<< -7ll << ' ' << 3000000000u << ' ' << 1.5 << ' ' << 0.25f << ' ' << 'c' << ' ' << FStringView("view");
	EXPECT_EQ(values, "true false -5 200 42 -7 3000000000 1.5 0.25 c view");

	constexpr FStringLiteral literal = "lit";
	FString chained;
	FString& result = chained << literal << FString("+") << std::string("std");
	EXPECT_EQ(&result, &chained);
	EXPECT_EQ(chained, "lit+std");
}

TEST(String, InsertAtWithIndexStrategies)
{
	FString text = "abc";
	text.InsertAt<IndexStrategy::IS_CircularClamped>(-1, "X");
	EXPECT_EQ(text, "abXc");

	text.InsertAt(text.Len(), "end");
	EXPECT_EQ(text, "abXcend");
	text.InsertAt(0, '>');
	EXPECT_EQ(text, ">abXcend");
	text.InsertAt(1, FStringView()).InsertAt(1, "");
	EXPECT_EQ(text, ">abXcend");

	FString cyrillic = "їж";
	cyrillic.InsertAt<IndexStrategy::IS_CircularClamped>(-2, "X");
	EXPECT_EQ(cyrillic, "їXж");
	cyrillic.InsertAt<IndexStrategy::IS_Circular>(-2, "Ґ");
	EXPECT_EQ(cyrillic, "їXҐж");
}

TEST(String, RemoveAtWithIndexStrategies)
{
	FString text = "abcdef";
	text.RemoveAt(4, 100);
	EXPECT_EQ(text, "abcd");
	text.RemoveAt(0);
	EXPECT_EQ(text, "bcd");
	text.RemoveAt<IndexStrategy::IS_Circular>(-1);
	EXPECT_EQ(text, "bc");

	FString cyrillic = "їжак";
	cyrillic.RemoveAt(2, 2);
	EXPECT_EQ(cyrillic, "їак");
	cyrillic.RemoveAt<IndexStrategy::IS_CircularClamped>(-4, 2);
	EXPECT_EQ(cyrillic, "їк");
}

TEST(String, RemoveFromStartEndAndAll)
{
	// IgnoreCase removes the matched haystack bytes: U+212A is 3 bytes, the needle 1.
	FString kelvin = "\u212Aey";
	EXPECT_TRUE(kelvin.RemoveFromStart<ESearchCase::IgnoreCase>("k"));
	EXPECT_EQ(kelvin, "ey");
	EXPECT_FALSE(kelvin.RemoveFromStart("x"));
	EXPECT_EQ(kelvin, "ey");

	FString suffix = "abЇ";
	EXPECT_TRUE(suffix.RemoveFromEnd<ESearchCase::IgnoreCase>("ї"));
	EXPECT_EQ(suffix, "ab");
	EXPECT_TRUE(suffix.RemoveFromEnd("b"));
	EXPECT_EQ(suffix, "a");

	FString text = "abxabyab";
	EXPECT_EQ(text.RemoveAll("ab"), 3u);
	EXPECT_EQ(text, "xy");
	EXPECT_EQ(text.RemoveAll("zz"), 0u);

	FString banana = "banana";
	EXPECT_EQ(banana.RemoveAll('a'), 3u);
	EXPECT_EQ(banana, "bnn");

	FString cyrillic = "їЇx";
	EXPECT_EQ(cyrillic.RemoveAll<ESearchCase::IgnoreCase>("Ї"), 2u);
	EXPECT_EQ(cyrillic, "x");

	FString kelvins = "a\u212AbK";
	EXPECT_EQ(kelvins.RemoveAll<ESearchCase::IgnoreCase>("k"), 2u);
	EXPECT_EQ(kelvins, "ab");
}

TEST(String, Replace)
{
	const FString source = "a-b-c";
	const FString replaced = source.Replace("-", "+");
	EXPECT_EQ(replaced, "a+b+c");
	EXPECT_EQ(source, "a-b-c");

	EXPECT_EQ(FString("a\u212Ab").Replace<ESearchCase::IgnoreCase>("K", "x"), "axb");
	EXPECT_EQ(FString("Їжак").Replace<ESearchCase::IgnoreCase>("ї", "i"), "iжак");

	FString text = "banana";
	EXPECT_EQ(text.ReplaceInline("a", "ab"), 3u);
	EXPECT_EQ(text, "babnabnab");
	EXPECT_EQ(text.ReplaceInline("ab", ""), 3u);
	EXPECT_EQ(text, "bnn");
	EXPECT_EQ(text.ReplaceInline("n", "m"), 2u);
	EXPECT_EQ(text, "bmm");
	EXPECT_EQ(text.ReplaceInline("x", "y"), 0u);
	EXPECT_EQ(text, "bmm");

	FString cyrillic = "їжак їжак";
	EXPECT_EQ(cyrillic.ReplaceInline("ї", "i"), 2u);
	EXPECT_EQ(cyrillic, "iжак iжак");

	FString path = "a/b/c";
	EXPECT_EQ(path.ReplaceCharInline('/', '\\'), 2u);
	EXPECT_EQ(path, "a\\b\\c");
}

TEST(String, RvalueReplaceReusesTheBuffer)
{
	for (const std::pair<FStringView, FStringView> fromTo : { std::pair<FStringView, FStringView>("-", "+"),
		std::pair<FStringView, FStringView>("--", "-"), std::pair<FStringView, FStringView>("-", "--") })
	{
		FString text = "a--b--c--d--e--f--g--h--i";
		text.Reserve(128);
		const char* block = text.GetData();
		const std::string expected = ReplaceAll(text.ToStdString(), fromTo.first.ToStdString(), fromTo.second.ToStdString());

		const FString result = std::move(text).Replace(fromTo.first, fromTo.second);
		EXPECT_EQ(result, expected);
		EXPECT_EQ(result.GetData(), block) << ::testing::PrintToString(fromTo.first) << " -> " << ::testing::PrintToString(fromTo.second);
	}
}


// ---------------------------------------------------------------------------------
// Substrings and trimming: const& copies, && reuse, Inline mutation
// ---------------------------------------------------------------------------------

TEST(String, SubstringCopies)
{
	const FString text = "abcdef";

	EXPECT_EQ(text.Left(2), "ab");
	EXPECT_EQ(text.LeftChop(2), "abcd");
	EXPECT_EQ(text.Right(2), "ef");
	EXPECT_EQ(text.RightChop(2), "cdef");
	EXPECT_EQ(text.Mid(1, 2), "bc");
	EXPECT_EQ(text.Mid(1), "bcdef");
	EXPECT_EQ(text.Slice(-2), "ef");
	EXPECT_EQ(text.Slice(1, -1), "bcde");
	EXPECT_EQ(text.Truncate(3), "abc");
	EXPECT_EQ(text.Left(100), "abcdef");
	EXPECT_TRUE(text.Mid(100).IsEmpty());
	EXPECT_EQ(text, "abcdef");

	EXPECT_EQ(FString("ґґґ").Truncate(5), "ґґ");
	EXPECT_EQ(FString(text).Left(3), "abc");
	EXPECT_EQ(FString(text).LeftChop(3), "abc");
	EXPECT_EQ(FString(text).Right(3), "def");
	EXPECT_EQ(FString(text).RightChop(3), "def");
	EXPECT_EQ(FString(text).Mid(2, 2), "cd");
	EXPECT_EQ(FString(text).Slice(-3, -1), "de");
	EXPECT_EQ(FString(text).Truncate(1), "a");
}

TEST(String, SubstringsInPlace)
{
	FString text = "abcdefgh";
	text.RightChopInline(1).LeftChopInline(1);
	EXPECT_EQ(text, "bcdefg");
	EXPECT_EQ(text.LeftInline(5), "bcdef");
	EXPECT_EQ(text.RightInline(4), "cdef");
	EXPECT_EQ(text.MidInline(1, 2), "de");
	EXPECT_EQ(text.SliceInline(-1), "e");

	FString sliced = "abcdef";
	EXPECT_EQ(sliced.SliceInline(1, -1), "bcde");

	FString cyrillic = "ґґґ";
	EXPECT_EQ(cyrillic.TruncateInline(3), "ґ");
}

TEST(String, RvalueSubstringsReuseTheBuffer)
{
	FString left = "  SOME ASCII TEXT THAT IS LONGER THAN INLINE  ";
	const char* leftBlock = left.GetData();
	const FString leftResult = std::move(left).Left(10);
	EXPECT_EQ(leftResult, "  SOME ASC");
	EXPECT_EQ(leftResult.GetData(), leftBlock);

	FString trimmed = "  SOME ASCII TEXT THAT IS LONGER THAN INLINE  ";
	const char* trimmedBlock = trimmed.GetData();
	const FString trimmedResult = std::move(trimmed).TrimStartAndEnd();
	EXPECT_EQ(trimmedResult, "SOME ASCII TEXT THAT IS LONGER THAN INLINE");
	EXPECT_EQ(trimmedResult.GetData(), trimmedBlock);

	FString lowered = "SOME ASCII TEXT THAT IS LONGER THAN INLINE";
	const char* loweredBlock = lowered.GetData();
	const FString loweredResult = std::move(lowered).ToLower();
	EXPECT_EQ(loweredResult, "some ascii text that is longer than inline");
	EXPECT_EQ(loweredResult.GetData(), loweredBlock);
}

TEST(String, TrimFamily)
{
	const FString padded = "\u3000 x \u00A0";
	EXPECT_EQ(padded.TrimStart(), "x \u00A0");
	EXPECT_EQ(padded.TrimEnd(), "\u3000 x");
	EXPECT_EQ(padded.TrimStartAndEnd(), "x");
	EXPECT_EQ(FString(padded).TrimStart(), "x \u00A0");
	EXPECT_EQ(FString(padded).TrimEnd(), "\u3000 x");

	FString inPlace = padded;
	EXPECT_EQ(inPlace.TrimStartInline(), "x \u00A0");
	EXPECT_EQ(inPlace.TrimEndInline(), "x");
	inPlace = " y ";
	EXPECT_EQ(inPlace.TrimStartAndEndInline(), "y");

	const FString dashes = "--x--";
	EXPECT_EQ(dashes.TrimStartAndEnd("-"), "x");
	EXPECT_EQ(dashes.TrimStart("-"), "x--");
	EXPECT_EQ(dashes.TrimEnd("-"), "--x");
	EXPECT_EQ(FString(dashes).TrimStartAndEnd("-"), "x");
	FString dashesInPlace = dashes;
	EXPECT_EQ(dashesInPlace.TrimStartAndEndInline("-"), "x");
	dashesInPlace = dashes;
	EXPECT_EQ(dashesInPlace.TrimStartInline("-"), "x--");
	EXPECT_EQ(dashesInPlace.TrimEndInline("-"), "x");

	EXPECT_EQ(FString("\"a\"").TrimQuotes(), "a");
	const FString quoted = "'b'";
	EXPECT_EQ(quoted.TrimQuotes("'"), "b");
	EXPECT_EQ(quoted.TrimQuotes(), "'b'");
	FString quotedInPlace = "\"\"c\"\"";
	EXPECT_EQ(quotedInPlace.TrimQuotesInline(), "\"c\"");
}


// ---------------------------------------------------------------------------------
// Case conversion (Unicode simple mappings, 6.7)
// ---------------------------------------------------------------------------------

TEST(StringCase, UkrainianAndLatin)
{
	EXPECT_EQ(FString(Uk).ToUpper(), "ПРИВІТ, СВІТЕ! ҐАНОК, ЇЖАК, Є");
	EXPECT_EQ(FString(Uk).ToLower(), "привіт, світе! ґанок, їжак, є");
	EXPECT_EQ(FString("ҐЇЄІ").ToLower(), "ґїєі");
	EXPECT_EQ(FString("ÀÉÎ").ToLower(), "àéî");
	EXPECT_EQ(FString("ǆ").ToUpper(), "Ǆ");
	EXPECT_EQ(FString("ǅ").ToLower(), "ǆ");
	EXPECT_EQ(FString("ǅ").ToUpper(), "Ǆ");

	// Simple mappings only: no ß -> SS, no combining dot for İ, no final-sigma rule.
	EXPECT_EQ(FString("ß").ToUpper(), "ß");
	EXPECT_EQ(FString("İ").ToLower(), "i");
	EXPECT_EQ(FString("ΟΔΟΣ").ToLower(), "οδοσ");
	EXPECT_EQ(FString("ς").ToUpper(), "Σ");
}

TEST(StringCase, FoldingDiffersFromLowering)
{
	EXPECT_EQ(FString("İ").FoldCase(), "İ");
	EXPECT_EQ(FString("ı").FoldCase(), "ı");
	EXPECT_EQ(FString("ı").ToUpper(), "I");

	// Cherokee folds to uppercase.
	EXPECT_EQ(FString("\uAB70").FoldCase(), "\u13A0");
	EXPECT_EQ(FString("\uAB70").ToLower(), "\uAB70");
	EXPECT_EQ(FString("\uAB70").ToUpper(), "\u13A0");

	EXPECT_EQ(FString("\u1C80").FoldCase(), "в");
	EXPECT_EQ(FString("ς").FoldCase(), "σ");
	EXPECT_EQ(FString("\u1E9E").FoldCase(), "ß");
	EXPECT_EQ(FString("\u212A").FoldCase(), "k");
	EXPECT_EQ(FString("ſ").FoldCase(), "s");
	EXPECT_EQ(FString("Straße").FoldCase(), "straße");
}

TEST(StringCase, ByteLengthChanges)
{
	// U+0250 grows (2 -> 3 bytes), U+0131 shrinks (2 -> 1).
	EXPECT_EQ(FString("\u0250\u0131").ToUpper(), "\u2C6FI");
	EXPECT_EQ(FString("\u212A").ToLower(), "k");
	EXPECT_EQ(FString("\u1E9E").ToLower(), "ß");

	// A full inline buffer whose uppercase no longer fits.
	FString grows = "\u0250aaaaaaaaaaaaaaaaaaaa";
	ASSERT_EQ(grows.Len(), 22u);
	ASSERT_FALSE(grows.IsHeapAllocated());
	grows.ToUpperInline();
	EXPECT_EQ(grows, "\u2C6FAAAAAAAAAAAAAAAAAAAA");
	EXPECT_EQ(grows.Len(), 23u);
	EXPECT_TRUE(grows.IsHeapAllocated());

	FString shrinks = "\u212A\u212A\u212A";
	shrinks.ToLowerInline();
	EXPECT_EQ(shrinks, "kkk");
}

TEST(StringCase, IllFormedBytesAndSurrogatesAreKept)
{
	EXPECT_EQ(FString("a\xFF" "b\xED\xA0\x80" "c").ToUpper(), "A\xFF" "B\xED\xA0\x80" "C");
	EXPECT_EQ(FString("A\xC3" "B").ToLower(), "a\xC3" "b");
	EXPECT_EQ(FString("\xE2\x82" "Ї").FoldCase(), "\xE2\x82" "ї");
}

TEST(StringCase, InlineAndRvalueForms)
{
	FString text = "Mixed Ґанок";
	EXPECT_EQ(text.ToUpperInline(), "MIXED ҐАНОК");
	EXPECT_EQ(text.ToLowerInline(), "mixed ґанок");
	EXPECT_EQ(text.FoldCaseInline(), "mixed ґанок");

	EXPECT_EQ(FString("Ґ").ToUpper(), "Ґ");
	EXPECT_EQ(FString("ґ").ToUpper(), "Ґ");
	EXPECT_EQ(FString("ΣΊΣΥΦΟΣ").FoldCase(), FString("σίσυφος").FoldCase());
	EXPECT_EQ(FString("ΣΊΣΥΦΟΣ").FoldCase(), "σίσυφοσ");
}


// ---------------------------------------------------------------------------------
// Concatenation (6.8)
// ---------------------------------------------------------------------------------

TEST(StringConcatenation, Values)
{
	const FString a = "abc";
	const FString b = "def";
	constexpr FStringLiteral literal = "lit";
	const std::string stdString = "std";
	const char* pointer = "ptr";

	EXPECT_EQ(a + b, "abcdef");
	EXPECT_EQ(a + "x", "abcx");
	EXPECT_EQ("x" + a, "xabc");
	EXPECT_EQ(a + 'c', "abcc");
	EXPECT_EQ('c' + a, "cabc");
	EXPECT_EQ(FStringView("v") + FStringView("w"), "vw");
	EXPECT_EQ(literal + "x", "litx");
	EXPECT_EQ("x" + literal, "xlit");
	EXPECT_EQ(literal + a, "litabc");
	EXPECT_EQ(stdString + a, "stdabc");
	EXPECT_EQ(a + stdString, "abcstd");
	EXPECT_EQ(a + pointer, "abcptr");
	EXPECT_EQ(pointer + a, "ptrabc");
	EXPECT_EQ(FString("ґ") + "ї" + 'x' + FString("є"), "ґїxє");
	EXPECT_EQ(a, "abc");
	EXPECT_EQ(b, "def");
}

TEST(StringConcatenation, RvaluesKeepTheirBuffer)
{
	const auto makeHeap = []()
	{
		FString text = "a heap string that is long enough";
		text.Reserve(100);
		return text;
	};
	const FString lvalue = "L";

	FString appendTo = makeHeap();
	const char* appendBlock = appendTo.GetData();
	const FString appended = std::move(appendTo) + "x";
	EXPECT_EQ(appended, "a heap string that is long enoughx");
	EXPECT_EQ(appended.GetData(), appendBlock);

	FString prependTo = makeHeap();
	const char* prependBlock = prependTo.GetData();
	const FString prepended = "x" + std::move(prependTo);
	EXPECT_EQ(prepended, "xa heap string that is long enough");
	EXPECT_EQ(prepended.GetData(), prependBlock);

	FString first = makeHeap();
	FString second = makeHeap();
	const char* firstBlock = first.GetData();
	const FString both = std::move(first) + std::move(second);
	EXPECT_EQ(both.GetData(), firstBlock);
	EXPECT_EQ(both.Len(), 66u);

	FString right = makeHeap();
	const char* rightBlock = right.GetData();
	const FString lvalueFirst = lvalue + std::move(right);
	EXPECT_EQ(lvalueFirst, "La heap string that is long enough");
	EXPECT_EQ(lvalueFirst.GetData(), rightBlock);

	FString withChar = makeHeap();
	const char* withCharBlock = withChar.GetData();
	const FString charAppended = std::move(withChar) + '!';
	EXPECT_EQ(charAppended.GetData(), withCharBlock);
	EXPECT_EQ(charAppended.Last(), '!');

	FString charFirst = makeHeap();
	const char* charFirstBlock = charFirst.GetData();
	const FString charPrepended = '!' + std::move(charFirst);
	EXPECT_EQ(charPrepended.GetData(), charFirstBlock);
	EXPECT_EQ(charPrepended.First(), '!');
}


// ---------------------------------------------------------------------------------
// Named constructors: Join, Repeat, hex, Escape
// ---------------------------------------------------------------------------------

TEST(StringBuilders, JoinAndJoinBy)
{
	const std::vector<FStringView> views = { "a", "b", "c" };
	EXPECT_EQ(FString::Join(views, ", "), "a, b, c");
	EXPECT_TRUE(FString::Join(std::vector<FStringView>(), ",").IsEmpty());
	EXPECT_EQ(FString::Join(std::vector<FString>{ FString("x") }, ","), "x");

	constexpr std::array<FStringLiteral, 3> names = { "Один", "Два", "Три" };
	EXPECT_EQ(FString::Join(names, "·"), "Один·Два·Три");
	const std::vector<std::string> stdStrings = { "s", "t" };
	EXPECT_EQ(FString::Join(stdStrings, "-"), "s-t");

	const std::vector<int32> numbers = { 1, 2, 3 };
	EXPECT_EQ(FString::JoinBy(numbers, "+", [](int32 Value) { return FString::FromNumber(Value); }), "1+2+3");

	const std::vector<char> letters = { 'x', 'y', 'z' };
	EXPECT_EQ(FString::JoinBy(letters, "", [](char Letter) { return Letter; }), "xyz");

	struct SNamed
	{
		FStringView Name;
	};
	const std::vector<SNamed> named = { { "Ґанок" }, { "Їжак" } };
	EXPECT_EQ(FString::JoinBy(named, " & ", [](const SNamed& Item) { return Item.Name; }), "Ґанок & Їжак");
}

TEST(StringBuilders, RepeatAndHex)
{
	EXPECT_EQ(FString::Repeat("ab", 3), "ababab");
	EXPECT_TRUE(FString::Repeat("ab", 0).IsEmpty());
	EXPECT_TRUE(FString::Repeat("", 5).IsEmpty());
	EXPECT_EQ(FString::Repeat("ї", 2), "її");

	const std::array<uint8, 3> bytes = { 0x00, 0xAB, 0x1F };
	EXPECT_EQ(FString::FromHexBytes(bytes), "00AB1F");
	EXPECT_EQ(FString::FromHexBytes(bytes, false), "00ab1f");
	EXPECT_TRUE(FString::FromHexBytes(std::span<const uint8>()).IsEmpty());

	std::array<uint8, 3> decoded = {};
	EXPECT_TRUE(FString::FromHexBytes(bytes).DecodeHex(decoded));
	EXPECT_EQ(decoded, bytes);
}

TEST(StringBuilders, EscapeControlsIllFormedAndInvisibleCharacters)
{
	using namespace frt::literals;

	EXPECT_EQ(FString::Escape("a\tb\n\r\"\\"), "a\\tb\\n\\r\\\"\\\\");
	EXPECT_EQ(FString::Escape("a\0b"_sv), "a\\0b");
	EXPECT_EQ(LowerHexEscapes(FString::Escape("\x01\x1F\x7F")), "\\x01\\x1f\\x7f");
	EXPECT_EQ(LowerHexEscapes(FString::Escape("\xFF")), "\\xff");
	EXPECT_EQ(LowerHexEscapes(FString::Escape("\xE2\x82" "a")), "\\xe2\\x82a");
	EXPECT_EQ(LowerHexEscapes(FString::Escape("\xED\xA0\x80")), "\\xed\\xa0\\x80");
	EXPECT_EQ(FString::Escape("ї漢😀"), "ї漢😀");

	// C1 controls, line/paragraph separators and bidi controls can hide or reorder text in logs.
	const std::array<std::pair<FStringView, FStringView>, 8> invisible =
	{{
		{ "\xC2\x85", "\\xc2\\x85" },
		{ "\xC2\x9B", "\\xc2\\x9b" },
		{ "\u2028", "\\xe2\\x80\\xa8" },
		{ "\u2029", "\\xe2\\x80\\xa9" },
		{ "\u202E", "\\xe2\\x80\\xae" },
		{ "\u2066", "\\xe2\\x81\\xa6" },
		{ "\u061C", "\\xd8\\x9c" },
		{ "\u200E", "\\xe2\\x80\\x8e" },
	}};
	for (const std::pair<FStringView, FStringView>& item : invisible)
	{
		const FString escaped = FString::Escape(item.first);
		EXPECT_EQ(LowerHexEscapes(escaped), item.second);
		EXPECT_EQ(FString::Unescape(escaped), FString(item.first));
	}
}


// ---------------------------------------------------------------------------------
// Maps and sets (6.11)
// ---------------------------------------------------------------------------------

TEST(StringMap, HeterogeneousLookupDoesNotAllocate)
{
	TStringMap<int32> map;
	const FString longKey = "a key that is definitely longer than inline";
	map.try_emplace(FString("short"), 1);
	map.try_emplace(longKey, 2);
	map.try_emplace(FString(FStringView("ключ")), 3);

	const std::string stdShort = "short";
	const std::string stdLong = longKey.ToStdString();
	const FStringView viewLong = longKey;

	const uint64 before = GetAllocations();
	EXPECT_EQ(map.find(FStringView("short"))->second, 1);
	EXPECT_EQ(map.find("short")->second, 1);
	EXPECT_EQ(map.find(stdShort)->second, 1);
	EXPECT_EQ(map.find(viewLong)->second, 2);
	EXPECT_EQ(map.find(stdLong)->second, 2);
	EXPECT_EQ(map.find("a key that is definitely longer than inline")->second, 2);
	EXPECT_TRUE(map.contains("ключ"));
	EXPECT_FALSE(map.contains("КЛЮЧ"));
	EXPECT_EQ(map.count(FStringView("missing")), 0u);
	EXPECT_EQ(GetAllocations(), before);
}

TEST(StringMap, IgnoreCaseMapAndSets)
{
	TStringMapIgnoreCase<int32> map;
	map.try_emplace("Привіт", 1);
	map.try_emplace("Ґанок", 2);
	map.try_emplace("\u212A", 3);

	EXPECT_EQ(map.find("ПРИВІТ")->second, 1);
	EXPECT_EQ(map.find("ґАНОК")->second, 2);
	EXPECT_EQ(map.find("k")->second, 3);
	EXPECT_EQ(map.find("K")->second, 3);
	EXPECT_FALSE(map.try_emplace("привіт", 9).second);
	EXPECT_EQ(map.size(), 3u);

	FStringSet set;
	set.insert("alpha");
	set.emplace("бета");
	EXPECT_TRUE(set.contains(FStringView("alpha")));
	EXPECT_TRUE(set.contains("бета"));
	EXPECT_FALSE(set.contains("Alpha"));

	FStringSetIgnoreCase caseless;
	caseless.insert("Alpha");
	caseless.insert("БЕТА");
	EXPECT_TRUE(caseless.contains("ALPHA"));
	EXPECT_TRUE(caseless.contains("бета"));
	EXPECT_FALSE(caseless.insert("alpha").second);
}

TEST(StringMap, PathMapComparesTheNtfsWay)
{
	TPathMap<int32> paths;
	EXPECT_TRUE(paths.try_emplace("C:/Dir/A.png", 1).second);
	EXPECT_FALSE(paths.try_emplace("c:/dir/a.PNG", 2).second);
	EXPECT_EQ(paths.find("c:/DIR/a.png")->second, 1);

	// BMP letters are upper-cased, Cyrillic included...
	EXPECT_TRUE(paths.try_emplace("тест.png", 3).second);
	EXPECT_FALSE(paths.try_emplace("ТЕСТ.PNG", 4).second);

	// ...but U+212A is not 'k' on NTFS, and supplementary-plane letters are never folded.
	EXPECT_TRUE(paths.try_emplace("\u212A.png", 5).second);
	EXPECT_TRUE(paths.try_emplace("k.png", 6).second);
	EXPECT_TRUE(paths.try_emplace("\U00010400.png", 7).second);
	EXPECT_TRUE(paths.try_emplace("\U00010428.png", 8).second);
	EXPECT_EQ(paths.size(), 6u);

	EXPECT_EQ(SPathHash{}("C:/Dir/A.png"), SPathHash{}("c:/dir/a.PNG"));
	EXPECT_TRUE(SPathEqual{}("C:/Dir/A.png", "c:/dir/a.PNG"));
	EXPECT_FALSE(SPathEqual{}("\u212A", "k"));
	EXPECT_FALSE(SPathEqual{}("\U00010400", "\U00010428"));
}

TEST(StringMap, StringsInTArray)
{
	FRT_TEST_MEMORY_POOL();

	TArray<FString> array;
	std::vector<std::string> reference;
	for (uint32 i = 0; i < 100; ++i)
	{
		FString item = i % 2 == 0 ? FString::FromNumber(i) : FString::Format("heap string number {} з текстом", i);
		reference.push_back(item.ToStdString());
		array.Add(std::move(item));
	}
	ASSERT_EQ(array.Count(), 100u);
	EXPECT_TRUE(array.Contains("42"));
	EXPECT_EQ(array.Find(FString("42")), 42u);

	array.RemoveAt(0);
	reference.erase(reference.begin());
	array.Insert(FString("inserted at the front, longer than inline"), 0);
	reference.insert(reference.begin(), "inserted at the front, longer than inline");

	ASSERT_EQ(array.Count(), reference.size());
	for (uint32 i = 0; i < array.Count(); ++i)
	{
		EXPECT_EQ(array[i], reference[i]);
	}
}


// ---------------------------------------------------------------------------------
// FormatToBuffer (6.7)
// ---------------------------------------------------------------------------------

TEST(StringFormatToBuffer, FitsWithoutAllocating)
{
	char buffer[32];
	const uint64 before = GetAllocations();
	const FStringView text = FormatToBuffer(buffer, "x={} {}", 5, "ї");
	EXPECT_EQ(GetAllocations(), before);

	EXPECT_EQ(text, "x=5 ї");
	EXPECT_EQ(text.GetData(), static_cast<const char*>(buffer));
	EXPECT_EQ(buffer[text.Len()], '\0');
	EXPECT_STREQ(text.GetData(), "x=5 ї");
}

TEST(StringFormatToBuffer, TruncatesLabelSafely)
{
	const uint64 before = GetAllocations();

	// The cut would land inside "ж".
	char six[6];
	const FStringView cutCodePoint = FormatToBuffer(six, "{}", "abїжк");
	EXPECT_EQ(cutCodePoint, "abї");
	EXPECT_EQ(six[cutCodePoint.Len()], '\0');

	// The cut would land inside the combining mark: its base goes too.
	char five[5];
	const FStringView cutCombining = FormatToBuffer(five, "{}", "abe\u0301x");
	EXPECT_EQ(cutCombining, "ab");
	EXPECT_EQ(five[cutCombining.Len()], '\0');

	char one[1];
	const FStringView nothing = FormatToBuffer(one, "{}", "abc");
	EXPECT_TRUE(nothing.IsEmpty());
	EXPECT_EQ(one[0], '\0');

	EXPECT_EQ(GetAllocations(), before);

	// Same result as formatting in full and applying Truncate.
	EXPECT_EQ(cutCodePoint, FString::Format("{}", "abїжк").Truncate(5));
	EXPECT_EQ(cutCombining, FString::Format("{}", "abe\u0301x").Truncate(4));
}
