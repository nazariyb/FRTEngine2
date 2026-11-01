// The canonical emitter (design §3.7, §15 I17): the bytes of every file FRT writes itself. What must hold is that two saves
// of the same data give the same bytes, so a merge works and an unchanged re-save touches nothing: key order ($-keys,
// declared, then original; a keyed map sorted), block and flow style, the one spelling of each number, the quoting rule,
// LF and a final newline. And that the output never changes what a tree says: every test's output is read back through
// ParseYaml by the emitter itself, and what cannot be read back is refused. Hostile text must survive or be refused,
// never altered.
#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <limits>
#include <random>
#include <vector>

#include "Containers/String.h"
#include "Diagnostics/Diagnostics.h"
#include "Reflect/CanonicalEmitter.h"
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

FString Emit (const CDataTree& Tree, const SEmitPolicy& Policy = {})
{
	FString out;
	SDiagnostic error;
	EXPECT_TRUE(EmitCanonical(Tree, Policy, out, &error)) << error.Message.CStr();
	return out;
}

FString EmitText (FStringView Text, const SEmitPolicy& Policy = {})
{
	return Emit(Parse(Text), Policy);
}

// A one-scalar document, as a builder writes it (no parser's rules in the way).
CDataTree MakeScalarTree (FStringView Text, EScalarStyle Style)
{
	CDataTreeBuilder builder;
	builder.BeginMap(0u, 0u);
	builder.Key("Value", 0u, 0u);
	builder.Scalar(Text, Style, 0u, 0u);
	builder.End();
	CDataTree tree;
	EXPECT_TRUE(builder.Finish(tree));
	return tree;
}

// The bytes after "Value: ", without the newline.
FString EmitScalar (FStringView Text, EScalarStyle Style)
{
	const FString out = Emit(MakeScalarTree(Text, Style));
	EXPECT_TRUE(out.View().StartsWith("Value: ")) << out.CStr();
	EXPECT_TRUE(out.View().EndsWith('\n')) << out.CStr();
	return FString(out.View().RightChop(7u).LeftChop(1u));
}

void ExpectText (FStringView Text, FStringView Expected, const SEmitPolicy& Policy = {})
{
	EXPECT_EQ(EmitText(Text, Policy).View(), Expected);
}
}


// ---- Key order ----

TEST (CanonicalEmitterTest, DollarKeysComeFirstInTheirFixedOrderEvenWhenTheFileHasThemElsewhere)
{
	ExpectText(
		"Damage: 24\n$note: keep it\n$rowVersion: 2\nName: Rifle\n$rowType: Game.Row\n$version: 3\n$type: Game.Data\n",
		"$type: Game.Data\n$version: 3\n$rowType: Game.Row\n$rowVersion: 2\n$note: keep it\nDamage: 24\nName: Rifle\n");
}

TEST (CanonicalEmitterTest, DeclaredKeysFollowTheDollarKeysAndUnknownKeysKeepTheirOriginalOrder)
{
	static constexpr FStringLiteral Order[] = { "Alpha", "Beta", "NotInTheFile", "Gamma" };
	const SEmitRule rules[] = { { .Path = "", .KeyOrder = Order } };
	ExpectText("Zed: 1\nGamma: 2\nYak: 3\nBeta: 4\n$type: T\nAlpha: 5\n",
		"$type: T\nAlpha: 5\nBeta: 4\nGamma: 2\nZed: 1\nYak: 3\n", { .Rules = rules });
}

TEST (CanonicalEmitterTest, ARuleAppliesOnlyWhereItsPathSays)
{
	static constexpr FStringLiteral Order[] = { "B", "A" };
	const SEmitRule rules[] = {
		{ .Path = "One", .KeyOrder = Order },
		{ .Path = "Two.*", .KeyOrder = Order },
		{ .Path = "List.*.Inner", .KeyOrder = Order },
	};
	ExpectText(
		"A: 1\nB: 2\n"
		"One:\n  A: 1\n  B: 2\n  Deep:\n    A: 1\n    B: 2\n"
		"Two:\n  A: 1\n  B: 2\n  Row:\n    A: 1\n    B: 2\n"
		"List:\n  - A: 1\n    B: 2\n    Inner:\n      A: 1\n      B: 2\n",
		"A: 1\nB: 2\n"
		"One:\n  B: 2\n  A: 1\n  Deep:\n    A: 1\n    B: 2\n"
		"Two:\n  A: 1\n  B: 2\n  Row:\n    B: 2\n    A: 1\n"
		"List:\n  - A: 1\n    B: 2\n    Inner:\n      B: 2\n      A: 1\n",
		{ .Rules = rules });
}

TEST (CanonicalEmitterTest, AKeyedMapIsSortedByBytesSoParallelAdditionsMerge)
{
	const SEmitRule rules[] = { { .Path = "Rows", .bSortKeys = true } };
	// Ordinal: 'Z' before 'a', and a Cyrillic key after every ASCII one.
	ExpectText("Rows:\n  b: 1\n  Яр: 2\n  Z: 3\n  a: 4\n", "Rows:\n  Z: 3\n  a: 4\n  b: 1\n  Яр: 2\n", { .Rules = rules });
}

