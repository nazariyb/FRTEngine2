// Material files on disk. The version probe (CanRead) and the reader must agree on what a file holds:
// when no reader takes a file, CMaterialLibrary falls back to the default material, and when the
// legacy reader takes one it cannot read, the library's upgrade writes the default material over it.
#include <gtest/gtest.h>

#include "TestCommon.h"

#include <filesystem>
#include <optional>
#include <system_error>

#include "Containers/String.h"
#include "Graphics/Render/Material.h"
#include "Graphics/Render/Serializers/MaterialSerializers.h"
#include "Platform/Platform.h"

#include <Windows.h>

using namespace frt;
using namespace frt::graphics;


namespace
{
// The file a Windows tool writes as "Unicode" (UTF-16LE after a BOM), for ASCII text.
FString MakeUtf16LeFile (FStringView Ascii)
{
	FString bytes("\xFF\xFE");
	for (const char ch : Ascii)
	{
		bytes.AppendChar(ch);
		bytes.AppendChar('\0');
	}
	return bytes;
}

// In a directory with a non-ASCII name, of this process's own (PlatformTest.cpp has the reasons).
class MaterialFileTest : public ::testing::Test
{
protected:
	void SetUp () override
	{
		Dir = std::filesystem::temp_directory_path() / FString::Format("FRT Material Тест {}", GetCurrentProcessId()).ToPath();
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
}


TEST_F(MaterialFileTest, YamlVersionWithATrailingCommentIsRead)
{
	FRT_TEST_MEMORY_POOL();
	const std::filesystem::path path = Dir / "Цегла.frtmat.yml";

	SMaterial brick;
	brick.Name = "Цегла";
	brick.Roughness = 0.25f;
	const CMaterialYamlSerializer yaml;
	ASSERT_TRUE(yaml.Write(path, brick));

	// Hand-edited: a YAML comment after the version, which yaml-cpp skips.
	std::optional<FString> text = platform::ReadFileText(path);
	ASSERT_TRUE(text.has_value());
	ASSERT_EQ(text->ReplaceInline("version: 3\n", "version: 3  # bumped for v3\n"), 1u) << *text;
	ASSERT_TRUE(platform::WriteFileText(path, *text));

	EXPECT_TRUE(yaml.CanRead(path));
	EXPECT_FALSE(CMaterialLegacyTextSerializer().CanRead(path));
	SMaterial read;
	ASSERT_TRUE(yaml.Read(path, read));
	EXPECT_EQ(read.Name, "Цегла");
	EXPECT_EQ(read.Roughness, 0.25f);
}

TEST_F(MaterialFileTest, LegacyUtf16FileIsReadAsItsVersionWasProbed)
{
	const std::filesystem::path path = Dir / "Brick.frtmat";
	ASSERT_TRUE(platform::WriteFileText(path,
		MakeUtf16LeFile("version: 1\r\nname: Brick\r\nroughness: 0.25\r\nalpha_blend: yes\r\n")));

	const CMaterialLegacyTextSerializer legacy;
	ASSERT_TRUE(legacy.CanRead(path));
	SMaterial read;
	ASSERT_TRUE(legacy.Read(path, read));
	EXPECT_EQ(read.Name, "Brick") << "read as UTF-16 bytes, no key matches";
	EXPECT_EQ(read.Roughness, 0.25f);
	EXPECT_TRUE(read.bAlphaBlend);
}

TEST_F(MaterialFileTest, YamlRoundTripsNonAsciiTextAndNamedValues)
{
	FRT_TEST_MEMORY_POOL();
	const std::filesystem::path path = Dir / u8"Стіна.frtmat.yml";

	SMaterial wall;
	wall.Name = "Цегляна стіна";
	wall.BaseColorTexturePath = "纹理/砖墙.png";
	wall.AlphaMode = EAlphaMode::Mask;
	wall.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
	wall.CullMode = D3D12_CULL_MODE_BACK;
	const CMaterialYamlSerializer yaml;
	ASSERT_TRUE(yaml.Write(path, wall));

	// The named values are text, not the `true` a C string reached through Write(bool); the texture
	// path is written as stored, with '/'.
	const std::optional<FString> text = platform::ReadFileText(path);
	ASSERT_TRUE(text.has_value());
	EXPECT_TRUE(text->Contains("name: Цегляна стіна")) << *text;
	EXPECT_TRUE(text->Contains("texture: 纹理/砖墙.png")) << *text;
	EXPECT_TRUE(text->Contains("mode: mask")) << *text;
	EXPECT_TRUE(text->Contains("func: greater_equal")) << *text;
	EXPECT_TRUE(text->Contains("cull: back")) << *text;

	SMaterial read;
	read.SourcePath = path;
	ASSERT_TRUE(yaml.Read(path, read));
	EXPECT_EQ(read.Name, "Цегляна стіна");
	EXPECT_EQ(read.BaseColorTexturePath, "纹理/砖墙.png");
	EXPECT_EQ(read.AlphaMode, EAlphaMode::Mask);
	EXPECT_EQ(read.DepthFunc, D3D12_COMPARISON_FUNC_GREATER_EQUAL);
	EXPECT_EQ(read.CullMode, D3D12_CULL_MODE_BACK);
	EXPECT_EQ(read.ResolveTexturePath(read.BaseColorTexturePath), (Dir / u8"纹理" / u8"砖墙.png").lexically_normal());
}

TEST_F(MaterialFileTest, YamlNamedValuesIgnoreAsciiCase)
{
	FRT_TEST_MEMORY_POOL();
	const std::filesystem::path path = Dir / "Glass.frtmat.yml";
	const CMaterialYamlSerializer yaml;
	ASSERT_TRUE(yaml.Write(path, SMaterial()));

	std::optional<FString> text = platform::ReadFileText(path);
	ASSERT_TRUE(text.has_value());
	ASSERT_EQ(text->ReplaceInline("mode: opaque", "mode: Blend"), 1u) << *text;
	ASSERT_EQ(text->ReplaceInline("func: less", "func: ALWAYS"), 1u) << *text;
	ASSERT_EQ(text->ReplaceInline("cull: none", "cull: Front"), 1u) << *text;
	ASSERT_TRUE(platform::WriteFileText(path, *text));

	SMaterial read;
	ASSERT_TRUE(yaml.Read(path, read));
	EXPECT_EQ(read.AlphaMode, EAlphaMode::Blend);
	EXPECT_EQ(read.DepthFunc, D3D12_COMPARISON_FUNC_ALWAYS);
	EXPECT_EQ(read.CullMode, D3D12_CULL_MODE_FRONT);
}

TEST_F(MaterialFileTest, YamlRefusesTextItWouldAlterAndWritesNoFile)
{
	FRT_TEST_MEMORY_POOL();
	const CMaterialYamlSerializer yaml;

	// A byte that is no UTF-8, and U+FFFE: yaml-cpp would write both as U+FFFD.
	constexpr FStringLiteral names[] = { "Brick\xFF", "Brick\xEF\xBF\xBE" };
	for (const FStringLiteral name : names)
	{
		const std::filesystem::path path = Dir / "Refused.frtmat.yml";
		SMaterial material;
		material.Name = name;
		EXPECT_FALSE(yaml.Write(path, material)) << FString::Escape(name).CStr();
		EXPECT_FALSE(std::filesystem::exists(path)) << FString::Escape(name).CStr();
	}
}

TEST_F(MaterialFileTest, LegacyValueThatDoesNotParseKeepsTheField)
{
	const std::filesystem::path path = Dir / "Brick.frtmat";
	ASSERT_TRUE(platform::WriteFileText(path,
		"version: 1\nname: Цегла\nmetallic: shiny\nroughness: 0.25\nemissive_intensity: 2x\n"
		"color: RGBA(0.5, 0.25, 1, 1)\ncull: BACK\ndepth_func: gequal\ndepth_write: maybe\n"));

	const CMaterialLegacyTextSerializer legacy;
	SMaterial read;
	read.Metallic = 0.75f;
	read.EmissiveIntensity = 3.0f;
	ASSERT_TRUE(legacy.Read(path, read)) << "a value that does not parse failed the read";
	EXPECT_EQ(read.Name, "Цегла");
	EXPECT_EQ(read.Metallic, 0.75f);
	EXPECT_EQ(read.Roughness, 0.25f);
	EXPECT_EQ(read.EmissiveIntensity, 3.0f);
	EXPECT_EQ(read.DiffuseAlbedo.G, 0.25f);
	EXPECT_EQ(read.CullMode, D3D12_CULL_MODE_BACK);
	EXPECT_EQ(read.DepthFunc, D3D12_COMPARISON_FUNC_GREATER_EQUAL);
	EXPECT_TRUE(read.bDepthWrite);
}

TEST_F(MaterialFileTest, LegacyValueWithATrailingCommentIsRead)
{
	// Hand-edited. CanRead's version probe skips the comment after the version, so the reader must skip
	// the ones after the values too: a value it dropped would be lost for good, since CMaterialLibrary
	// upgrades a v1 file on load by writing what was read over it.
	const std::filesystem::path path = Dir / "Brick.frtmat";
	ASSERT_TRUE(platform::WriteFileText(path,
		"version: 1  # old\nname: Brick # 2\nroughness: 0.25  # glossy\nmetallic: \"0.5\"\t# quoted\n"
		"emissive_intensity: 2 # lamp\ncolor: #FF8000  # orange\ncull: back  # x\ndepth_func: gequal # x\n"
		"depth_write: no # x\nalpha_blend: yes # x\nbase_color_texture: textures/C# 2.png\n"));

	const CMaterialLegacyTextSerializer legacy;
	ASSERT_TRUE(legacy.CanRead(path));
	SMaterial read;
	ASSERT_TRUE(legacy.Read(path, read));
	EXPECT_EQ(read.Roughness, 0.25f);
	EXPECT_EQ(read.Metallic, 0.5f);
	EXPECT_EQ(read.EmissiveIntensity, 2.0f);
	EXPECT_EQ(read.DiffuseAlbedo.ToHex(), "#FF8000FF");
	EXPECT_EQ(read.CullMode, D3D12_CULL_MODE_BACK);
	EXPECT_EQ(read.DepthFunc, D3D12_COMPARISON_FUNC_GREATER_EQUAL);
	EXPECT_FALSE(read.bDepthWrite);
	EXPECT_TRUE(read.bAlphaBlend);
	// Names and paths are text, taken as written: a '#' in them is theirs.
	EXPECT_EQ(read.Name, "Brick # 2");
	EXPECT_EQ(read.BaseColorTexturePath, "textures/C# 2.png");
}
