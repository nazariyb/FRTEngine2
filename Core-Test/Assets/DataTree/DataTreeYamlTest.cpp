// ParseYaml: YAML text to CDataTree through yaml-cpp's event parser. What it must keep that YAML::Node loses - key
// order, scalar styles, exact 1-based line and byte columns (non-ASCII text included) - and FRT's strict subset:
// duplicate keys, anchors, aliases, tags, complex keys, empty keys and second documents are errors with a position,
// hostile input (deep nesting, random bytes) fails cleanly, and nothing throws. Also the parse rate on §3.6 data
// tables (5k rows here; 50k as a DISABLED_ test), reported against §5.11's 8 MB/s per thread.
#include <gtest/gtest.h>

#include <cstdio>
#include <random>
#include <vector>

#include "Containers/String.h"
#include "Diagnostics/Diagnostics.h"
#include "Platform/Platform.h"
#include "Platform/Timestamp.h"
#include "Reflect/DataTree.h"
#include "Reflect/DataTreeYaml.h"

using namespace frt;
using namespace frt::reflect;


namespace
{
CDataTree Parse (FStringView Text)
{
	CDataTree tree;
	SDiagnostic error;
	EXPECT_TRUE(ParseYaml(Text, tree, &error)) << error.Line << ":" << error.Column << " " << error.Message.CStr();
	return tree;
}

SDiagnostic ParseError (FStringView Text)
{
	CDataTree tree;
	SDiagnostic error;
	EXPECT_FALSE(ParseYaml(Text, tree, &error)) << Text.GetData();
	EXPECT_TRUE(tree.IsEmpty());
	EXPECT_EQ(error.Severity, ESeverity::Error);
	EXPECT_FALSE(error.Message.IsEmpty());
	return error;
}

void ExpectPosition (const HDataNode& Node, uint32 Line, uint32 Column)
{
	EXPECT_EQ(Node.GetLine(), Line) << FString(Node.GetText()).CStr();
	EXPECT_EQ(Node.GetColumn(), Column) << FString(Node.GetText()).CStr();
}

/**
 * A data table in §3.6's format exactly: $type, $rowType, $rowVersion, Defaults, then Rows as a keyed mapping sorted
 * by key, one field per line, some rows pinning `$id: $<16 hex>` (plain scalars), some with a `$note`, some with an
 * `$asset` reference in a flow map. About 200 bytes per row, as the spec's 5k-row (1 MB) measurement.
 */
FString MakeDataTable (uint32 RowCount)
{
	FString text;
	text.Reserve(RowCount * 220u + 256u);
	text += "# Content/Data/Ammo.frt\n";
	text += "$type: frt.DataTable\n";
	text += "$rowType: Game.AmmoRow          # one reflected row struct per table\n";
	text += "$rowVersion: 2\n";
	text += "Defaults:                       # applied before every row\n";
	text += "  Penetration: 0.25\n";
	text += "  Rarity: Common\n";
	text += "Rows:\n";
	static constexpr const char* rarities[] = { "Common", "Rare", "Epic", "Legendary" };
	for (uint32 i = 0u; i < RowCount; ++i)
	{
		text.AppendFormat("  Ammo{:05}:\n", i);
		if (i % 10u == 3u)
		{
			text.AppendFormat("    $id: ${:016x}\n", 0x4c1f9e27a05d3b68ull ^ (static_cast<uint64>(i) * 0x9E3779B97F4A7C15ull));
		}
		if (i % 7u == 0u)
		{
			text.AppendFormat("    $note: \"Pellet damage, x{} pellets\"\n", i % 9u + 1u);
		}
		text.AppendFormat("    Damage: {}\n", 5u + i % 60u);
		text.AppendFormat("    Penetration: {}.{}\n", i % 3u, i % 100u);
		text.AppendFormat("    Range: {}\n", 100u + i % 900u);
		text.AppendFormat("    Rarity: {}\n", rarities[i % 4u]);
		text.AppendFormat("    DisplayName: \"Ammo type {}\"\n", i);
		if (i % 4u == 1u)
		{
			text.AppendFormat("    Tracer: {{ $asset: {:032x}, path: /Project/FX/Tracer{}.frt }}\n",
				(static_cast<uint64>(i) * 0x9E3779B97F4A7C15ull) | 1u, i % 16u);
		}
	}
	return text;
}

struct SParseRate
{
	double BestMilliseconds = 0.0;
	double MegabytesPerSecond = 0.0;
	uint32 Nodes = 0u;
};

SParseRate MeasureParse (const FString& Text, uint32 Rows, uint32 Attempts)
{
	SParseRate rate;
	rate.BestMilliseconds = 1e30;
	for (uint32 attempt = 0u; attempt < Attempts; ++attempt)
	{
		CDataTree tree;
		SDiagnostic error;
		const uint64 start = platform::ReadTimestamp();
		const bool bParsed = ParseYaml(Text.View(), tree, &error);
		const double milliseconds = platform::TimestampToMilliseconds(platform::ReadTimestamp() - start);
		EXPECT_TRUE(bParsed) << error.Line << ":" << error.Column << " " << error.Message.CStr();
		EXPECT_EQ(tree.Find("Rows").GetCount(), Rows);
		rate.Nodes = tree.GetNodeCount();
		rate.BestMilliseconds = milliseconds < rate.BestMilliseconds ? milliseconds : rate.BestMilliseconds;
	}
	rate.MegabytesPerSecond = static_cast<double>(Text.Len()) / 1e6 / (rate.BestMilliseconds / 1000.0);
	return rate;
}
}


