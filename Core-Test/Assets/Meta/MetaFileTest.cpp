// The `.meta` reader and writer (design §3.2): a meta is hand-edited and three-way merged, so what must hold is that an
// unchanged canonical file re-emits its exact bytes (every §3.2 example), that any spelling a person may type reads and is
// normalised on the next write, that nothing a build does not understand is ever dropped, and that a file it cannot
// read in full is Broken rather than half-read: Broken still names the GUIDs it holds, because identity outlives a bad edit.
#include <gtest/gtest.h>

#include <vector>

#include "Assets/AssetDiagnosticCodes.h"
#include "Assets/Meta/MetaFile.h"
#include "Containers/String.h"
#include "Diagnostics/Diagnostics.h"
#include "Reflect/DataTreeYaml.h"

using namespace frt;
using namespace frt::assets;


namespace
{
constexpr FStringLiteral GuidA = "5d1e7a3b9c0f42d68e1a2b3c4d5e6f70";
constexpr FStringLiteral GuidB = "3f2a9c4e7b1d4e0f9a8b6c5d4e3f2a1b";
constexpr FStringLiteral GuidC = "1c7e0a9b33d24f1e9a6b0c1d2e3f4a5b";

SGuid MakeGuid (FStringView Text)
{
	const std::optional<SGuid> guid = SGuid::Parse(Text);
	EXPECT_TRUE(guid.has_value());
	return guid.value_or(SGuid());
}

SMetaParseResult Parse (FStringView Bytes)
{
	SMetaParseResult result;
	ParseMeta(Bytes, result);
	return result;
}

FString Emit (const SAssetMeta& Meta)
{
	FString out;
	SDiagnostic error;
	EXPECT_TRUE(EmitMeta(Meta, out, &error)) << error.Message.CStr();
	return out;
}

// What a file becomes on its next write.
FString Reemit (FStringView Bytes)
{
	const SMetaParseResult result = Parse(Bytes);
	EXPECT_EQ(result.State, EMetaParseState::Ok) << (result.Diagnostics.empty() ? "" : result.Diagnostics.back().Message.CStr());
	return Emit(result.Meta);
}

const SDiagnostic* FindCode (const SMetaParseResult& Result, uint32 Code)
{
	for (const SDiagnostic& diagnostic : Result.Diagnostics)
	{
		if (diagnostic.Code == Code)
		{
			return &diagnostic;
		}
	}
	return nullptr;
}

// Broken, by the one code that says so, with exactly these GUIDs surviving.
void ExpectBroken (FStringView Bytes, std::vector<SGuid> Salvaged = {})
{
	const SMetaParseResult result = Parse(Bytes);
	EXPECT_EQ(result.State, EMetaParseState::Broken) << Bytes.GetData();
	EXPECT_FALSE(result.bCanonical);
	const SDiagnostic* const broken = FindCode(result, Meta004);
	ASSERT_NE(broken, nullptr) << Bytes.GetData();
	EXPECT_EQ(broken->Severity, ESeverity::Error);
	EXPECT_FALSE(broken->Message.IsEmpty());
	EXPECT_EQ(FindCode(result, Meta005), nullptr);
	EXPECT_EQ(result.SalvagedGuids, Salvaged) << Bytes.GetData();
	EXPECT_FALSE(result.Meta.Guid.IsValid());
}

// The §3.2 examples, without the comments and the spaces before them.
constexpr FStringLiteral TextureMeta =
	"FormatVersion: 1\n"
	"Guid: 5d1e7a3b9c0f42d68e1a2b3c4d5e6f70\n"
	"SettingsVersion: 1\n"
	"Settings:\n"
	"  ColorSpace: Linear\n"
	"  MaxSize: 2048\n"
	"Targets:\n"
	"  Win64:\n"
	"    Compression: BC7\n"
	"Labels:\n"
	"  - Environment\n"
	"  - Rock\n";

constexpr FStringLiteral ModelMeta =
	"FormatVersion: 1\n"
	"Guid: 3f2a9c4e7b1d4e0f9a8b6c5d4e3f2a1b\n"
	"SettingsVersion: 2\n"
	"Settings:\n"
	"  Hierarchy: Flatten\n"
	"  Tangents: Generate\n"
	"  ScaleFactor: 0.01\n"
	"SubAssets:\n"
	"  Renamed:\n"
	"    \"mesh:Body\": \"mesh:Torso\"\n"
	"  Remap:\n"
	"    \"material:Skin\": { $asset: 1c7e0a9b33d24f1e9a6b0c1d2e3f4a5b, path: /Project/Characters/Hero/HeroSkin.frt }\n"
	"  Overrides:\n"
	"    \"image:#0\":\n"
	"      ColorSpace: Srgb\n"
	"Labels:\n"
	"  - Character\n"
	"  - Hero\n"
	"Cook: Auto\n";

constexpr FStringLiteral MaterialMeta =
	"FormatVersion: 1\n"
	"Guid: 1c7e0a9b33d24f1e9a6b0c1d2e3f4a5b\n"
	"Labels:\n"
	"  - Character\n";

constexpr FStringLiteral DataAssetMeta =
	"FormatVersion: 1\n"
	"Guid: 7a41c2d9e8b34f60a1c2b3d4e5f60718\n"
	"Labels:\n"
	"  - Starter\n"
	"  - Weapon\n"
	"FormerNames:\n"
	"  - StarterRifle\n"
	"Cook: Always\n";

// One of every key, so that every path of both directions runs and the order of all of them is pinned.
constexpr FStringLiteral FullMeta =
	"FormatVersion: 1\n"
	"Guid: 5d1e7a3b9c0f42d68e1a2b3c4d5e6f70\n"
	"Importer: Texture.Png\n"
	"SettingsVersion: 3\n"
	"Settings:\n"
	"  MaxSize: 2048\n"
	"Targets:\n"
	"  Win64:\n"
	"    MaxSize: 1024\n"
	"SubAssets:\n"
	"  Renamed:\n"
	"    \"mesh:Body\": \"mesh:Torso\"\n"
	"Labels:\n"
	"  - A\n"
	"  - B\n"
	"FormerNames:\n"
	"  - Older\n"
	"Aliases:\n"
	"  - 3f2a9c4e7b1d4e0f9a8b6c5d4e3f2a1b\n"
	"FormerPaths:\n"
	"  - /Project/Old/Rock.png\n"
	"Cook: Never\n"
	"Chunk: 3\n"
	"RebuildNonce: 1c7e0a9b33d24f1e9a6b0c1d2e3f4a5b\n";
}


