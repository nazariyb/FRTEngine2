// CDataTree and CDataTreeBuilder without a parser: the builder's rules (one root, keys only where a map expects one,
// unique keys, a depth limit, the first error wins and is reported with its position), the typed accessors' strict
// YAML 1.2 core reading (plain scalars only, exact spellings, overflow fails), lookups by key and path over small
// and large maps, and the binary form - versioned, round-trip exact, and safe against truncated or corrupted bytes.
#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <random>
#include <vector>

#include "Containers/String.h"
#include "Diagnostics/Diagnostics.h"
#include "Reflect/DataTree.h"

using namespace frt;
using namespace frt::reflect;


namespace
{
// { name: "Rock", size: 3, tags: [a, 'b'], empty: ~, raw: <raw> } with positions on line 1.
CDataTree MakeSampleTree ()
{
	CDataTreeBuilder builder;
	EXPECT_TRUE(builder.BeginMap(1u, 1u));
	EXPECT_TRUE(builder.Key("name", 1u, 1u));
	EXPECT_TRUE(builder.Scalar("Rock", EScalarStyle::DoubleQuoted, 1u, 7u));
	EXPECT_TRUE(builder.Key("size", 2u, 1u));
	EXPECT_TRUE(builder.Scalar("3", EScalarStyle::Plain, 2u, 7u));
	EXPECT_TRUE(builder.Key("tags", 3u, 1u));
	EXPECT_TRUE(builder.BeginSequence(3u, 7u, ECollectionStyle::Flow));
	EXPECT_TRUE(builder.Scalar("a", EScalarStyle::Plain, 3u, 8u));
	EXPECT_TRUE(builder.Scalar("b", EScalarStyle::SingleQuoted, 3u, 11u));
	EXPECT_TRUE(builder.End());
	EXPECT_TRUE(builder.Key("empty", 4u, 1u));
	EXPECT_TRUE(builder.Null(4u, 8u));
	EXPECT_TRUE(builder.Key("raw", 5u, 1u));
	EXPECT_TRUE(builder.Raw("unparsed: [text", 5u, 6u));
	EXPECT_TRUE(builder.End());
	CDataTree tree;
	SDiagnostic error;
	EXPECT_TRUE(builder.Finish(tree, &error)) << error.Message.CStr();
	return tree;
}

CDataTree MakeScalarTree (FStringView Text, EScalarStyle Style = EScalarStyle::Plain)
{
	CDataTreeBuilder builder;
	EXPECT_TRUE(builder.Scalar(Text, Style, 3u, 5u));
	CDataTree tree;
	EXPECT_TRUE(builder.Finish(tree));
	return tree;
}

std::vector<uint8> SerializeTree (const CDataTree& Tree)
{
	std::vector<uint8> bytes;
	Tree.Serialize(bytes);
	return bytes;
}
}


// ---------------- Building ----------------

TEST(DataTreeTest, BuiltTreeKeepsKindsStylesPositionsAndKeyOrder)
{
	const CDataTree tree = MakeSampleTree();
	ASSERT_FALSE(tree.IsEmpty());
	const HDataNode root = tree.GetRoot();
	ASSERT_TRUE(root.IsMap());
	EXPECT_EQ(root.GetCount(), 5u);
	EXPECT_EQ(tree.GetNodeCount(), 8u);

	// Keys in insertion order, not sorted.
	const char* const keys[] = { "name", "size", "tags", "empty", "raw" };
	for (uint32 i = 0u; i < 5u; ++i)
	{
		EXPECT_EQ(root.GetKey(i), keys[i]);
		EXPECT_EQ(root.GetChild(i).KeyLine, i + 1u);
		EXPECT_EQ(root.GetChild(i).KeyColumn, 1u);
	}

	const HDataNode name = root.FindChild("name");
	EXPECT_TRUE(name.IsScalar());
	EXPECT_EQ(name.GetScalarStyle(), EScalarStyle::DoubleQuoted);
	EXPECT_EQ(name.GetText(), "Rock");
	EXPECT_EQ(name.GetLine(), 1u);
	EXPECT_EQ(name.GetColumn(), 7u);

	const HDataNode tags = root.FindChild("tags");
	EXPECT_TRUE(tags.IsSequence());
	EXPECT_EQ(tags.GetCollectionStyle(), ECollectionStyle::Flow);
	ASSERT_EQ(tags.GetCount(), 2u);
	EXPECT_EQ(tags.GetValue(0u).GetText(), "a");
	EXPECT_EQ(tags.GetValue(1u).GetScalarStyle(), EScalarStyle::SingleQuoted);
	EXPECT_EQ(tags.GetKey(0u), "");

	EXPECT_TRUE(root.FindChild("empty").IsNull());
	EXPECT_EQ(root.FindChild("empty").GetKind(), EDataNodeKind::Null);
	EXPECT_TRUE(root.FindChild("raw").IsRaw());
	EXPECT_EQ(root.FindChild("raw").GetText(), "unparsed: [text");
	EXPECT_EQ(root.GetCollectionStyle(), ECollectionStyle::Block);
}