// ---------------- Structure, order, positions ----------------

TEST(DataTreeYamlTest, MapsKeepSourceOrderAndNestedStructure)
{
	const CDataTree tree = Parse(
		"zeta: 1\n"
		"alpha:\n"
		"  - one\n"
		"  - { inner: true, list: [a, b] }\n"
		"mid: text\n");
	const HDataNode root = tree.GetRoot();
	ASSERT_TRUE(root.IsMap());
	ASSERT_EQ(root.GetCount(), 3u);
	EXPECT_EQ(root.GetKey(0u), "zeta");
	EXPECT_EQ(root.GetKey(1u), "alpha");
	EXPECT_EQ(root.GetKey(2u), "mid");
	EXPECT_EQ(root.GetCollectionStyle(), ECollectionStyle::Block);

	const HDataNode alpha = root.FindChild("alpha");
	ASSERT_TRUE(alpha.IsSequence());
	EXPECT_EQ(alpha.GetCollectionStyle(), ECollectionStyle::Block);
	EXPECT_EQ(alpha.GetValue(0u).GetText(), "one");
	EXPECT_EQ(tree.Find("alpha.1").GetCollectionStyle(), ECollectionStyle::Flow);
	EXPECT_EQ(tree.Find("alpha.1.inner").AsBool(), std::optional<bool>(true));
	EXPECT_EQ(tree.Find("alpha.1.list.1").GetText(), "b");
	EXPECT_EQ(tree.Find("alpha.1.list").GetCollectionStyle(), ECollectionStyle::Flow);
	EXPECT_EQ(tree.Find("zeta").AsInt64(), std::optional<int64>(1));
}

