// Core's JSON lines (Text/JsonWriter.h: AssetTool's --json output, design §13.1, and Rules.cache): every string
// comes out as an RFC 8259 string literal that any JSON reader takes back as the same text - quotes, backslashes
// (every Windows path an error or an owner names), control characters, non-ASCII text as UTF-8 - and engine text
// that is not UTF-8 still makes valid UTF-8 JSON: a WTF-8 surrogate (a Windows path's unpaired one) as its \u
// escape, a byte that is no UTF-8 at all as \ufffd. Objects keep their fields in the order they were added.
// Reflect/DataTreeJson.h reads such lines back (DataTreeJsonTest).
#include <gtest/gtest.h>

#include "TestCommon.h"

#include <limits>

#include "Containers/String.h"

#include "Text/JsonWriter.h"

using namespace frt;
using json::AppendJsonString;
using json::CJsonObject;


namespace
{
FString JsonString (FStringView Text)
{
	FString out;
	AppendJsonString(out, Text);
	return out;
}
}


TEST(JsonWriterTest, QuotesAndBackslashesAreEscaped)
{
	EXPECT_EQ(JsonString(""), "\"\"");
	EXPECT_EQ(JsonString("plain text"), "\"plain text\"");
	EXPECT_EQ(JsonString("say \"hi\""), "\"say \\\"hi\\\"\"");
	EXPECT_EQ(JsonString("C:\\Program Files\\FRT\\Editor-Development-Default.exe"),
		"\"C:\\\\Program Files\\\\FRT\\\\Editor-Development-Default.exe\"");
	EXPECT_EQ(JsonString("\\\\server\\share\\"), "\"\\\\\\\\server\\\\share\\\\\"");
	// '/' needs no escape, and gets none.
	EXPECT_EQ(JsonString("/Project/Content/Rock.png"), "\"/Project/Content/Rock.png\"");
}

TEST(JsonWriterTest, ControlCharactersAreEscaped)
{
	EXPECT_EQ(JsonString("a\nb\rc\td\be\ff"), "\"a\\nb\\rc\\td\\be\\ff\"");
	EXPECT_EQ(JsonString(FStringView("\x01\x1f", 2u)), "\"\\u0001\\u001f\"");
	// NUL inside the text is a character like any other.
	EXPECT_EQ(JsonString(FStringView("a\0b", 3u)), "\"a\\u0000b\"");
	// DEL is no control character to JSON: as it is.
	EXPECT_EQ(JsonString("\x7f"), "\"\x7f\"");
}

TEST(JsonWriterTest, NonAsciiTextStaysUtf8)
{
	EXPECT_EQ(JsonString("Гра 游戏 ї 😀"), "\"Гра 游戏 ї 😀\"");
}

TEST(JsonWriterTest, AnUnpairedSurrogateIsEscapedAndNeverCopied)
{
	// A Windows path may hold an unpaired surrogate; FString::FromPath keeps it as WTF-8 (U+D800 as ED A0 80),
	// which no UTF-8 reader accepts. JSON readers take \ud800 back as that surrogate.
	const FStringView lead("a\xED\xA0\x80" "b", 5u);
	EXPECT_EQ(JsonString(lead), "\"a\\ud800b\"");
	EXPECT_EQ(JsonString(FStringView("\xED\xB0\x81", 3u)), "\"\\udc01\"");
	// A pair spelled as two surrogates is no canonical WTF-8: the lead one is U+FFFD, the trail one stays.
	EXPECT_EQ(JsonString(FStringView("\xED\xA0\x80\xED\xB0\x80", 6u)), "\"\\ufffd\\udc00\"");
	// The output is valid UTF-8 either way.
	EXPECT_TRUE(JsonString(lead).View().IsValidUtf8());
}

TEST(JsonWriterTest, BytesThatAreNoUtf8BecomeTheReplacementCharacter)
{
	EXPECT_EQ(JsonString(FStringView("a\xff" "b", 3u)), "\"a\\ufffdb\"");
	// A truncated sequence is one unit, so one U+FFFD.
	EXPECT_EQ(JsonString(FStringView("\xE6\xB8", 2u)), "\"\\ufffd\"");
	EXPECT_EQ(JsonString(FStringView("\xC0\xAF", 2u)), "\"\\ufffd\\ufffd\"");
}

TEST(JsonWriterTest, ObjectsKeepTheirFieldsInOrder)
{
	EXPECT_EQ(CJsonObject().ToString(), "{}");

	CJsonObject object;
	object.AddString("guid", "9fc0d4010bbf28b4594072e72b8655ab").AddInt("exitCode", -4)
		.AddUInt("pid", std::numeric_limits<uint64>::max()).AddBool("writes", true).AddBool("bad", false);
	EXPECT_EQ(object.ToString(),
		"{\"guid\":\"9fc0d4010bbf28b4594072e72b8655ab\",\"exitCode\":-4,\"pid\":18446744073709551615,\"writes\":true,\"bad\":false}");

	// Names are escaped like values.
	EXPECT_EQ(CJsonObject().AddString("a\"b", "c").ToString(), "{\"a\\\"b\":\"c\"}");

	// AddFields appends another object's fields after these, and adds nothing for an empty one.
	CJsonObject error;
	error.AddString("error", "C:\\x owns /Project").AddInt("exitCode", 4);
	EXPECT_EQ(CJsonObject(error).AddFields(CJsonObject()).ToString(), "{\"error\":\"C:\\\\x owns /Project\",\"exitCode\":4}");
	EXPECT_EQ(CJsonObject(error).AddFields(CJsonObject().AddString("mount", "/Project")).ToString(),
		"{\"error\":\"C:\\\\x owns /Project\",\"exitCode\":4,\"mount\":\"/Project\"}");
	EXPECT_EQ(CJsonObject().AddFields(error).ToString(), error.ToString());
}