TEST(DataTreeTest, ChildrenIterateInSourceOrder)
{
	const CDataTree tree = MakeSampleTree();
	std::vector<FString> seen;
	for (const SDataChild& child : tree.GetRoot().GetChildren())
	{
		seen.push_back(FString(child.Key));
		EXPECT_TRUE(child.Value.IsValid());
	}
	ASSERT_EQ(seen.size(), 5u);
	EXPECT_EQ(seen[0], "name");
	EXPECT_EQ(seen[4], "raw");

	uint32 items = 0u;
	for (const SDataChild& child : tree.Find("tags").GetChildren())
	{
		EXPECT_TRUE(child.Key.IsEmpty());
		++items;
	}
	EXPECT_EQ(items, 2u);
	EXPECT_TRUE(tree.Find("name").GetChildren().empty());
}

TEST(DataTreeTest, AnEmptyBuilderGivesAnEmptyTreeAndInvalidHandlesAnswerEmpty)
{
	CDataTreeBuilder builder;
	CDataTree tree = MakeSampleTree();
	ASSERT_TRUE(builder.Finish(tree));
	EXPECT_TRUE(tree.IsEmpty());
	EXPECT_FALSE(tree.GetRoot().IsValid());

	const HDataNode none = tree.Find("anything");
	EXPECT_FALSE(none);
	EXPECT_EQ(none.GetKind(), EDataNodeKind::Null);
	EXPECT_FALSE(none.IsNull());
	EXPECT_EQ(none.GetCount(), 0u);
	EXPECT_EQ(none.GetText(), "");
	EXPECT_FALSE(none.FindChild("x").IsValid());
	SDiagnostic error;
	EXPECT_FALSE(none.AsInt64(&error).has_value());
	EXPECT_FALSE(error.Message.IsEmpty());
	EXPECT_FALSE(none.AsText().has_value());
}

TEST(DataTreeTest, BuilderRejectsMisuseAndReportsTheFirstError)
{
	{
		CDataTreeBuilder builder;
		EXPECT_FALSE(builder.Key("orphan", 4u, 2u));
		// Failed: everything after is refused, and Finish reports the first error.
		EXPECT_FALSE(builder.Scalar("x", EScalarStyle::Plain, 9u, 9u));
		CDataTree tree;
		SDiagnostic error;
		EXPECT_FALSE(builder.Finish(tree, &error));
		EXPECT_TRUE(tree.IsEmpty());
		EXPECT_EQ(error.Line, 4u);
		EXPECT_EQ(error.Column, 2u);
		EXPECT_TRUE(error.Message.Contains("orphan")) << error.Message.CStr();
	}
	{
		CDataTreeBuilder builder;
		ASSERT_TRUE(builder.BeginMap(1u, 1u));
		EXPECT_TRUE(builder.IsExpectingKey());
		EXPECT_FALSE(builder.Scalar("value", EScalarStyle::Plain, 1u, 5u));   // a value where a key is due
		SDiagnostic error;
		CDataTree tree;
		EXPECT_FALSE(builder.Finish(tree, &error));
		EXPECT_EQ(error.Column, 5u);
	}
	{
		CDataTreeBuilder builder;
		ASSERT_TRUE(builder.Scalar("root", EScalarStyle::Plain, 1u, 1u));
		EXPECT_FALSE(builder.Scalar("second root", EScalarStyle::Plain, 2u, 1u));
	}
	{
		CDataTreeBuilder builder;
		EXPECT_FALSE(builder.End());
	}
	{
		CDataTreeBuilder builder;
		ASSERT_TRUE(builder.BeginMap(1u, 1u));
		ASSERT_TRUE(builder.Key("k", 1u, 1u));
		EXPECT_FALSE(builder.End());   // the key has no value
	}
	{
		CDataTreeBuilder builder;
		ASSERT_TRUE(builder.BeginSequence(2u, 3u));
		CDataTree tree;
		SDiagnostic error;
		EXPECT_FALSE(builder.Finish(tree, &error));   // not closed
		EXPECT_EQ(error.Line, 2u);
		EXPECT_TRUE(error.Message.Contains("not closed")) << error.Message.CStr();

		// Finish resets the builder for another tree.
		EXPECT_TRUE(builder.Scalar("again", EScalarStyle::Plain, 1u, 1u));
		EXPECT_TRUE(builder.Finish(tree));
		EXPECT_EQ(tree.GetRoot().GetText(), "again");
	}
	{
		CDataTreeBuilder builder;
		builder.Fail("the producer's own error", 7u, 8u);
		builder.Fail("a later error", 1u, 1u);
		CDataTree tree;
		SDiagnostic error;
		EXPECT_FALSE(builder.Finish(tree, &error));
		EXPECT_EQ(error.Message, "the producer's own error");
		EXPECT_EQ(error.Line, 7u);
	}
}