TEST(DataTreeYamlTest, EveryNodeAndKeyHasItsOneBasedLineAndColumn)
{
	const CDataTree tree = Parse(
		"a: 1\n"                    // 1
		"b:\n"                      // 2
		"  c: [x, 'y']\n"           // 3
		"  d: |\n"                  // 4
		"    text\n"                // 5
		"  e: >\n"                  // 6
		"    folded\n"              // 7
		"list:\n"                   // 8
		"- \"q\"\n"                 // 9
		"-\n");                     // 10
	const HDataNode root = tree.GetRoot();
	ExpectPosition(root, 1u, 1u);
	EXPECT_EQ(root.GetChild(0u).KeyLine, 1u);
	EXPECT_EQ(root.GetChild(0u).KeyColumn, 1u);
	ExpectPosition(tree.Find("a"), 1u, 4u);
	EXPECT_EQ(root.GetChild(1u).KeyLine, 2u);
	ExpectPosition(tree.Find("b"), 3u, 3u);                   // a block map starts at its first key
	EXPECT_EQ(tree.Find("b").GetChild(0u).KeyLine, 3u);
	EXPECT_EQ(tree.Find("b").GetChild(0u).KeyColumn, 3u);
	ExpectPosition(tree.Find("b.c"), 3u, 6u);
	ExpectPosition(tree.Find("b.c.0"), 3u, 7u);
	ExpectPosition(tree.Find("b.c.1"), 3u, 10u);
	ExpectPosition(tree.Find("b.d"), 4u, 6u);
	EXPECT_EQ(tree.Find("b.d").GetText(), "text\n");
	ExpectPosition(tree.Find("b.e"), 6u, 6u);
	EXPECT_EQ(tree.Find("b.e").GetText(), "folded\n");
	ExpectPosition(tree.Find("list"), 9u, 1u);
	ExpectPosition(tree.Find("list.0"), 9u, 3u);
	EXPECT_TRUE(tree.Find("list.1").IsNull());
}

TEST(DataTreeYamlTest, ColumnsCountBytesOfUtf8Text)
{
	const CDataTree tree = Parse(
		"ключ: значення\n"
		"key2: {α: 1, β: 2}\n"
		"漢字: \"引用\"\n");
	// "ключ" is 8 bytes: ':' is byte 9, the value starts at byte 11.
	ExpectPosition(tree.Find("ключ"), 1u, 11u);
	EXPECT_EQ(tree.Find("ключ").GetText(), "значення");
	ExpectPosition(tree.Find("key2.α"), 2u, 12u);
	EXPECT_EQ(tree.Find("key2").GetChild(1u).KeyColumn, 15u);
	ExpectPosition(tree.Find("key2.β"), 2u, 19u);
	// "漢字" is 6 bytes: the quoted value starts at byte 9.
	ExpectPosition(tree.Find("漢字"), 3u, 9u);
	EXPECT_EQ(tree.Find("漢字").GetText(), "引用");
	EXPECT_EQ(tree.Find("漢字").GetScalarStyle(), EScalarStyle::DoubleQuoted);
}

TEST(DataTreeYamlTest, LineEndingsAndByteOrderMarkDoNotShiftPositions)
{
	const CDataTree crlf = Parse("a: 1\r\nb: two\r\n");
	ExpectPosition(crlf.Find("b"), 2u, 4u);
	EXPECT_EQ(crlf.Find("b").GetText(), "two");

	const CDataTree bom = Parse("\xEF\xBB\xBF" "a: 1\nb: 2\n");
	ExpectPosition(bom.Find("a"), 1u, 4u);
	EXPECT_EQ(bom.Find("b").AsInt64(), std::optional<int64>(2));
}

TEST(DataTreeYamlTest, ScalarStylesAreDetected)
{
	const CDataTree tree = Parse(
		"plain: 1\n"
		"single: '1'\n"
		"double: \"1\"\n"
		"literal: |\n"
		"  1\n"
		"folded: >-\n"
		"  1\n"
		"emptyQuoted: ''\n"
		"inFlow: ['a', \"b\", c]\n");
	EXPECT_EQ(tree.Find("plain").GetScalarStyle(), EScalarStyle::Plain);
	EXPECT_EQ(tree.Find("single").GetScalarStyle(), EScalarStyle::SingleQuoted);
	EXPECT_EQ(tree.Find("double").GetScalarStyle(), EScalarStyle::DoubleQuoted);
	EXPECT_EQ(tree.Find("literal").GetScalarStyle(), EScalarStyle::Literal);
	EXPECT_EQ(tree.Find("folded").GetScalarStyle(), EScalarStyle::Folded);
	EXPECT_EQ(tree.Find("folded").GetText(), "1");
	EXPECT_EQ(tree.Find("emptyQuoted").GetScalarStyle(), EScalarStyle::SingleQuoted);
	EXPECT_TRUE(tree.Find("emptyQuoted").IsScalar());
	EXPECT_FALSE(tree.Find("emptyQuoted").IsNull());
	EXPECT_EQ(tree.Find("inFlow.0").GetScalarStyle(), EScalarStyle::SingleQuoted);
	EXPECT_EQ(tree.Find("inFlow.1").GetScalarStyle(), EScalarStyle::DoubleQuoted);
	EXPECT_EQ(tree.Find("inFlow.2").GetScalarStyle(), EScalarStyle::Plain);

	// '1' is text: only the plain 1 is a number.
	EXPECT_EQ(tree.Find("plain").AsInt64(), std::optional<int64>(1));
	SDiagnostic error;
	EXPECT_FALSE(tree.Find("single").AsInt64(&error).has_value());
	EXPECT_EQ(error.Line, 2u);
	EXPECT_EQ(error.Column, 9u);
	EXPECT_FALSE(tree.Find("double").AsInt64().has_value());
	EXPECT_FALSE(tree.Find("literal").AsInt64().has_value());
	EXPECT_EQ(tree.Find("single").AsText(), std::optional<FStringView>("1"));
}