TEST (CanonicalEmitterTest, ASortedMapStillLeadsWithItsDeclaredKeys)
{
	static constexpr FStringLiteral Order[] = { "z" };
	const SEmitRule rules[] = { { .Path = "", .KeyOrder = Order, .bSortKeys = true } };
	ExpectText("b: 1\nz: 2\na: 3\n", "z: 2\na: 3\nb: 1\n", { .Rules = rules });
}


// ---- Style ----

TEST (CanonicalEmitterTest, BlockStyleIndentsTwoSpacesWithSequenceItemsUnderTheirKey)
{
	ExpectText("A:\n  B:\n    C: 1\n  L:\n  - x\n  - z\n", "A:\n  B:\n    C: 1\n  L:\n    - x\n    - z\n");
}

TEST (CanonicalEmitterTest, MapsAndSequencesInASequenceStartOnTheDashLine)
{
	ExpectText("L:\n- a: 1\n  b:\n    c: 2\n- - x\n  - z\n- plain\n",
		"L:\n  - a: 1\n    b:\n      c: 2\n  - - x\n    - z\n  - plain\n");
}

TEST (CanonicalEmitterTest, TheStyleATreeCameInIsIgnored)
{
	ExpectText("A: { B: 1, C: [ x, z ] }\n", "A:\n  B: 1\n  C:\n    - x\n    - z\n");
}

TEST (CanonicalEmitterTest, AReferenceIsFlowWithItsKeysInTheirFixedOrderAnywhereInADocument)
{
	ExpectText(
		"Ref:\n  path: /Project/A.png\n  $asset: 2d8f1e4a6b3c4d5e8f9a0b1c2d3e4f50\n"
		"Sub:\n  extra: x\n  path: /Project/B.gltf\n  sub: mesh:Body\n  $asset: 7f3c9a1e5b2d4c6f8a0b1c2d3e4f5061\n"
		"List:\n  - { row: Rifle556, id: abc, path: /Project/C.frt, $asset: 1b2c3d4e5f604182a3b4c5d6e7f80912 }\n",
		"Ref: { $asset: 2d8f1e4a6b3c4d5e8f9a0b1c2d3e4f50, path: /Project/A.png }\n"
		"Sub: { $asset: 7f3c9a1e5b2d4c6f8a0b1c2d3e4f5061, sub: \"mesh:Body\", path: /Project/B.gltf, extra: x }\n"
		"List:\n  - { $asset: 1b2c3d4e5f604182a3b4c5d6e7f80912, row: Rifle556, id: abc, path: /Project/C.frt }\n");
}

TEST (CanonicalEmitterTest, TheRootIsADocumentEvenWhenItHoldsAnAssetKey)
{
	// $type stays the first line, since routing a .frt reads only that (§3.7), and the block form stays line per key.
	ExpectText("$type: T\n$asset: x\n", "$type: T\n$asset: x\n");
	ExpectText("$asset: x\npath: p\n$version: 2\n$type: T\nDamage: 24\n", "$type: T\n$version: 2\n$asset: x\npath: p\nDamage: 24\n");
	// Only $asset's own value holds an id there: the root's other keys are not a reference's.
	ExpectText("$asset: 1.50\nOther: 1.50\nid: 1.50\n", "$asset: \"1.50\"\nOther: 1.5\nid: 1.5\n");
	// One level down it is a reference again, in a sequence item as under a key.
	ExpectText("- path: p\n  $asset: x\n", "- { $asset: x, path: p }\n");
	ExpectText("$asset: x\nRef:\n  path: p\n  $asset: z\n", "$asset: x\nRef: { $asset: z, path: p }\n");
}

TEST (CanonicalEmitterTest, OnlyAReferencesOwnKeysAreTextItsOtherValuesKeepTheirTypes)
{
	const FString out = EmitText("A: { count: 3, scale: 1.50, flag: true, row: 007, $asset: x, more: { v: 2.50 }, id: { v: 1.50 } }\n");
	EXPECT_EQ(out.View(), "A: { $asset: x, row: \"007\", id: { v: \"1.50\" }, count: 3, scale: 1.5, flag: true, more: { v: 2.5 } }\n");

	const CDataTree read = Parse(out.View());
	EXPECT_EQ(read.Find("A.count").AsInt64().value_or(0), 3);
	EXPECT_EQ(read.Find("A.scale").AsDouble().value_or(0.0), 1.5);
	EXPECT_EQ(read.Find("A.flag").AsBool().value_or(false), true);

	const CDataTree canonical = Parse("A: { $asset: x, path: p, scale: 1 }\n");
	EXPECT_TRUE(IsSameContent(Parse(Emit(canonical).View()), canonical));
}