TEST(DataTreeTest, DuplicateKeysFailAtTheSecondOccurrence)
{
	CDataTreeBuilder builder;
	ASSERT_TRUE(builder.BeginMap(1u, 1u));
	ASSERT_TRUE(builder.Key("b", 1u, 1u));
	ASSERT_TRUE(builder.Null(1u, 3u));
	ASSERT_TRUE(builder.Key("a", 2u, 1u));
	ASSERT_TRUE(builder.Null(2u, 3u));
	ASSERT_TRUE(builder.Key("b", 3u, 1u));
	ASSERT_TRUE(builder.Null(3u, 3u));
	ASSERT_TRUE(builder.Key("a", 4u, 1u));
	ASSERT_TRUE(builder.Null(4u, 3u));
	EXPECT_FALSE(builder.End());
	CDataTree tree;
	SDiagnostic error;
	EXPECT_FALSE(builder.Finish(tree, &error));
	// Both keys repeat; the repetition first in the source is "b" on line 3.
	EXPECT_EQ(error.Line, 3u);
	EXPECT_TRUE(error.Message.Contains("'b'")) << error.Message.CStr();
	EXPECT_TRUE(error.Message.Contains("line 1")) << error.Message.CStr();

	// Keys differing in case, or in a nested map, are not duplicates.
	CDataTreeBuilder other;
	ASSERT_TRUE(other.BeginMap(1u, 1u));
	ASSERT_TRUE(other.Key("Key", 1u, 1u));
	ASSERT_TRUE(other.BeginMap(1u, 1u));
	ASSERT_TRUE(other.Key("key", 1u, 1u));
	ASSERT_TRUE(other.Null(1u, 1u));
	ASSERT_TRUE(other.End());
	ASSERT_TRUE(other.Key("key", 2u, 1u));
	ASSERT_TRUE(other.Null(2u, 1u));
	ASSERT_TRUE(other.End());
	EXPECT_TRUE(other.Finish(tree));
}

TEST(DataTreeTest, NestingPastTheDepthLimitFails)
{
	CDataTreeBuilder builder(4u);
	ASSERT_TRUE(builder.BeginSequence(1u, 1u));
	ASSERT_TRUE(builder.BeginMap(1u, 2u));
	ASSERT_TRUE(builder.Key("k", 1u, 3u));
	ASSERT_TRUE(builder.BeginSequence(1u, 4u));
	ASSERT_TRUE(builder.BeginSequence(1u, 5u));
	EXPECT_EQ(builder.GetDepth(), 4u);
	EXPECT_FALSE(builder.BeginSequence(1u, 6u));
	CDataTree tree;
	SDiagnostic error;
	EXPECT_FALSE(builder.Finish(tree, &error));
	EXPECT_EQ(error.Column, 6u);
	EXPECT_TRUE(error.Message.Contains("deeper than 4")) << error.Message.CStr();
}