// ---- Canonical bytes ----

TEST (MetaFileTest, EveryExampleOfTheFormatReemitsItsExactBytes)
{
	for (const FStringLiteral bytes : { TextureMeta, ModelMeta, MaterialMeta, DataAssetMeta, FullMeta })
	{
		const SMetaParseResult result = Parse(bytes);
		EXPECT_EQ(result.State, EMetaParseState::Ok) << bytes.GetData();
		EXPECT_TRUE(result.bCanonical) << bytes.GetData();
		EXPECT_TRUE(result.Diagnostics.empty()) << bytes.GetData();
		EXPECT_TRUE(result.SalvagedGuids.empty());
		EXPECT_EQ(Emit(result.Meta).View(), FStringView(bytes));
	}
}

TEST (MetaFileTest, EveryKeyIsReadIntoItsField)
{
	const SMetaParseResult result = Parse(FullMeta);
	ASSERT_EQ(result.State, EMetaParseState::Ok);
	const SAssetMeta& meta = result.Meta;
	EXPECT_EQ(meta.FormatVersion, 1u);
	EXPECT_EQ(meta.Guid, MakeGuid(GuidA));
	EXPECT_EQ(meta.Importer.View(), "Texture.Png");
	EXPECT_EQ(meta.SettingsVersion, std::optional<uint32>(3u));
	EXPECT_EQ(meta.Settings.Find("MaxSize").AsInt64(), std::optional<int64>(2048));
	EXPECT_EQ(meta.Targets.Find("Win64.MaxSize").AsInt64(), std::optional<int64>(1024));
	EXPECT_EQ(meta.SubAssets.GetRoot().FindChild("Renamed").FindChild("mesh:Body").GetText(), "mesh:Torso");
	ASSERT_EQ(meta.Labels.size(), 2u);
	EXPECT_EQ(meta.Labels[1].View(), "B");
	ASSERT_EQ(meta.FormerNames.size(), 1u);
	EXPECT_EQ(meta.FormerNames[0].View(), "Older");
	EXPECT_EQ(meta.Aliases, std::vector<SGuid>{ MakeGuid(GuidB) });
	ASSERT_EQ(meta.FormerPaths.size(), 1u);
	EXPECT_EQ(meta.FormerPaths[0].View(), "/Project/Old/Rock.png");
	EXPECT_EQ(meta.Cook, std::optional<ECookRule>(ECookRule::Never));
	EXPECT_EQ(meta.Chunk, std::optional<uint16>(static_cast<uint16>(3u)));
	EXPECT_EQ(meta.RebuildNonce, std::optional<SGuid>(MakeGuid(GuidC)));
	EXPECT_TRUE(meta.UnknownKeys.empty());
}

TEST (MetaFileTest, AMetaWithOnlyTheRequiredKeysStaysThatShortSinceAPresentKeyIsAnOverride)
{
	SAssetMeta meta;
	meta.Guid = MakeGuid(GuidA);
	EXPECT_EQ(Emit(meta).View(), "FormatVersion: 1\nGuid: 5d1e7a3b9c0f42d68e1a2b3c4d5e6f70\n");
	const SMetaParseResult result = Parse(Emit(meta).View());
	EXPECT_TRUE(result.bCanonical);
	EXPECT_TRUE(result.Meta.Importer.IsEmpty());
	EXPECT_FALSE(result.Meta.SettingsVersion.has_value());
	EXPECT_TRUE(result.Meta.Settings.IsEmpty());
	EXPECT_TRUE(result.Meta.Labels.empty());
	EXPECT_FALSE(result.Meta.Cook.has_value());
	EXPECT_FALSE(result.Meta.Chunk.has_value());
	EXPECT_FALSE(result.Meta.RebuildNonce.has_value());
}

TEST (MetaFileTest, AnOverrideEqualToTheDefaultStaysBecauseItIsStillAnOverride)
{
	const FStringView bytes = "FormatVersion: 1\nGuid: 5d1e7a3b9c0f42d68e1a2b3c4d5e6f70\nSettings: {}\nCook: Auto\nChunk: 0\n";
	EXPECT_TRUE(Parse(bytes).bCanonical);
	EXPECT_EQ(Reemit(bytes).View(), bytes);
}

