// ParseJson (Reflect/DataTreeJson.h): the reader of the JSON lines Text/JsonWriter.h writes. Whatever the writer
// writes - quotes, backslashes, control characters, non-ASCII text, an unpaired WTF-8 surrogate as its escape - comes
// back byte for byte; an escaped surrogate pair is one code point; the tree has the kinds and styles the typed
// accessors expect (strings quoted, numbers and booleans plain); malformed JSON fails with a position; random input
// never crashes.
#include <gtest/gtest.h>

#include <random>

#include "Containers/String.h"
#include "Diagnostics/Diagnostics.h"
#include "Reflect/DataTree.h"
#include "Reflect/DataTreeJson.h"
#include "Text/JsonWriter.h"

using namespace frt;
using namespace frt::reflect;


namespace
{
CDataTree Parse (FStringView Text)
{
	CDataTree tree;
	SDiagnostic error;
	EXPECT_TRUE(ParseJson(Text, tree, &error)) << error.Line << ":" << error.Column << " " << error.Message.CStr();
	return tree;
}

SDiagnostic ParseError (FStringView Text)
{
	CDataTree tree;
	SDiagnostic error;
	EXPECT_FALSE(ParseJson(Text, tree, &error)) << FString(Text).CStr();
	EXPECT_TRUE(tree.IsEmpty());
	EXPECT_FALSE(error.Message.IsEmpty());
	return error;
}

// Text through the writer and back through the reader.
FString RoundTrip (FStringView Text)
{
	const CDataTree tree = Parse(json::CJsonObject().AddString("v", Text).ToString().View());
	const std::optional<FStringView> value = tree.GetRoot().FindChild("v").AsText();
	return value.has_value() ? FString(*value) : FString("<none>");
}
}


TEST(DataTreeJsonTest, ObjectsArraysAndScalarsHaveTheirKindsAndStyles)
{
	const CDataTree tree = Parse("{\"s\":\"text\",\"i\":-42,\"d\":1.5e3,\"t\":true,\"f\":false,\"n\":null,\"a\":[1,\"x\",{}],\"o\":{\"k\":[]}}");
	const HDataNode root = tree.GetRoot();
	ASSERT_TRUE(root.IsMap());
	EXPECT_EQ(root.GetCount(), 8u);
	EXPECT_EQ(root.GetKey(0u), "s");
	EXPECT_EQ(root.FindChild("s").GetScalarStyle(), EScalarStyle::DoubleQuoted);
	EXPECT_EQ(root.FindChild("s").AsText().value_or("<none>"), FStringView("text"));
	EXPECT_EQ(root.FindChild("i").AsInt64(), std::optional<int64>(-42));
	EXPECT_EQ(root.FindChild("d").AsDouble(), std::optional<double>(1500.0));
	EXPECT_EQ(root.FindChild("t").AsBool(), std::optional<bool>(true));
	EXPECT_EQ(root.FindChild("f").AsBool(), std::optional<bool>(false));
	EXPECT_TRUE(root.FindChild("n").IsNull());
	EXPECT_TRUE(root.FindChild("a").IsSequence());
	EXPECT_EQ(root.Find("a.1").AsText().value_or("<none>"), FStringView("x"));
	EXPECT_TRUE(root.Find("a.2").IsMap());
	EXPECT_TRUE(root.Find("o.k").IsSequence());
	// A quoted number is text, as in YAML.
	EXPECT_FALSE(Parse("{\"i\":\"1\"}").GetRoot().FindChild("i").AsInt64().has_value());
	// Whitespace between tokens, and a UTF-8 byte order mark, are allowed.
	EXPECT_EQ(Parse("\xEF\xBB\xBF { \"k\" :\t[ 1 , 2 ] }\r\n").GetRoot().Find("k.1").AsInt64(), std::optional<int64>(2));
}