TEST(DataTreeYamlTest, NullValuesAndNullSpelledKeys)
{
	const CDataTree tree = Parse(
		"empty:\n"
		"tilde: ~\n"
		"word: null\n"
		"null: 1\n"
		"~: 2\n"
		"\"quoted\": ''\n");
	EXPECT_TRUE(tree.Find("empty").IsNull());
	EXPECT_EQ(tree.Find("empty").GetKind(), EDataNodeKind::Null);
	EXPECT_TRUE(tree.Find("tilde").IsNull());
	EXPECT_TRUE(tree.Find("word").IsNull());
	// A key spelled like null is that text.
	EXPECT_EQ(tree.GetRoot().FindChild("null").AsInt64(), std::optional<int64>(1));
	EXPECT_EQ(tree.GetRoot().FindChild("~").AsInt64(), std::optional<int64>(2));
	EXPECT_FALSE(tree.Find("empty").AsText().has_value());

	const SDiagnostic error = ParseError("a: 1\n: 2\n");
	EXPECT_EQ(error.Line, 2u);
	EXPECT_TRUE(error.Message.Contains("empty")) << error.Message.CStr();
}

TEST(DataTreeYamlTest, BlankTextGivesAnEmptyTree)
{
	CDataTree tree = Parse("a: 1");
	SDiagnostic error;
	EXPECT_TRUE(ParseYaml("", tree, &error));
	EXPECT_TRUE(tree.IsEmpty());
	EXPECT_TRUE(ParseYaml("# only a comment\n\n   \n", tree, &error));
	EXPECT_TRUE(tree.IsEmpty());
	EXPECT_TRUE(ParseYaml("\xEF\xBB\xBF", tree, &error));
	EXPECT_TRUE(tree.IsEmpty());

	// An explicit empty document has a null root; a document end marker is fine.
	EXPECT_TRUE(ParseYaml("---\n", tree, &error));
	ASSERT_FALSE(tree.IsEmpty());
	EXPECT_TRUE(tree.GetRoot().IsNull());
	EXPECT_TRUE(ParseYaml("a: 1\n...\n", tree, &error)) << error.Message.CStr();
	EXPECT_EQ(tree.Find("a").AsInt64(), std::optional<int64>(1));

	// A scalar document.
	EXPECT_TRUE(ParseYaml("just text", tree, &error));
	EXPECT_EQ(tree.GetRoot().GetText(), "just text");
}

TEST(DataTreeYamlTest, NonAsciiKeysAndValuesSurvive)
{
	const CDataTree tree = Parse(
		"Текстура: Камінь.png\n"
		"名前: { 値: \"γειά σου\", emoji: \"😀 ok\" }\n"
		"list: [ü, ñ, 'ß']\n");
	EXPECT_EQ(tree.Find("Текстура").GetText(), "Камінь.png");
	EXPECT_EQ(tree.Find("名前.値").GetText(), "γειά σου");
	EXPECT_EQ(tree.Find("名前.emoji").GetText(), "😀 ok");
	EXPECT_EQ(tree.Find("list.2").GetText(), "ß");

	// Escapes in double quotes decode to UTF-8.
	const CDataTree escaped = Parse("a: \"\\u0416\\t\\\"x\\\"\"\n");
	EXPECT_EQ(escaped.Find("a").GetText(), "Ж\t\"x\"");
}