TEST (CanonicalEmitterTest, ARuleCanTakeAwayTheLeadKeysSoAFormatWithItsOwnOrderKeepsIt)
{
	static constexpr FStringLiteral Order[] = { "Alpha", "Beta" };
	const SEmitRule rules[] = { { .Path = "", .KeyOrder = Order, .bKeyOrderOnly = true } };
	ExpectText("$note: x\nZed: 1\nBeta: 2\n$type: T\nAlpha: 3\n", "Alpha: 3\nBeta: 2\n$note: x\nZed: 1\n$type: T\n", { .Rules = rules });
	// Text keys are still text: the rule moves them and no more. And it ends where its path does.
	ExpectText("Sub:\n  B: 1\n  $type: T\n$note: 1.50\n", "Sub:\n  $type: T\n  B: 1\n$note: \"1.50\"\n", { .Rules = rules });
}

TEST (CanonicalEmitterTest, ARuleCanMakeARecordFlowAndEverythingBelowItFollows)
{
	const SEmitRule rules[] = {
		{ .Path = "Color", .bFlow = true },
		{ .Path = "Modes.*", .bFlow = true },
		{ .Path = "Pos", .bFlow = true },
	};
	ExpectText(
		"Color:\n  - 1\n  - 0.5\n  - 0\n"
		"Modes:\n  Auto:\n    Rate: 650\n    Ramp:\n      Start: 0\n      Steps:\n        - 1\n        - 2\n"
		"Pos:\n  X: 1\n  Note: a, b\n  Ask: why?\n",
		"Color: [ 1, 0.5, 0 ]\n"
		"Modes:\n  Auto: { Rate: 650, Ramp: { Start: 0, Steps: [ 1, 2 ] } }\n"
		"Pos: { X: 1, Note: \"a, b\", Ask: \"why?\" }\n",
		{ .Rules = rules });
}

TEST (CanonicalEmitterTest, EmptyCollectionsHaveOnlyAFlowForm)
{
	ExpectText("A: {}\nB: []\nC:\n  - {}\n  - []\n", "A: {}\nB: []\nC:\n  - {}\n  - []\n");
}

TEST (CanonicalEmitterTest, ARootOfAnyKindIsWritten)
{
	ExpectText("[ a, b ]\n", "- a\n- b\n");
	ExpectText("just text\n", "just text\n");
	ExpectText("42\n", "42\n");
	ExpectText("{ $asset: 1b2c3d4e5f604182a3b4c5d6e7f80912 }\n", "$asset: 1b2c3d4e5f604182a3b4c5d6e7f80912\n");
}

TEST (CanonicalEmitterTest, AnEmptyTreeIsNoBytesAtAll)
{
	EXPECT_TRUE(Emit(CDataTree()).IsEmpty());
}

TEST (CanonicalEmitterTest, TheOutputIsLfWithAFinalNewlineAndNoBomWhateverTheInputHad)
{
	const FString out = EmitText("\xEF\xBB\xBF" "A: 1\r\nB:\r\n  - x\r\n");
	EXPECT_EQ(out.View(), "A: 1\nB:\n  - x\n");
}

TEST (CanonicalEmitterTest, CommentsAreNeverWritten)
{
	ExpectText("# a file comment\nA: 1 # a line comment\n# another\nB: 2\n", "A: 1\nB: 2\n");
}


// ---- Numbers, booleans, null ----

TEST (CanonicalEmitterTest, EachNumberHasOneSpelling)
{
	struct SCase
	{
		FStringLiteral Input;
		FStringLiteral Expected;
	};
	// to_chars takes the shorter of fixed and exponent form, fixed on a tie: 1e5 as a float is "1e+05", 10000.0 "10000".
	static constexpr SCase Cases[] = {
		{ "0.1", "0.1" }, { "1.50", "1.5" }, { "1.0", "1" }, { "+5", "5" }, { "-12", "-12" }, { "100000", "100000" },
		{ "1e5", "1e+05" }, { "10000.0", "10000" }, { "2.5e-3", "0.0025" }, { ".5", "0.5" }, { "1.", "1" }, { "1e21", "1e+21" },
		{ "1E-7", "1e-07" }, { "0.30000000000000004", "0.30000000000000004" },
		{ "0.1000000000000000055", "0.1" }, { ".inf", ".inf" }, { "+.inf", ".inf" }, { "-.inf", "-.inf" }, { ".nan", ".nan" },
		{ "-9223372036854775808", "-9223372036854775808" },
	};
	for (const SCase& testCase : Cases)
	{
		EXPECT_EQ(EmitScalar(testCase.Input, EScalarStyle::Plain).View(), FStringView(testCase.Expected)) << testCase.Input.GetData();
	}
}

TEST (CanonicalEmitterTest, NegativeZeroKeepsItsSignAndIsAFixedPoint)
{
	EXPECT_EQ(EmitScalar("-0.0", EScalarStyle::Plain).View(), "-0");
	EXPECT_EQ(EmitScalar("-0", EScalarStyle::Plain).View(), "-0");
	EXPECT_EQ(EmitScalar("+0", EScalarStyle::Plain).View(), "0");
}

