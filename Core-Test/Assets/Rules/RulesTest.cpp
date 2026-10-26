// The rules engine (Assets/Pipeline/Rules.h, design §3.5, §5.4 step 3, §13.4 RULE001/RULE002): rule files parse into
// the normalised entries list-rules shows, with a RulesHash that ignores comments and layout; every error is RULE001
// with file, line and column, and rejects the whole file; per file, membership, importer, settings, cook rule, chunk
// and labels follow the precedence of §3.5.2/§3.5.4 (deepest folder file first, first set wins per field, Final,
// presets resolved innermost-first, Targets blocks, Type/Label selectors that never touch Analyze); scoping keeps
// project, plugin and engine rules apart; a malformed file keeps the last good rules; Types, Validation, RULE002
// and Rules.cache behave as documented.
#include <gtest/gtest.h>

#include <vector>

#include "RulesTestUtils.h"

#include "Assets/AssetTypes.h"
#include "Assets/MountPaths.h"
#include "Assets/Pipeline/ImporterTable.h"
#include "Platform/Platform.h"

using namespace frt;
using namespace frt::assets;
using frt::test::rules::CountCode;
using frt::test::rules::CRulesTree;
using frt::test::rules::DescribeDiagnostics;
using frt::test::rules::FindProvenance;
using frt::test::rules::FindSetting;


namespace
{
SRuleFileOrigin MountOrigin (FStringView Mount = "Project")
{
	return SRuleFileOrigin { FString(Mount), JoinMountPath(Mount, FStringView()), FString("Config/Assets.yml"), ERuleFileLevel::Mount };
}

SRuleFileOrigin FolderOrigin (FStringView FolderMountPath = "/Project/Art")
{
	return SRuleFileOrigin { FString("Project"), FString(FolderMountPath), FString("Content/Art/Assets.yml"), ERuleFileLevel::Folder };
}

std::shared_ptr<const CRuleFile> Parse (FStringView Text, std::vector<SDiagnostic>& OutDiagnostics, const SRuleFileOrigin& Origin = MountOrigin())
{
	return CRuleFile::Parse(Text, Origin, OutDiagnostics);
}

// The diagnostics of a text that must fail, each RULE001 with the origin's file and subject.
std::vector<SDiagnostic> ParseErrors (FStringView Text, const SRuleFileOrigin& Origin = MountOrigin())
{
	std::vector<SDiagnostic> diagnostics;
	const std::shared_ptr<const CRuleFile> file = CRuleFile::Parse(Text, Origin, diagnostics);
	EXPECT_EQ(file, nullptr) << Text.GetData();
	for (const SDiagnostic& diagnostic : diagnostics)
	{
		EXPECT_EQ(diagnostic.Code, Rule001);
		EXPECT_EQ(diagnostic.Severity, ESeverity::Error);
		EXPECT_EQ(diagnostic.File, Origin.File);
		EXPECT_EQ(diagnostic.Subject, Origin.FolderMountPath);
	}
	return diagnostics;
}

bool HasError (std::span<const SDiagnostic> Diagnostics, uint32 Line, FStringView Needle)
{
	return std::any_of(Diagnostics.begin(), Diagnostics.end(), [&] (const SDiagnostic& Diagnostic)
	{
		return Diagnostic.Line == Line && Diagnostic.Message.Contains(Needle);
	});
}

// The §3.5.3 project example, minus nothing: every section a mount file may hold.
constexpr FStringLiteral FullExample = R"(# <project>/Config/Assets.yml
FormatVersion: 1

Scan:
  Include: [ "**" ]
  Exclude:
    - "**/Reference/**"
    - "Docs/**"
  Quiet:
    - "license.txt"
    - "*.md"
  Raw:
    - "Data/Raw/**"
    - "Text/**/*.json"

Importers:
  Exclude: [ ]

Types:
  Include: [ ]
  Exclude: [ Game.DebugData ]

Presets:
  Texture/Color:  { ColorSpace: Srgb,   Mips: Box,  Compression: None }
  Texture/Normal: { ColorSpace: Normal, Mips: Box,  Compression: None }
  Texture/Mask:   { ColorSpace: Linear, Mips: Box,  Compression: None }
  Texture/UI:     { ColorSpace: Srgb,   Mips: None, Compression: None, MaxSize: 1024 }
  Model.Gltf/Static: { Hierarchy: Flatten, Tangents: Generate, ScaleFactor: 1.0 }

Rules:
  - Match: "UI/**/*.{png,tga}"
    Preset: Texture/UI
    Labels: [ UI ]
  - Match: [ "*_n.*", "*_normal.*", "*Normal*.png" ]
    Preset: Texture/Normal
  - Match: [ "*_orm.*", "*_mr.*", "*_rough*.*" ]
    Preset: Texture/Mask
  - Match: "Models/Sponza/textures/*.jpg"
    For: Texture
    Settings: { MaxSize: 2048 }
  - Match: "Data/**/*.table.csv"
    Importer: DataTable.Csv
  - Match: "Audio/**"
    Targets:
      Win64-Headless: { Cook: Never }
  - Match: "Debug/**"
    Cook: DevelopmentOnly
  - Match: "Maps/Test/**"
    Cook: EditorOnly
  - Match: "Levels/Desert/**"
    Chunk: 1
  - Match: "Data/Legacy/**"
    Cook: Never
    Final: true
  - Type: frt.Texture
    Label: Prototype
    Cook: DevelopmentOnly
  - Match: "**/*.{png,jpg,jpeg,tga,psd,kra}"
    Preset: Texture/Color
  - Match: "**/*.{gltf,glb}"
    Preset: Model.Gltf/Static

PrimaryAssetTypes:
  - Type: Weapon
    Directories: [ "Data/Weapons/**" ]
    Cook: Always
  - Type: DataTable
    Directories: [ "Data/**" ]
    Cook: Always

Cook:
  Mode: AllIncluded
  Roots: [ ]
  PathLookup: true
  CookedTags:
    frt.Texture: [ Width, Height ]
  Targets: [ Win64-D3D12, Win64-Headless ]
  ContentId: 9c41e2a07b5d4f18a3c6d0e2f4b6a8c1
  ContentVersion: 1

Safety:
  AutoRepairMetas: true
  DerivedAutoGuids: true
  AutoResolveDuplicates: true
  OrphanPairing: true
  BuilderSourceGuard: true
  IdLedger: false
  GuestProcesses: false

Validation:
  TYPE003: Warning
  REF010: Info
  Naming:
    - { Type: frt.Texture,  Pattern: "T_*" }
)";

const SRuleListEntry* FindEntry (const CRuleFile& File, FStringView Kind, uint32 Nth = 0u)
{
	for (const SRuleListEntry& entry : File.GetEntries())
	{
		if (entry.Kind == Kind && Nth-- == 0u)
		{
			return &entry;
		}
	}
	return nullptr;
}
}