// ---------------- FRT's subset: errors with positions ----------------

TEST(DataTreeYamlTest, DuplicateKeysAreErrorsAtTheSecondOccurrence)
{
	SDiagnostic error = ParseError(
		"name: a\n"
		"size: 1\n"
		"name: b\n");
	EXPECT_EQ(error.Line, 3u);
	EXPECT_EQ(error.Column, 1u);
	EXPECT_TRUE(error.Message.Contains("duplicate key 'name'")) << error.Message.CStr();
	EXPECT_TRUE(error.Message.Contains("line 1")) << error.Message.CStr();

	error = ParseError("outer:\n  x: 1\n  y: 2\n  x: 3\n");
	EXPECT_EQ(error.Line, 4u);
	EXPECT_EQ(error.Column, 3u);

	error = ParseError("flow: {a: 1, b: 2, a: 3}\n");
	EXPECT_EQ(error.Line, 1u);
	EXPECT_EQ(error.Column, 20u);

	// A table with many rows (the sorted check), the repeat near the end.
	FString rows("Rows:\n");
	for (uint32 i = 0u; i < 500u; ++i)
	{
		rows.AppendFormat("  Row{}: {}\n", i, i);
	}
	rows += "  Row17: again\n";
	error = ParseError(rows.View());
	EXPECT_EQ(error.Line, 502u);
	EXPECT_TRUE(error.Message.Contains("'Row17'")) << error.Message.CStr();

	// Different case is a different key.
	Parse("Name: a\nname: b\n");
}

TEST(DataTreeYamlTest, AnchorsAliasesAndTagsAreRefused)
{
	SDiagnostic error = ParseError("a: &shared 1\nb: *shared\n");
	EXPECT_EQ(error.Line, 1u);
	EXPECT_TRUE(error.Message.Contains("anchor")) << error.Message.CStr();

	error = ParseError("base: &b {x: 1}\n");
	EXPECT_TRUE(error.Message.Contains("anchor")) << error.Message.CStr();

	error = ParseError("a: !!str 1\n");
	EXPECT_EQ(error.Line, 1u);
	EXPECT_EQ(error.Column, 4u);
	EXPECT_TRUE(error.Message.Contains("tag")) << error.Message.CStr();

	error = ParseError("a: !custom value\n");
	EXPECT_TRUE(error.Message.Contains("tag")) << error.Message.CStr();
	error = ParseError("a: ! value\n");
	EXPECT_TRUE(error.Message.Contains("tag")) << error.Message.CStr();
	error = ParseError("a: !!map {x: 1}\n");
	EXPECT_TRUE(error.Message.Contains("tag")) << error.Message.CStr();
	error = ParseError("a: !!seq [1]\n");
	EXPECT_TRUE(error.Message.Contains("tag")) << error.Message.CStr();

	// yaml-cpp itself rejects an alias to an unknown anchor; still an error with a position.
	error = ParseError("b: *nowhere\n");
	EXPECT_EQ(error.Line, 1u);
}

TEST(DataTreeYamlTest, ComplexKeysAndSecondDocumentsAreRefused)
{
	SDiagnostic error = ParseError("? [a, b]\n: 1\n");
	EXPECT_TRUE(error.Message.Contains("key must be a scalar")) << error.Message.CStr();
	error = ParseError("{a: 1}: 2\n");
	EXPECT_TRUE(error.Message.Contains("key must be a scalar")) << error.Message.CStr();

	error = ParseError("a: 1\n---\nb: 2\n");
	EXPECT_EQ(error.Line, 2u);
	EXPECT_TRUE(error.Message.Contains("second YAML document")) << error.Message.CStr();
}

