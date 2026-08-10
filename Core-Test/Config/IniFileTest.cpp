#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <gtest/gtest.h>

#include "Config/IniFile.h"

// No FRT_TEST_MEMORY_POOL here, and that is the point rather than an omission: CIniFile
// allocates through the standard allocator, never through the engine arena, because it
// runs before MakeThisPrimaryInstance does.

using frt::config::CIniFile;


namespace
{
std::filesystem::path MakeTempPath (const char* Name)
{
	return std::filesystem::temp_directory_path() / "FrtIniFileTest" / Name;
}


std::string ReadFile (const std::filesystem::path& Path)
{
	std::ifstream in(Path, std::ios::binary);
	return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
}


TEST(IniFileTest, ParsesSectionsAndKeys)
{
	CIniFile ini;
	ini.Parse("[Display]\nWidth=1920\nHeight=1080\n\n[Audio]\nVolume=0.8\n");

	ASSERT_NE(ini.Find("Display", "Width"), nullptr);
	EXPECT_EQ(*ini.Find("Display", "Width"), "1920");
	EXPECT_EQ(*ini.Find("Display", "Height"), "1080");
	EXPECT_EQ(*ini.Find("Audio", "Volume"), "0.8");
	EXPECT_EQ(ini.Find("Display", "Volume"), nullptr);
}


TEST(IniFileTest, LookupIsCaseInsensitive)
{
	CIniFile ini;
	ini.Parse("[Display]\nVSync=true\n");

	EXPECT_NE(ini.Find("display", "vsync"), nullptr);
	EXPECT_NE(ini.Find("DISPLAY", "VSYNC"), nullptr);
}


TEST(IniFileTest, TrimsWhitespaceAroundNamesAndValues)
{
	CIniFile ini;
	ini.Parse("  [  Display  ]  \n\t Width \t = \t 1920 \t \n");

	ASSERT_NE(ini.Find("Display", "Width"), nullptr);
	EXPECT_EQ(*ini.Find("Display", "Width"), "1920");
}


TEST(IniFileTest, SkipsWholeLineComments)
{
	CIniFile ini;
	ini.Parse("; leading comment\n[Display]\n# another\nWidth=1920\n");

	EXPECT_EQ(ini.GetSections().size(), 1u);
	EXPECT_NE(ini.Find("Display", "Width"), nullptr);
}


TEST(IniFileTest, KeepsCommentCharactersInsideValues)
{
	// Inline comments are deliberately unsupported: paths and format strings contain
	// these characters, and stripping them would corrupt the value.
	CIniFile ini;
	ini.Parse("[Paths]\nCache=C:\\tmp;secondary # not a comment\n");

	ASSERT_NE(ini.Find("Paths", "Cache"), nullptr);
	EXPECT_EQ(*ini.Find("Paths", "Cache"), "C:\\tmp;secondary # not a comment");
}


TEST(IniFileTest, HandlesCrlfAndUtf8Bom)
{
	CIniFile ini;
	ini.Parse("\xEF\xBB\xBF[Display]\r\nWidth=1920\r\n");

	ASSERT_NE(ini.Find("Display", "Width"), nullptr);
	EXPECT_EQ(*ini.Find("Display", "Width"), "1920");
}


TEST(IniFileTest, KeysBeforeFirstSectionLandInUnnamedSection)
{
	CIniFile ini;
	ini.Parse("Orphan=1\n[Display]\nWidth=1920\n");

	ASSERT_NE(ini.Find("", "Orphan"), nullptr);
	EXPECT_EQ(*ini.Find("", "Orphan"), "1");
}


TEST(IniFileTest, LastDuplicateKeyWins)
{
	CIniFile ini;
	ini.Parse("[Display]\nWidth=1280\nWidth=1920\n");

	ASSERT_NE(ini.Find("Display", "Width"), nullptr);
	EXPECT_EQ(*ini.Find("Display", "Width"), "1920");
}


TEST(IniFileTest, SkipsMalformedLinesWithoutLosingTheRest)
{
	CIniFile ini;
	ini.Parse("[Display]\nthis line has no equals sign\n=novalue\nWidth=1920\n[Unclosed\nHeight=1080\n");

	EXPECT_NE(ini.Find("Display", "Width"), nullptr);
	// The unclosed header is skipped, so the key after it stays in the section before it.
	EXPECT_NE(ini.Find("Display", "Height"), nullptr);
}


TEST(IniFileTest, EmptyValueIsPresentAndEmpty)
{
	CIniFile ini;
	ini.Parse("[Display]\nName=\n");

	ASSERT_NE(ini.Find("Display", "Name"), nullptr);
	EXPECT_EQ(*ini.Find("Display", "Name"), "");
}


TEST(IniFileTest, SetOverwritesAndCreates)
{
	CIniFile ini;
	ini.Set("Display", "Width", "1920");
	ini.Set("Display", "Width", "2560");
	ini.Set("Audio", "Volume", "0.5");

	EXPECT_EQ(*ini.Find("Display", "Width"), "2560");
	EXPECT_EQ(*ini.Find("Audio", "Volume"), "0.5");
	EXPECT_EQ(ini.GetSections().size(), 2u);
}


TEST(IniFileTest, RemoveDropsOnlyTheNamedKey)
{
	CIniFile ini;
	ini.Parse("[Display]\nWidth=1920\nHeight=1080\n");

	EXPECT_TRUE(ini.Remove("Display", "Width"));
	EXPECT_EQ(ini.Find("Display", "Width"), nullptr);
	EXPECT_NE(ini.Find("Display", "Height"), nullptr);

	EXPECT_FALSE(ini.Remove("Display", "Width"));
	EXPECT_FALSE(ini.Remove("Missing", "Height"));
}


TEST(IniFileTest, RoundTripsThroughAFile)
{
	const std::filesystem::path path = MakeTempPath("roundtrip.ini");

	CIniFile written;
	written.Set("Display", "Width", "1920");
	written.Set("Display", "FullscreenMode", "Borderless");
	written.Set("Audio", "Volume", "0.5");
	ASSERT_TRUE(written.SaveToFile(path));

	CIniFile read;
	ASSERT_TRUE(read.LoadFromFile(path));
	EXPECT_EQ(*read.Find("Display", "Width"), "1920");
	EXPECT_EQ(*read.Find("Display", "FullscreenMode"), "Borderless");
	EXPECT_EQ(*read.Find("Audio", "Volume"), "0.5");

	std::filesystem::remove(path);
}


TEST(IniFileTest, RoundTripPreservesSignificantSpacesAndQuotes)
{
	const std::filesystem::path path = MakeTempPath("quoting.ini");

	CIniFile written;
	written.Set("Text", "Padded", "  spaced  ");
	written.Set("Text", "Quoted", "\"quoted\"");
	written.Set("Text", "Plain", "plain");
	ASSERT_TRUE(written.SaveToFile(path));

	CIniFile read;
	ASSERT_TRUE(read.LoadFromFile(path));
	EXPECT_EQ(*read.Find("Text", "Padded"), "  spaced  ");
	EXPECT_EQ(*read.Find("Text", "Quoted"), "\"quoted\"");
	EXPECT_EQ(*read.Find("Text", "Plain"), "plain");

	std::filesystem::remove(path);
}


TEST(IniFileTest, SaveCreatesMissingDirectories)
{
	const std::filesystem::path path = MakeTempPath("nested/deeper/created.ini");
	std::filesystem::remove_all(path.parent_path().parent_path());

	CIniFile ini;
	ini.Set("Display", "Width", "1920");
	ASSERT_TRUE(ini.SaveToFile(path));
	EXPECT_TRUE(std::filesystem::exists(path));

	std::filesystem::remove_all(path.parent_path().parent_path());
}


TEST(IniFileTest, SaveWritesLfOnly)
{
	const std::filesystem::path path = MakeTempPath("endings.ini");

	CIniFile ini;
	ini.Set("Display", "Width", "1920");
	ASSERT_TRUE(ini.SaveToFile(path));

	EXPECT_EQ(ReadFile(path).find('\r'), std::string::npos);

	std::filesystem::remove(path);
}


TEST(IniFileTest, LoadingAMissingFileIsNotAnErrorAndClears)
{
	CIniFile ini;
	ini.Set("Display", "Width", "1920");

	EXPECT_FALSE(ini.LoadFromFile(MakeTempPath("does-not-exist.ini")));
	EXPECT_TRUE(ini.IsEmpty());
	EXPECT_EQ(ini.Find("Display", "Width"), nullptr);
}


TEST(IniFileTest, ParseReplacesPreviousContents)
{
	CIniFile ini;
	ini.Parse("[Display]\nWidth=1920\n");
	ini.Parse("[Audio]\nVolume=0.5\n");

	EXPECT_EQ(ini.Find("Display", "Width"), nullptr);
	EXPECT_NE(ini.Find("Audio", "Volume"), nullptr);
}


TEST(IniFileTest, EmptyInputParsesToNothing)
{
	CIniFile ini;
	ini.Parse("");

	EXPECT_TRUE(ini.IsEmpty());
	EXPECT_TRUE(ini.GetSections().empty());
}


TEST(IniFileTest, SectionHeaderWithNoKeysIsStillPresent)
{
	CIniFile ini;
	ini.Parse("[Empty]\n");

	ASSERT_EQ(ini.GetSections().size(), 1u);
	EXPECT_EQ(ini.GetSections()[0].Name, "Empty");
	EXPECT_TRUE(ini.IsEmpty());
}


TEST(IniFileTest, RepeatedSectionHeaderMergesRatherThanDuplicating)
{
	CIniFile ini;
	ini.Parse("[Display]\nWidth=1920\n[Audio]\nVolume=0.5\n[Display]\nHeight=1080\n");

	EXPECT_EQ(ini.GetSections().size(), 2u);
	EXPECT_NE(ini.Find("Display", "Width"), nullptr);
	EXPECT_NE(ini.Find("Display", "Height"), nullptr);
}


TEST(IniFileTest, FinalLineWithoutNewlineIsParsed)
{
	CIniFile ini;
	ini.Parse("[Display]\nWidth=1920");

	ASSERT_NE(ini.Find("Display", "Width"), nullptr);
	EXPECT_EQ(*ini.Find("Display", "Width"), "1920");
}


TEST(IniFileTest, ValueMayContainEqualsSigns)
{
	CIniFile ini;
	ini.Parse("[Args]\nFlags=-D FOO=1 -D BAR=2\n");

	ASSERT_NE(ini.Find("Args", "Flags"), nullptr);
	EXPECT_EQ(*ini.Find("Args", "Flags"), "-D FOO=1 -D BAR=2");
}
