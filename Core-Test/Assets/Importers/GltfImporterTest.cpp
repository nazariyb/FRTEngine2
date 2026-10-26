// The Model.Gltf importer's Analyze (E3, design §7.11, §3.2, §4.3, §4.4): glTF-native facts through cgltf on the JSON
// alone - counts summed over accessors, mesh:/material:/image: sub-entries labelled by name, index and document-order
// duplicate suffix (a SubId collision is an error, never a silent merge), external image URIs as Build | Path |
// Declared edges resolved as Translate's Assimp I/O will open them, buffers (and images no importer claims) as
// companions only, never edges, since a companion has no record an edge could name - for every
// Demo model, a .gltf hashed in its one read, and a .glb whose BIN chunk is never read.
#include <gtest/gtest.h>

#include <vector>

#include "ImporterTestUtils.h"

#include "Assets/FileHash.h"
#include "Assets/Pipeline/GltfParse.h"

using namespace frt;
using namespace frt::assets;
using namespace frt::test::importers;


namespace
{
constexpr EDepFlags UriEdgeFlags = EDepFlags::Build | EDepFlags::Path | EDepFlags::Declared;

void AppendU32 (std::vector<uint8>& Out, uint32 Value)
{
	for (uint32 i = 0u; i < 4u; ++i)
	{
		Out.push_back(static_cast<uint8>(Value >> (8u * i)));
	}
}

// A GLB: header, JSON chunk (padded with spaces to 4 bytes), BIN chunk of BinBytes bytes.
std::vector<uint8> MakeGlb (FStringView Json, uint32 BinBytes)
{
	std::vector<uint8> json(Json.AsBytes().begin(), Json.AsBytes().end());
	while (json.size() % 4u != 0u)
	{
		json.push_back(' ');
	}
	const uint32 total = GlbHeaderBytes + GlbChunkHeaderBytes + static_cast<uint32>(json.size()) + GlbChunkHeaderBytes + BinBytes;
	std::vector<uint8> glb;
	AppendU32(glb, GlbMagic);
	AppendU32(glb, 2u);
	AppendU32(glb, total);
	AppendU32(glb, static_cast<uint32>(json.size()));
	AppendU32(glb, GlbJsonChunkType);
	glb.insert(glb.end(), json.begin(), json.end());
	AppendU32(glb, BinBytes);
	AppendU32(glb, GlbBinChunkType);
	glb.insert(glb.end(), BinBytes, uint8(0xFE));
	return glb;
}

std::vector<FString> GetLabels (const SAnalyzeResult& Result)
{
	std::vector<FString> labels;
	for (const SSubEntryDecl& sub : Result.SubEntries)
	{
		labels.push_back(sub.Label);
	}
	return labels;
}

struct SDemoModel
{
	FStringLiteral Path;
	int64 Meshes;
	int64 Materials;
	int64 Images;
	int64 Vertices;
	int64 Triangles;
	FStringLiteral Buffer;
};

// Counted from the files' accessors (POSITION counts; indices / 3 for triangle lists).
constexpr SDemoModel DemoModels[] = {
	{ "/Project/Models/Cube/Cube.gltf", 1, 1, 2, 36, 12, "/Project/Models/Cube/Cube.bin" },
	{ "/Project/Models/Duck/Duck.gltf", 1, 1, 1, 2399, 4212, "/Project/Models/Duck/Duck0.bin" },
	{ "/Project/Models/Fox/Fox.gltf", 1, 1, 1, 1728, 576, "/Project/Models/Fox/Fox.bin" },
	{ "/Project/Models/Lantern/Lantern.gltf", 3, 1, 4, 4145, 5394, "/Project/Models/Lantern/Lantern.bin" },
	{ "/Project/Models/Skull/scene.gltf", 1, 1, 2, 17358, 31016, "/Project/Models/Skull/scene.bin" },
	{ "/Project/Models/Sponza/Sponza.gltf", 1, 25, 69, 192496, 262267, "/Project/Models/Sponza/Sponza.bin" },
};
}


