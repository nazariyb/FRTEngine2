#include <gtest/gtest.h>

#include <format>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include "Containers/String.h"

using namespace frt;
using namespace frt::literals;


namespace
{
enum EPrintfTestEnum { PrintfTestValue = 65 };

// C varargs promote unscoped enums only: these would reach vsnprintf's %d at their own width.
enum class EPrintfByteEnum : uint8 { Value = 200 };
enum class EPrintfSignedEnum : int8 { Value = -5 };
enum class EPrintfWordEnum : uint16 { Value = 40000 };
}


// ---------------------------------------------------------------------------------
// FString::Format / AppendFormat (std::format underneath)
// ---------------------------------------------------------------------------------

TEST(StringFormat, FormatsEveryTextType)
{
	constexpr FStringLiteral literal = "lit";
	const FString heap = "a string long enough for the heap";

	EXPECT_EQ(FString::Format("{} {}", FString("a"), FStringView("b")), "a b");
	EXPECT_EQ(FString::Format("[{}]", literal), "[lit]");
	EXPECT_EQ(FString::Format("{}{}", "ґ", std::string("ї")), "ґї");
	EXPECT_EQ(FString::Format("<{}>", heap), "<a string long enough for the heap>");
	EXPECT_EQ(FString::Format("{}", 42), "42");
	EXPECT_TRUE(FString::Format("").IsEmpty());

	// The std::formatter specializations work with std::format itself.
	EXPECT_EQ(std::format("{}", heap), "a string long enough for the heap");
	EXPECT_EQ(std::format("{:>5}", FString("ab")), "   ab");
	EXPECT_EQ(std::format("{:>4}|{:<4}", literal, FStringView("v")), " lit|v   ");
}

TEST(StringFormat, WidthAndPrecisionAreDisplayColumns)
{
	// Precision cuts at code points, never inside one.
	EXPECT_EQ(FString::Format("{:.2}", "ґїє"), "ґї");
	EXPECT_EQ(FString::Format("{:.2}", FStringView("ґїє")), "ґї");
	// A CJK ideograph is two columns wide.
	EXPECT_EQ(FString::Format("{:>4}", "漢"), "  漢");
	EXPECT_EQ(FString::Format("{:*<6}", FStringView("їж")), "їж****");
	EXPECT_EQ(FString::Format("{:^5}", FString("ж")), "  ж  ");
}

TEST(StringFormat, NumericSpecs)
{
	EXPECT_EQ(FString::Format("{:.2f}", 3.14159), "3.14");
	EXPECT_EQ(FString::Format("{:02X}", 10), "0A");
	EXPECT_EQ(FString::Format("{:#x}", 255), "0xff");
	EXPECT_EQ(FString::Format("{:+d}", 5), "+5");
	EXPECT_EQ(FString::Format("{:08.3f}", -1.5), "-001.500");
}

TEST(StringFormat, AppendFormatChainsAndReusesScratch)
{
	FString text = "x=";
	FString& result = text.AppendFormat("{}", 5);
	EXPECT_EQ(&result, &text);
	EXPECT_EQ(text, "x=5");
	text.AppendFormat(", y={:.1f}", 2.5).AppendFormat("; {}", "ї");
	EXPECT_EQ(text, "x=5, y=2.5; ї");

	// The per-frame idiom: one scratch string, Clear() keeps its block.
	FString scratch;
	scratch.Reserve(64);
	const char* block = scratch.GetData();
	for (int32 frame = 0; frame < 3; ++frame)
	{
		scratch.Clear();
		scratch.AppendFormat("Frame {} of a reused scratch string", frame);
	}
	EXPECT_EQ(scratch, "Frame 2 of a reused scratch string");
	EXPECT_EQ(scratch.GetData(), block);
}


// ---------------------------------------------------------------------------------
// Printf / Appendf (vsnprintf underneath; widths and precisions are bytes)
// ---------------------------------------------------------------------------------