// ---------------- Typed accessors ----------------

TEST(DataTreeTest, AsBoolAcceptsExactlyTrueAndFalse)
{
	EXPECT_EQ(MakeScalarTree("true").GetRoot().AsBool(), std::optional<bool>(true));
	EXPECT_EQ(MakeScalarTree("false").GetRoot().AsBool(), std::optional<bool>(false));
	for (const char* text : { "True", "TRUE", "yes", "no", "on", "off", "1", "0", "" })
	{
		SDiagnostic error;
		EXPECT_FALSE(MakeScalarTree(text).GetRoot().AsBool(&error).has_value()) << text;
		EXPECT_EQ(error.Line, 3u);
		EXPECT_EQ(error.Column, 5u);
		EXPECT_EQ(error.Severity, ESeverity::Error);
		EXPECT_TRUE(error.Message.Contains("boolean")) << error.Message.CStr();
	}
	// Quoted text is text, whatever it spells.
	SDiagnostic error;
	EXPECT_FALSE(MakeScalarTree("true", EScalarStyle::SingleQuoted).GetRoot().AsBool(&error).has_value());
	EXPECT_TRUE(error.Message.Contains("quoted")) << error.Message.CStr();
}

TEST(DataTreeTest, AsInt64ReadsSignedDecimalAndFailsOnOverflow)
{
	EXPECT_EQ(MakeScalarTree("42").GetRoot().AsInt64(), std::optional<int64>(42));
	EXPECT_EQ(MakeScalarTree("-7").GetRoot().AsInt64(), std::optional<int64>(-7));
	EXPECT_EQ(MakeScalarTree("+3").GetRoot().AsInt64(), std::optional<int64>(3));
	EXPECT_EQ(MakeScalarTree("007").GetRoot().AsInt64(), std::optional<int64>(7));
	EXPECT_EQ(MakeScalarTree("9223372036854775807").GetRoot().AsInt64(), std::optional<int64>(std::numeric_limits<int64>::max()));
	EXPECT_EQ(MakeScalarTree("-9223372036854775808").GetRoot().AsInt64(), std::optional<int64>(std::numeric_limits<int64>::min()));

	for (const char* text : { "9223372036854775808", "-9223372036854775809", "99999999999999999999", "0x10", "1.5", "1e3",
		"", "-", "+", " 1", "1 ", "1_000", "one" })
	{
		SDiagnostic error;
		EXPECT_FALSE(MakeScalarTree(text).GetRoot().AsInt64(&error).has_value()) << text;
		EXPECT_FALSE(error.Message.IsEmpty());
	}
	EXPECT_FALSE(MakeScalarTree("1", EScalarStyle::DoubleQuoted).GetRoot().AsInt64().has_value());
	EXPECT_FALSE(MakeScalarTree("1", EScalarStyle::Literal).GetRoot().AsInt64().has_value());
}

TEST(DataTreeTest, AsDoubleReadsDecimalExponentAndSpecialValues)
{
	EXPECT_EQ(MakeScalarTree("1").GetRoot().AsDouble(), std::optional<double>(1.0));
	EXPECT_EQ(MakeScalarTree("-0.5").GetRoot().AsDouble(), std::optional<double>(-0.5));
	EXPECT_EQ(MakeScalarTree("+2.5e-3").GetRoot().AsDouble(), std::optional<double>(2.5e-3));
	EXPECT_EQ(MakeScalarTree(".5").GetRoot().AsDouble(), std::optional<double>(0.5));
	EXPECT_EQ(MakeScalarTree("1.").GetRoot().AsDouble(), std::optional<double>(1.0));
	EXPECT_EQ(MakeScalarTree("0.1").GetRoot().AsDouble(), std::optional<double>(0.1));
	EXPECT_EQ(MakeScalarTree("1E10").GetRoot().AsDouble(), std::optional<double>(1e10));
	EXPECT_EQ(MakeScalarTree(".inf").GetRoot().AsDouble(), std::optional<double>(std::numeric_limits<double>::infinity()));
	EXPECT_EQ(MakeScalarTree("+.inf").GetRoot().AsDouble(), std::optional<double>(std::numeric_limits<double>::infinity()));
	EXPECT_EQ(MakeScalarTree("-.inf").GetRoot().AsDouble(), std::optional<double>(-std::numeric_limits<double>::infinity()));
	const std::optional<double> nan = MakeScalarTree(".nan").GetRoot().AsDouble();
	ASSERT_TRUE(nan.has_value());
	EXPECT_TRUE(std::isnan(*nan));

	for (const char* text : { "1e400", "-1e400", ".", "e5", ".e5", "1e", "1e+", "--1", "inf", "nan", ".Inf", ".NaN",
		"0x1p3", "1,5", "", " 1", "1.5f" })
	{
		SDiagnostic error;
		EXPECT_FALSE(MakeScalarTree(text).GetRoot().AsDouble(&error).has_value()) << text;
		EXPECT_FALSE(error.Message.IsEmpty());
	}
	EXPECT_FALSE(MakeScalarTree("1.5", EScalarStyle::SingleQuoted).GetRoot().AsDouble().has_value());
}