TEST(DataTreeYamlTest, MalformedYamlReportsYamlCppPosition)
{
	SDiagnostic error = ParseError("a: [1, 2\nb: 3\n");
	EXPECT_GT(error.Line, 0u);

	// yaml-cpp 0.8.0 accepts a quote left open at the end of the text when a line break precedes the end; ParseYaml
	// does not.
	error = ParseError("a: 'unterminated\n");
	EXPECT_EQ(error.Line, 1u);
	EXPECT_EQ(error.Column, 4u);
	EXPECT_TRUE(error.Message.Contains("not closed")) << error.Message.CStr();
	error = ParseError("a: \"unterminated \\\"\n\n");
	EXPECT_EQ(error.Column, 4u);
	error = ParseError("a: 'it''s\n");
	EXPECT_EQ(error.Column, 4u);
	error = ParseError("a: 'x'\nb: 'open");
	EXPECT_EQ(error.Line, 2u);

	// Closed quotes, escaped quotes inside, and multi-line quoted scalars are fine.
	const CDataTree closed = Parse("a: 'it''s'\nb: \"say \\\"hi\\\"\"\nc: 'two\n  lines'\nd: \"\\\\\"\n");
	EXPECT_EQ(closed.Find("a").GetText(), "it's");
	EXPECT_EQ(closed.Find("b").GetText(), "say \"hi\"");
	EXPECT_EQ(closed.Find("c").GetText(), "two lines");
	EXPECT_EQ(closed.Find("d").GetText(), "\\");

	error = ParseError("key: value\n bad indent: here\n  - x\n");
	EXPECT_GT(error.Line, 0u);

	error = ParseError("a: b: c\n");
	EXPECT_EQ(error.Line, 1u);
	EXPECT_GT(error.Column, 0u);
}