TEST(RulesParse, TheSpecsFullExampleParsesIntoNormalisedEntries)
{
	std::vector<SDiagnostic> diagnostics;
	const std::shared_ptr<const CRuleFile> file = Parse(FullExample, diagnostics);
	ASSERT_NE(file, nullptr) << DescribeDiagnostics(diagnostics).CStr();
	EXPECT_TRUE(diagnostics.empty());
	EXPECT_NE(file->GetRulesHash(), 0u);

	const SRuleListEntry* exclude = FindEntry(*file, "Scan.Exclude", 1u);
	ASSERT_NE(exclude, nullptr);
	EXPECT_EQ(exclude->Text, "\"Docs/**\"");
	EXPECT_EQ(exclude->Line, 8u);

	const SRuleListEntry* types = FindEntry(*file, "Types.Exclude");
	ASSERT_NE(types, nullptr);
	EXPECT_EQ(types->Text, "\"Game.DebugData\"") << "a game type is kept as written until reflection knows it";

	const SRuleListEntry* ui = FindEntry(*file, "Preset", 3u);
	ASSERT_NE(ui, nullptr);
	EXPECT_EQ(ui->Text, "\"Texture/UI\": { ColorSpace: Srgb, Mips: None, Compression: None, MaxSize: 1024 }");
	const SRuleListEntry* gltf = FindEntry(*file, "Preset", 4u);
	ASSERT_NE(gltf, nullptr);
	EXPECT_EQ(gltf->Text, "\"Model.Gltf/Static\": { Hierarchy: Flatten, Tangents: Generate, ScaleFactor: 1 }");

	const SRuleListEntry* first = FindEntry(*file, "Rule", 0u);
	ASSERT_NE(first, nullptr);
	EXPECT_EQ(first->RuleIndex, 0);
	EXPECT_EQ(first->Text, "{ Match: [ \"UI/**/*.{png,tga}\" ], Preset: \"Texture/UI\", Labels: [ \"UI\" ] }");
	const SRuleListEntry* sponza = FindEntry(*file, "Rule", 3u);
	ASSERT_NE(sponza, nullptr);
	EXPECT_EQ(sponza->Text, "{ Match: [ \"Models/Sponza/textures/*.jpg\" ], For: Texture, Settings: { MaxSize: 2048 } }");
	const SRuleListEntry* audio = FindEntry(*file, "Rule", 5u);
	ASSERT_NE(audio, nullptr);
	EXPECT_EQ(audio->Text, "{ Match: [ \"Audio/**\" ], Targets: { Win64-Headless: { Cook: Never } } }");
	const SRuleListEntry* legacy = FindEntry(*file, "Rule", 9u);
	ASSERT_NE(legacy, nullptr);
	EXPECT_EQ(legacy->Text, "{ Match: [ \"Data/Legacy/**\" ], Cook: Never, Final: true }");
	const SRuleListEntry* prototype = FindEntry(*file, "Rule", 10u);
	ASSERT_NE(prototype, nullptr);
	EXPECT_EQ(prototype->Text, "{ Type: frt.Texture, Label: \"Prototype\", Cook: DevelopmentOnly }");
	EXPECT_EQ(FindEntry(*file, "Rule", 13u), nullptr);

	ASSERT_NE(FindEntry(*file, "Cook.CookedTags"), nullptr);
	EXPECT_EQ(FindEntry(*file, "Cook.CookedTags")->Text, "frt.Texture: [ Width, Height ]");
	EXPECT_EQ(FindEntry(*file, "Cook.Targets")->Text, "[ Win64-D3D12, Win64-Headless ]");
	EXPECT_EQ(FindEntry(*file, "Validation", 1u)->Text, "REF010: Info");
	EXPECT_EQ(FindEntry(*file, "Validation.Naming")->Text, "{ Type: frt.Texture, Pattern: \"T_*\" }");
	EXPECT_EQ(FindEntry(*file, "Safety", 6u)->Text, "GuestProcesses: false");
	EXPECT_EQ(FindEntry(*file, "PrimaryAssetTypes", 1u)->Text, "{ Type: \"DataTable\", Directories: [ \"Data/**\" ], Cook: Always }");
}

TEST(RulesParse, TheRulesHashIgnoresCommentsLayoutAndLinesButNotRules)
{
	constexpr FStringLiteral Base = "FormatVersion: 1\nScan:\n  Exclude: [ \"Docs/**\" ]\nRules:\n  - { Match: \"*.png\", Cook: Never }\n";
	constexpr FStringLiteral Reformatted = "# a comment\n\nFormatVersion: 1   # trailing\n\nScan:\n  Exclude:\n    - 'Docs/**'\n"
		"Rules:\n  - Match: \"*.png\"\n    Cook: never\n";
	constexpr FStringLiteral Changed = "FormatVersion: 1\nScan:\n  Exclude: [ \"Docs/**\" ]\nRules:\n  - { Match: \"*.png\", Cook: Always }\n";
	std::vector<SDiagnostic> diagnostics;
	const auto base = Parse(Base, diagnostics);
	const auto reformatted = Parse(Reformatted, diagnostics);
	const auto changed = Parse(Changed, diagnostics);
	ASSERT_TRUE(base && reformatted && changed) << DescribeDiagnostics(diagnostics).CStr();
	EXPECT_EQ(base->GetRulesHash(), reformatted->GetRulesHash());
	EXPECT_NE(base->GetRulesHash(), changed->GetRulesHash());
	EXPECT_NE(FindEntry(*base, "Rule")->Line, FindEntry(*reformatted, "Rule")->Line);

	// A file of comments only has rules (none) and a hash; that hash is not 0.
	const auto empty = Parse("# nothing yet\n", diagnostics);
	ASSERT_NE(empty, nullptr);
	EXPECT_TRUE(empty->GetEntries().empty());
	EXPECT_NE(empty->GetRulesHash(), 0u);
}

TEST(RulesParse, EveryErrorIsARule001WithItsPositionAndRejectsTheFile)
{
	const std::vector<SDiagnostic> errors = ParseErrors(
		"FormatVersion: 1\n"                                    // 1
		"Scan:\n"                                               // 2
		"  Exclude: [ \"[unclosed\" ]\n"                        // 3 bad glob
		"  Ignore: [ \"x\" ]\n"                                 // 4 unknown key
		"Importers:\n"                                          // 5
		"  Exclude: [ Model.Gltff ]\n"                          // 6 unknown importer
		"Presets:\n"                                            // 7
		"  Texture/Bad: { ColorSpace: Purple, MaxSize: big }\n" // 8 bad enum value, bad int
		"  Nope/Bad: { }\n"                                     // 9 unknown importer
		"  Raw/Bad: { }\n"                                      // 10 importer without settings
		"Rules:\n"                                              // 11
		"  - Match: \"*.png\"\n"                                // 12
		"    Final: yes\n"                                      // 13 not a bool
		"    Colour: Red\n"                                     // 14 unknown rule key
		"  - Match: \"*.png\"\n"                                // 15
		"    Settings: { MaxSize: 1 }\n"                        // 16 Settings without For
		"  - Type: frt.Textur\n"                                // 17 unknown engine type
		"  - Type: frt.Texture\n"                               // 18
		"    Importer: Raw\n"                                   // 19 a Type rule cannot set Importer
		"  - Match: \"*.ogg\"\n"                                // 20
		"    Targets: { Win32: { Cook: Never } }\n"             // 21 unknown target
		"  - Match: \"*.ogg\"\n"                                // 22
		"    Cook: Sometimes\n"                                 // 23 unknown cook rule
		"    Chunk: 70000\n"                                    // 24 out of range
		"Validation:\n"                                         // 25
		"  ref010: Info\n"                                      // 26 not a code
		"  REF010: Loud\n"                                      // 27 not a severity
		"Extras: 1\n");                                         // 28 unknown section
	const std::pair<uint32, FStringView> expected[] = {
		{ 3u, "bad glob" }, { 4u, "unknown key 'Ignore'" }, { 6u, "unknown importer 'Model.Gltff'" }, { 8u, "'Purple' is not a ColorSpace value" },
		{ 8u, "MaxSize must be a whole number" }, { 9u, "unknown importer 'Nope'" }, { 10u, "has no settings" }, { 13u, "Final must be true or false" },
		{ 14u, "unknown key 'Colour'" }, { 16u, "Settings need 'For: <Importer>'" }, { 17u, "unknown engine type 'frt.Textur'" },
		{ 19u, "cannot set Importer" }, { 21u, "unknown platform or target 'Win32'" }, { 23u, "'Sometimes' is not a cook rule" },
		{ 24u, "Chunk must be between 0 and 65535" }, { 26u, "'ref010' is not a diagnostic code" }, { 27u, "'Loud' is not a severity" },
		{ 28u, "unknown section 'Extras'" },
	};
	for (const auto& [line, needle] : expected)
	{
		EXPECT_TRUE(HasError(errors, line, needle)) << "line " << line << ": " << needle.GetData() << DescribeDiagnostics(errors).CStr();
	}
	EXPECT_EQ(errors.size(), std::size(expected)) << DescribeDiagnostics(errors).CStr();
	for (const SDiagnostic& error : errors)
	{
		EXPECT_GT(error.Column, 0u) << error.Message.CStr();
	}
}

