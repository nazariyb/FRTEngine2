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