TEST(DataTreeYamlTest, DeepNestingStopsAtTheDepthLimit)
{
	// Unbalanced flow collections far past the builder's limit (and past yaml-cpp's own guard of 500): an error,
	// whichever rule yaml-cpp's reading reaches first, and no stack overflow.
	FString deep;
	FString deepMaps;
	for (uint32 i = 0u; i < 100'000u; ++i)
	{
		deep += '[';
		deepMaps += "{a: ";
	}
	SDiagnostic error = ParseError(deep.View());
	EXPECT_EQ(error.Line, 1u);
	error = ParseError(deepMaps.View());
	EXPECT_EQ(error.Line, 1u);

	// Balanced, just past the limit.
	FString balanced;
	for (uint32 i = 0u; i <= CDataTreeBuilder::DefaultMaxDepth; ++i)
	{
		balanced += '[';
	}
	for (uint32 i = 0u; i <= CDataTreeBuilder::DefaultMaxDepth; ++i)
	{
		balanced += ']';
	}
	error = ParseError(balanced.View());
	EXPECT_EQ(error.Line, 1u);
	EXPECT_EQ(error.Column, CDataTreeBuilder::DefaultMaxDepth + 1u);
	EXPECT_TRUE(error.Message.Contains("deeper")) << error.Message.CStr();

	// At the limit: fine.
	FString atLimit;
	for (uint32 i = 0u; i < CDataTreeBuilder::DefaultMaxDepth; ++i)
	{
		atLimit += '[';
	}
	for (uint32 i = 0u; i < CDataTreeBuilder::DefaultMaxDepth; ++i)
	{
		atLimit += ']';
	}
	Parse(atLimit.View());

	// Block maps, one level per line.
	FString blocks;
	for (uint32 i = 0u; i < 300u; ++i)
	{
		blocks += FString::Repeat(" ", i);
		blocks.AppendFormat("k{}:\n", i);
	}
	error = ParseError(blocks.View());
	EXPECT_TRUE(error.Message.Contains("deeper")) << error.Message.CStr();
}

TEST(DataTreeYamlTest, HostileInputNeverCrashesOrThrows)
{
	// Random text from YAML's own punctuation, quotes, escapes, invalid UTF-8 and NUL bytes: every outcome is a
	// tree or an error with a message.
	static constexpr const char* pieces[] = {
		"a", "b: ", ":", "- ", "[", "]", "{", "}", ",", "'", "\"", "\\", "|", ">", "&x ", "*x", "!", "!!str ", "?", "#",
		"\n", "\r\n", "  ", "\t", "---\n", "...\n", "%YAML 1.2\n", "~", "null", "\xFF", "\xC3", "\xE2\x82", "😀", "ключ",
		"\\u12", "\"\\x", "@", "`", "%", "1e999", "-.inf",
	};
	std::mt19937 random(5151u);
	for (uint32 iteration = 0u; iteration < 3000u; ++iteration)
	{
		FString text;
		const uint32 count = random() % 40u;
		for (uint32 i = 0u; i < count; ++i)
		{
			text += pieces[random() % std::size(pieces)];
		}
		if (random() % 8u == 0u)
		{
			text.AppendChar('\0');
			text += "after: nul";
		}
		CDataTree tree;
		SDiagnostic error;
		if (!ParseYaml(text.View(), tree, &error))
		{
			EXPECT_FALSE(error.Message.IsEmpty()) << text.CStr();
			EXPECT_TRUE(tree.IsEmpty());
		}
		else
		{
			// Whatever parses round-trips through the binary form.
			std::vector<uint8> bytes;
			tree.Serialize(bytes);
			CDataTree decoded;
			EXPECT_TRUE(decoded.Deserialize(bytes));
			EXPECT_TRUE(decoded == tree);
		}
	}
}


// ---------------- Round trips ----------------

TEST(DataTreeYamlTest, ParsedTreesRoundTripThroughTheBinaryForm)
{
	const FString table = MakeDataTable(200u);
	const CDataTree tree = Parse(table.View());
	std::vector<uint8> bytes;
	tree.Serialize(bytes);
	CDataTree decoded;
	SDiagnostic error;
	ASSERT_TRUE(decoded.Deserialize(bytes, &error)) << error.Message.CStr();
	EXPECT_TRUE(decoded == tree);
	std::vector<uint8> again;
	decoded.Serialize(again);
	EXPECT_EQ(again, bytes);

	// Parsing the same text twice gives equal trees and equal bytes.
	const CDataTree second = Parse(table.View());
	EXPECT_TRUE(second == tree);
}

TEST(DataTreeYamlTest, DataTableFormatParsesAsTheSpecDescribes)
{
	const FString table = MakeDataTable(20u);
	const CDataTree tree = Parse(table.View());
	EXPECT_EQ(tree.Find("$type").GetText(), "frt.DataTable");
	EXPECT_EQ(tree.Find("$rowType").GetText(), "Game.AmmoRow");
	EXPECT_EQ(tree.Find("$rowVersion").AsInt64(), std::optional<int64>(2));
	EXPECT_EQ(tree.Find("Defaults.Penetration").AsDouble(), std::optional<double>(0.25));
	const HDataNode rows = tree.Find("Rows");
	ASSERT_EQ(rows.GetCount(), 20u);
	EXPECT_EQ(rows.GetKey(0u), "Ammo00000");

	// $id: a plain scalar starting with '$'.
	const HDataNode id = tree.Find("Rows.Ammo00003.$id");
	EXPECT_EQ(id.GetScalarStyle(), EScalarStyle::Plain);
	EXPECT_EQ(id.GetText().Len(), 17u);
	EXPECT_TRUE(id.GetText().StartsWith('$'));

	// $asset reference in a flow map.
	const HDataNode tracer = tree.Find("Rows.Ammo00001.Tracer");
	EXPECT_EQ(tracer.GetCollectionStyle(), ECollectionStyle::Flow);
	EXPECT_EQ(tracer.FindChild("$asset").GetText().Len(), 32u);
	EXPECT_EQ(tracer.FindChild("path").GetText(), "/Project/FX/Tracer1.frt");
	EXPECT_EQ(tree.Find("Rows.Ammo00000.$note").GetScalarStyle(), EScalarStyle::DoubleQuoted);
	EXPECT_EQ(tree.Find("Rows.Ammo00002.Damage").AsInt64(), std::optional<int64>(7));
}


// ---------------- Benchmark ----------------

// §5.11: "YAML parse + gather, worker: 8 MB/s per thread (yaml-cpp, measured 8.5)". This measures parse + tree
// building (no gather yet) of a 5k-row table, the best of three, and prints it. Timings mean something in optimized
// builds only; Debug prints them without judging. Like the other budget tests it fails only on a clear regression
// (half the rate) unless FRT_PERF_STRICT=1.
TEST(DataTreeYamlTest, FiveThousandRowTableParsesAtTheBudgetRate)
{
	constexpr bool bOptimized = !FRT_BUILD_DEBUG;
	const bool bStrict = platform::GetEnvironmentValue("FRT_PERF_STRICT").value_or(FString()) == "1";
	const double slack = bStrict ? 1.0 : 2.0;
	constexpr uint32 rows = 5000u;

	const FString table = MakeDataTable(rows);
	const SParseRate rate = MeasureParse(table, rows, bOptimized ? 3u : 1u);
	std::printf("[ perf     ] 5k-row table: %.2f MB in %.1f ms = %.2f MB/s (%u nodes; budget 8 MB/s per thread)\n",
		static_cast<double>(table.Len()) / 1e6, rate.BestMilliseconds, rate.MegabytesPerSecond, rate.Nodes);
	RecordProperty("Bytes", static_cast<int>(table.Len()));
	RecordProperty("Milliseconds", FString::Format("{:.2f}", rate.BestMilliseconds).CStr());
	RecordProperty("MegabytesPerSecond", FString::Format("{:.2f}", rate.MegabytesPerSecond).CStr());
	if (bOptimized)
	{
		EXPECT_GE(rate.MegabytesPerSecond * slack, 8.0) << (bStrict ? "the budget" : "half the budget");
	}

	// The binary tree cache's rates, for the record (what a re-gather reads instead of YAML).
	const CDataTree tree = Parse(table.View());
	std::vector<uint8> bytes;
	uint64 start = platform::ReadTimestamp();
	tree.Serialize(bytes);
	const double serializeMs = platform::TimestampToMilliseconds(platform::ReadTimestamp() - start);
	CDataTree decoded;
	start = platform::ReadTimestamp();
	EXPECT_TRUE(decoded.Deserialize(bytes));
	const double deserializeMs = platform::TimestampToMilliseconds(platform::ReadTimestamp() - start);
	std::printf("[ perf     ] 5k-row tree binary form: %.2f MB, serialize %.2f ms, deserialize %.2f ms\n",
		static_cast<double>(bytes.size()) / 1e6, serializeMs, deserializeMs);
	RecordProperty("BinaryBytes", static_cast<int>(bytes.size()));
	RecordProperty("SerializeMilliseconds", FString::Format("{:.2f}", serializeMs).CStr());
	RecordProperty("DeserializeMilliseconds", FString::Format("{:.2f}", deserializeMs).CStr());
}

// The 50k-row table (about 10 MB): run on demand with --gtest_also_run_disabled_tests.
TEST(DataTreeYamlTest, DISABLED_FiftyThousandRowTableParseRate)
{
	constexpr uint32 rows = 50'000u;
	const FString table = MakeDataTable(rows);
	const SParseRate rate = MeasureParse(table, rows, 2u);
	std::printf("[ perf     ] 50k-row table: %.2f MB in %.1f ms = %.2f MB/s (%u nodes; budget 8 MB/s per thread)\n",
		static_cast<double>(table.Len()) / 1e6, rate.BestMilliseconds, rate.MegabytesPerSecond, rate.Nodes);
	RecordProperty("Bytes", static_cast<int>(table.Len()));
	RecordProperty("Milliseconds", FString::Format("{:.2f}", rate.BestMilliseconds).CStr());
	RecordProperty("MegabytesPerSecond", FString::Format("{:.2f}", rate.MegabytesPerSecond).CStr());
}