TEST(RulesParse, VersionsSyntaxAndShapeAreChecked)
{
	EXPECT_TRUE(HasError(ParseErrors("Scan: { Exclude: [ \"a\" ] }\n"), 1u, "FormatVersion is missing"));
	EXPECT_TRUE(HasError(ParseErrors("FormatVersion: 2\n"), 1u, "unsupported FormatVersion 2"));
	EXPECT_TRUE(HasError(ParseErrors("FormatVersion: 1\nRules: { Match: x }\n"), 2u, "Rules must be a list"));
	EXPECT_TRUE(HasError(ParseErrors("- FormatVersion: 1\n"), 1u, "a map of sections"));
	const std::vector<SDiagnostic> yaml = ParseErrors("FormatVersion: 1\nScan:\n  Exclude: [ **/x ]\n");
	ASSERT_EQ(yaml.size(), 1u);
	EXPECT_TRUE(yaml[0].Message.StartsWith("not valid YAML")) << yaml[0].Message.CStr();
	EXPECT_EQ(yaml[0].Line, 3u);
}

TEST(RulesParse, FolderFilesTakeNoMountSectionsAndNoMountPrefixes)
{
	const SRuleFileOrigin folder = FolderOrigin();
	const std::vector<SDiagnostic> errors = ParseErrors(
		"FormatVersion: 1\n"
		"Scan:\n"
		"  Exclude: [ \"/Project/Scans/**\" ]\n"
		"Cook:\n"
		"  Mode: AllIncluded\n"
		"Safety: { IdLedger: true }\n"
		"PrimaryAssetTypes: [ ]\n", folder);
	EXPECT_TRUE(HasError(errors, 3u, "cannot use the mount prefix '/Project/'"));
	EXPECT_TRUE(HasError(errors, 4u, "mount-level section 'Cook'"));
	EXPECT_TRUE(HasError(errors, 6u, "mount-level section 'Safety'"));
	EXPECT_TRUE(HasError(errors, 7u, "mount-level section 'PrimaryAssetTypes'"));
	EXPECT_EQ(errors.size(), 4u) << DescribeDiagnostics(errors).CStr();

	// A leading '/' that names no mount just anchors the pattern to the folder.
	std::vector<SDiagnostic> diagnostics;
	EXPECT_NE(Parse("FormatVersion: 1\nScan: { Exclude: [ \"/Scans/**\" ] }\n", diagnostics, folder), nullptr) << DescribeDiagnostics(diagnostics).CStr();
}

TEST(RulesParse, AMountFileReachesOnlyItsOwnMountButTheEngineFileAnyMount)
{
	EXPECT_TRUE(HasError(ParseErrors("FormatVersion: 1\nScan: { Exclude: [ \"/Engine/Fonts/**\" ] }\n"), 2u, "never to /Engine"));
	EXPECT_TRUE(HasError(ParseErrors("FormatVersion: 1\nRules: [ { Match: \"/Plugin.Foo/**\", Cook: Never } ]\n"), 2u, "never to /Plugin.Foo"));
	std::vector<SDiagnostic> diagnostics;
	EXPECT_NE(Parse("FormatVersion: 1\nScan: { Exclude: [ \"/Project/Docs/**\" ] }\n", diagnostics), nullptr) << DescribeDiagnostics(diagnostics).CStr();
	EXPECT_NE(Parse("FormatVersion: 1\nScan: { Exclude: [ \"/Engine/Shaders/**\", \"/Project/Docs/**\", \"/Plugin.Foo/X/**\" ] }\n", diagnostics,
		MountOrigin("Engine")), nullptr) << DescribeDiagnostics(diagnostics).CStr();

	// The engine owns the frt. types: its own file may not list one in Cook.CookedTags; a project's may.
	EXPECT_TRUE(HasError(ParseErrors("FormatVersion: 1\nCook:\n  CookedTags:\n    frt.Texture: [ Width ]\n", MountOrigin("Engine")), 4u,
		"one of /Engine's own types"));
	EXPECT_TRUE(HasError(ParseErrors("FormatVersion: 1\nCook:\n  CookedTags:\n    frt.Texture: [ Depth ]\n"), 4u, "has no tag 'Depth'"));
	EXPECT_TRUE(HasError(ParseErrors("FormatVersion: 1\nCook: { Targets: [ Win64 ] }\n"), 2u, "unknown target 'Win64'"));
}

TEST(RulesParse, UnavailableImportersAreNoErrorAndEnumValuesFoldToTheirSpelling)
{
	std::vector<SDiagnostic> diagnostics;
	const auto file = Parse("FormatVersion: 1\nPresets:\n  texture.psd/Flat: { colorspace: linear }\nRules:\n"
		"  - { Match: \"*.csv\", Importer: datatable.csv }\n  - { Match: \"*.blend\", Importer: Model.Blender, Cook: auto }\n", diagnostics);
	ASSERT_NE(file, nullptr) << DescribeDiagnostics(diagnostics).CStr();
	EXPECT_EQ(FindEntry(*file, "Preset")->Text, "\"Texture.Psd/Flat\": { ColorSpace: Linear }");
	EXPECT_EQ(FindEntry(*file, "Rule", 0u)->Text, "{ Match: [ \"*.csv\" ], Importer: DataTable.Csv }");
	EXPECT_EQ(FindEntry(*file, "Rule", 1u)->Text, "{ Match: [ \"*.blend\" ], Importer: Model.Blender, Cook: Auto }");
}


// ---------------- Evaluation ----------------

namespace
{
// The engine over a fresh tree: the rule files written first, then the mount table resolved.
struct SEngineFixture
{
	explicit SEngineFixture (FStringView Name)
		: Tree(Name)
	{}

	CRulesEngine& Start (SRulesOptions Options = {})
	{
		Mounts = Tree.Resolve();
		Engine = std::make_unique<CRulesEngine>(Mounts, std::move(Options));
		return *Engine;
	}

	std::shared_ptr<const SScanScope> Open (FStringView FolderMountPath)
	{
		std::vector<SDiagnostic> diagnostics;
		std::shared_ptr<const SScanScope> scope = Engine->OpenScope(FolderMountPath, diagnostics);
		EXPECT_TRUE(diagnostics.empty()) << DescribeDiagnostics(diagnostics).CStr();
		return scope;
	}

	// The scope of FolderMountPath with its own rule file given as text (nullopt: none).
	std::shared_ptr<const SScanScope> Enter (const std::shared_ptr<const SScanScope>& Parent, FStringView FolderMountPath, std::optional<FStringView> Text,
		std::vector<SDiagnostic>* OutDiagnostics = nullptr, uint64* OutHash = nullptr)
	{
		uint64 hash = 0u;
		std::vector<SDiagnostic> diagnostics;
		std::shared_ptr<const SScanScope> scope = Engine->EnterFolderFromText(Parent, FolderMountPath, Text, hash, diagnostics);
		if (OutDiagnostics != nullptr)
		{
			*OutDiagnostics = diagnostics;
		}
		else
		{
			EXPECT_TRUE(diagnostics.empty()) << DescribeDiagnostics(diagnostics).CStr();
		}
		if (OutHash != nullptr)
		{
			*OutHash = hash;
		}
		return scope;
	}

	CRulesTree Tree;
	CMountTable Mounts;
	std::unique_ptr<CRulesEngine> Engine;
};

constexpr FStringLiteral EngineDefaults = R"(FormatVersion: 1
Scan:
  Exclude: [ "/Engine/Shaders/**" ]
  Raw:     [ "/Engine/Fonts/*.{ttf,otf}" ]
  Quiet:   [ "/Engine/Fonts/OFL.txt", "**/README.md" ]
Presets:
  Texture/Color:  { ColorSpace: Srgb,   Mips: Box, Compression: None }
  Texture/Normal: { ColorSpace: Normal, Mips: Box, Compression: None }
Rules:
  - { Match: "/Engine/Defaults/**", Cook: Always }
  - { Match: "**/*.{png,jpg,jpeg,tga,bmp,psd,kra,ora}", Preset: Texture/Color }
)";
}

