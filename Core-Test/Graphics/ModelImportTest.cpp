// Model import through Assimp: the model and its side files are opened by their UTF-16 names
// (CAssimpWideIOSystem), and the material files the import creates store texture paths with '/'.
#include <gtest/gtest.h>

#include "TestCommon.h"

#include <array>
#include <filesystem>
#include <optional>
#include <system_error>

#include "Containers/String.h"
#include "Graphics/Model.h"
#include "Graphics/Render/MaterialLibrary.h"
#include "Platform/Platform.h"

#include <Windows.h>

using namespace frt;
using namespace frt::graphics;


namespace
{
// One triangle drawn twice: material 0 names a texture of its own (a non-ASCII URI, as authored),
// material 1 has none and takes the override. The vertices are in a side file, which Assimp opens
// through the same IO handler as the model.
constexpr FStringLiteral TriangleGltf = R"json({
	"asset": { "version": "2.0" },
	"scene": 0,
	"scenes": [ { "nodes": [ 0 ] } ],
	"nodes": [ { "mesh": 0 } ],
	"meshes": [ { "primitives": [
		{ "attributes": { "POSITION": 0, "NORMAL": 1 }, "material": 0 },
		{ "attributes": { "POSITION": 0, "NORMAL": 1 }, "material": 1 } ] } ],
	"materials": [
		{ "name": "Цегла", "pbrMetallicRoughness": { "baseColorTexture": { "index": 0 } } },
		{ "pbrMetallicRoughness": { "baseColorFactor": [ 1, 1, 1, 1 ] } } ],
	"textures": [ { "source": 0 } ],
	"images": [ { "uri": "textures/Текстура.png" } ],
	"buffers": [ { "uri": "triangle.bin", "byteLength": 72 } ],
	"bufferViews": [ { "buffer": 0, "byteOffset": 0, "byteLength": 72 } ],
	"accessors": [
		{ "bufferView": 0, "byteOffset": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [ 0, 0, 0 ], "max": [ 1, 1, 0 ] },
		{ "bufferView": 0, "byteOffset": 36, "componentType": 5126, "count": 3, "type": "VEC3" } ]
}
)json";

// Percent-encoded image URIs, as glTF exporters write them (Blender encodes every character outside
// the URI set). Material 0's base colour is "Brick Wall.png" and material 1's "Текстура.png";
// material 1's normal texture is a file named exactly as its URI is spelled (as is "RawA.png", what
// it would decode to), and material 0's has no file at all.
constexpr FStringLiteral EncodedUriGltf = R"json({
	"asset": { "version": "2.0" },
	"scene": 0,
	"scenes": [ { "nodes": [ 0 ] } ],
	"nodes": [ { "mesh": 0 } ],
	"meshes": [ { "primitives": [
		{ "attributes": { "POSITION": 0, "NORMAL": 1 }, "material": 0 },
		{ "attributes": { "POSITION": 0, "NORMAL": 1 }, "material": 1 } ] } ],
	"materials": [
		{ "pbrMetallicRoughness": { "baseColorTexture": { "index": 0 } }, "normalTexture": { "index": 2 } },
		{ "pbrMetallicRoughness": { "baseColorTexture": { "index": 1 } }, "normalTexture": { "index": 3 } } ],
	"textures": [ { "source": 0 }, { "source": 1 }, { "source": 2 }, { "source": 3 } ],
	"images": [
		{ "uri": "textures/Brick%20Wall.png" },
		{ "uri": "textures/%D0%A2%D0%B5%D0%BA%D1%81%D1%82%D1%83%D1%80%D0%B0.png" },
		{ "uri": "textures/Missing%20File.png" },
		{ "uri": "textures/Raw%41.png" } ],
	"buffers": [ { "uri": "triangle.bin", "byteLength": 72 } ],
	"bufferViews": [ { "buffer": 0, "byteOffset": 0, "byteLength": 72 } ],
	"accessors": [
		{ "bufferView": 0, "byteOffset": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [ 0, 0, 0 ], "max": [ 1, 1, 0 ] },
		{ "bufferView": 0, "byteOffset": 36, "componentType": 5126, "count": 3, "type": "VEC3" } ]
}
)json";

