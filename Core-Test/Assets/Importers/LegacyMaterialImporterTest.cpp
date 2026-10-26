// The Material.Legacy importer's Analyze (E3, design §4.4 "Legacy path references", §14.1): `.frtmat.yml` files are
// dispatched by their version as the runtime dispatches them - v2/v3 through ParseYaml, v1 through a port of the
// runtime's line reader - and every texture path becomes a Build | Path edge relative to the material's folder with
// one REF007 notice and the LegacyReferences flag; a file outside FRT's YAML subset keeps its record with a warning,
// and the file is hashed in its one read.
#include <gtest/gtest.h>

#include <vector>

#include "ImporterTestUtils.h"

#include "Assets/FileHash.h"

using namespace frt;
using namespace frt::assets;
using namespace frt::test::importers;


namespace
{
constexpr EDepFlags LegacyEdgeFlags = EDepFlags::Build | EDepFlags::Path;

void ExpectLegacyEdge (const SAnalyzeResult& Result, FStringView Target, FStringView FieldPath)
{
	const SEdgeDecl* edge = FindEdgeTo(Result, Target);
	ASSERT_NE(edge, nullptr) << Target.ToStdString();
	EXPECT_EQ(edge->Flags, LegacyEdgeFlags);
	EXPECT_EQ(edge->FieldPath, FieldPath);
	EXPECT_EQ(edge->FromSub, 0u);
	EXPECT_FALSE(edge->Target.IsValid());
}
}


TEST(LegacyMaterialImporterTest, YamlMaterialRecordsEveryTextureKey)
{
	const CImporterTree tree("Yaml");
	tree.Write("Матеріали/Brick.frtmat.yml",
		"version: 3\n"
		"name: Brick\n"
		"albedo:\n"
		"  color: { r: 1, g: 1, b: 1, a: 1 }\n"
		"  texture: Textures/brick_albedo.png\n"
		"emissive:\n"
		"  texture: \"../Shared/glow.png\"\n"
		"metallicRoughnessTexture: Textures\\orm.png\n"
		"normal:\n"
		"  texture: Textures/normal.png\n"
		"occlusion:\n"
		"  texture: ''\n"
		"roughness: 0.5\n");
	const SAnalyzeResult result = tree.Analyze("Матеріали/Brick.frtmat.yml");
	ASSERT_EQ(result.Status, EImportStatus::Analyzed) << DescribeDiagnostics(result).CStr();
	EXPECT_EQ(result.Type, asset_types::Material);
	EXPECT_TRUE(result.Tags.empty());
	EXPECT_TRUE((result.Flags & ERecordFlags::LegacyReferences) != ERecordFlags::None);

	ASSERT_EQ(result.Edges.size(), 4u);
	ExpectLegacyEdge(result, "/Project/Матеріали/Textures/brick_albedo.png", "albedo.texture");
	ExpectLegacyEdge(result, "/Project/Shared/glow.png", "emissive.texture");
	ExpectLegacyEdge(result, "/Project/Матеріали/Textures/orm.png", "metallicRoughnessTexture");
	ExpectLegacyEdge(result, "/Project/Матеріали/Textures/normal.png", "normal.texture");

	// One REF007 notice per reference, pointing at the value.
	ASSERT_EQ(CountDiagnostics(result, ESeverity::Info, Ref007), 4u) << DescribeDiagnostics(result).CStr();
	EXPECT_EQ(result.Diagnostics.size(), 4u) << DescribeDiagnostics(result).CStr();
	const SDiagnostic& albedo = result.Diagnostics[0];
	EXPECT_EQ(albedo.Subject, "/Project/Матеріали/Brick.frtmat.yml");
	EXPECT_EQ(albedo.File, "Content/Матеріали/Brick.frtmat.yml");
	EXPECT_EQ(albedo.Line, 5u);
	EXPECT_EQ(albedo.Column, 12u);
	EXPECT_EQ(albedo.FieldPath, "albedo.texture");
	EXPECT_TRUE(albedo.Message.View().Contains("Textures/brick_albedo.png"));

	ASSERT_TRUE(result.ContentHash.has_value());
	EXPECT_EQ(result.ContentHash, HashFileStable(tree.File("Матеріали/Brick.frtmat.yml")));
}

TEST(LegacyMaterialImporterTest, LineFormatIsReadAsTheRuntimeReadsIt)
{
	const CImporterTree tree("LineFormat");
	tree.Write("Old/stone.frtmat.yml",
		"version = 1  # legacy\n"
		"name = stone\n"
		"base_color_texture = first.png\n"
		"color = 1 1 1\n"
		"base_color_texture: \"Тексту́ри/stone #1.png\"\n");
	const SAnalyzeResult result = tree.Analyze("Old/stone.frtmat.yml");
	ASSERT_EQ(result.Status, EImportStatus::Analyzed) << DescribeDiagnostics(result).CStr();
	// The runtime overwrites a repeated key: the last value wins; quotes are removed, " #" kept in a path.
	ASSERT_EQ(result.Edges.size(), 1u);
	ExpectLegacyEdge(result, "/Project/Old/Тексту́ри/stone #1.png", "base_color_texture");
	ASSERT_EQ(result.Diagnostics.size(), 1u);
	EXPECT_EQ(result.Diagnostics[0].Code, Ref007);
	EXPECT_EQ(result.Diagnostics[0].Line, 5u);
	EXPECT_EQ(result.Diagnostics[0].Column, 22u);
}

