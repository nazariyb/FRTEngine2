// glTF parsing through cgltf on bytes FRT read itself: every glTF model the Demo template ships parses (and the
// counts are cgltf's own, glTF-native ones), a GLB's JSON chunk is located from its headers alone - so a huge GLB is
// never read in full - and read without its BIN chunk, and damaged headers or JSON fail with a reason.
#include <gtest/gtest.h>

#include <filesystem>
#include <system_error>
#include <vector>

#include "TestCommon.h"

#include "Assets/Pipeline/GltfParse.h"
#include "Containers/String.h"
#include "EnginePaths.h"
#include "Platform/FileSystem.h"
#include "Platform/Platform.h"

#include <Windows.h>

using namespace frt;
using namespace frt::assets;


namespace
{
constexpr uint8 BinFiller = 0xFEu;

void AppendU32 (std::vector<uint8>& Out, uint32 Value)
{
	for (uint32 i = 0u; i < 4u; ++i)
	{
		Out.push_back(static_cast<uint8>(Value >> (8u * i)));
	}
}

// A GLB: header, JSON chunk (padded with spaces to 4 bytes), BIN chunk.
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
	// 0xFE never occurs in UTF-8, so a BIN byte in what is read as JSON is recognizable.
	glb.insert(glb.end(), BinBytes, BinFiller);
	return glb;
}

constexpr FStringLiteral MinimalGltf = R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":12}],"meshes":[{"name":"Тест","primitives":[{"attributes":{}}]}]})";
}


TEST(GltfParseTest, EveryDemoModelParses)
{
	const std::filesystem::path models = paths::GetEngineDir() / "Templates" / "Demo" / "Content" / "Models";
	std::vector<std::filesystem::path> files;
	std::error_code ec;
	for (const auto& folder : std::filesystem::directory_iterator(models, ec))
	{
		if (!folder.is_directory())
		{
			continue;
		}
		for (const auto& file : std::filesystem::directory_iterator(folder.path(), ec))
		{
			if (file.path().extension() == ".gltf")
			{
				files.push_back(file.path());
			}
		}
	}
	ASSERT_EQ(files.size(), 6u) << "Templates/Demo/Content/Models/*/*.gltf";

	for (const std::filesystem::path& file : files)
	{
		const FString name = FString::FromPath(file.filename());
		const std::optional<std::vector<uint8>> bytes = platform::ReadFileBytes(file);
		ASSERT_TRUE(bytes.has_value()) << name.CStr();
		FString error;
		const TGltfDataPtr document = ParseGltfJson(*bytes, &error);
		ASSERT_TRUE(document) << name.CStr() << ": " << error.CStr();
		const SGltfObjectCounts counts = CountGltfObjects(*document);
		EXPECT_GE(counts.Meshes, 1u) << name.CStr();
		EXPECT_GE(counts.Primitives, counts.Meshes) << name.CStr();
		EXPECT_GE(counts.Accessors, 1u) << name.CStr();
		EXPECT_GE(counts.Buffers, 1u) << name.CStr();
		EXPECT_GE(counts.Nodes, 1u) << name.CStr();
		EXPECT_GE(counts.Scenes, 1u) << name.CStr();
		if (name == "Fox.gltf")
		{
			EXPECT_EQ(counts.Meshes, 1u);
			EXPECT_GE(counts.Animations, 1u);
			EXPECT_EQ(counts.Skins, 1u);
			EXPECT_EQ(counts.Images, 1u);
		}
		if (name == "Sponza.gltf")
		{
			EXPECT_GT(counts.Materials, 10u);
			EXPECT_GT(counts.Images, 10u);
		}
	}
}

TEST(GltfParseTest, GlbHeaderLocatesTheJsonChunkWithoutReadingFurther)
{
	const std::vector<uint8> glb = MakeGlb(MinimalGltf, 12u);
	FString error;
	std::optional<SGlbJsonChunk> chunk = ParseGlbHeader(glb, glb.size(), &error);
	ASSERT_TRUE(chunk.has_value()) << error.CStr();
	EXPECT_EQ(chunk->Offset, 20u);
	EXPECT_EQ(chunk->Length % 4u, 0u);
	EXPECT_GE(chunk->Length, MinimalGltf.Len());

	// A 500 MB GLB: only its first 20 bytes are needed to find the JSON.
	constexpr uint32 hugeTotal = 500u * 1024u * 1024u;
	std::vector<uint8> headers;
	AppendU32(headers, GlbMagic);
	AppendU32(headers, 2u);
	AppendU32(headers, hugeTotal);
	AppendU32(headers, 4096u);
	AppendU32(headers, GlbJsonChunkType);
	chunk = ParseGlbHeader(headers, hugeTotal, &error);
	ASSERT_TRUE(chunk.has_value()) << error.CStr();
	EXPECT_EQ(chunk->Offset, 20u);
	EXPECT_EQ(chunk->Length, 4096u);
}