TEST(DataTreeTest, AsTextReadsAnyScalarAndNothingElse)
{
	const CDataTree tree = MakeSampleTree();
	EXPECT_EQ(tree.Find("name").AsText(), std::optional<FStringView>("Rock"));
	EXPECT_EQ(tree.Find("size").AsText(), std::optional<FStringView>("3"));
	SDiagnostic error;
	EXPECT_FALSE(tree.Find("tags").AsText(&error).has_value());
	EXPECT_TRUE(error.Message.Contains("sequence")) << error.Message.CStr();
	EXPECT_EQ(error.Line, 3u);
	EXPECT_FALSE(tree.Find("empty").AsText().has_value());
	EXPECT_FALSE(tree.Find("raw").AsText().has_value());
	EXPECT_FALSE(tree.GetRoot().AsInt64().has_value());
}

TEST(DataTreeTest, PlainNullSpellingsCountAsNull)
{
	EXPECT_TRUE(MakeScalarTree("~").GetRoot().IsNull());
	EXPECT_TRUE(MakeScalarTree("null").GetRoot().IsNull());
	EXPECT_TRUE(MakeScalarTree("").GetRoot().IsNull());
	EXPECT_FALSE(MakeScalarTree("null", EScalarStyle::DoubleQuoted).GetRoot().IsNull());
	EXPECT_FALSE(MakeScalarTree("nil").GetRoot().IsNull());
}


// ---------------- Lookups ----------------

TEST(DataTreeTest, FindWalksMapKeysAndSequenceIndices)
{
	CDataTreeBuilder builder;
	ASSERT_TRUE(builder.BeginMap(1u, 1u));
	ASSERT_TRUE(builder.Key("albedo", 1u, 1u));
	ASSERT_TRUE(builder.BeginMap(2u, 3u));
	ASSERT_TRUE(builder.Key("texture", 2u, 3u));
	ASSERT_TRUE(builder.Scalar("T_Rock.png", EScalarStyle::Plain, 2u, 12u));
	ASSERT_TRUE(builder.End());
	ASSERT_TRUE(builder.Key("Rules", 3u, 1u));
	ASSERT_TRUE(builder.BeginSequence(4u, 3u));
	ASSERT_TRUE(builder.BeginMap(4u, 5u));
	ASSERT_TRUE(builder.Key("Match", 4u, 5u));
	ASSERT_TRUE(builder.Scalar("**/*.png", EScalarStyle::DoubleQuoted, 4u, 12u));
	ASSERT_TRUE(builder.End());
	ASSERT_TRUE(builder.End());
	ASSERT_TRUE(builder.Key("dotted.key", 5u, 1u));
	ASSERT_TRUE(builder.Scalar("literal", EScalarStyle::Plain, 5u, 13u));
	ASSERT_TRUE(builder.End());
	CDataTree tree;
	ASSERT_TRUE(builder.Finish(tree));

	EXPECT_EQ(tree.Find("albedo.texture").GetText(), "T_Rock.png");
	EXPECT_EQ(tree.Find("Rules.0.Match").GetText(), "**/*.png");
	EXPECT_EQ(tree.Find("").GetKind(), EDataNodeKind::Map);
	EXPECT_FALSE(tree.Find("Rules.1").IsValid());
	EXPECT_FALSE(tree.Find("Rules.x").IsValid());
	EXPECT_FALSE(tree.Find("Rules.-1").IsValid());
	EXPECT_FALSE(tree.Find("albedo.texture.more").IsValid());
	EXPECT_FALSE(tree.Find("Albedo.texture").IsValid());   // keys are case-sensitive
	EXPECT_FALSE(tree.Find("dotted.key").IsValid());       // '.' always separates
	EXPECT_EQ(tree.GetRoot().FindChild("dotted.key").GetText(), "literal");
	EXPECT_EQ(tree.GetRoot().FindKeyIndex("Rules"), std::optional<uint32>(1u));
	EXPECT_FALSE(tree.GetRoot().FindKeyIndex("rules").has_value());
}