TEST(LegacyMaterialImporterTest, ReferencesTheRuntimeReadsAsMachinePathsAreWarnings)
{
	const CImporterTree tree("MachinePaths");
	tree.Write("m.frtmat.yml",
		"version: 2\n"
		"albedo:\n"
		"  texture: C:/Textures/a.png\n"
		"normal:\n"
		"  texture: ../../outside.png\n"
		"emissive:\n"
		"  texture: { path: x }\n");
	const SAnalyzeResult result = tree.Analyze("m.frtmat.yml");
	ASSERT_EQ(result.Status, EImportStatus::Analyzed) << DescribeDiagnostics(result).CStr();
	EXPECT_TRUE(result.Edges.empty());
	EXPECT_TRUE((result.Flags & ERecordFlags::LegacyReferences) == ERecordFlags::None);
	EXPECT_EQ(CountDiagnostics(result, ESeverity::Warning, 0u), 3u) << DescribeDiagnostics(result).CStr();
	EXPECT_EQ(CountDiagnostics(result, ESeverity::Info), 0u);
}

TEST(LegacyMaterialImporterTest, YamlOutsideTheSubsetKeepsTheRecordWithAWarning)
{
	const CImporterTree tree("Subset");
	// yaml-cpp's YAML::Load (the runtime) takes a duplicate key; ParseYaml refuses it.
	tree.Write("dup.frtmat.yml", "version: 3\nname: a\nname: b\nalbedo:\n  texture: a.png\n");
	tree.Write("syntax.frtmat.yml", "version: 3\nalbedo: [unclosed\n");
	tree.Write("future.frtmat.yml", "version: 9\nalbedo:\n  texture: a.png\n");
	tree.Write("unversioned.frtmat.yml", "albedo:\n  texture: a.png\n");

	const SAnalyzeResult dup = tree.Analyze("dup.frtmat.yml");
	EXPECT_EQ(dup.Status, EImportStatus::AnalyzeFailed);
	EXPECT_EQ(dup.Type, asset_types::Material);
	ASSERT_EQ(dup.Diagnostics.size(), 1u) << DescribeDiagnostics(dup).CStr();
	EXPECT_EQ(dup.Diagnostics[0].Severity, ESeverity::Warning);
	EXPECT_EQ(dup.Diagnostics[0].Line, 3u);
	EXPECT_TRUE(dup.ContentHash.has_value());

	for (const FStringView file : { FStringView("syntax.frtmat.yml"), FStringView("future.frtmat.yml"), FStringView("unversioned.frtmat.yml") })
	{
		SCOPED_TRACE(file.ToStdString());
		const SAnalyzeResult result = tree.Analyze(file);
		EXPECT_EQ(result.Status, EImportStatus::AnalyzeFailed);
		ASSERT_EQ(result.Diagnostics.size(), 1u) << DescribeDiagnostics(result).CStr();
		EXPECT_EQ(result.Diagnostics[0].Severity, ESeverity::Error);
		EXPECT_EQ(result.Diagnostics[0].Code, 0u);
	}
}

TEST(LegacyMaterialImporterTest, Utf16MaterialIsDecodedAsTheRuntimeDecodesIt)
{
	const CImporterTree tree("Utf16");
	const FStringView text = "version: 3\nalbedo:\n  texture: тест.png\n";
	std::vector<uint8> bytes = { 0xFF, 0xFE };
	for (const char32_t codePoint : text.CodePoints())
	{
		bytes.push_back(static_cast<uint8>(codePoint & 0xFFu));
		bytes.push_back(static_cast<uint8>(codePoint >> 8u));
	}
	tree.WriteBinary("wide.frtmat.yml", bytes);
	const SAnalyzeResult result = tree.Analyze("wide.frtmat.yml");
	ASSERT_EQ(result.Status, EImportStatus::Analyzed) << DescribeDiagnostics(result).CStr();
	ExpectLegacyEdge(result, "/Project/тест.png", "albedo.texture");
}

TEST(LegacyMaterialImporterTest, DemoMaterialsReferenceTheirTextures)
{
	const CMountTable mounts = ResolveDemoMounts();
	const SAnalyzeResult duck = AnalyzeAt(mounts, "/Project/Models/Duck/Duck_mat0.frtmat.yml");
	ASSERT_EQ(duck.Status, EImportStatus::Analyzed) << DescribeDiagnostics(duck).CStr();
	ASSERT_EQ(duck.Edges.size(), 1u);
	ExpectLegacyEdge(duck, "/Project/Models/Duck/DuckCM.png", "albedo.texture");
	EXPECT_EQ(CountDiagnostics(duck, ESeverity::Info, Ref007), 1u);

	// No texture, no reference, no notice.
	const SAnalyzeResult light = AnalyzeAt(mounts, "/Project/Light/light_mat0.frtmat.yml");
	ASSERT_EQ(light.Status, EImportStatus::Analyzed) << DescribeDiagnostics(light).CStr();
	EXPECT_TRUE(light.Edges.empty());
	EXPECT_TRUE(light.Diagnostics.empty());
	EXPECT_TRUE((light.Flags & ERecordFlags::LegacyReferences) == ERecordFlags::None);
}
