// GetTextAssetVersion is the cheap probe that picks a text asset's reader (the material serializers'
// CanRead): a version line the reader itself accepts must pass the probe too, or no reader takes the
// file and it is not read at all.
#include <gtest/gtest.h>

#include "TestCommon.h"

#include <filesystem>
#include <system_error>

#include "Assets/TextAssetIO.h"
#include "Containers/String.h"
#include "Platform/Platform.h"

#include <Windows.h>

using namespace frt;


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
class TextAssetFileTest : public ::testing::Test
{
protected:
	void SetUp () override
	{
		Dir = std::filesystem::temp_directory_path() / FString::Format("FRT Assets Тест {}", GetCurrentProcessId()).ToPath();
		std::error_code ec;
		std::filesystem::remove_all(Dir, ec);
		ASSERT_TRUE(std::filesystem::create_directories(Dir, ec)) << FString::FromUtf8Lossy(ec.message()).CStr();
	}

	void TearDown () override
	{
		std::error_code ec;
		std::filesystem::remove_all(Dir, ec);
	}

	// Bytes as a file of its own, probed.
	int32 VersionOf (FStringView Bytes)
	{
		const std::filesystem::path path = Dir / FString::Format("Asset{}.frtmat.yml", FileCount++).ToPath();
		EXPECT_TRUE(platform::WriteFileText(path, Bytes));
		return assets::text::GetTextAssetVersion(path);
	}

	std::filesystem::path Dir;
	uint32 FileCount = 0u;
};
}


TEST_F(TextAssetFileTest, VersionIsTheFirstKeysValue)
{
	EXPECT_EQ(VersionOf("version: 3\nname: Brick\n"), 3);
	EXPECT_EQ(VersionOf("# a comment line first\n\nversion: 2\n"), 2);
	EXPECT_EQ(VersionOf("version = 1\r\nname = Brick\r\n"), 1);
	EXPECT_EQ(VersionOf("name: Brick\nversion: 3\n"), 0) << "a file that does not open with its version has none";
	EXPECT_EQ(VersionOf(""), 0);
	EXPECT_EQ(assets::text::GetTextAssetVersion(Dir / "Missing.frtmat.yml"), 0);
}

TEST_F(TextAssetFileTest, VersionReadsThroughAByteOrderMark)
{
	EXPECT_EQ(VersionOf("\xEF\xBB\xBF" "version: 3\n"), 3);
	EXPECT_EQ(VersionOf(MakeUtf16LeFile("version: 1\nname: Brick\n")), 1);
}

TEST_F(TextAssetFileTest, VersionIgnoresATrailingYamlComment)
{
	// yaml-cpp reads each of these as 3: a comment is a '#' after whitespace.
	EXPECT_EQ(VersionOf("version: 3  # bumped for v3\n"), 3);
	EXPECT_EQ(VersionOf("version: 3\t# after a tab\n"), 3);
	EXPECT_EQ(VersionOf("version: \"3\" # quoted\n"), 3);
}

TEST_F(TextAssetFileTest, MalformedVersionIsNone)
{
	// Strict, unlike the strtol it replaced: a number's prefix is not taken for the number.
	EXPECT_EQ(VersionOf("version: 3x\n"), 0);
	EXPECT_EQ(VersionOf("version: 3#x\n"), 0) << "no whitespace before '#': part of the value, as in YAML";
	EXPECT_EQ(VersionOf("version: # none\n"), 0);
	EXPECT_EQ(VersionOf("version:\n"), 0);
}

TEST(TextAssetValue, TrailingYamlCommentIsStripped)
{
	using assets::text::StripTrailingComment;
	EXPECT_EQ(StripTrailingComment("0.25  # glossy"), "0.25");
	EXPECT_EQ(StripTrailingComment("yes\t# after a tab"), "yes");
	EXPECT_EQ(StripTrailingComment("\"0.5\" # quoted"), "0.5");
	EXPECT_EQ(StripTrailingComment("back"), "back");
	EXPECT_EQ(StripTrailingComment("#FF8000  # orange"), "#FF8000") << "a '#' that starts the value is the value's";
	EXPECT_EQ(StripTrailingComment("3#x"), "3#x") << "no whitespace before '#': part of the value, as in YAML";
}