TEST(GltfImporterTest, DuckGivesItsNativeFacts)
{
	const CMountTable mounts = ResolveDemoMounts();
	const SAnalyzeResult result = AnalyzeAt(mounts, "/Project/Models/Duck/Duck.gltf");
	ASSERT_EQ(result.Status, EImportStatus::Analyzed) << DescribeDiagnostics(result).CStr();
	EXPECT_TRUE(result.Diagnostics.empty()) << DescribeDiagnostics(result).CStr();
	EXPECT_EQ(result.Type, asset_types::Model);

	// Tags in the type's key order.
	ASSERT_EQ(result.Tags.size(), 5u);
	EXPECT_EQ(result.Tags[0].Key, "MeshCount");
	EXPECT_EQ(result.Tags[4].Key, "TriangleCount");
	EXPECT_EQ(GetIntTag(result.Tags, "MeshCount"), 1);
	EXPECT_EQ(GetIntTag(result.Tags, "MaterialCount"), 1);
	EXPECT_EQ(GetIntTag(result.Tags, "ImageCount"), 1);
	EXPECT_EQ(GetIntTag(result.Tags, "VertexCount"), 2399);
	EXPECT_EQ(GetIntTag(result.Tags, "TriangleCount"), 4212);

	EXPECT_EQ(GetLabels(result), (std::vector<FString> { "mesh:LOD3spShape", "material:blinn3-fx", "image:#0" }));
	const SSubEntryDecl* mesh = FindSubEntry(result, "mesh:LOD3spShape");
	ASSERT_NE(mesh, nullptr);
	EXPECT_EQ(mesh->Type, asset_types::Mesh);
	EXPECT_EQ(mesh->SubId, MakeSubId("mesh:LOD3spShape"));
	EXPECT_EQ(GetIntTag(mesh->Tags, "VertexCount"), 2399);
	EXPECT_EQ(GetIntTag(mesh->Tags, "TriangleCount"), 4212);
	EXPECT_EQ(FindSubEntry(result, "material:blinn3-fx")->Type, asset_types::Material);
	const SSubEntryDecl* image = FindSubEntry(result, "image:#0");
	ASSERT_NE(image, nullptr);
	EXPECT_EQ(image->Type, asset_types::Texture);

	ASSERT_EQ(result.Edges.size(), 1u);
	const SEdgeDecl* texture = FindEdgeTo(result, "/Project/Models/Duck/DuckCM.png");
	ASSERT_NE(texture, nullptr);
	EXPECT_EQ(texture->FromSub, image->SubId);
	EXPECT_EQ(texture->Flags, UriEdgeFlags);
	EXPECT_EQ(texture->FieldPath, "images.0.uri");
	EXPECT_FALSE(texture->Target.IsValid());
	// The buffer is a companion only: it has no record, so no edge names it.
	EXPECT_EQ(FindEdgeTo(result, "/Project/Models/Duck/Duck0.bin"), nullptr);
	EXPECT_EQ(result.Companions, (std::vector<FString> { "/Project/Models/Duck/Duck0.bin" }));

	// Hashed in the same read: the registry's content hash of the file.
	const std::optional<SAnalyzeInput> input = SAnalyzeInput::FromMount(*mounts.Find("Project"), "/Project/Models/Duck/Duck.gltf");
	ASSERT_TRUE(input.has_value());
	EXPECT_EQ(input->SourceFile, "Content/Models/Duck/Duck.gltf");
	ASSERT_TRUE(result.ContentHash.has_value());
	EXPECT_EQ(result.ContentHash, HashFileStable(input->Path));
}