TEST(RulesEngine, MembershipFollowsIncludeExcludeRawQuietAndTheImporterClaims)
{
	SEngineFixture fixture("Membership");
	fixture.Tree.WriteEngineRules(EngineDefaults);
	fixture.Tree.WriteProjectRules("FormatVersion: 1\nScan:\n  Exclude: [ \"Docs/**\", \"*.psd\" ]\n  Quiet: [ \"license.txt\" ]\n  Raw: [ \"Data/Raw/**\" ]\n"
		"Importers:\n  Exclude: [ Model.Gltf ]\nRules:\n  - { Match: \"Tables/*.csv\", Importer: Raw }\n  - { Match: \"*.blend\", Importer: Model.Blender }\n");
	CRulesEngine& engine = fixture.Start();
	const auto project = fixture.Open("/Project");
	const auto root = fixture.Enter(project, "/Project", std::nullopt);

	const auto verdict = [&] (FStringView Path) { return engine.Classify(*root, Path); };
	EXPECT_EQ(verdict("/Project/Rock.png"), (SFileVerdict { EFileClass::Asset, false }));
	EXPECT_EQ(verdict("/Project/Docs/Guide.png"), std::nullopt);
	EXPECT_EQ(verdict("/Project/Art/Layered.PSD"), std::nullopt) << "globs compare under the portable fold";
	EXPECT_EQ(verdict("/Project/Notes/license.txt"), (SFileVerdict { EFileClass::Unclaimed, true }));
	EXPECT_EQ(verdict("/Project/Notes/todo.txt"), (SFileVerdict { EFileClass::Unclaimed, false }));
	EXPECT_EQ(verdict("/Project/Data/Raw/blob.png"), (SFileVerdict { EFileClass::Raw, false })) << "Scan.Raw beats the extension claim";
	EXPECT_EQ(verdict("/Project/Tables/Ammo.csv"), (SFileVerdict { EFileClass::Raw, false })) << "a rule's Importer";
	EXPECT_EQ(verdict("/Project/Models/Fox.gltf"), (SFileVerdict { EFileClass::Unclaimed, true })) << "a disabled importer leaves the file quiet";
	EXPECT_EQ(verdict("/Project/Models/Fox.blend"), (SFileVerdict { EFileClass::Unclaimed, false })) << "Model.Blender is not available in M3";
	EXPECT_EQ(verdict("/Project/Thumbs.db"), std::nullopt) << "built-in excludes";
	EXPECT_EQ(verdict("/Project/README.md"), (SFileVerdict { EFileClass::Unclaimed, true })) << "the engine's Quiet is every mount's fallback";

	EXPECT_FALSE(engine.ShouldDescend(*root, "/Project/Docs"));
	EXPECT_FALSE(engine.ShouldDescend(*root, "/Project/.git"));
	EXPECT_TRUE(engine.ShouldDescend(*root, "/Project/Art"));
	EXPECT_TRUE(engine.ShouldDescend(*project, "/Project"));

	// Include narrows per level: the project's and a folder's must both match.
	const auto narrowed = fixture.Enter(root, "/Project/Ui", FStringView("FormatVersion: 1\nScan: { Include: [ \"*.png\" ] }\n"));
	EXPECT_EQ(engine.Classify(*narrowed, "/Project/Ui/A.png"), (SFileVerdict { EFileClass::Asset, false }));
	EXPECT_EQ(engine.Classify(*narrowed, "/Project/Ui/A.jpg"), std::nullopt);

	// The engine's absolute patterns stay in /Engine.
	const auto engineRoot = fixture.Open("/Engine");
	EXPECT_FALSE(engine.ShouldDescend(*engineRoot, "/Engine/Shaders"));
	EXPECT_TRUE(engine.ShouldDescend(*project, "/Project/Shaders"));
	EXPECT_EQ(engine.Classify(*engineRoot, "/Engine/Fonts/Roboto.ttf"), (SFileVerdict { EFileClass::Raw, false }));
	EXPECT_EQ(engine.Classify(*engineRoot, "/Engine/Fonts/OFL.txt"), (SFileVerdict { EFileClass::Unclaimed, true }));
	EXPECT_EQ(engine.Classify(*root, "/Project/Fonts/Roboto.ttf"), (SFileVerdict { EFileClass::Unclaimed, false }));
}

TEST(RulesEngine, FieldsTakeTheFirstMatchingRuleDeepestFileFirstWithProvenance)
{
	SEngineFixture fixture("Precedence");
	fixture.Tree.WriteEngineRules(EngineDefaults);
	fixture.Tree.WriteProjectRules(
		"FormatVersion: 1\n"                                              // 1
		"Rules:\n"                                                        // 2
		"  - Match: \"*_n.*\"\n"                                          // 3
		"    Preset: Texture/Normal\n"                                    // 4
		"  - Match: \"Environment/**\"\n"                                 // 5
		"    For: Texture\n"                                              // 6
		"    Settings: { MaxSize: 4096, Mips: None }\n"                   // 7
		"    Chunk: 2\n"                                                  // 8
		"    Labels: [ Env ]\n");                                         // 9
	CRulesEngine& engine = fixture.Start();
	const auto project = fixture.Open("/Project");
	const auto foliage = fixture.Enter(project, "/Project/Environment/Foliage", FStringView(
		"FormatVersion: 1\n"                                              // 1
		"Rules:\n"                                                        // 2
		"  - Match: \"*.{png,psd}\"\n"                                    // 3
		"    For: Texture\n"                                              // 4
		"    Settings: { MaxSize: 1024 }\n"                               // 5
		"    Labels: [ Foliage, env ]\n"                                  // 6
		"  - Match: \"Trees/**\"\n"                                       // 7
		"    Chunk: 1\n"));                                               // 8

	const SEffectiveFileSettings bark = engine.ResolveFile(*foliage, "/Project/Environment/Foliage/Trees/Bark_n.png");
	ASSERT_NE(bark.Importer, nullptr);
	EXPECT_EQ(bark.Importer->Name, "Texture");
	EXPECT_EQ(bark.Class, EFileClass::Asset);
	EXPECT_EQ(FindSetting(bark, "MaxSize"), "1024") << "the folder file is consulted first";
	EXPECT_EQ(FindSetting(bark, "ColorSpace"), "Normal") << "a later, outer rule still sets what nothing set before";
	EXPECT_EQ(FindSetting(bark, "Mips"), "Box") << "the project's Texture/Normal preset rule comes before its Environment rule";
	EXPECT_EQ(FindSetting(bark, "Compression"), "None");
	EXPECT_EQ(bark.Chunk, 1u);
	EXPECT_EQ(bark.RuleLabels, (std::vector<FString> { "Foliage", "env" })) << "labels accumulate, once each under the fold, first spelling kept";

	const SProvenance* maxSize = FindProvenance(bark, "MaxSize");
	ASSERT_NE(maxSize, nullptr);
	EXPECT_EQ(maxSize->File, "Content/Environment/Foliage/Assets.yml");
	EXPECT_EQ(maxSize->Line, 5u);
	EXPECT_EQ(maxSize->RuleIndex, 0);
	const SProvenance* colorSpace = FindProvenance(bark, "ColorSpace");
	ASSERT_NE(colorSpace, nullptr);
	EXPECT_EQ(colorSpace->File, "Config/Assets.yml");
	EXPECT_EQ(colorSpace->Line, 4u);
	const SProvenance* chunk = FindProvenance(bark, "Chunk");
	EXPECT_EQ(chunk->File, "Content/Environment/Foliage/Assets.yml");
	EXPECT_EQ(chunk->RuleIndex, 1);
	const SProvenance* importer = FindProvenance(bark, "Importer");
	ASSERT_NE(importer, nullptr);
	EXPECT_TRUE(importer->File.IsEmpty()) << "the extension claim";
	const SProvenance* cook = FindProvenance(bark, "Cook");
	ASSERT_NE(cook, nullptr);
	EXPECT_TRUE(cook->File.IsEmpty()) << "no rule set it";
	EXPECT_EQ(bark.CookRule, ECookRule::Default);

	// Outside the folder file's subtree only the outer files apply.
	const SEffectiveFileSettings ground = engine.ResolveFile(*project, "/Project/Environment/Ground.png");
	EXPECT_EQ(FindSetting(ground, "MaxSize"), "4096");
	EXPECT_EQ(FindSetting(ground, "Mips"), "None");
	EXPECT_EQ(FindSetting(ground, "ColorSpace"), "Srgb") << "the engine's fallback rule and preset";
	EXPECT_EQ(FindProvenance(ground, "ColorSpace")->File, "Config/Assets.yml") << "the engine file relative to its owner root";
	EXPECT_EQ(ground.Chunk, 2u);

	// The cheap form resolves the same values without provenance.
	const SEffectiveFileSettings cheap = engine.ResolveFile(*foliage, "/Project/Environment/Foliage/Trees/Bark_n.png", {}, false);
	EXPECT_TRUE(cheap.Provenance.empty());
	ASSERT_EQ(cheap.Settings.Values.size(), bark.Settings.Values.size());
	for (uint32 i = 0u; i < cheap.Settings.Values.size(); ++i)
	{
		EXPECT_EQ(cheap.Settings.Values[i].Text, bark.Settings.Values[i].Text);
	}
	EXPECT_EQ(cheap.Settings.AnalyzeHash, 0u) << "no M3 setting is Analyze-relevant";
}