TEST (MetaFileTest, NonAsciiTextRoundTrips)
{
	const FStringView bytes =
		"FormatVersion: 1\n"
		"Guid: 5d1e7a3b9c0f42d68e1a2b3c4d5e6f70\n"
		"Settings:\n"
		"  Заголовок: значення 游戏\n"
		"Labels:\n"
		"  - Гра\n"
		"  - 游戏\n"
		"FormerPaths:\n"
		"  - /Project/Папка 游戏/Камінь.png\n";
	EXPECT_TRUE(Parse(bytes).bCanonical);
	EXPECT_EQ(Reemit(bytes).View(), bytes);
}


// ---- Hand-edited input ----

TEST (MetaFileTest, TheCommentedExamplesOfTheSpecParseAndLoseOnlyTheirCommentsOnTheNextWrite)
{
	const FStringView commented =
		"# Content/Characters/Hero/HeroSkin.frt.meta\n"
		"FormatVersion: 1\n"
		"Guid: 3f2a9c4e7b1d4e0f9a8b6c5d4e3f2a1b\n"
		"SettingsVersion: 2\n"
		"Settings:                       # overrides of: importer default < Assets.yml rules < this\n"
		"  Hierarchy: Flatten            # today's aiProcess_PreTransformVertices\n"
		"  Tangents: Generate\n"
		"  ScaleFactor: 0.01\n"
		"SubAssets:\n"
		"  Renamed:                      # confirmed DCC rename\n"
		"    \"mesh:Body\": \"mesh:Torso\"\n"
		"  Remap:\n"
		"    \"material:Skin\": { $asset: 1c7e0a9b33d24f1e9a6b0c1d2e3f4a5b, path: /Project/Characters/Hero/HeroSkin.frt }\n"
		"  Overrides:\n"
		"    \"image:#0\":                 # embedded image without a name\n"
		"      ColorSpace: Srgb\n"
		"Labels:\n"
		"  - Character\n"
		"  - Hero\n"
		"Cook: Auto\n";
	const SMetaParseResult result = Parse(commented);
	EXPECT_EQ(result.State, EMetaParseState::Ok);
	EXPECT_FALSE(result.bCanonical);
	const SDiagnostic* const nonCanonical = FindCode(result, Meta005);
	ASSERT_NE(nonCanonical, nullptr);
	EXPECT_EQ(nonCanonical->Severity, ESeverity::Warning);
	EXPECT_EQ(Emit(result.Meta).View(), FStringView(ModelMeta));
}

TEST (MetaFileTest, TopLevelKeysGoInTheirFixedOrderHoweverAPersonOrderedThem)
{
	const FStringView shuffled =
		"RebuildNonce: 1c7e0a9b33d24f1e9a6b0c1d2e3f4a5b\n"
		"Chunk: 3\n"
		"Cook: Never\n"
		"FormerPaths:\n  - /Project/Old/Rock.png\n"
		"Aliases:\n  - 3f2a9c4e7b1d4e0f9a8b6c5d4e3f2a1b\n"
		"FormerNames:\n  - Older\n"
		"Labels:\n  - B\n  - A\n"
		"SubAssets:\n  Renamed:\n    \"mesh:Body\": \"mesh:Torso\"\n"
		"Targets:\n  Win64:\n    MaxSize: 1024\n"
		"Settings:\n  MaxSize: 2048\n"
		"SettingsVersion: 3\n"
		"Importer: Texture.Png\n"
		"Guid: 5d1e7a3b9c0f42d68e1a2b3c4d5e6f70\n"
		"FormatVersion: 1\n";
	EXPECT_FALSE(Parse(shuffled).bCanonical);
	EXPECT_EQ(Reemit(shuffled).View(), FStringView(FullMeta));
}

TEST (MetaFileTest, LabelsAreSortedByBytesAndARepeatedOneGoes)
{
	const FString out = Reemit("FormatVersion: 1\nGuid: 5d1e7a3b9c0f42d68e1a2b3c4d5e6f70\nLabels: [ b, Z, a, b, Я ]\n");
	EXPECT_EQ(out.View(), "FormatVersion: 1\nGuid: 5d1e7a3b9c0f42d68e1a2b3c4d5e6f70\nLabels:\n  - Z\n  - a\n  - b\n  - Я\n");
}

TEST (MetaFileTest, FormerNamesAndPathsAndAliasesKeepTheirOrder)
{
	const FStringView bytes =
		"FormatVersion: 1\n"
		"Guid: 5d1e7a3b9c0f42d68e1a2b3c4d5e6f70\n"
		"FormerNames:\n  - Z\n  - A\n"
		"Aliases:\n  - 3f2a9c4e7b1d4e0f9a8b6c5d4e3f2a1b\n  - 1c7e0a9b33d24f1e9a6b0c1d2e3f4a5b\n"
		"FormerPaths:\n  - /Project/Z.png\n  - /Project/A.png\n";
	EXPECT_TRUE(Parse(bytes).bCanonical);
}