TEST(GltfImporterTest, EveryDemoModelAnalyzesCleanly)
{
	const CMountTable mounts = ResolveDemoMounts();
	for (const SDemoModel& model : DemoModels)
	{
		SCOPED_TRACE(model.Path.GetData());
		const SAnalyzeResult result = AnalyzeAt(mounts, model.Path);
		ASSERT_EQ(result.Status, EImportStatus::Analyzed) << DescribeDiagnostics(result).CStr();
		EXPECT_TRUE(result.Diagnostics.empty()) << DescribeDiagnostics(result).CStr();
		EXPECT_EQ(GetIntTag(result.Tags, "MeshCount"), model.Meshes);
		EXPECT_EQ(GetIntTag(result.Tags, "MaterialCount"), model.Materials);
		EXPECT_EQ(GetIntTag(result.Tags, "ImageCount"), model.Images);
		EXPECT_EQ(GetIntTag(result.Tags, "VertexCount"), model.Vertices);
		EXPECT_EQ(GetIntTag(result.Tags, "TriangleCount"), model.Triangles);
		EXPECT_EQ(result.SubEntries.size(), static_cast<size_t>(model.Meshes + model.Materials + model.Images));
		EXPECT_EQ(result.Companions, (std::vector<FString> { FString(model.Buffer) }));
		// One edge per external image, none to the buffer; never a link to the _mat<N> files beside the model.
		EXPECT_EQ(result.Edges.size(), static_cast<size_t>(model.Images));
		for (const SEdgeDecl& edge : result.Edges)
		{
			EXPECT_FALSE(edge.TargetPath.View().EndsWith(".frtmat.yml")) << edge.TargetPath.CStr();
			EXPECT_TRUE(IsWellFormedMountPath(edge.TargetPath)) << edge.TargetPath.CStr();
		}
	}

	// Unnamed objects fall back to their index; image URIs below the model's folder resolve into it.
	const SAnalyzeResult sponza = AnalyzeAt(mounts, "/Project/Models/Sponza/Sponza.gltf");
	EXPECT_NE(FindSubEntry(sponza, "mesh:#0"), nullptr);
	EXPECT_NE(FindSubEntry(sponza, "material:#24"), nullptr);
	EXPECT_NE(FindSubEntry(sponza, "image:#68"), nullptr);
	const SAnalyzeResult skull = AnalyzeAt(mounts, "/Project/Models/Skull/scene.gltf");
	EXPECT_NE(FindEdgeTo(skull, "/Project/Models/Skull/textures/defaultMat_baseColor.jpeg"), nullptr);
}

TEST(GltfImporterTest, LabelsFollowTheVocabulary)
{
	const CImporterTree tree("Labels");
	tree.Write("Модель/labels.gltf", R"({"asset":{"version":"2.0"},
		"meshes":[{"name":"Body","primitives":[]},{"name":"Body","primitives":[]},{"primitives":[]},{"name":"Body","primitives":[]},
			{"name":"Тіло","primitives":[]},{"name":"Arm\/Left \u00e9","primitives":[]},{"name":"","primitives":[]}],
		"materials":[{"name":"Body"},{}],
		"images":[{"name":"Skin","bufferView":0,"mimeType":"image/png"}],
		"buffers":[{"byteLength":4}],"bufferViews":[{"buffer":0,"byteLength":4}]})");
	const SAnalyzeResult result = tree.Analyze("Модель/labels.gltf");
	ASSERT_EQ(result.Status, EImportStatus::Analyzed) << DescribeDiagnostics(result).CStr();
	EXPECT_TRUE(result.Diagnostics.empty()) << DescribeDiagnostics(result).CStr();
	EXPECT_EQ(GetLabels(result), (std::vector<FString> { "mesh:Body", "mesh:Body#2", "mesh:#2", "mesh:Body#3", "mesh:Тіло", "mesh:Arm/Left é",
		"mesh:#6", "material:Body", "material:#1", "image:Skin" }));
	for (const SSubEntryDecl& sub : result.SubEntries)
	{
		EXPECT_EQ(sub.SubId, MakeSubId(sub.Label)) << sub.Label.CStr();
	}
	// An embedded image (bufferView) and a GLB-less buffer without a URI make no edge.
	EXPECT_TRUE(result.Edges.empty());
	EXPECT_TRUE(result.Companions.empty());
}

TEST(GltfImporterTest, SubIdCollisionIsAnErrorAndSkipsTheLaterObject)
{
	const CImporterTree tree("Collision");
	tree.Write("collide.gltf", R"({"asset":{"version":"2.0"},
		"meshes":[{"name":"Body#2","primitives":[]},{"name":"Body","primitives":[]},{"name":"Body","primitives":[]}]})");
	const SAnalyzeResult result = tree.Analyze("collide.gltf");
	EXPECT_EQ(result.Status, EImportStatus::Analyzed);
	EXPECT_EQ(GetLabels(result), (std::vector<FString> { "mesh:Body#2", "mesh:Body" }));
	ASSERT_EQ(result.Diagnostics.size(), 1u) << DescribeDiagnostics(result).CStr();
	EXPECT_EQ(result.Diagnostics[0].Severity, ESeverity::Error);
	EXPECT_EQ(result.Diagnostics[0].Subject, "/Project/collide.gltf#mesh:Body#2");
	EXPECT_EQ(result.Diagnostics[0].File, "Content/collide.gltf");
	EXPECT_EQ(GetIntTag(result.Tags, "MeshCount"), 3);   // the record still counts every mesh
}