TEST(RulesEngine, FinalStopsOuterFilesAndSettingsBeatPresetsInsideOneRule)
{
	SEngineFixture fixture("Final");
	fixture.Tree.WriteEngineRules(EngineDefaults);
	fixture.Tree.WriteProjectRules("FormatVersion: 1\nRules:\n  - { Match: \"**\", Cook: EditorOnly, Labels: [ Outer ] }\n");
	CRulesEngine& engine = fixture.Start();
	const auto project = fixture.Open("/Project");
	const auto legacy = fixture.Enter(project, "/Project/Legacy", FStringView(
		"FormatVersion: 1\nRules:\n"
		"  - { Match: \"*.png\", Preset: Texture/Normal, For: Texture, Settings: { ColorSpace: Linear }, Labels: [ Old ] }\n"
		"  - { Match: \"Frozen/**\", Cook: Never, Final: true }\n"
		"  - { Match: \"**\", Chunk: 7 }\n"));

	const SEffectiveFileSettings frozen = engine.ResolveFile(*legacy, "/Project/Legacy/Frozen/A.png");
	EXPECT_EQ(FindSetting(frozen, "ColorSpace"), "Linear") << "the rule's Settings beat its preset";
	EXPECT_EQ(FindSetting(frozen, "Mips"), "Box") << "the rest of the preset still applies";
	EXPECT_EQ(frozen.CookRule, ECookRule::Never);
	EXPECT_EQ(frozen.Chunk, 0u) << "rules after Final are not consulted";
	EXPECT_EQ(frozen.RuleLabels, (std::vector<FString> { "Old" })) << "nor are outer files";

	const SEffectiveFileSettings loose = engine.ResolveFile(*legacy, "/Project/Legacy/B.png");
	EXPECT_EQ(loose.CookRule, ECookRule::EditorOnly);
	EXPECT_EQ(loose.Chunk, 7u);
	EXPECT_EQ(loose.RuleLabels, (std::vector<FString> { "Old", "Outer" }));

	const SRuleExplanation explanation = engine.Explain(*legacy, "/Project/Legacy/Frozen/A.png");
	ASSERT_TRUE(explanation.Verdict.has_value());
	ASSERT_EQ(explanation.Chain.size(), 3u);
	EXPECT_EQ(explanation.Chain[0].Origin.File, "Content/Legacy/Assets.yml");
	EXPECT_EQ(explanation.Chain[1].Origin.File, "Config/Assets.yml");
	EXPECT_EQ(explanation.Chain[2].Origin.MountName, "Engine");
	const bool bFinalNoted = std::any_of(explanation.Notes.begin(), explanation.Notes.end(),
		[] (const FString& Note) { return Note.Contains("Final rule 2 (Content/Legacy/Assets.yml:4)"); });
	EXPECT_TRUE(bFinalNoted) << (explanation.Notes.empty() ? "" : explanation.Notes.front().CStr());
}

TEST(RulesEngine, PresetsResolveInnermostFirstAndExplainNotesTheShadowing)
{
	SEngineFixture fixture("Presets");
	fixture.Tree.WriteEngineRules(EngineDefaults);
	fixture.Tree.WriteProjectRules("FormatVersion: 1\nPresets:\n  Texture/Color: { ColorSpace: Linear, MaxSize: 512 }\n");
	CRulesEngine& engine = fixture.Start();
	const auto project = fixture.Open("/Project");
	const auto engineRoot = fixture.Open("/Engine");

	// The engine's own rule names Texture/Color; in /Project the project's definition replaces the engine's.
	const SEffectiveFileSettings projectFile = engine.ResolveFile(*project, "/Project/A.png");
	EXPECT_EQ(FindSetting(projectFile, "ColorSpace"), "Linear");
	EXPECT_EQ(FindSetting(projectFile, "MaxSize"), "512");
	EXPECT_EQ(FindSetting(projectFile, "Mips"), "Box") << "fields the preset does not set fall to the defaults";
	const SEffectiveFileSettings engineFile = engine.ResolveFile(*engineRoot, "/Engine/A.png");
	EXPECT_EQ(FindSetting(engineFile, "ColorSpace"), "Srgb");
	EXPECT_EQ(FindSetting(engineFile, "MaxSize"), "0");

	const SRuleExplanation explanation = engine.Explain(*project, "/Project/A.png");
	const bool bShadowNoted = std::any_of(explanation.Notes.begin(), explanation.Notes.end(), [] (const FString& Note)
	{
		return Note.Contains("Texture/Color defined at Config/Assets.yml:3") && Note.Contains("shadows the one at Config/Assets.yml:7");
	});
	EXPECT_TRUE(bShadowNoted) << (explanation.Notes.empty() ? "no notes" : explanation.Notes.front().CStr());

	// A preset no file of the chain defines is a RULE001 when the file is bound, and contributes nothing.
	std::vector<SDiagnostic> diagnostics;
	const auto unknown = fixture.Enter(project, "/Project/Ui", FStringView("FormatVersion: 1\nRules:\n  - { Match: \"*.png\", Preset: Texture/UI }\n"),
		&diagnostics);
	ASSERT_EQ(CountCode(diagnostics, Rule001), 1u) << DescribeDiagnostics(diagnostics).CStr();
	EXPECT_EQ(diagnostics[0].Line, 3u);
	EXPECT_EQ(diagnostics[0].Subject, "/Project/Ui");
	EXPECT_EQ(diagnostics[0].File, "Content/Ui/Assets.yml");
	EXPECT_EQ(FindSetting(engine.ResolveFile(*unknown, "/Project/Ui/A.png"), "ColorSpace"), "Linear");
}

TEST(RulesEngine, SettingsApplyOnlyToImportersOfTheSameTable)
{
	SEngineFixture fixture("Tables");
	fixture.Tree.WriteProjectRules("FormatVersion: 1\nRules:\n"
		"  - { Match: \"Ui/**\", For: Texture, Settings: { MaxSize: 256 }, Cook: Always, Labels: [ Ui ] }\n"
		"  - { Match: \"Ui/**\", For: Model.Gltf, Settings: { ScaleFactor: 0.01 } }\n");
	CRulesEngine& engine = fixture.Start();
	const auto project = fixture.Open("/Project");
	const SEffectiveFileSettings model = engine.ResolveFile(*project, "/Project/Ui/Cursor.gltf");
	EXPECT_EQ(FindSetting(model, "ScaleFactor"), "0.01");
	EXPECT_EQ(model.CookRule, ECookRule::Always) << "a rule's cook and labels apply whatever the importer";
	EXPECT_EQ(model.RuleLabels, (std::vector<FString> { "Ui" }));
	const SEffectiveFileSettings material = engine.ResolveFile(*project, "/Project/Ui/Panel.frtmat.yml");
	ASSERT_NE(material.Importer, nullptr);
	EXPECT_EQ(material.Importer->Name, "Material.Legacy");
	EXPECT_EQ(material.Settings.Table, nullptr);
	EXPECT_TRUE(material.Settings.Values.empty());
}