TEST (MetaFileTest, SettingsKeepTheirKeysInTheOrderReadUntilReflectionDeclaresOne)
{
	const FStringView bytes = "FormatVersion: 1\nGuid: 5d1e7a3b9c0f42d68e1a2b3c4d5e6f70\nSettings:\n  Zed: 1\n  Alpha: 2\n";
	EXPECT_TRUE(Parse(bytes).bCanonical);
	EXPECT_EQ(Reemit(bytes).View(), bytes);
}

TEST (MetaFileTest, ValuesInsideATreeTakeTheirCanonicalStyle)
{
	const FString out = Reemit(
		"FormatVersion: 1\nGuid: 5d1e7a3b9c0f42d68e1a2b3c4d5e6f70\n"
		"Settings: { ScaleFactor: 1.50, Name: 'Rock', Quoted: '12' }\n"
		"Targets: { Win64: { MaxSize: 1024 } }\n");
	EXPECT_EQ(out.View(),
		"FormatVersion: 1\nGuid: 5d1e7a3b9c0f42d68e1a2b3c4d5e6f70\n"
		"Settings:\n  ScaleFactor: 1.5\n  Name: Rock\n  Quoted: \"12\"\n"
		"Targets:\n  Win64:\n    MaxSize: 1024\n");
}

TEST (MetaFileTest, AKeyThatHoldsNothingReadsAsAbsentAndGoesOnTheNextWrite)
{
	const FStringView bytes =
		"FormatVersion: 1\nGuid: 5d1e7a3b9c0f42d68e1a2b3c4d5e6f70\nImporter:\nSettings: ~\nLabels:\nFormerNames: []\nCook: null\nChunk:\n";
	const SMetaParseResult result = Parse(bytes);
	EXPECT_EQ(result.State, EMetaParseState::Ok);
	EXPECT_FALSE(result.bCanonical);
	EXPECT_EQ(Emit(result.Meta).View(), "FormatVersion: 1\nGuid: 5d1e7a3b9c0f42d68e1a2b3c4d5e6f70\n");
}

TEST (MetaFileTest, SettingsVersionIsWrittenOnlyWithSettings)
{
	const FStringView bytes = "FormatVersion: 1\nGuid: 5d1e7a3b9c0f42d68e1a2b3c4d5e6f70\nSettingsVersion: 2\n";
	const SMetaParseResult result = Parse(bytes);
	EXPECT_EQ(result.Meta.SettingsVersion, std::optional<uint32>(2u));
	EXPECT_FALSE(result.bCanonical);
	EXPECT_EQ(Emit(result.Meta).View(), "FormatVersion: 1\nGuid: 5d1e7a3b9c0f42d68e1a2b3c4d5e6f70\n");
}

TEST (MetaFileTest, ACookRuleIsNormalisedToItsReflectedName)
{
	EXPECT_EQ(Reemit("FormatVersion: 1\nGuid: 5d1e7a3b9c0f42d68e1a2b3c4d5e6f70\nCook: editoronly\n").View(),
		"FormatVersion: 1\nGuid: 5d1e7a3b9c0f42d68e1a2b3c4d5e6f70\nCook: EditorOnly\n");
}


// ---- GUIDs ----

TEST (MetaFileTest, EverySpellingOfAGuidReadsAndIsNormalisedOnTheNextWrite)
{
	static constexpr FStringLiteral Spellings[] = {
		"5d1e7a3b-9c0f-42d6-8e1a-2b3c4d5e6f70",
		"5D1E7A3B9C0F42D68E1A2B3C4D5E6F70",
		"5D1E7A3B-9C0F-42D6-8E1A-2B3C4D5E6F70",
		"\"5d1e7a3b9c0f42d68e1a2b3c4d5e6f70\"",
		"'5D1E7A3B-9C0F-42D6-8E1A-2B3C4D5E6F70'",
		"\"{5d1e7a3b-9c0f-42d6-8e1a-2b3c4d5e6f70}\"",
		"{5d1e7a3b-9c0f-42d6-8e1a-2b3c4d5e6f70}",          // unquoted, YAML reads a flow map with one empty value
		"{5D1E7A3B9C0F42D68E1A2B3C4D5E6F70}",
	};
	for (const FStringLiteral spelling : Spellings)
	{
		const FString bytes = FString::Format("FormatVersion: 1\nGuid: {}\n", spelling);
		const SMetaParseResult result = Parse(bytes.View());
		ASSERT_EQ(result.State, EMetaParseState::Ok) << bytes.CStr();
		EXPECT_EQ(result.Meta.Guid, MakeGuid(GuidA)) << bytes.CStr();
		EXPECT_FALSE(result.bCanonical);
		EXPECT_EQ(Emit(result.Meta).View(), "FormatVersion: 1\nGuid: 5d1e7a3b9c0f42d68e1a2b3c4d5e6f70\n");
	}
}

TEST (MetaFileTest, AliasesAndTheNonceTakeTheSameSpellings)
{
	const FString out = Reemit(
		"FormatVersion: 1\nGuid: 5d1e7a3b9c0f42d68e1a2b3c4d5e6f70\n"
		"Aliases:\n  - 3F2A9C4E-7B1D-4E0F-9A8B-6C5D4E3F2A1B\n  - {1c7e0a9b-33d2-4f1e-9a6b-0c1d2e3f4a5b}\n"
		"RebuildNonce: {1C7E0A9B33D24F1E9A6B0C1D2E3F4A5B}\n");
	EXPECT_EQ(out.View(),
		"FormatVersion: 1\nGuid: 5d1e7a3b9c0f42d68e1a2b3c4d5e6f70\n"
		"Aliases:\n  - 3f2a9c4e7b1d4e0f9a8b6c5d4e3f2a1b\n  - 1c7e0a9b33d24f1e9a6b0c1d2e3f4a5b\n"
		"RebuildNonce: 1c7e0a9b33d24f1e9a6b0c1d2e3f4a5b\n");
}