TEST (CanonicalEmitterTest, AnIntegerPastInt64IsKeptVerbatimSinceNoNumberTypeHereHoldsItsDigits)
{
	EXPECT_EQ(EmitScalar("18446744073709551615", EScalarStyle::Plain).View(), "18446744073709551615");
}

TEST (CanonicalEmitterTest, AnIntegerWithLeadingZerosIsKeptVerbatimSinceNothingSaysItIsANumberAndTheyWouldGo)
{
	static constexpr FStringLiteral Spellings[] = { "007", "-007", "+007", "00", "-00", "0123456789" };
	for (const FStringLiteral text : Spellings)
	{
		EXPECT_EQ(EmitScalar(text, EScalarStyle::Plain).View(), FStringView(text));
	}
	// Plain and with its digits, it still reads both ways: nothing retyped it.
	const CDataTree source = Parse("Prefix: 007\n");
	const CDataTree read = Parse(Emit(source).View());
	EXPECT_TRUE(IsSameContent(read, source));
	EXPECT_EQ(read.Find("Prefix").AsText().value_or(FStringView()), "007");
	EXPECT_EQ(read.Find("Prefix").AsInt64().value_or(0), 7);
	// Quoted, it was text and stays text; a lone zero and a plain number have one spelling as before.
	EXPECT_EQ(EmitScalar("007", EScalarStyle::DoubleQuoted).View(), "\"007\"");
	EXPECT_EQ(EmitScalar("0", EScalarStyle::Plain).View(), "0");
	EXPECT_EQ(EmitScalar("70", EScalarStyle::Plain).View(), "70");
}

TEST (CanonicalEmitterTest, EveryDoubleSurvivesItsShortestFormBitForBit)
{
	std::mt19937_64 random(0x46525432u);
	for (uint32 i = 0u; i < 2000u; ++i)
	{
		const double value = std::bit_cast<double>(random());
		if (value != value || value == std::numeric_limits<double>::infinity() || value == -std::numeric_limits<double>::infinity())
		{
			continue;
		}
		FString text;
		AppendCanonicalNumber(text, value);
		const CDataTree tree = MakeScalarTree(text.View(), EScalarStyle::Plain);
		const std::optional<double> read = tree.GetRoot().FindChild("Value").AsDouble();
		ASSERT_TRUE(read.has_value()) << text.CStr();
		EXPECT_EQ(std::bit_cast<uint64>(*read), std::bit_cast<uint64>(value)) << text.CStr();
		// Emitting that text again does not move it.
		const CDataTree reread = Parse(Emit(tree).View());
		EXPECT_EQ(std::bit_cast<uint64>(*reread.GetRoot().FindChild("Value").AsDouble()), std::bit_cast<uint64>(value)) << text.CStr();
	}
}

TEST (CanonicalEmitterTest, AFloatPrintsAtItsOwnPrecisionNotADoublesNoise)
{
	FString text;
	AppendCanonicalNumber(text, 0.1f);
	EXPECT_EQ(text.View(), "0.1");
	text.Clear();
	AppendCanonicalNumber(text, static_cast<double>(0.1f));
	EXPECT_EQ(text.View(), "0.10000000149011612");
	text.Clear();
	AppendCanonicalNumber(text, std::numeric_limits<float>::infinity());
	EXPECT_EQ(text.View(), ".inf");
}

TEST (CanonicalEmitterTest, BooleansAreTrueAndFalseAndEverySpellingOfNullIsNull)
{
	ExpectText("A: true\nB: false\nC: ~\nD:\nE: null\n", "A: true\nB: false\nC: null\nD: null\nE: null\n");
	EXPECT_EQ(Emit(MakeScalarTree("", EScalarStyle::Plain)).View(), "Value: null\n");
	EXPECT_EQ(Emit(MakeScalarTree("~", EScalarStyle::Plain)).View(), "Value: null\n");
}

TEST (CanonicalEmitterTest, TheScalarsOfAReferenceAndTextKeysAreNeverNumbers)
{
	// An all-digit id must keep its digits. Shortening it as a number would change identity.
	ExpectText("A: { $asset: 12345678901234567890123456789012, row: 007 }\n",
		"A: { $asset: \"12345678901234567890123456789012\", row: \"007\" }\n");
	ExpectText("$type: 1.50\n$note: 1.50\nOther: 1.50\n", "$type: \"1.50\"\n$note: \"1.50\"\nOther: 1.5\n");
	ExpectText("$note:\n  - 1.50\n  - x: 1.50\n", "$note:\n  - \"1.50\"\n  - x: \"1.50\"\n");
}


// ---- Strings ----