TEST(RulesEngine, TargetBlocksBeatPlatformBlocksBeatPlainFields)
{
	constexpr FStringLiteral Rules = "FormatVersion: 1\nRules:\n"
		"  - Match: \"Audio/**\"\n"
		"    Cook: Always\n"
		"    For: Texture\n"
		"    Settings: { MaxSize: 64 }\n"
		"    Targets:\n"
		"      Win64: { Chunk: 3, Settings: { MaxSize: 128 } }\n"
		"      Win64-Headless: { Cook: Never }\n";
	constexpr FStringLiteral Targets[] = { "", "Win64-D3D12", "win64-headless" };
	for (const FStringLiteral target : Targets)
	{
		SEngineFixture fixture("Targets");
		fixture.Tree.WriteProjectRules(Rules);
		CRulesEngine& engine = fixture.Start(SRulesOptions { FString(target) });
		const auto project = fixture.Open("/Project");
		const SEffectiveFileSettings settings = engine.ResolveFile(*project, "/Project/Audio/Wave.png");
		const bool bTarget = !target.IsEmpty();
		const bool bHeadless = target == "win64-headless";
		EXPECT_EQ(settings.CookRule, bHeadless ? ECookRule::Never : ECookRule::Always) << target.GetData();
		EXPECT_EQ(settings.Chunk, bTarget ? 3u : 0u) << target.GetData();
		EXPECT_EQ(FindSetting(settings, "MaxSize"), bTarget ? "128" : "64") << target.GetData();
		EXPECT_EQ(FindProvenance(settings, "Cook")->Line, bHeadless ? 9u : 4u);
	}
}

TEST(RulesEngine, TypeAndLabelSelectorsWaitForAnalyzeAndNeverChangeTheImporter)
{
	SEngineFixture fixture("Selectors");
	fixture.Tree.WriteProjectRules("FormatVersion: 1\nRules:\n"
		"  - { Type: frt.Texture, Label: Prototype, Cook: DevelopmentOnly }\n"
		"  - { Type: Texture, Chunk: 4, Final: true }\n"
		"  - { Match: \"*.png\", Importer: Raw }\n"
		"  - { Match: \"*.png\", Labels: [ Late ] }\n");
	CRulesEngine& engine = fixture.Start();
	const auto project = fixture.Open("/Project");

	const SEffectiveFileSettings before = engine.ResolveFile(*project, "/Project/A.png");
	EXPECT_EQ(before.Class, EFileClass::Raw);
	EXPECT_EQ(before.Chunk, 0u);
	EXPECT_EQ(before.RuleLabels, (std::vector<FString> { "Late" }));

	SFileFacts facts;
	facts.Type = asset_types::Texture;
	const SEffectiveFileSettings typed = engine.ResolveFile(*project, "/Project/A.png", facts);
	EXPECT_EQ(typed.Class, EFileClass::Raw) << "a Type rule's Final never stops the importer";
	EXPECT_EQ(typed.Chunk, 4u);
	EXPECT_EQ(typed.CookRule, ECookRule::Default) << "no authored label Prototype";
	EXPECT_TRUE(typed.RuleLabels.empty()) << "the Final stops the labels of later rules";

	const FString prototype = "prototype";
	facts.AuthoredLabels = std::span<const FString>(&prototype, 1u);
	EXPECT_EQ(engine.ResolveFile(*project, "/Project/A.png", facts).CookRule, ECookRule::DevelopmentOnly);
	facts.Type = asset_types::Model;
	EXPECT_EQ(engine.ResolveFile(*project, "/Project/A.png", facts).CookRule, ECookRule::Default);
}

TEST(RulesEngine, ProjectPluginAndEngineRulesStayInTheirMounts)
{
	SEngineFixture fixture("Scoping");
	fixture.Tree.WriteEngineRules("FormatVersion: 1\nRules:\n  - { Match: \"**\", Labels: [ EngineFallback ] }\n");
	fixture.Tree.WriteProjectRules("FormatVersion: 1\nScan: { Exclude: [ \"Secret/**\" ] }\nRules:\n  - { Match: \"**\", Cook: Never, Labels: [ ProjectRule ] }\n");
	fixture.Tree.WritePluginRules("Ворота", "FormatVersion: 1\nRules:\n  - { Match: \"**\", Chunk: 9, Labels: [ PluginRule ] }\n");
	fixture.Tree.WritePluginContent("Ворота", "A.png");
	CRulesEngine& engine = fixture.Start();
	const auto plugin = fixture.Open("/Plugin.Ворота");
	const auto project = fixture.Open("/Project");
	const auto engineRoot = fixture.Open("/Engine");

	const SEffectiveFileSettings pluginFile = engine.ResolveFile(*plugin, "/Plugin.Ворота/Secret/A.png");
	EXPECT_EQ(pluginFile.Class, EFileClass::Asset) << "the project's Exclude never reaches a plugin";
	EXPECT_EQ(pluginFile.CookRule, ECookRule::Default);
	EXPECT_EQ(pluginFile.Chunk, 9u);
	EXPECT_EQ(pluginFile.RuleLabels, (std::vector<FString> { "PluginRule", "EngineFallback" }));
	EXPECT_EQ(FindProvenance(pluginFile, "Chunk")->File, "Plugins/Ворота/Config/Assets.yml");

	const SEffectiveFileSettings projectFile = engine.ResolveFile(*project, "/Project/A.png");
	EXPECT_EQ(projectFile.Chunk, 0u) << "plugin rules never reach /Project";
	EXPECT_EQ(projectFile.RuleLabels, (std::vector<FString> { "ProjectRule", "EngineFallback" }));
	EXPECT_EQ(engine.ResolveFile(*project, "/Project/Secret/A.png").Class, EFileClass::Excluded);

	const SEffectiveFileSettings engineFile = engine.ResolveFile(*engineRoot, "/Engine/A.png");
	EXPECT_EQ(engineFile.RuleLabels, (std::vector<FString> { "EngineFallback" })) << "no other mount's rules reach /Engine";
	EXPECT_EQ(engineFile.CookRule, ECookRule::Default);
}