TEST(DataTreeTest, LargeMapsFindEveryKey)
{
	// Past the linear-scan size: lookups go through the sorted index.
	constexpr uint32 count = 2000u;
	CDataTreeBuilder builder;
	ASSERT_TRUE(builder.BeginMap(1u, 1u));
	for (uint32 i = 0u; i < count; ++i)
	{
		// Insertion order differs from byte order on purpose.
		const FString key = FString::Format("Row{}", (i * 7919u) % count);
		ASSERT_TRUE(builder.Key(key.View(), i + 2u, 3u));
		ASSERT_TRUE(builder.Scalar(FString::Format("{}", i).View(), EScalarStyle::Plain, i + 2u, 10u));
	}
	ASSERT_TRUE(builder.End());
	CDataTree tree;
	ASSERT_TRUE(builder.Finish(tree));

	const HDataNode root = tree.GetRoot();
	for (uint32 i = 0u; i < count; ++i)
	{
		const FString key = FString::Format("Row{}", (i * 7919u) % count);
		EXPECT_EQ(root.FindChild(key.View()).AsInt64(), std::optional<int64>(i)) << key.CStr();
		EXPECT_EQ(root.FindKeyIndex(key.View()), std::optional<uint32>(i));
	}
	EXPECT_FALSE(root.FindChild("Row").IsValid());
	EXPECT_FALSE(root.FindChild("Row2000").IsValid());
	EXPECT_FALSE(root.FindChild("").IsValid());
	EXPECT_EQ(root.GetKey(0u), "Row0");
	EXPECT_EQ(root.GetKey(1u), FString::Format("Row{}", 7919u % count));
}

TEST(DataTreeTest, CopiesAndMovesKeepTheContent)
{
	const CDataTree original = MakeSampleTree();
	CDataTree copy = original;
	EXPECT_TRUE(copy == original);
	EXPECT_EQ(copy.Find("tags.1").GetText(), "b");
	// Handles name a node of one tree: the copy's root is another handle.
	EXPECT_FALSE(copy.GetRoot() == original.GetRoot());

	CDataTree moved = std::move(copy);
	EXPECT_TRUE(moved == original);
	EXPECT_EQ(moved.Find("name").GetText(), "Rock");
}


// ---------------- Binary form ----------------

TEST(DataTreeTest, BinaryFormRoundTripsExactly)
{
	const CDataTree original = MakeSampleTree();
	const std::vector<uint8> bytes = SerializeTree(original);
	CDataTree decoded;
	SDiagnostic error;
	ASSERT_TRUE(decoded.Deserialize(bytes, &error)) << error.Message.CStr();
	EXPECT_TRUE(decoded == original);
	EXPECT_EQ(SerializeTree(decoded), bytes);
	EXPECT_EQ(decoded.Find("raw").GetText(), "unparsed: [text");
	EXPECT_EQ(decoded.Find("tags").GetCollectionStyle(), ECollectionStyle::Flow);
	EXPECT_EQ(decoded.Find("name").GetColumn(), 7u);

	// An empty tree too.
	const std::vector<uint8> emptyBytes = SerializeTree(CDataTree());
	EXPECT_TRUE(decoded.Deserialize(emptyBytes, &error)) << error.Message.CStr();
	EXPECT_TRUE(decoded.IsEmpty());

	// A scalar root.
	const CDataTree scalar = MakeScalarTree("Text", EScalarStyle::Folded);
	ASSERT_TRUE(decoded.Deserialize(SerializeTree(scalar), &error)) << error.Message.CStr();
	EXPECT_TRUE(decoded == scalar);
	EXPECT_FALSE(decoded == original);
}