TEST (MetaFileTest, AGuidWhoseTextSpellsANumberIsQuotedSoNoReaderTypesIt)
{
	// Generated GUIDs avoid this (SGuid::HasNumericText), but a person can type one.
	for (const FStringView text : { FStringView("12345678901234567890123456789012"), FStringView("1234567890123456789012345678e123") })
	{
		SAssetMeta meta;
		meta.Guid = MakeGuid(text);
		const FString out = Emit(meta);
		EXPECT_EQ(out.View(), FString::Format("FormatVersion: 1\nGuid: \"{}\"\n", text).View());
		const SMetaParseResult result = Parse(out.View());
		EXPECT_EQ(result.Meta.Guid, meta.Guid);
		EXPECT_TRUE(result.bCanonical);
		// The plain form still reads, from its text and not from what it would be as a number.
		EXPECT_EQ(Parse(FString::Format("FormatVersion: 1\nGuid: {}\n", text).View()).Meta.Guid, meta.Guid);
	}
}


// ---- Unknown keys ----

TEST (MetaFileTest, UnknownKeysAreKeptAfterTheKnownOnesInFileOrderAndReported)
{
	const FStringView bytes =
		"Zeta: 1\n"
		"FormatVersion: 1\n"
		"Future:\n  Nested: [ a, { b: 2.50 } ]\n"
		"Guid: 5d1e7a3b9c0f42d68e1a2b3c4d5e6f70\n"
		"Alpha: ~\n";
	const SMetaParseResult result = Parse(bytes);
	ASSERT_EQ(result.State, EMetaParseState::Ok);
	ASSERT_EQ(result.Meta.UnknownKeys.size(), 3u);
	EXPECT_EQ(result.Meta.UnknownKeys[0].Key.View(), "Zeta");
	EXPECT_EQ(result.Meta.UnknownKeys[1].Key.View(), "Future");
	EXPECT_EQ(result.Meta.UnknownKeys[2].Key.View(), "Alpha");
	EXPECT_TRUE(result.Meta.UnknownKeys[2].Value.GetRoot().IsNull());

	uint32 reported = 0u;
	for (const SDiagnostic& diagnostic : result.Diagnostics)
	{
		if (diagnostic.Code == 0u)
		{
			EXPECT_EQ(diagnostic.Severity, ESeverity::Warning);
			EXPECT_FALSE(diagnostic.FieldPath.IsEmpty());
			EXPECT_NE(diagnostic.Line, 0u);
			++reported;
		}
	}
	EXPECT_EQ(reported, 3u);

	EXPECT_EQ(Emit(result.Meta).View(),
		"FormatVersion: 1\nGuid: 5d1e7a3b9c0f42d68e1a2b3c4d5e6f70\n"
		"Zeta: 1\nFuture:\n  Nested:\n    - a\n    - b: 2.5\nAlpha: null\n");
}

TEST (MetaFileTest, ACanonicalFileWithAnUnknownKeyIsStillCanonicalAndStillReported)
{
	const FStringView bytes = "FormatVersion: 1\nGuid: 5d1e7a3b9c0f42d68e1a2b3c4d5e6f70\nCook: Auto\nTomorrow: 5\n";
	const SMetaParseResult result = Parse(bytes);
	EXPECT_TRUE(result.bCanonical);
	EXPECT_EQ(FindCode(result, Meta005), nullptr);
	ASSERT_EQ(result.Diagnostics.size(), 1u);
	EXPECT_EQ(result.Diagnostics[0].FieldPath.View(), "Tomorrow");
	EXPECT_EQ(Emit(result.Meta).View(), bytes);
}

TEST (MetaFileTest, ANewerFormatVersionIsKeptAndReportedNotRefused)
{
	const FStringView bytes = "FormatVersion: 9\nGuid: 5d1e7a3b9c0f42d68e1a2b3c4d5e6f70\nCook: Auto\n";
	const SMetaParseResult result = Parse(bytes);
	EXPECT_EQ(result.State, EMetaParseState::Ok);
	EXPECT_EQ(result.Meta.FormatVersion, 9u);
	EXPECT_TRUE(result.bCanonical);
	ASSERT_EQ(result.Diagnostics.size(), 1u);
	EXPECT_EQ(result.Diagnostics[0].Severity, ESeverity::Warning);
	EXPECT_EQ(Emit(result.Meta).View(), bytes);
}


// ---- Bytes the text does not show ----