TEST(GltfImporterTest, CountsAreSummedOverAccessors)
{
	const CImporterTree tree("Counts");
	tree.Write("counts.gltf", R"({"asset":{"version":"2.0"},
		"accessors":[
			{"componentType":5126,"count":4,"type":"VEC3"},
			{"componentType":5123,"count":6,"type":"SCALAR"},
			{"componentType":5126,"count":9,"type":"VEC3"},
			{"componentType":5126,"count":5,"type":"VEC3"},
			{"componentType":5126,"count":7,"type":"VEC3"}],
		"meshes":[
			{"name":"A","primitives":[{"attributes":{"POSITION":0},"indices":1},{"attributes":{"POSITION":2}}]},
			{"name":"B","primitives":[{"attributes":{"POSITION":3},"mode":5},{"attributes":{"POSITION":0},"indices":1,"mode":6},
				{"attributes":{"POSITION":4},"mode":0},{"attributes":{"POSITION":4},"mode":1},{"attributes":{"NORMAL":4}}]}]})");
	const SAnalyzeResult result = tree.Analyze("counts.gltf");
	ASSERT_EQ(result.Status, EImportStatus::Analyzed) << DescribeDiagnostics(result).CStr();
	const SSubEntryDecl* a = FindSubEntry(result, "mesh:A");
	const SSubEntryDecl* b = FindSubEntry(result, "mesh:B");
	ASSERT_NE(a, nullptr);
	ASSERT_NE(b, nullptr);
	// A: 4 indexed vertices, 6 indices -> 2 triangles; 9 vertices as a list -> 3.
	EXPECT_EQ(GetIntTag(a->Tags, "VertexCount"), 13);
	EXPECT_EQ(GetIntTag(a->Tags, "TriangleCount"), 5);
	// B: a strip of 5 -> 3; an indexed fan of 6 -> 4; points and lines -> 0; no POSITION -> nothing. Accessor 4,
	// drawn as points and as lines, is one set of 7 vertices.
	EXPECT_EQ(GetIntTag(b->Tags, "VertexCount"), 5 + 4 + 7);
	EXPECT_EQ(GetIntTag(b->Tags, "TriangleCount"), 7);
	// The file counts each POSITION accessor once: accessor 0 is shared by A and B.
	EXPECT_EQ(GetIntTag(result.Tags, "VertexCount"), 4 + 9 + 5 + 7);
	EXPECT_EQ(GetIntTag(result.Tags, "TriangleCount"), 12);
}

// A character outside the BMP written as an escaped surrogate pair (Python's json.dumps does so by default) is that
// character, in names (so the label and SubId do not depend on how the exporter escaped) and in URIs (so the edge
// names the file Assimp's JSON reader opens); an unpaired surrogate reads as U+FFFD with a warning.
TEST(GltfImporterTest, EscapedSurrogatePairsAreOneCharacter)
{
	const CImporterTree tree("Surrogates");
	tree.Write("E/\xF0\x9F\x98\x80.png", "x");
	tree.Write("E/model.gltf", R"({"asset":{"version":"2.0"},
		"meshes":[{"name":"\ud83d\ude00","primitives":[]},{"name":"\uD83D\uDE00 B","primitives":[]},{"name":"lone \ud83d!","primitives":[]},
			{"name":"\ude00","primitives":[]}],
		"images":[{"uri":"\ud83d\ude00.png"}]})");
	const SAnalyzeResult result = tree.Analyze("E/model.gltf");
	ASSERT_EQ(result.Status, EImportStatus::Analyzed) << DescribeDiagnostics(result).CStr();
	EXPECT_EQ(GetLabels(result), (std::vector<FString> { "mesh:\xF0\x9F\x98\x80", "mesh:\xF0\x9F\x98\x80 B", "mesh:lone \xEF\xBF\xBD!",
		"mesh:\xEF\xBF\xBD", "image:#0" }));
	const SSubEntryDecl* emoji = FindSubEntry(result, "mesh:\xF0\x9F\x98\x80");
	ASSERT_NE(emoji, nullptr);
	EXPECT_EQ(emoji->SubId, MakeSubId(FStringView("mesh:\xF0\x9F\x98\x80")));
	const SEdgeDecl* image = FindEdgeTo(result, "/Project/E/\xF0\x9F\x98\x80.png");
	ASSERT_NE(image, nullptr);
	EXPECT_EQ(image->FieldPath, "images.0.uri");
	EXPECT_TRUE(result.Companions.empty());   // a .png has its own Texture record
	// The two unpaired surrogates: one warning each, naming the field.
	EXPECT_EQ(CountDiagnostics(result, ESeverity::Warning, 0u), 2u) << DescribeDiagnostics(result).CStr();
	const SDiagnostic* lone = FindDiagnostic(result, "unpaired");
	ASSERT_NE(lone, nullptr);
	EXPECT_EQ(lone->FieldPath, "meshes.2.name");
}