TEST(StringFormat, Printf)
{
	const FString key = "key";
	EXPECT_EQ(FString::Printf("%s=%d", key.CStr(), 5), "key=5");
	EXPECT_EQ(FString::Printf("%.3f", 1.5), "1.500");
	EXPECT_EQ(FString::Printf("%llu", (std::numeric_limits<uint64>::max)()), "18446744073709551615");
	EXPECT_EQ(FString::Printf("%c%c", 'o', 'k'), "ok");
	EXPECT_EQ(FString::Printf("%s", "Ґанок"), "Ґанок");
	EXPECT_EQ(FString::Printf("%d", PrintfTestValue), "65");
	EXPECT_EQ(FString::Printf("plain"), "plain");
	EXPECT_FALSE(FString::Printf("%p", static_cast<const void*>(&key)).IsEmpty());

	// Printf takes numbers, enums, C strings and void* only. The static_assert is not SFINAE-visible, so the
	// negative case was verified once by hand:
	// const FString wide = FString::Printf("%ls", L"x");   // error: Printf: numbers, enums, C strings ...
}

TEST(StringFormat, PrintfBeyondOneKilobyte)
{
	const FString padded = FString::Printf("%0*d", 1500, 7);
	EXPECT_EQ(padded.Len(), 1500u);
	EXPECT_EQ(padded.First(), '0');
	EXPECT_EQ(padded.Last(), '7');

	const FString longText = FString::Repeat("ї", 700);
	EXPECT_EQ(FString::Printf("[%s]", longText.CStr()), "[" + longText + "]");
}

TEST(StringFormat, Appendf)
{
	FString text = "n=";
	FString& result = text.Appendf("%d", 42);
	EXPECT_EQ(&result, &text);
	text.Appendf(" %s", "ok").Appendf("%c", '!');
	EXPECT_EQ(text, "n=42 ok!");
}

TEST(StringFormat, PrintfScopedEnumsArePromoted)
{
	// A scoped enum passes through "..." unpromoted, so %d would read an int where only its own bytes
	// were written. The values come from runtime words whose other bits are set, which is what a
	// register passed on as is would carry.
	const volatile uint32 packedByte = 0xABCDEFC8u;     // low byte 200
	const volatile uint32 packedSigned = 0x123456FBu;   // low byte -5
	const volatile uint32 packedWord = 0x89AB9C40u;     // low half 40000
	const EPrintfByteEnum byteValue = static_cast<EPrintfByteEnum>(packedByte);
	const EPrintfSignedEnum signedValue = static_cast<EPrintfSignedEnum>(packedSigned);
	const EPrintfWordEnum wordValue = static_cast<EPrintfWordEnum>(packedWord);

	EXPECT_EQ(FString::Printf("%d %d %d", byteValue, signedValue, wordValue), "200 -5 40000");
	EXPECT_EQ(FString::Printf("%d %d", EPrintfByteEnum::Value, EPrintfSignedEnum::Value), "200 -5");

	FString text;
	text.Appendf("%d|%d|%d", byteValue, signedValue, wordValue);
	EXPECT_EQ(text, "200|-5|40000");
}


// ---------------------------------------------------------------------------------
// Streaming into FString and out to std::ostream / gtest
// ---------------------------------------------------------------------------------

TEST(StringFormat, StreamChaining)
{
	FString text;
	text << "a" << static_cast<int8>(-5) << true << U'ї' << 'c' << 1.5f << FStringView("|") << FString("end");
	EXPECT_EQ(text, "a-5trueїc1.5|end");
}

TEST(StringFormat, GtestPrintsQuotedEscapedText)
{
	EXPECT_EQ(::testing::PrintToString(FString("a\"b\n")), "\"a\\\"b\\n\"");
	EXPECT_EQ(::testing::PrintToString(FString("ab")), "\"ab\"");
	EXPECT_EQ(::testing::PrintToString(FStringView("ї")), "\"ї\"");
	constexpr FStringLiteral literal = "lit";
	EXPECT_EQ(::testing::PrintToString(literal), "\"lit\"");

	const std::string illFormed = ::testing::PrintToString(FStringView("\xFF"));
	EXPECT_TRUE(illFormed == "\"\\xFF\"" || illFormed == "\"\\xff\"") << illFormed;

	EXPECT_EQ(::testing::PrintToString(std::vector<FString>{ FString("a"), FString("b") }), "{ \"a\", \"b\" }");
}

TEST(StringFormat, OstreamWritesTheBytes)
{
	constexpr FStringLiteral literal = "lit";
	std::ostringstream out;
	out << FStringView("ab") << '|' << "a\0b"_sv << '|' << FString("Ґ") << '|' << literal;
	EXPECT_EQ(out.str(), std::string("ab|") + std::string("a\0b", 3) + "|Ґ|lit");
}