TEST(RulesEngine, AMalformedFileKeepsTheLastGoodRulesAndAColdOneContributesNothing)
{
	SEngineFixture fixture("LastGood");
	CRulesEngine& engine = fixture.Start();
	const auto project = fixture.Open("/Project");

	// Cold start with a broken file: the parent's scope, RulesHash 0, RULE001.
	std::vector<SDiagnostic> diagnostics;
	uint64 hash = 1u;
	const auto cold = fixture.Enter(project, "/Project/Art", FStringView("FormatVersion: 1\nRules: [ { Match: \"*.png\", Cook: Nope } ]\n"),
		&diagnostics, &hash);
	EXPECT_EQ(cold, project);
	EXPECT_EQ(hash, 0u);
	ASSERT_EQ(CountCode(diagnostics, Rule001), 1u);
	EXPECT_EQ(diagnostics[0].Subject, "/Project/Art");
	EXPECT_EQ(diagnostics[0].File, "Content/Art/Assets.yml");
	EXPECT_EQ(diagnostics[0].Line, 2u);

	// A good version, then a broken edit: the good rules stay, with their hash, and RULE001 is reported.
	uint64 goodHash = 0u;
	const auto good = fixture.Enter(project, "/Project/Art", FStringView("FormatVersion: 1\nRules: [ { Match: \"*.png\", Cook: Never } ]\n"),
		nullptr, &goodHash);
	EXPECT_NE(goodHash, 0u);
	EXPECT_EQ(engine.ResolveFile(*good, "/Project/Art/A.png").CookRule, ECookRule::Never);
	uint64 brokenHash = 0u;
	const auto broken = fixture.Enter(project, "/Project/Art", FStringView("FormatVersion: 1\nRules: [ { Match: \"*.png\", Cook: Never, Oops: 1 } ]\n"),
		&diagnostics, &brokenHash);
	EXPECT_EQ(brokenHash, goodHash);
	EXPECT_EQ(CountCode(diagnostics, Rule001), 1u);
	EXPECT_EQ(engine.ResolveFile(*broken, "/Project/Art/A.png").CookRule, ECookRule::Never);

	// The listing shows the last good entries and the latest errors.
	const SRulesListing listing = engine.GetListing();
	const auto art = std::find_if(listing.Files.begin(), listing.Files.end(), [] (const SRuleFileListing& File) { return File.Origin.FolderMountPath == "/Project/Art"; });
	ASSERT_NE(art, listing.Files.end());
	EXPECT_TRUE(art->bMalformed);
	EXPECT_EQ(art->RulesHash, goodHash);
	EXPECT_FALSE(art->Entries.empty());
	EXPECT_EQ(CountCode(art->Diagnostics, Rule001), 1u);

	// EnterFolderCached repeats the last read without I/O; the file gone, the folder has no rules.
	std::vector<SDiagnostic> cachedDiagnostics;
	uint64 cachedHash = 0u;
	const auto cached = engine.EnterFolderCached(project, "/Project/Art", cachedHash, cachedDiagnostics);
	EXPECT_EQ(cachedHash, goodHash);
	EXPECT_EQ(CountCode(cachedDiagnostics, Rule001), 1u);
	EXPECT_EQ(engine.ResolveFile(*cached, "/Project/Art/A.png").CookRule, ECookRule::Never);
	const auto gone = fixture.Enter(project, "/Project/Art", std::nullopt, nullptr, &hash);
	EXPECT_EQ(gone, project);
	EXPECT_EQ(hash, 0u);
	EXPECT_EQ(engine.EnterFolderCached(project, "/Project/Art", cachedHash, cachedDiagnostics), project);
}

TEST(RulesEngine, MountFilesLoadOnceReportTheirProblemsAtTheMountRootAndReloadOnDemand)
{
	SEngineFixture fixture("MountFiles");
	fixture.Tree.WriteEngineRules(EngineDefaults);
	fixture.Tree.WriteProjectRules("FormatVersion: 1\nRules: [ { Match: \"**\", Cook: Never } ]\n");
	CRulesEngine& engine = fixture.Start();
	std::vector<SDiagnostic> diagnostics;
	std::vector<FString> changed;
	engine.LoadMountRules(&diagnostics, &changed);
	EXPECT_TRUE(diagnostics.empty()) << DescribeDiagnostics(diagnostics).CStr();
	EXPECT_EQ(changed, (std::vector<FString> { "Engine", "Project" })) << "everything at the first load";
	const uint64 projectHash = engine.GetMountRulesHash("Project");
	EXPECT_NE(projectHash, 0u);
	EXPECT_NE(engine.GetMountRulesHash("Engine"), 0u);

	changed.clear();
	engine.LoadMountRules(&diagnostics, &changed);
	EXPECT_TRUE(changed.empty()) << "nothing changed";

	// A broken project file: the last good rules stay; the problem is reported at the mount root only.
	fixture.Tree.WriteProjectRules("FormatVersion: 1\nRules: [ { Match: \"**\", Cook: Never } ]\nBogus: 1\n");
	changed.clear();
	engine.LoadMountRules(&diagnostics, &changed);
	EXPECT_TRUE(changed.empty());
	EXPECT_EQ(engine.GetMountRulesHash("Project"), projectHash);
	ASSERT_EQ(CountCode(diagnostics, Rule001), 1u);
	EXPECT_EQ(diagnostics[0].Subject, "/Project");
	EXPECT_EQ(diagnostics[0].File, "Config/Assets.yml");
	EXPECT_EQ(diagnostics[0].Line, 3u);
	std::vector<SDiagnostic> rootDiagnostics;
	const auto root = engine.OpenScope("/Project", rootDiagnostics);
	EXPECT_EQ(CountCode(rootDiagnostics, Rule001), 1u);
	std::vector<SDiagnostic> subtreeDiagnostics;
	engine.OpenScope("/Project/Art", subtreeDiagnostics);
	EXPECT_TRUE(subtreeDiagnostics.empty()) << DescribeDiagnostics(subtreeDiagnostics).CStr();
	EXPECT_EQ(engine.ResolveFile(*root, "/Project/A.png").CookRule, ECookRule::Never);

	// An engine edit changes every mount's chain.
	fixture.Tree.WriteEngineRules("FormatVersion: 1\n");
	changed.clear();
	engine.LoadMountRules(nullptr, &changed);
	EXPECT_EQ(changed, (std::vector<FString> { "Engine", "Project" }));
	std::vector<SDiagnostic> reloadDiagnostics;
	const auto reloaded = engine.OpenScope("/Project", reloadDiagnostics);
	EXPECT_EQ(CountCode(reloadDiagnostics, Rule001), 1u) << "the project file is still broken";
	EXPECT_EQ(FindSetting(engine.ResolveFile(*reloaded, "/Project/A.png"), "ColorSpace"), "Srgb") << "the importer default";
	EXPECT_TRUE(FindProvenance(engine.ResolveFile(*reloaded, "/Project/A.png"), "ColorSpace")->File.IsEmpty());
	EXPECT_EQ(engine.ResolveFile(*root, "/Project/A.png").CookRule, ECookRule::Never) << "a scope keeps the rules it was made with";
}

TEST(RulesEngine, OpenScopeOfASubtreeReadsItsAncestorsButNothingInsideAnExcludedFolder)
{
	SEngineFixture fixture("Subtree");
	fixture.Tree.WriteProjectRules("FormatVersion: 1\nScan: { Exclude: [ \"Attic/**\" ] }\n");
	fixture.Tree.WriteProjectContent("Assets.yml", "FormatVersion: 1\nRules: [ { Match: \"**\", Chunk: 5 } ]\n");
	fixture.Tree.WriteProjectContent("Art/Assets.yml", "FormatVersion: 1\nRules: [ { Match: \"**\", Cook: Never } ]\n");
	fixture.Tree.WriteProjectContent("Attic/Assets.yml", "this: [ is not, valid");
	CRulesEngine& engine = fixture.Start();

	const auto scope = fixture.Open("/Project/Art/Rocks");
	const SEffectiveFileSettings rock = engine.ResolveFile(*scope, "/Project/Art/Rocks/Big/A.png");
	EXPECT_EQ(rock.Chunk, 5u);
	EXPECT_EQ(rock.CookRule, ECookRule::Never);

	const auto attic = fixture.Open("/Project/Attic/Box");
	EXPECT_FALSE(engine.ShouldDescend(*attic, "/Project/Attic/Box"));
	EXPECT_EQ(engine.Classify(*attic, "/Project/Attic/Box/A.png"), std::nullopt);
	const SRulesListing listing = engine.GetListing();
	const bool bAtticRead = std::any_of(listing.Files.begin(), listing.Files.end(),
		[] (const SRuleFileListing& File) { return File.Origin.FolderMountPath == "/Project/Attic"; });
	EXPECT_FALSE(bAtticRead) << "a rule file inside an excluded folder is never read";

	// The listing: mount files by mount id, then folders in order.
	ASSERT_EQ(listing.Files.size(), 3u);
	EXPECT_EQ(listing.Files[0].Origin.File, "Config/Assets.yml");
	EXPECT_EQ(listing.Files[0].Origin.Level, ERuleFileLevel::Mount);
	EXPECT_EQ(listing.Files[1].Origin.FolderMountPath, "/Project");
	EXPECT_EQ(listing.Files[1].Origin.File, "Content/Assets.yml");
	EXPECT_EQ(listing.Files[2].Origin.FolderMountPath, "/Project/Art");
	engine.ForgetFolders("/Project/Art");
	EXPECT_EQ(engine.GetListing().Files.size(), 2u);
}