// Positions, then normals.
constexpr std::array<float, 18> TriangleVertices = {
	0.0f, 0.0f, 0.0f,   1.0f, 0.0f, 0.0f,   0.0f, 1.0f, 0.0f,
	0.0f, 0.0f, 1.0f,   0.0f, 0.0f, 1.0f,   0.0f, 0.0f, 1.0f,
};

// In a directory of this process's own (PlatformTest.cpp has the reasons) whose name has Cyrillic,
// CJK, a character outside the BMP (a surrogate pair in UTF-16) and an unpaired surrogate (U+D800
// as WTF-8): Windows allows it, and Assimp's own IO handler turns it into U+FFFD and finds nothing.
class ModelImportTest : public ::testing::Test
{
protected:
	void SetUp () override
	{
		FString name("FRT Модель 模型 😀 \xED\xA0\x80 ");
		name.AppendNumber(GetCurrentProcessId());
		Dir = std::filesystem::temp_directory_path() / name.ToPath();
		std::error_code ec;
		std::filesystem::remove_all(Dir, ec);
		ASSERT_TRUE(std::filesystem::create_directories(Dir, ec)) << FString::FromUtf8Lossy(ec.message()).CStr();
	}

	void TearDown () override
	{
		std::error_code ec;
		std::filesystem::remove_all(Dir, ec);
	}

	// The vertices, as the side file the glTF names.
	static bool WriteTriangleBuffer (const std::filesystem::path& Path)
	{
		return platform::WriteFileText(Path,
			FStringView(reinterpret_cast<const char*>(TriangleVertices.data()), sizeof(TriangleVertices)));
	}

	std::filesystem::path Dir;
};

// A file held open the way a program that is still writing it holds it (an exporter, a sync client):
// for writing, with readers let in.
class CWriterHandle
{
public:
	explicit CWriterHandle (const std::filesystem::path& Path)
		: Handle(CreateFileW(Path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr))
	{}

	~CWriterHandle ()
	{
		if (IsOpen())
		{
			CloseHandle(Handle);
		}
	}

	CWriterHandle (const CWriterHandle&) = delete;
	CWriterHandle& operator= (const CWriterHandle&) = delete;

	bool IsOpen () const { return Handle != INVALID_HANDLE_VALUE; }

private:
	HANDLE Handle;
};
}


TEST_F(ModelImportTest, NonAsciiModelLoadsAndItsMaterialsStoreGenericTexturePaths)
{
	FRT_TEST_MEMORY_POOL();
	const std::filesystem::path modelPath = Dir / u8"Трикутник.gltf";
	ASSERT_TRUE(platform::WriteFileText(modelPath, TriangleGltf));
	ASSERT_TRUE(WriteTriangleBuffer(Dir / "triangle.bin"));

	CMaterialLibrary library;
	const SRenderModel model = SRenderModel::LoadFromFile(modelPath, Dir / "textures" / "override.png", library);

	EXPECT_EQ(model.SourcePath, modelPath);
	EXPECT_FALSE(model.Sections.IsEmpty());
	// Assimp appends a default material of its own (for meshes without one).
	ASSERT_GE(model.Materials.Count(), 2u);
	EXPECT_EQ(model.Materials[0]->Name, "Цегла");
	EXPECT_EQ(model.Materials[0]->BaseColorTexturePath, "textures/Текстура.png");
	// The override is made relative to the model's directory, which std::filesystem spells with '\'.
	EXPECT_EQ(model.Materials[1]->BaseColorTexturePath, "textures/override.png");

	// The material files are named after the model, and hold what the import found.
	EXPECT_TRUE(std::filesystem::exists(Dir / u8"Трикутник_mat0.frtmat.yml"));
	const std::optional<FString> text = platform::ReadFileText(Dir / u8"Трикутник_mat1.frtmat.yml");
	ASSERT_TRUE(text.has_value());
	EXPECT_TRUE(text->Contains("texture: textures/override.png")) << *text;
}