TEST(GltfParseTest, DamagedGlbHeadersAreRefused)
{
	const std::vector<uint8> good = MakeGlb(MinimalGltf, 12u);
	FString error;

	EXPECT_FALSE(ParseGlbHeader(std::span<const uint8>(good.data(), 19u), good.size(), &error).has_value());
	EXPECT_TRUE(error.Contains("shorter")) << error.CStr();

	std::vector<uint8> bad = good;
	bad[0] = 'x';
	EXPECT_FALSE(ParseGlbHeader(bad, bad.size(), &error).has_value());
	EXPECT_TRUE(error.Contains("magic")) << error.CStr();

	bad = good;
	bad[4] = 1u;   // version 1
	EXPECT_FALSE(ParseGlbHeader(bad, bad.size(), &error).has_value());
	EXPECT_TRUE(error.Contains("version")) << error.CStr();

	// The total length claims more than the file has (truncated download).
	EXPECT_FALSE(ParseGlbHeader(good, good.size() - 1u, &error).has_value());
	EXPECT_TRUE(error.Contains("length")) << error.CStr();

	bad = good;
	bad[16] = 'B';   // first chunk type no longer JSON
	EXPECT_FALSE(ParseGlbHeader(bad, bad.size(), &error).has_value());
	EXPECT_TRUE(error.Contains("JSON")) << error.CStr();

	bad = good;
	bad[12] = 0xFFu;   // JSON chunk length past the end
	bad[13] = 0xFFu;
	EXPECT_FALSE(ParseGlbHeader(bad, bad.size(), &error).has_value());
	EXPECT_TRUE(error.Contains("past")) << error.CStr();
}

class GltfFileTest : public ::testing::Test
{
protected:
	void SetUp () override
	{
		Dir = std::filesystem::temp_directory_path() / FString::Format("FRT glTF Тест {}", GetCurrentProcessId()).ToPath();
		std::error_code ec;
		std::filesystem::remove_all(Dir, ec);
		ASSERT_TRUE(std::filesystem::create_directories(Dir, ec)) << FString::FromUtf8Lossy(ec.message()).CStr();
	}

	void TearDown () override
	{
		std::error_code ec;
		std::filesystem::remove_all(Dir, ec);
	}

	std::filesystem::path Dir;
};

TEST_F(GltfFileTest, ReadGlbJsonChunkReadsOnlyTheJson)
{
	const std::vector<uint8> glb = MakeGlb(MinimalGltf, 4096u);
	const std::filesystem::path path = Dir / FStringView("Модель.glb").ToPath();
	ASSERT_TRUE(platform::WriteFileBytes(path, glb));

	FString error;
	const std::optional<std::vector<uint8>> json = ReadGlbJsonChunk(path, &error);
	ASSERT_TRUE(json.has_value()) << error.CStr();
	EXPECT_EQ(json->size() % 4u, 0u);
	EXPECT_LT(json->size(), 200u);
	EXPECT_EQ(FStringView(reinterpret_cast<const char*>(json->data()), MinimalGltf.Len()), FStringView(MinimalGltf));
	for (const uint8 byte : *json)
	{
		EXPECT_NE(byte, BinFiller) << "a BIN chunk byte in the JSON";
	}

	// cgltf takes the chunk as plain glTF JSON.
	const TGltfDataPtr document = ParseGltfJson(*json, &error);
	ASSERT_TRUE(document) << error.CStr();
	const SGltfObjectCounts counts = CountGltfObjects(*document);
	EXPECT_EQ(counts.Meshes, 1u);
	EXPECT_EQ(counts.Primitives, 1u);
	EXPECT_EQ(counts.Buffers, 1u);

	// A file too short, or missing, fails with a reason.
	const std::filesystem::path shortFile = Dir / "short.glb";
	ASSERT_TRUE(platform::WriteFileBytes(shortFile, std::span<const uint8>(glb.data(), 10u)));
	EXPECT_FALSE(ReadGlbJsonChunk(shortFile, &error).has_value());
	EXPECT_FALSE(error.IsEmpty());
	error.Clear();
	EXPECT_FALSE(ReadGlbJsonChunk(Dir / "missing.glb", &error).has_value());
	EXPECT_FALSE(error.IsEmpty());
}

TEST(GltfParseTest, InvalidJsonOrGltfFails)
{
	FString error;
	const FStringView garbage = "{ not json";
	EXPECT_FALSE(ParseGltfJson(garbage.AsBytes(), &error));
	EXPECT_TRUE(error.StartsWith("cgltf:")) << error.CStr();

	const FStringView badIndex = R"({"asset":{"version":"2.0"},"meshes":[{"primitives":[{"attributes":{"POSITION":7}}]}]})";
	EXPECT_FALSE(ParseGltfJson(badIndex.AsBytes(), &error));
	EXPECT_FALSE(error.IsEmpty());

	EXPECT_FALSE(ParseGltfJson({}, &error));

	const FStringView legacy = R"({"asset":{"version":"1.0"}})";
	EXPECT_FALSE(ParseGltfJson(legacy.AsBytes(), &error));
	EXPECT_FALSE(error.IsEmpty());
}
