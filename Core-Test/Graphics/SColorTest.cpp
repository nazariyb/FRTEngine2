// SColor's text forms: material files and the legacy material reader store colors as "#RRGGBBAA" or
// "RGBA(r, g, b, a)", so each form must read back what it writes, and text that is neither must
// leave the color alone.
#include <gtest/gtest.h>

#include "TestCommon.h"

#include <limits>

#include "Containers/String.h"
#include "Graphics/SColor.h"

using namespace frt;


TEST(Color, ToHexWritesEightHexDigits)
{
	EXPECT_EQ(SColor(1.0f, 0.5f, 0.0f, 1.0f).ToHex(), "#FF8000FF");
	EXPECT_EQ(SColor::White.ToHex(), "#FFFFFFFF");
	EXPECT_EQ(SColor::Black.ToHex(), "#000000FF");
	EXPECT_EQ(SColor(0.5f).ToString(true), "#808080FF");

	// Clamped to [0, 1], NaN as 0.
	const float nan = std::numeric_limits<float>::quiet_NaN();
	EXPECT_EQ(SColor(2.0f, -1.0f, nan, 0.25f).ToHex(), "#FF000040");
}

TEST(Color, FromHexReadsEveryLengthAndCase)
{
	const SColor short3 = SColor::FromHex("#f80");
	EXPECT_EQ(short3.R, 1.0f);
	EXPECT_FLOAT_EQ(short3.G, 136.0f / 255.0f);
	EXPECT_EQ(short3.B, 0.0f);
	EXPECT_EQ(short3.A, 1.0f);

	EXPECT_EQ(SColor::FromHex("FF8000").ToHex(), "#FF8000FF");
	EXPECT_EQ(SColor::FromHex("#ff800080").ToHex(), "#FF800080");
	EXPECT_FLOAT_EQ(SColor::FromHex("#00000080").A, 128.0f / 255.0f);

	// Anything else is White.
	constexpr FStringLiteral rejected[] = { "", "#", "#12345", "#FF80001", "#GG0000", "#FF 800", "##FF8000" };
	for (const FStringLiteral text : rejected)
	{
		EXPECT_EQ(SColor::FromHex(text).ToHex(), "#FFFFFFFF") << text.CStr();
	}
}

TEST(Color, HexRoundTripsEveryByte)
{
	for (uint32 i = 0; i < 256; ++i)
	{
		const FString hex = FString::Format("#{:02X}{:02X}{:02X}{:02X}", i, 255 - i, (i * 7) & 255, (i * 13) & 255);
		EXPECT_EQ(SColor::FromHex(hex).ToHex(), hex);
	}

	const FString hex = SColor(0.1f, 0.2f, 0.3f, 0.4f).ToHex();
	EXPECT_EQ(SColor::FromString(hex).ToHex(), hex);
}

TEST(Color, ToStringReadsBackToTheSameFloats)
{
	EXPECT_EQ(SColor(0.1f, 0.2f, 0.3f, 0.4f).ToString(), "RGBA(0.1, 0.2, 0.3, 0.4)");

	// Six significant digits, as an iostream writes them, would not read back to 1/3.
	const SColor written(1.0f / 3.0f, 0.7f, 1e-7f, 1.0f);
	const SColor read = SColor::FromString(written.ToString());
	EXPECT_EQ(read.R, written.R);
	EXPECT_EQ(read.G, written.G);
	EXPECT_EQ(read.B, written.B);
	EXPECT_EQ(read.A, written.A);
}

TEST(Color, FromStringSkipsWhitespaceAroundNumbers)
{
	const SColor color = SColor::FromString("RGBA( 0.5 ,0.25,  1 , 0 )");
	EXPECT_EQ(color.R, 0.5f);
	EXPECT_EQ(color.G, 0.25f);
	EXPECT_EQ(color.B, 1.0f);
	EXPECT_EQ(color.A, 0.0f);
}

TEST(Color, TextThatIsNoColorLeavesItUnchanged)
{
	constexpr FStringLiteral rejected[] = {
		"", "RGBA()", "RGBA(1, 2, 3)", "RGBA(1, 2, 3, 4, 5)", "RGBA(1, 2, 3, 4", "RGBA(1 2, 3, 4)",
		"RGBA(1, 2, 3, 4x)", "RGBA(inf, 0, 0, 1)", "RGBA(nan, 0, 0, 1)", "RGBA(1,, 2, 3)", "rgba(1, 2, 3, 4)",
		" RGBA(1, 2, 3, 4)", "#12345", "1, 2, 3, 4"
	};
	for (const FStringLiteral text : rejected)
	{
		SColor color(0.5f, 0.25f, 0.125f, 0.75f);
		EXPECT_FALSE(color.FillFromString(text)) << text.CStr();
		EXPECT_EQ(color.ToString(), "RGBA(0.5, 0.25, 0.125, 0.75)") << text.CStr();
		EXPECT_EQ(SColor::FromString(text).ToHex(), "#FFFFFFFF") << text.CStr();
	}
}