TEST (MetaFileTest, ABomIsAFactOfTheFileAndGoesOnTheNextWrite)
{
	const SMetaParseResult result = Parse("\xEF\xBB\xBF" "FormatVersion: 1\nGuid: 5d1e7a3b9c0f42d68e1a2b3c4d5e6f70\n");
	EXPECT_EQ(result.State, EMetaParseState::Ok);
	EXPECT_TRUE(result.bHasBom);
	EXPECT_FALSE(result.bCanonical);
	EXPECT_NE(FindCode(result, Meta005), nullptr);
	EXPECT_EQ(result.Meta.Guid, MakeGuid(GuidA));
	EXPECT_EQ(Emit(result.Meta).View(), "FormatVersion: 1\nGuid: 5d1e7a3b9c0f42d68e1a2b3c4d5e6f70\n");
}

TEST (MetaFileTest, CrLfIsAFactOfTheFileAndAnErrorOfItsOwn)
{
	const SMetaParseResult result = Parse("FormatVersion: 1\r\nGuid: 5d1e7a3b9c0f42d68e1a2b3c4d5e6f70\r\nLabels:\r\n  - A\r\n");
	EXPECT_EQ(result.State, EMetaParseState::Ok);
	EXPECT_TRUE(result.bHasCrLf);
	EXPECT_FALSE(result.bCanonical);
	EXPECT_NE(FindCode(result, Meta005), nullptr);
	const SDiagnostic* const crlf = FindCode(result, Vcs001);
	ASSERT_NE(crlf, nullptr);
	EXPECT_EQ(crlf->Severity, ESeverity::Error);
	EXPECT_EQ(Emit(result.Meta).View(), "FormatVersion: 1\nGuid: 5d1e7a3b9c0f42d68e1a2b3c4d5e6f70\nLabels:\n  - A\n");
}

TEST (MetaFileTest, CanonicalBytesCarryNoneOfThoseFacts)
{
	const SMetaParseResult result = Parse(MaterialMeta);
	EXPECT_FALSE(result.bHasBom);
	EXPECT_FALSE(result.bHasCrLf);
	EXPECT_FALSE(result.bInvalidUtf8);
}

TEST (MetaFileTest, AMissingFinalNewlineOrAStrayBlankLineIsNotCanonical)
{
	EXPECT_FALSE(Parse("FormatVersion: 1\nGuid: 5d1e7a3b9c0f42d68e1a2b3c4d5e6f70").bCanonical);
	EXPECT_FALSE(Parse("FormatVersion: 1\n\nGuid: 5d1e7a3b9c0f42d68e1a2b3c4d5e6f70\n").bCanonical);
	EXPECT_FALSE(Parse("FormatVersion: 1\nGuid: 5d1e7a3b9c0f42d68e1a2b3c4d5e6f70\n\n").bCanonical);
	EXPECT_FALSE(Parse("FormatVersion: 1\nGuid:   5d1e7a3b9c0f42d68e1a2b3c4d5e6f70\n").bCanonical);
	EXPECT_FALSE(Parse("---\nFormatVersion: 1\nGuid: 5d1e7a3b9c0f42d68e1a2b3c4d5e6f70\n").bCanonical);
}

TEST (MetaFileTest, TheHashIsOfTheRawBytesSoACopyVerifiesAByteForByteRestore)
{
	const FStringView lf = "FormatVersion: 1\nGuid: 5d1e7a3b9c0f42d68e1a2b3c4d5e6f70\n";
	const FStringView crlf = "FormatVersion: 1\r\nGuid: 5d1e7a3b9c0f42d68e1a2b3c4d5e6f70\r\n";
	EXPECT_EQ(HashMetaBytes(lf), hash::Xxh3_64(lf));
	EXPECT_NE(HashMetaBytes(lf), HashMetaBytes(crlf));
}


// ---- Broken ----

TEST (MetaFileTest, AParseErrorIsBrokenAndALoneGuidLineSurvivesIt)
{
	ExpectBroken("FormatVersion: 1\nGuid: 5d1e7a3b9c0f42d68e1a2b3c4d5e6f70\nSettings: { oops\n", { MakeGuid(GuidA) });
	const SMetaParseResult result = Parse("FormatVersion: 1\nGuid: 5d1e7a3b9c0f42d68e1a2b3c4d5e6f70\nSettings: { oops\n");
	EXPECT_NE(FindCode(result, Meta004)->Line, 0u);
}

TEST (MetaFileTest, ConflictMarkersAreBrokenEvenWhereTheLinesWouldParseAndBothSidesGuidsSurvive)
{
	ExpectBroken(
		"FormatVersion: 1\n"
		"<<<<<<< HEAD\n"
		"Guid: 5d1e7a3b9c0f42d68e1a2b3c4d5e6f70\n"
		"=======\n"
		"Guid: 3f2a9c4e7b1d4e0f9a8b6c5d4e3f2a1b\n"
		">>>>>>> feature\n",
		{ MakeGuid(GuidA), MakeGuid(GuidB) });
	// Only the labels conflict: the one identity is unambiguous.
	ExpectBroken(
		"FormatVersion: 1\nGuid: 5d1e7a3b9c0f42d68e1a2b3c4d5e6f70\nLabels:\n"
		"<<<<<<< HEAD\n  - A\n||||||| base\n=======\n  - B\n>>>>>>> feature\n",
		{ MakeGuid(GuidA) });
	EXPECT_EQ(FindCode(Parse("FormatVersion: 1\n<<<<<<< HEAD\nGuid: 5d1e7a3b9c0f42d68e1a2b3c4d5e6f70\n"), Meta004)->Line, 2u);
}