TEST(RulesEngine, TypesAndValidationFollowTheChain)
{
	SEngineFixture fixture("TypesValidation");
	fixture.Tree.WriteEngineRules("FormatVersion: 1\nValidation: { RULE002: Info, REF007: Warning }\n");
	fixture.Tree.WriteProjectRules("FormatVersion: 1\nTypes:\n  Exclude: [ frt.Mesh ]\nValidation: { RULE002: Error }\n");
	CRulesEngine& engine = fixture.Start();
	const auto project = fixture.Open("/Project");
	EXPECT_TRUE(engine.IsTypeIncluded(*project, asset_types::Texture));
	EXPECT_FALSE(engine.IsTypeIncluded(*project, asset_types::Mesh));
	const auto onlyTextures = fixture.Enter(project, "/Project/Ui", FStringView("FormatVersion: 1\nTypes: { Include: [ Texture, Game.Icon ] }\n"
		"Validation: { REF007: Info }\n"));
	EXPECT_TRUE(engine.IsTypeIncluded(*onlyTextures, asset_types::Texture));
	EXPECT_FALSE(engine.IsTypeIncluded(*onlyTextures, asset_types::Model));
	EXPECT_FALSE(engine.IsTypeIncluded(*onlyTextures, asset_types::Mesh)) << "an outer Exclude still wins";

	SDiagnostic diagnostic;
	diagnostic.Code = Rule002;
	diagnostic.Severity = ESeverity::Warning;
	engine.ApplyValidation(*onlyTextures, diagnostic);
	EXPECT_EQ(diagnostic.Severity, ESeverity::Error) << "the project's override beats the engine's fallback";
	diagnostic.Code = Ref007;
	engine.ApplyValidation(*onlyTextures, diagnostic);
	EXPECT_EQ(diagnostic.Severity, ESeverity::Info) << "the folder's wins";
	diagnostic.Code = Path003;
	diagnostic.Severity = ESeverity::Warning;
	engine.ApplyValidation(*onlyTextures, diagnostic);
	EXPECT_EQ(diagnostic.Severity, ESeverity::Warning);

	SFileFacts facts;
	facts.Type = asset_types::Model;
	const SRuleExplanation explanation = engine.Explain(*onlyTextures, "/Project/Ui/Cursor.gltf", facts);
	EXPECT_FALSE(explanation.bTypeIncluded);
}

TEST(RulesEngine, ExplainNamesWhatExcludedAFile)
{
	SEngineFixture fixture("ExplainExcluded");
	fixture.Tree.WriteProjectRules("FormatVersion: 1\nScan:\n  Include: [ \"Art/**\", \"Ui/**\" ]\n  Exclude: [ \"**/Reference/**\" ]\n");
	CRulesEngine& engine = fixture.Start();
	const auto project = fixture.Open("/Project");
	EXPECT_EQ(engine.Explain(*project, "/Project/Art/Reference/A.png").ExcludedBy, "Scan.Exclude \"**/Reference/**\" (Config/Assets.yml:4)");
	EXPECT_EQ(engine.Explain(*project, "/Project/Docs/A.png").ExcludedBy, "not in Scan.Include (Config/Assets.yml:3)");
	EXPECT_EQ(engine.Explain(*project, "/Project/Art/A.png~").ExcludedBy, "a built-in exclude");
	const SRuleExplanation excluded = engine.Explain(*project, "/Project/Docs/A.png");
	EXPECT_FALSE(excluded.Verdict.has_value());
	EXPECT_EQ(excluded.Settings.Class, EFileClass::Excluded);
}


// ---------------- RULE002 ----------------

TEST(RulesUnclaimed, UnclaimedFilesAreReportedOncePerFolderAndExtensionUnlessQuiet)
{
	const SUnclaimedFile files[] = {
		{ "/Project/Art/Src/Wood.spp" }, { "/Project/Art/Src/stone.SPP" }, { "/Project/Art/Src/notes.txt", true },
		{ "/Project/Art/Src/Makefile" }, { "/Project/Art/Bark.spp" }, { "/Project/Art/Src/Brick.spp" },
	};
	const std::vector<SDiagnostic> diagnostics = ReportUnclaimedFiles(files);
	ASSERT_EQ(diagnostics.size(), 3u) << DescribeDiagnostics(diagnostics).CStr();
	for (const SDiagnostic& diagnostic : diagnostics)
	{
		EXPECT_EQ(diagnostic.Code, Rule002);
		EXPECT_EQ(diagnostic.Severity, ESeverity::Warning);
	}
	EXPECT_EQ(diagnostics[0].Subject, "/Project/Art");
	EXPECT_EQ(diagnostics[0].Message, "no importer for .spp under /Project/Art (Bark.spp); list them in Scan.Quiet if they are not assets");
	EXPECT_EQ(diagnostics[1].Subject, "/Project/Art/Src");
	EXPECT_TRUE(diagnostics[1].Message.StartsWith("no importer for files without an extension")) << diagnostics[1].Message.CStr();
	EXPECT_EQ(diagnostics[2].Message, "no importer for .spp under /Project/Art/Src (3 files, e.g. Brick.spp); list them in Scan.Quiet if they are not assets");
}


// ---------------- Rules.cache ----------------

TEST(RulesCache, AListingRoundTripsThroughRulesCacheAndAnUnchangedOneIsNotRewritten)
{
	SEngineFixture fixture("Cache");
	fixture.Tree.WriteEngineRules(EngineDefaults);
	fixture.Tree.WriteProjectRules(FullExample);
	CRulesEngine& engine = fixture.Start();
	const auto project = fixture.Open("/Project");
	fixture.Enter(project, "/Project/Тека \"q\"", FStringView("FormatVersion: 1\nRules: [ { Match: \"ж*.png\", Labels: [ \"Ліс\\tтабуляція\" ] } ]\n"));

	SRulesListing listing = engine.GetListing();
	ASSERT_EQ(listing.Files.size(), 3u);
	for (SRuleFileListing& file : listing.Files)
	{
		EXPECT_TRUE(file.Diagnostics.empty());
	}
	const std::filesystem::path path = GetRulesCachePath(fixture.Tree.Tree.ProjectDir());
	FString error;
	ASSERT_TRUE(WriteRulesCache(path, listing, &error)) << error.CStr();
	const std::optional<SRulesListing> read = ReadRulesCache(path, &error);
	ASSERT_TRUE(read.has_value()) << error.CStr();
	EXPECT_EQ(*read, listing);

	const std::optional<platform::SFileStat> before = platform::Stat(path);
	ASSERT_TRUE(WriteRulesCache(path, listing));
	const std::optional<platform::SFileStat> after = platform::Stat(path);
	ASSERT_TRUE(before.has_value() && after.has_value());
	EXPECT_EQ(before->Identity, after->Identity) << "an identical cache is left alone";

	const FString text = FormatRulesCache(listing);
	EXPECT_TRUE(text.StartsWith("{\"format\": \"frt.rules.cache\", \"version\": 1}\n"));
	EXPECT_TRUE(text.Contains("{\"kind\": \"file\", \"mount\": \"Project\", \"folder\": \"/Project\", \"file\": \"Config/Assets.yml\", \"level\": \"Mount\""));

	ASSERT_TRUE(platform::WriteFileBytes(path, FStringView("{\"format\": \"frt.rules.cache\", \"version\": 2}\n").AsBytes()));
	EXPECT_FALSE(ReadRulesCache(path).has_value());
	ASSERT_TRUE(platform::WriteFileBytes(path, FStringView("{\"format\": \"frt.rules.cache\", \"version\": 1}\n{\"kind\": \"Rule\"}\n").AsBytes()));
	EXPECT_FALSE(ReadRulesCache(path).has_value());
}