TEST (CanonicalEmitterTest, TextThatYamlWouldRetypeIsQuotedWhateverTheStyleItCameIn)
{
	static constexpr FStringLiteral Retyped[] = {
		"yes", "Yes", "YES", "no", "No", "NO", "on", "On", "ON", "off", "Off", "OFF", "y", "Y", "n", "N", "True", "TRUE", "False", "FALSE",
		"Null", "NULL", "0x1F", "0o17", "0b101", "1_000", ".Inf", "-.INF", ".NaN", ".NAN", "1e400",
	};
	for (const FStringLiteral text : Retyped)
	{
		const FString quoted = FString::Format("\"{}\"", text);
		EXPECT_EQ(EmitScalar(text, EScalarStyle::Plain).View(), quoted.View());
		EXPECT_EQ(EmitScalar(text, EScalarStyle::SingleQuoted).View(), quoted.View());
	}
	// Quoted, a typed value's spelling stays text.
	static constexpr FStringLiteral Typed[] = { "true", "false", "null", "~", "12", "1.5", "1e5", ".inf", ".nan", "" };
	for (const FStringLiteral text : Typed)
	{
		EXPECT_EQ(EmitScalar(text, EScalarStyle::DoubleQuoted).View(), FString::Format("\"{}\"", text).View());
	}
}

TEST (CanonicalEmitterTest, TextThatNeedsNoQuotesLosesThemSoAFileHasOneForm)
{
	EXPECT_EQ(EmitScalar("Assault Rifle", EScalarStyle::DoubleQuoted).View(), "Assault Rifle");
	EXPECT_EQ(EmitScalar("Assault Rifle", EScalarStyle::SingleQuoted).View(), "Assault Rifle");
	EXPECT_EQ(EmitScalar("/Project/A b/c.frt", EScalarStyle::Literal).View(), "/Project/A b/c.frt");
	EXPECT_EQ(EmitScalar("it's \"fine\"", EScalarStyle::DoubleQuoted).View(), "it's \"fine\"");
	EXPECT_EQ(EmitScalar("Гра 游戏 💎", EScalarStyle::DoubleQuoted).View(), "Гра 游戏 💎");
	EXPECT_EQ(EmitScalar("a, b [c] {d}", EScalarStyle::DoubleQuoted).View(), "a, b [c] {d}");
}

TEST (CanonicalEmitterTest, ColonsAndHashesAlwaysQuote)
{
	EXPECT_EQ(EmitScalar("mesh:Body", EScalarStyle::Plain).View(), "\"mesh:Body\"");
	EXPECT_EQ(EmitScalar("image:#0", EScalarStyle::DoubleQuoted).View(), "\"image:#0\"");
	EXPECT_EQ(EmitScalar("a#b", EScalarStyle::Plain).View(), "\"a#b\"");
	ExpectText("\"mesh:Body\": \"mesh:Torso\"\n\"image:#0\": x\n", "\"mesh:Body\": \"mesh:Torso\"\n\"image:#0\": x\n");
}

TEST (CanonicalEmitterTest, LeadingIndicatorsAndOuterSpacesQuote)
{
	static constexpr FStringLiteral Texts[] = {
		"-x", "?x", ":x", ",x", "[x", "]x", "{x", "}x", "#x", "&x", "*x", "!x", "|x", ">x", "'x", "\"x", "%x", "@x", "`x", " x", "x ",
		"<<", "=", "...", "---",
	};
	for (const FStringLiteral text : Texts)
	{
		const FString emitted = EmitScalar(text, EScalarStyle::DoubleQuoted);
		EXPECT_TRUE(emitted.View().StartsWith('"')) << emitted.CStr();
		const CDataTree read = Parse(FString::Format("Value: {}\n", emitted.View()).View());
		EXPECT_EQ(read.GetRoot().FindChild("Value").GetText(), FStringView(text));
	}
}

TEST (CanonicalEmitterTest, ControlCharactersAndLineBreaksAreEscapedOnOneLine)
{
	EXPECT_EQ(EmitScalar("a\nb\tc\rd", EScalarStyle::Literal).View(), "\"a\\nb\\tc\\rd\"");
	EXPECT_EQ(EmitScalar("a\"b\\c\n", EScalarStyle::Literal).View(), "\"a\\\"b\\\\c\\n\"");
	EXPECT_EQ(EmitScalar(FStringView("\x01\x7F", 2u), EScalarStyle::DoubleQuoted).View(), "\"\\x01\\x7F\"");
	EXPECT_EQ(EmitScalar("\xC2\x85", EScalarStyle::DoubleQuoted).View(), "\"\\u0085\"");   // NEL, a YAML 1.1 line break
	EXPECT_EQ(EmitScalar("\xE2\x80\xA8", EScalarStyle::DoubleQuoted).View(), "\"\\u2028\"");
	EXPECT_EQ(EmitScalar("\xEF\xBB\xBFx", EScalarStyle::DoubleQuoted).View(), "\"\\uFEFFx\"");
	EXPECT_EQ(EmitScalar("\xF0\x9F\xBF\xBE", EScalarStyle::DoubleQuoted).View(), "\"\\U0001FFFE\"");   // a noncharacter
}