// An image no available importer claims (KHR_texture_basisu's .ktx2, EXT_texture_webp's .webp, MSFT_texture_dds's
// .dds) has no record: the model names it as a companion, like a buffer, and no edge names it.
TEST(GltfImporterTest, UnclaimedImagesAreCompanions)
{
	const CImporterTree tree("ImageCompanions");
	tree.Write("M/model.gltf", R"({"asset":{"version":"2.0"},
		"images":[{"uri":"albedo.ktx2"},{"uri":"albedo.ktx2"},{"uri":"normal.webp"},{"uri":"rough.dds"},{"uri":"base.png"}],
		"buffers":[{"uri":"model.bin","byteLength":4}]})");
	const SAnalyzeResult result = tree.Analyze("M/model.gltf");
	ASSERT_EQ(result.Status, EImportStatus::Analyzed) << DescribeDiagnostics(result).CStr();
	EXPECT_EQ(result.Companions, (std::vector<FString> { "/Project/M/albedo.ktx2", "/Project/M/normal.webp", "/Project/M/rough.dds",
		"/Project/M/model.bin" }));
	ASSERT_EQ(result.Edges.size(), 1u);
	EXPECT_EQ(FindEdgeTo(result, "/Project/M/albedo.ktx2"), nullptr);
	const SEdgeDecl* png = FindEdgeTo(result, "/Project/M/base.png");
	ASSERT_NE(png, nullptr);
	EXPECT_EQ(png->FromSub, MakeSubId("image:#4"));
	EXPECT_EQ(png->Flags, UriEdgeFlags);
}

TEST(GltfImporterTest, UrisResolveAsTranslateWillOpenThem)
{
	const CImporterTree tree("Uris");
	tree.Write("M/Brick Wall.png", "x");
	tree.Write("M/Raw%41.png", "x");
	tree.Write("M/Текстура.png", "x");
	tree.Write("M/data.bin", "x");
	tree.Write("M/model.gltf", R"({"asset":{"version":"2.0"},
		"images":[
			{"uri":"Brick%20Wall.png"},
			{"uri":"Raw%41.png"},
			{"uri":"Missing%20File.png"},
			{"uri":"%D0%A2%D0%B5%D0%BA%D1%81%D1%82%D1%83%D1%80%D0%B0.png"},
			{"uri":"sub\/..\/tex\/a.png"},
			{"uri":"data:image/png;base64,AAAA"},
			{"uri":"../../../escape.png"},
			{"uri":"C:/Textures/x.png"},
			{"uri":"/rooted.png"},
			{"uri":"https://example.com/y.png"}],
		"buffers":[{"uri":"data.bin","byteLength":1},{"uri":"data.bin","byteLength":1},{"uri":"data:application/octet-stream;base64,AAAA","byteLength":3}]})");
	const SAnalyzeResult result = tree.Analyze("M/model.gltf");
	ASSERT_EQ(result.Status, EImportStatus::Analyzed) << DescribeDiagnostics(result).CStr();

	// The name as written when it names a file, else decoded (also when nothing exists yet).
	EXPECT_NE(FindEdgeTo(result, "/Project/M/Brick Wall.png"), nullptr);
	EXPECT_NE(FindEdgeTo(result, "/Project/M/Raw%41.png"), nullptr);
	EXPECT_NE(FindEdgeTo(result, "/Project/M/Missing File.png"), nullptr);
	EXPECT_NE(FindEdgeTo(result, "/Project/M/Текстура.png"), nullptr);
	EXPECT_NE(FindEdgeTo(result, "/Project/M/tex/a.png"), nullptr);
	const SEdgeDecl* brick = FindEdgeTo(result, "/Project/M/Brick Wall.png");
	ASSERT_NE(brick, nullptr);
	EXPECT_EQ(brick->FromSub, MakeSubId("image:#0"));
	EXPECT_EQ(brick->FieldPath, "images.0.uri");

	// data: URIs are embedded bytes: no edge and no complaint. The rest is not a folder-relative path.
	EXPECT_EQ(CountDiagnostics(result, ESeverity::Warning, 0u), 4u) << DescribeDiagnostics(result).CStr();
	const SDiagnostic* escape = FindDiagnostic(result, "../../../escape.png");
	ASSERT_NE(escape, nullptr);
	EXPECT_EQ(escape->FieldPath, "images.6.uri");
	EXPECT_NE(FindDiagnostic(result, "climbs above the mount root"), nullptr);
	EXPECT_NE(FindDiagnostic(result, "C:/Textures/x.png"), nullptr);
	EXPECT_NE(FindDiagnostic(result, "/rooted.png"), nullptr);
	EXPECT_NE(FindDiagnostic(result, "https://example.com/y.png"), nullptr);

	// Two buffers naming one file: one companion and no edge.
	EXPECT_EQ(result.Companions, (std::vector<FString> { "/Project/M/data.bin" }));
	EXPECT_EQ(FindEdgeTo(result, "/Project/M/data.bin"), nullptr);
	EXPECT_EQ(result.Edges.size(), 5u);
}