TEST_F(ModelImportTest, PercentEncodedTextureUrisNameTheFilesTheyEncode)
{
	FRT_TEST_MEMORY_POOL();
	const std::filesystem::path modelPath = Dir / "Wall.gltf";
	ASSERT_TRUE(platform::WriteFileText(modelPath, EncodedUriGltf));
	ASSERT_TRUE(WriteTriangleBuffer(Dir / "triangle.bin"));
	std::error_code ec;
	ASSERT_TRUE(std::filesystem::create_directories(Dir / "textures", ec));
	for (const std::filesystem::path name : { u8"Brick Wall.png", u8"Текстура.png", u8"Raw%41.png", u8"RawA.png" })
	{
		ASSERT_TRUE(platform::WriteFileText(Dir / "textures" / name, {}));
	}

	CMaterialLibrary library;
	const SRenderModel model = SRenderModel::LoadFromFile(modelPath, {}, library);

	ASSERT_GE(model.Materials.Count(), 2u);
	EXPECT_EQ(model.Materials[0]->BaseColorTexturePath, "textures/Brick Wall.png");
	EXPECT_EQ(model.Materials[1]->BaseColorTexturePath, "textures/Текстура.png");
	// Decoded only to find a file: a name that is one as written stays, and so does one that names
	// no file either way.
	EXPECT_EQ(model.Materials[1]->NormalTexturePath, "textures/Raw%41.png");
	EXPECT_EQ(model.Materials[0]->NormalTexturePath, "textures/Missing%20File.png");

	const std::optional<FString> text = platform::ReadFileText(Dir / "Wall_mat1.frtmat.yml");
	ASSERT_TRUE(text.has_value());
	EXPECT_TRUE(text->Contains("texture: textures/Текстура.png")) << *text;
}

TEST_F(ModelImportTest, PercentEncodedBufferUriOpens)
{
	FRT_TEST_MEMORY_POOL();
	// "вершини 1.bin": Assimp hands the URI to the IO handler as written.
	FString gltf(TriangleGltf);
	ASSERT_EQ(gltf.ReplaceInline("triangle.bin", "%D0%B2%D0%B5%D1%80%D1%88%D0%B8%D0%BD%D0%B8%201.bin"), 1u);
	const std::filesystem::path modelPath = Dir / "Triangle.gltf";
	ASSERT_TRUE(platform::WriteFileText(modelPath, gltf));
	ASSERT_TRUE(WriteTriangleBuffer(Dir / u8"вершини 1.bin"));

	CMaterialLibrary library;
	const SRenderModel model = SRenderModel::LoadFromFile(modelPath, {}, library);

	EXPECT_FALSE(model.Sections.IsEmpty());
	EXPECT_FALSE(model.Vertices.IsEmpty());
}

TEST_F(ModelImportTest, ModelOpensWhileAnotherProgramWritesIt)
{
	FRT_TEST_MEMORY_POOL();
	const std::filesystem::path modelPath = Dir / "Triangle.gltf";
	ASSERT_TRUE(platform::WriteFileText(modelPath, TriangleGltf));
	ASSERT_TRUE(WriteTriangleBuffer(Dir / "triangle.bin"));
	const CWriterHandle modelWriter(modelPath);
	const CWriterHandle bufferWriter(Dir / "triangle.bin");
	ASSERT_TRUE(modelWriter.IsOpen() && bufferWriter.IsOpen());

	CMaterialLibrary library;
	const SRenderModel model = SRenderModel::LoadFromFile(modelPath, {}, library);

	EXPECT_FALSE(model.Sections.IsEmpty());
	EXPECT_FALSE(model.Vertices.IsEmpty());
}