TEST (CanonicalEmitterTest, AKeyFollowsTheSameQuotingRuleAsAString)
{
	static constexpr FStringLiteral Keys[] = { "plain key", "true", "null", "12", "1.5", "a:b" };
	CDataTreeBuilder builder;
	builder.BeginMap(0u, 0u);
	for (const FStringLiteral key : Keys)
	{
		builder.Key(key, 0u, 0u);
		builder.Scalar("x", EScalarStyle::Plain, 0u, 0u);
	}
	builder.End();
	CDataTree tree;
	ASSERT_TRUE(builder.Finish(tree));
	const FString out = Emit(tree);
	EXPECT_EQ(out.View(), "plain key: x\n\"true\": x\n\"null\": x\n\"12\": x\n\"1.5\": x\n\"a:b\": x\n");
	EXPECT_TRUE(IsSameContent(Parse(out.View()), tree));
}

TEST (CanonicalEmitterTest, HostileTextSurvivesExactlyOrIsRefusedNeverAltered)
{
	// Short strings of what breaks a YAML writer, in every style. The emitter's own read-back is under test here too, so
	// a refusal is a finding: any valid UTF-8 must be writable, or a meta holding it could never be saved.
	static constexpr FStringLiteral Pieces[] = {
		" ", "  ", "\t", "\n", "\r", ":", ": ", " #", "#", "-", "- ", "?", "? ", ",", "[", "]", "{", "}", "&", "*", "!", "|", ">",
		"'", "''", "\"", "\\", "\\n", "%", "@", "`", "~", "null", "true", "1", "0x", ".", "..", "...", "---", "e", "_", "=", "<<",
		"Я", "游", "💎", "\xC2\x85", "\xE2\x80\xA8", "\xEF\xBB\xBF", "\xEF\xBF\xBE", "a", "$asset",
	};
	const uint32 pieceCount = static_cast<uint32>(std::size(Pieces));
	const std::array<EScalarStyle, 3u> styles = { EScalarStyle::DoubleQuoted, EScalarStyle::SingleQuoted, EScalarStyle::Literal };
	std::mt19937 random(0x46525433u);
	for (uint32 round = 0u; round < 1500u; ++round)
	{
		FString text;
		const uint32 count = 1u + random() % 4u;
		for (uint32 i = 0u; i < count; ++i)
		{
			text.Append(FStringView(Pieces[random() % pieceCount]));
		}
		CDataTreeBuilder builder;
		builder.BeginMap(0u, 0u);
		builder.Key(text.View(), 0u, 0u);
		builder.BeginMap(0u, 0u, ECollectionStyle::Flow);
		builder.Key("InFlow", 0u, 0u);
		builder.Scalar(text.View(), styles[round % styles.size()], 0u, 0u);
		builder.Key(text.View(), 0u, 0u);
		builder.Scalar("KeyInFlow", EScalarStyle::Plain, 0u, 0u);
		builder.End();
		builder.Key("InBlock", 0u, 0u);
		builder.Scalar(text.View(), styles[round % styles.size()], 0u, 0u);
		builder.End();
		CDataTree tree;
		ASSERT_TRUE(builder.Finish(tree)) << text.CStr();

		const SEmitRule rules[] = { { .Path = "*", .bFlow = true } };
		FString out;
		SDiagnostic error;
		ASSERT_TRUE(EmitCanonical(tree, { .Rules = rules }, out, &error)) << "[" << text.CStr() << "]: " << error.Message.CStr();
		const CDataTree read = Parse(out.View());
		ASSERT_EQ(read.GetRoot().GetCount(), 2u) << out.CStr();
		EXPECT_EQ(read.GetRoot().GetKey(0u), text.View()) << out.CStr();
		EXPECT_EQ(read.Find("InBlock").GetText(), text.View()) << out.CStr();
		EXPECT_EQ(read.GetRoot().GetValue(0u).FindChild("InFlow").GetText(), text.View()) << out.CStr();
		EXPECT_EQ(read.GetRoot().GetValue(0u).FindChild(text.View()).GetText(), "KeyInFlow") << out.CStr();
		EXPECT_EQ(Emit(read, { .Rules = rules }).View(), out.View());
	}
}

TEST (CanonicalEmitterTest, ANulByteIsEscapedAndComesBack)
{
	const FStringView text("a\0b", 3u);
	const FString out = Emit(MakeScalarTree(text, EScalarStyle::DoubleQuoted));
	EXPECT_EQ(out.View(), "Value: \"a\\x00b\"\n");
	EXPECT_EQ(Parse(out.View()).Find("Value").GetText(), text);
}


// ---- Refusals ----