TEST (MetaFileTest, OneSideOfAConflictWithTheSameGuidOnBothSidesSalvagesItOnce)
{
	ExpectBroken(
		"FormatVersion: 1\n<<<<<<< HEAD\nGuid: 5d1e7a3b9c0f42d68e1a2b3c4d5e6f70\nCook: Auto\n=======\n"
		"Guid: 5D1E7A3B-9C0F-42D6-8E1A-2B3C4D5E6F70\nCook: Never\n>>>>>>> feature\n",
		{ MakeGuid(GuidA) });
}

TEST (MetaFileTest, TwoGuidLinesAreBrokenAndBothSurviveInFileOrder)
{
	ExpectBroken("FormatVersion: 1\nGuid: 3f2a9c4e7b1d4e0f9a8b6c5d4e3f2a1b\nGuid: 5d1e7a3b9c0f42d68e1a2b3c4d5e6f70\n",
		{ MakeGuid(GuidB), MakeGuid(GuidA) });
	EXPECT_EQ(FindCode(Parse("FormatVersion: 1\nGuid: 3f2a9c4e7b1d4e0f9a8b6c5d4e3f2a1b\nGuid: 5d1e7a3b9c0f42d68e1a2b3c4d5e6f70\n"), Meta004)->Line, 3u);
}

TEST (MetaFileTest, SalvageReadsEverySpellingOfTheLine)
{
	static constexpr FStringLiteral Lines[] = {
		"Guid: 5d1e7a3b9c0f42d68e1a2b3c4d5e6f70",
		"Guid:    5d1e7a3b9c0f42d68e1a2b3c4d5e6f70   ",
		"Guid: 5d1e7a3b9c0f42d68e1a2b3c4d5e6f70 # a comment",
		"Guid : 5d1e7a3b9c0f42d68e1a2b3c4d5e6f70",
		"Guid: 5D1E7A3B-9C0F-42D6-8E1A-2B3C4D5E6F70",
		"Guid: \"5d1e7a3b9c0f42d68e1a2b3c4d5e6f70\"",
		"Guid: '5d1e7a3b9c0f42d68e1a2b3c4d5e6f70' # quoted",
		"Guid: {5d1e7a3b-9c0f-42d6-8e1a-2b3c4d5e6f70}",
	};
	for (const FStringLiteral line : Lines)
	{
		ExpectBroken(FString::Format("{}\nCook: [ unclosed\n", line).View(), { MakeGuid(GuidA) });
	}
	// CRLF and a BOM do not stop it either.
	ExpectBroken("\xEF\xBB\xBF" "Guid: 5d1e7a3b9c0f42d68e1a2b3c4d5e6f70\r\nCook: [ unclosed\r\n", { MakeGuid(GuidA) });
}

TEST (MetaFileTest, SalvageTakesOnlyTopLevelGuidLinesThatHoldAGuid)
{
	static constexpr FStringLiteral Bodies[] = {
		"  Guid: 5d1e7a3b9c0f42d68e1a2b3c4d5e6f70\nCook: [ unclosed\n",        // nested, not this file's identity
		"# Guid: 5d1e7a3b9c0f42d68e1a2b3c4d5e6f70\nCook: [ unclosed\n",        // a comment
		"Guide: 5d1e7a3b9c0f42d68e1a2b3c4d5e6f70\nCook: [ unclosed\n",         // another key
		"Guid:5d1e7a3b9c0f42d68e1a2b3c4d5e6f70\nCook: [ unclosed\n",           // YAML reads no key there
		"Guid: not a guid\nCook: [ unclosed\n",
		"Guid: 00000000000000000000000000000000\nCook: [ unclosed\n",          // names nothing
		"Guid: \"5d1e7a3b9c0f42d68e1a2b3c4d5e6f70\nCook: [ unclosed\n",        // an open quote
	};
	for (const FStringLiteral body : Bodies)
	{
		ExpectBroken(body);
	}
}

TEST (MetaFileTest, InvalidUtf8IsBrokenAndTheGuidLineStillReads)
{
	const SMetaParseResult result = Parse("FormatVersion: 1\nGuid: 5d1e7a3b9c0f42d68e1a2b3c4d5e6f70\n# caf\xE9\n");
	EXPECT_EQ(result.State, EMetaParseState::Broken);
	EXPECT_TRUE(result.bInvalidUtf8);
	EXPECT_EQ(result.SalvagedGuids, std::vector<SGuid>{ MakeGuid(GuidA) });
	// A UTF-16 file is not valid UTF-8 either.
	EXPECT_EQ(Parse("\xFF\xFEG\0u\0i\0d\0").State, EMetaParseState::Broken);
}

TEST (MetaFileTest, AFileThatHoldsNoMapIsBroken)
{
	ExpectBroken("");
	ExpectBroken("# only a comment\n");
	ExpectBroken("just text\n");
	ExpectBroken("- FormatVersion: 1\n");
}

TEST (MetaFileTest, TheRequiredKeysAreRequired)
{
	ExpectBroken("Guid: 5d1e7a3b9c0f42d68e1a2b3c4d5e6f70\n", { MakeGuid(GuidA) });
	ExpectBroken("FormatVersion: 1\nCook: Auto\n");
	ExpectBroken("FormatVersion: 1\nGuid:\n");
	ExpectBroken("FormatVersion:\nGuid: 5d1e7a3b9c0f42d68e1a2b3c4d5e6f70\n", { MakeGuid(GuidA) });
}