TEST(DataTreeJsonTest, WhatTheWriterWritesComesBackByteForByte)
{
	for (const FStringView text : {
		FStringView(""), FStringView("plain"), FStringView("say \"hi\""), FStringView("C:\\Program Files\\FRT"),
		FStringView("a\nb\rc\td\be\ff"), FStringView("\x01\x1f\x7f", 3u), FStringView("a\0b", 3u),
		FStringView("Гра 游戏 ї 😀"), FStringView("/Project/Тека/Rock.png") })
	{
		EXPECT_EQ(RoundTrip(text), FString(text));
	}
}

TEST(DataTreeJsonTest, AnEscapedLoneSurrogateBecomesItsWtf8Bytes)
{
	// The writer escapes a WTF-8 surrogate (a Windows name's unpaired one); yaml-cpp refuses such an escape.
	const FStringView lead("a\xED\xA0\x80" "b", 5u);
	const FStringView trail("\xED\xB0\x81", 3u);
	EXPECT_EQ(RoundTrip(lead), FString(lead));
	EXPECT_EQ(RoundTrip(trail), FString(trail));

	// An escaped pair is one code point (U+1F600), as every JSON reader reads it; upper-case hex is accepted.
	const FString pair = FString("{\"v\":\"") + "\x5C" "uD83D" "\x5C" "ude00" + "\"}";
	EXPECT_EQ(Parse(pair.View()).GetRoot().FindChild("v").AsText().value_or("<none>"), FStringView("😀"));
	// A lead escape followed by an escape that is no trail: two units, each its own.
	const FString split = FString("{\"v\":\"") + "\x5C" "ud83d" "\x5C" "u0041" + "\"}";
	EXPECT_EQ(Parse(split.View()).GetRoot().FindChild("v").AsText().value_or("<none>"), FStringView("\xED\xA0\xBD" "A", 4u));
}

TEST(DataTreeJsonTest, MalformedJsonFailsWithAPosition)
{
	EXPECT_EQ(ParseError("").Column, 1u);
	const SDiagnostic trailing = ParseError("{\"a\":1,}");
	EXPECT_EQ(trailing.Line, 1u);
	EXPECT_EQ(trailing.Column, 8u);
	EXPECT_EQ(ParseError("{\"a\":1}\n{\"b\":2}").Line, 2u);
	ParseError("{a:1}");
	ParseError("{\"a\" 1}");
	ParseError("[1 2]");
	ParseError("{\"a\":01}");
	ParseError("{\"a\":+1}");
	ParseError("{\"a\":1.}");
	ParseError("{\"a\":1e}");
	ParseError("{\"a\":tru}");
	ParseError("{\"a\":\"open}");
	ParseError("{\"a\":\"tab\there\"}");
	ParseError("{\"a\":\"\\q\"}");
	ParseError(FString(FString("{\"a\":\"") + "\x5C" "u12" + "\"}").View());
	ParseError("{\"a\":1,\"a\":2}");                       // duplicate key, as everywhere in CDataTree
	ParseError(FStringView("{\"a\":\"\xff\"}", 8u));       // not UTF-8
	ParseError("// comment\n{}");

	FString deep;
	for (uint32 i = 0u; i < 200u; ++i)
	{
		deep += '[';
	}
	ParseError(deep.View());
}

TEST(DataTreeJsonTest, RandomInputNeverCrashes)
{
	std::mt19937 random(1234u);
	constexpr char Alphabet[] = "{}[]:,\"\\ u0123456789abcdefeE+-.tfnrl\n\t";
	for (uint32 round = 0u; round < 5000u; ++round)
	{
		FString text;
		const uint32 length = random() % 40u;
		for (uint32 i = 0u; i < length; ++i)
		{
			text += Alphabet[random() % (sizeof(Alphabet) - 1u)];
		}
		CDataTree tree;
		SDiagnostic error;
		if (!ParseJson(text.View(), tree, &error))
		{
			EXPECT_GE(error.Line, 1u) << text.CStr();
			EXPECT_TRUE(tree.IsEmpty());
		}
	}
}