TEST (CanonicalEmitterTest, InvalidUtf8InAScalarIsRefusedAtItsPosition)
{
	CDataTreeBuilder builder;
	builder.BeginMap(1u, 1u);
	builder.Key("A", 1u, 1u);
	builder.Scalar("ok", EScalarStyle::Plain, 1u, 4u);
	builder.Key("B", 2u, 1u);
	builder.Scalar("\xFF\xFE", EScalarStyle::Plain, 2u, 4u);
	builder.End();
	CDataTree tree;
	ASSERT_TRUE(builder.Finish(tree));

	FString out("left over");
	SDiagnostic error;
	EXPECT_FALSE(EmitCanonical(tree, {}, out, &error));
	EXPECT_TRUE(out.IsEmpty());
	EXPECT_EQ(error.Severity, ESeverity::Error);
	EXPECT_EQ(error.Line, 2u);
	EXPECT_EQ(error.Column, 4u);
	EXPECT_TRUE(error.Message.View().Contains(FStringView("UTF-8"))) << error.Message.CStr();
}

TEST (CanonicalEmitterTest, InvalidUtf8InAKeyIsRefused)
{
	CDataTreeBuilder builder;
	builder.BeginMap(0u, 0u);
	builder.Key("\xC0\xAF", 0u, 0u);
	builder.Scalar("x", EScalarStyle::Plain, 0u, 0u);
	builder.End();
	CDataTree tree;
	ASSERT_TRUE(builder.Finish(tree));
	FString out;
	EXPECT_FALSE(EmitCanonical(tree, {}, out));
	EXPECT_TRUE(out.IsEmpty());
}

TEST (CanonicalEmitterTest, ARawNodeIsRefusedSinceNothingSaysItsTextFitsWhereItWouldLand)
{
	CDataTreeBuilder builder;
	builder.BeginMap(0u, 0u);
	builder.Key("Unknown", 0u, 0u);
	builder.Raw("{ a: 1 }", 3u, 5u);
	builder.End();
	CDataTree tree;
	ASSERT_TRUE(builder.Finish(tree));
	FString out;
	SDiagnostic error;
	EXPECT_FALSE(EmitCanonical(tree, {}, out, &error));
	EXPECT_EQ(error.Line, 3u);
	EXPECT_TRUE(out.IsEmpty());
}

TEST (CanonicalEmitterTest, TextThatWouldNotParseBackIsRefusedNotWritten)
{
	// yaml-cpp allows a simple key of at most 1024 bytes: past it, the output is no YAML the parser would read.
	const std::vector<char> longKey(2000u, 'k');
	CDataTreeBuilder builder;
	builder.BeginMap(0u, 0u);
	builder.Key(FStringView(longKey.data(), longKey.size()), 0u, 0u);
	builder.Scalar("x", EScalarStyle::Plain, 0u, 0u);
	builder.End();
	CDataTree tree;
	ASSERT_TRUE(builder.Finish(tree));
	FString out;
	SDiagnostic error;
	EXPECT_FALSE(EmitCanonical(tree, {}, out, &error));
	EXPECT_TRUE(out.IsEmpty());
	EXPECT_FALSE(error.Message.IsEmpty());
	// A check that is switched off is what lets ParseMeta compare without a second parse; it writes the text.
	EXPECT_TRUE(EmitCanonical(tree, { .bVerifyRoundTrip = false }, out));
	EXPECT_FALSE(out.IsEmpty());
}


// ---- Whole documents and fixed points ----

TEST (CanonicalEmitterTest, ReemittingCanonicalBytesReproducesThem)
{
	// The material and the data asset of §3.2, without their comments.
	static constexpr FStringLiteral Material =
		"$type: frt.Material\n"
		"$version: 4\n"
		"base: { $asset: 3f2a9c4e7b1d4e0f9a8b6c5d4e3f2a1b, sub: \"material:Skin\", path: /Project/Characters/Hero/Hero.gltf }\n"
		"albedo:\n"
		"  texture: { $asset: 9b1d0c2e3f405162738495a6b7c8d9e0, path: /Project/Characters/Hero/Hero_Body.png }\n"
		"roughness: 0.6\n"
		"normal:\n"
		"  texture: { $asset: 5b0e2c7d9a4f4e31b8c6d2a1f0e9c873, path: /Project/Characters/Hero/Hero_Body_n.png }\n"
		"  scale: 1\n";
	EXPECT_EQ(EmitText(Material).View(), FStringView(Material));

	static constexpr FStringLiteral DataAsset =
		"$type: Game.WeaponData\n"
		"$version: 3\n"
		"$note: Starter rifle. Keep DPS below the SMG.\n"
		"DisplayName: Assault Rifle\n"
		"Damage: 24\n"
		"Rarity: Common\n"
		"Icon: { $asset: 2d8f1e4a6b3c4d5e8f9a0b1c2d3e4f50, path: /Project/UI/Icons/rifle.png }\n"
		"Mesh: { $asset: 7f3c9a1e5b2d4c6f8a0b1c2d3e4f5061, sub: \"mesh:Rifle_LOD0\", path: /Project/Weapons/Rifle.gltf }\n"
		"Ammo: { $asset: 1b2c3d4e5f604182a3b4c5d6e7f80912, row: Rifle556, path: /Project/Data/Ammo.frt }\n"
		"FireModes:\n"
		"  Auto: { RoundsPerMinute: 650 }\n"
		"  Burst: { RoundsPerMinute: 900, BurstCount: 3 }\n";
	const SEmitRule rules[] = { { .Path = "FireModes", .bSortKeys = true }, { .Path = "FireModes.*", .bFlow = true } };
	EXPECT_EQ(EmitText(DataAsset, { .Rules = rules }).View(), FStringView(DataAsset));
}