TEST (MetaFileTest, AKnownKeyWithTheWrongShapeBreaksTheMetaRatherThanLosingTheKeyOnTheNextWrite)
{
	static constexpr FStringLiteral Cases[] = {
		"FormatVersion: one\n",
		"FormatVersion: \"1\"\n",
		"FormatVersion: 0\n",
		"FormatVersion: -1\n",
		"FormatVersion: 4294967296\n",
		"Importer: [ a ]\n",
		"SettingsVersion: x\n",
		"SettingsVersion: -2\n",
		"Settings: 5\n",
		"Settings: [ a ]\n",
		"Targets: text\n",
		"SubAssets: text\n",
		"Labels: Rock\n",
		"Labels: { A: 1 }\n",
		"Labels: [ [ a ] ]\n",
		"Labels: [ ~ ]\n",
		"FormerNames: x\n",
		"Aliases: 3f2a9c4e7b1d4e0f9a8b6c5d4e3f2a1b\n",
		"Aliases: [ not-a-guid ]\n",
		"Aliases: [ 00000000000000000000000000000000 ]\n",
		"FormerPaths: /Project/A.png\n",
		"Cook: Banana\n",
		"Cook: [ Auto ]\n",
		"Chunk: 65536\n",
		"Chunk: -1\n",
		"Chunk: 1.5\n",
		"RebuildNonce: 5\n",
		"RebuildNonce: 00000000000000000000000000000000\n",
	};
	for (const FStringLiteral line : Cases)
	{
		const FString bytes = FString::Format("FormatVersion: 1\nGuid: 5d1e7a3b9c0f42d68e1a2b3c4d5e6f70\n{}", line);
		// A second FormatVersion is the parser's duplicate: move it into the Guid line's place.
		const FString moved = FStringView(line).StartsWith("FormatVersion")
			? FString::Format("{}Guid: 5d1e7a3b9c0f42d68e1a2b3c4d5e6f70\n", line) : bytes;
		ExpectBroken(moved.View(), { MakeGuid(GuidA) });
		const SMetaParseResult result = Parse(moved.View());
		const SDiagnostic* const broken = FindCode(result, Meta004);
		ASSERT_NE(broken, nullptr);
		EXPECT_NE(broken->Line, 0u) << moved.CStr();
		EXPECT_FALSE(broken->FieldPath.IsEmpty()) << moved.CStr();
	}
}

TEST (MetaFileTest, AWrongGuidValueIsBrokenWithNothingToSalvage)
{
	ExpectBroken("FormatVersion: 1\nGuid: nonsense\n");
	ExpectBroken("FormatVersion: 1\nGuid: 00000000000000000000000000000000\n");
	ExpectBroken("FormatVersion: 1\nGuid: [ 5d1e7a3b9c0f42d68e1a2b3c4d5e6f70 ]\n");
	ExpectBroken("FormatVersion: 1\nGuid: { a: 1 }\n");
}

TEST (MetaFileTest, ParsingAgainReplacesEverythingFromTheLastFile)
{
	SMetaParseResult result;
	ParseMeta("FormatVersion: 1\r\nGuid: 5d1e7a3b9c0f42d68e1a2b3c4d5e6f70\r\nCook: [\r\n", result);
	EXPECT_EQ(result.State, EMetaParseState::Broken);
	ParseMeta(MaterialMeta, result);
	EXPECT_EQ(result.State, EMetaParseState::Ok);
	EXPECT_TRUE(result.Diagnostics.empty());
	EXPECT_TRUE(result.SalvagedGuids.empty());
	EXPECT_FALSE(result.bHasCrLf);
	EXPECT_TRUE(result.bCanonical);
}


// ---- Refusals ----

TEST (MetaFileTest, AMetaThatCannotBeWrittenFaithfullyIsRefused)
{
	const auto expectRefused = [] (const SAssetMeta& Meta)
	{
		FString out("left over");
		SDiagnostic error;
		EXPECT_FALSE(EmitMeta(Meta, out, &error));
		EXPECT_TRUE(out.IsEmpty());
		EXPECT_FALSE(error.Message.IsEmpty());
	};

	SAssetMeta valid;
	valid.Guid = MakeGuid(GuidA);
	ASSERT_FALSE(Emit(valid).IsEmpty());

	expectRefused(SAssetMeta());   // no Guid

	SAssetMeta meta = valid;
	meta.FormatVersion = 0u;
	expectRefused(meta);

	meta = valid;
	meta.Aliases.push_back(SGuid());
	expectRefused(meta);

	meta = valid;
	meta.RebuildNonce = SGuid();
	expectRefused(meta);

	meta = valid;
	meta.Labels.push_back(FString(FStringView("\xFF\xFE")));
	expectRefused(meta);

	meta = valid;
	reflect::CDataTree scalar;
	ASSERT_TRUE(reflect::ParseYaml("5\n", scalar));
	meta.Settings = scalar;
	expectRefused(meta);

	meta = valid;
	meta.UnknownKeys.push_back({ FString("Cook"), scalar });   // would be a second key of that name
	meta.Cook = ECookRule::Auto;
	expectRefused(meta);
}