TEST(GltfImporterTest, GlbReadsOnlyTheJsonChunk)
{
	const CImporterTree tree("Glb");
	constexpr FStringLiteral Json = R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":64}],
		"images":[{"name":"Albedo","uri":"albedo.png"}],"meshes":[{"name":"Rock","primitives":[]}]})";
	tree.WriteBinary("Rocks/rock.glb", MakeGlb(Json, 64u));
	const SAnalyzeResult glb = tree.Analyze("Rocks/rock.glb");
	ASSERT_EQ(glb.Status, EImportStatus::Analyzed) << DescribeDiagnostics(glb).CStr();
	EXPECT_EQ(GetLabels(glb), (std::vector<FString> { "mesh:Rock", "image:Albedo" }));
	ASSERT_EQ(glb.Edges.size(), 1u);
	EXPECT_EQ(glb.Edges[0].TargetPath, "/Project/Rocks/albedo.png");
	EXPECT_EQ(glb.Edges[0].FromSub, MakeSubId("image:Albedo"));
	EXPECT_TRUE(glb.Companions.empty());          // the BIN chunk is the GLB's own buffer
	EXPECT_FALSE(glb.ContentHash.has_value());    // never read whole, so never hashed

	// Binary glTF under the .gltf extension: read whole (and hashed), the JSON chunk taken from the bytes.
	tree.WriteBinary("Rocks/binary.gltf", MakeGlb(Json, 64u));
	const SAnalyzeResult disguised = tree.Analyze("Rocks/binary.gltf");
	ASSERT_EQ(disguised.Status, EImportStatus::Analyzed) << DescribeDiagnostics(disguised).CStr();
	EXPECT_EQ(GetLabels(disguised), GetLabels(glb));
	EXPECT_TRUE(disguised.ContentHash.has_value());
}

TEST(GltfImporterTest, MalformedDocumentsFailAndKeepTheModelType)
{
	const CImporterTree tree("Malformed");
	tree.Write("broken.gltf", "{ \"asset\": ");
	std::vector<uint8> truncated = MakeGlb(R"({"asset":{"version":"2.0"}})", 16u);
	truncated.resize(GlbHeaderBytes + 4u);
	tree.WriteBinary("truncated.glb", truncated);

	for (const FStringView file : { FStringView("broken.gltf"), FStringView("truncated.glb") })
	{
		SCOPED_TRACE(file.ToStdString());
		const SAnalyzeResult result = tree.Analyze(file);
		EXPECT_EQ(result.Status, EImportStatus::AnalyzeFailed);
		EXPECT_EQ(result.Type, asset_types::Model);
		EXPECT_TRUE(result.SubEntries.empty());
		ASSERT_EQ(CountDiagnostics(result, ESeverity::Error, 0u), 1u) << DescribeDiagnostics(result).CStr();
		EXPECT_EQ(result.Diagnostics[0].Subject, CImporterTree::MountPath(file));
	}
}