TEST(DataTreeTest, BinaryFormChecksMagicAndVersion)
{
	std::vector<uint8> bytes = SerializeTree(MakeSampleTree());
	CDataTree decoded;
	SDiagnostic error;

	std::vector<uint8> badMagic = bytes;
	badMagic[0] = 'X';
	EXPECT_FALSE(decoded.Deserialize(badMagic, &error));
	EXPECT_TRUE(error.Message.Contains("magic")) << error.Message.CStr();

	// The version varint follows the 8-byte magic.
	std::vector<uint8> otherVersion = bytes;
	ASSERT_EQ(otherVersion[8], CDataTree::BinaryVersion);
	otherVersion[8] = static_cast<uint8>(CDataTree::BinaryVersion + 1u);
	EXPECT_FALSE(decoded.Deserialize(otherVersion, &error));
	EXPECT_TRUE(error.Message.Contains("version")) << error.Message.CStr();
	EXPECT_TRUE(decoded.IsEmpty());

	std::vector<uint8> trailing = bytes;
	trailing.push_back(0u);
	EXPECT_FALSE(decoded.Deserialize(trailing, &error));
	EXPECT_TRUE(error.Message.Contains("after the end")) << error.Message.CStr();
}

TEST(DataTreeTest, TruncatedOrCorruptedBinaryFailsCleanly)
{
	const CDataTree original = MakeSampleTree();
	const std::vector<uint8> bytes = SerializeTree(original);

	// Every proper prefix fails.
	for (size_t length = 0u; length < bytes.size(); ++length)
	{
		CDataTree decoded;
		SDiagnostic error;
		EXPECT_FALSE(decoded.Deserialize(std::span<const uint8>(bytes.data(), length), &error)) << length;
		EXPECT_FALSE(error.Message.IsEmpty());
		EXPECT_TRUE(decoded.IsEmpty());
	}

	// Random corruption never crashes; whatever still decodes is a well-formed tree that round-trips (varints read
	// in a longer form than needed re-encode shorter, so the bytes themselves may differ).
	std::mt19937 random(20261025u);
	for (uint32 iteration = 0u; iteration < 5000u; ++iteration)
	{
		std::vector<uint8> corrupted = bytes;
		const uint32 flips = 1u + random() % 4u;
		for (uint32 i = 0u; i < flips; ++i)
		{
			corrupted[random() % corrupted.size()] = static_cast<uint8>(random());
		}
		CDataTree decoded;
		if (decoded.Deserialize(corrupted))
		{
			CDataTree again;
			EXPECT_TRUE(again.Deserialize(SerializeTree(decoded)));
			EXPECT_TRUE(again == decoded);
		}
		else
		{
			EXPECT_TRUE(decoded.IsEmpty());
		}
	}

	// Hostile counts: a node count or collection size far past the bytes is refused before anything is sized.
	std::vector<uint8> hostile(bytes.begin(), bytes.begin() + 9);
	hostile.insert(hostile.end(), { 0xFF, 0xFF, 0xFF, 0xFF, 0x0F });
	CDataTree decoded;
	EXPECT_FALSE(decoded.Deserialize(hostile));
}

TEST(DataTreeTest, BinaryNestingPastTheDepthLimitFails)
{
	// 200 nested one-item sequences, encoded by hand: the decoder rebuilds through the builder, whose depth limit
	// stops it.
	std::vector<uint8> bytes = { 'F', 'R', 'T', 'D', 'T', 'R', 'E', 'E', static_cast<uint8>(CDataTree::BinaryVersion) };
	constexpr uint32 depth = 200u;
	bytes.push_back(static_cast<uint8>((depth + 1u) & 0x7Fu) | 0x80u);
	bytes.push_back(static_cast<uint8>((depth + 1u) >> 7u));
	for (uint32 i = 0u; i < depth; ++i)
	{
		bytes.insert(bytes.end(), { static_cast<uint8>(EDataNodeKind::Sequence), 1u, 1u, 1u });
	}
	bytes.insert(bytes.end(), { static_cast<uint8>(EDataNodeKind::Null), 1u, 1u });
	CDataTree decoded;
	SDiagnostic error;
	EXPECT_FALSE(decoded.Deserialize(bytes, &error));
	EXPECT_TRUE(error.Message.Contains("deeper")) << error.Message.CStr();
}