TEST (CanonicalEmitterTest, AReparseOfCanonicalBytesHoldsTheSameContent)
{
	const CDataTree original = Parse("$type: T\nA: 1\nB:\n  - x\n  - y: 2.5\nC: { $asset: 2d8f1e4a6b3c4d5e8f9a0b1c2d3e4f50 }\nD: \"12\"\nE: null\n");
	EXPECT_TRUE(IsSameContent(Parse(Emit(original).View()), original));
}

TEST (CanonicalEmitterTest, EmittingTwiceIsEmittingOnce)
{
	const FString once = EmitText("B: 'x y'\nA: { c: 1.50, d: [ 'q', ~ ] }\nC: 1e5\n");
	EXPECT_EQ(once.View(), "B: x y\nA:\n  c: 1.5\n  d:\n    - q\n    - null\nC: 1e+05\n");
	EXPECT_EQ(EmitText(once.View()).View(), once.View());
}

TEST (CanonicalEmitterTest, NestingAsDeepAsATreeMayBeIsWritten)
{
	CDataTreeBuilder builder;
	for (uint32 i = 0u; i < CDataTreeBuilder::DefaultMaxDepth; ++i)
	{
		builder.BeginMap(0u, 0u);
		builder.Key("k", 0u, 0u);
	}
	builder.Scalar("x", EScalarStyle::Plain, 0u, 0u);
	for (uint32 i = 0u; i < CDataTreeBuilder::DefaultMaxDepth; ++i)
	{
		builder.End();
	}
	CDataTree tree;
	ASSERT_TRUE(builder.Finish(tree));
	const CDataTree read = Parse(Emit(tree).View());
	EXPECT_TRUE(IsSameContent(read, tree));
}


// ---- IsSameContent and Subtree, the two additions to the tree that this file leans on ----

TEST (DataTreeContentTest, PositionsAndCollectionStylesAndQuoteKindsAreNotContent)
{
	EXPECT_TRUE(IsSameContent(Parse("A: 1\nB: [ x ]\nC: 'q'\n"), Parse("# moved\n\nA:   1\nB:\n  - x\nC: \"q\"\n")));
	EXPECT_TRUE(IsSameContent(Parse("A: ~\n"), Parse("A:\n")));
	EXPECT_TRUE(IsSameContent(CDataTree(), CDataTree()));
}

TEST (DataTreeContentTest, TextKeysOrderAndPlainVersusQuotedAreContent)
{
	EXPECT_FALSE(IsSameContent(Parse("A: 1\n"), Parse("A: 2\n")));
	EXPECT_FALSE(IsSameContent(Parse("A: 1\n"), Parse("B: 1\n")));
	EXPECT_FALSE(IsSameContent(Parse("A: 1\nB: 2\n"), Parse("B: 2\nA: 1\n")));
	EXPECT_FALSE(IsSameContent(Parse("A: 1\n"), Parse("A: '1'\n")));
	EXPECT_FALSE(IsSameContent(Parse("A: []\n"), Parse("A: {}\n")));
	EXPECT_FALSE(IsSameContent(Parse("A: 1\n"), CDataTree()));
	EXPECT_FALSE(IsSameContent(Parse("A: [ x ]\n"), Parse("A: [ x, y ]\n")));
	EXPECT_FALSE(IsSameContent(Parse("A: ~\n"), Parse("A: x\n")));
}

TEST (DataTreeContentTest, ASubtreeCopiesStylesPositionsAndChildren)
{
	const CDataTree source = Parse("Keep:\n  A: 'q'\n  B: [ 1, { c: ~ } ]\nDrop: 1\n");
	CDataTreeBuilder builder;
	ASSERT_TRUE(builder.Subtree(source.Find("Keep")));
	CDataTree copy;
	ASSERT_TRUE(builder.Finish(copy));

	EXPECT_EQ(copy.GetRoot().GetLine(), source.Find("Keep").GetLine());
	EXPECT_EQ(copy.Find("A").GetScalarStyle(), EScalarStyle::SingleQuoted);
	EXPECT_EQ(copy.Find("A").GetColumn(), source.Find("Keep.A").GetColumn());
	EXPECT_EQ(copy.Find("B").GetCollectionStyle(), ECollectionStyle::Flow);
	EXPECT_TRUE(copy.Find("B.1.c").IsNull());
	EXPECT_EQ(copy.GetRoot().GetCount(), 2u);
}

TEST (DataTreeContentTest, ASubtreeOfNoNodeFailsTheBuilder)
{
	CDataTreeBuilder builder;
	EXPECT_FALSE(builder.Subtree(HDataNode()));
	EXPECT_TRUE(builder.HasFailed());
}
