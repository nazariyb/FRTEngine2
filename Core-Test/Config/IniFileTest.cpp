#include <filesystem>
#include <optional>

#include <gtest/gtest.h>

#include "Config/IniFile.h"
#include "Containers/String.h"
#include "Containers/WideString.h"
#include "Platform/Platform.h"

// No FRT_TEST_MEMORY_POOL here, and that is the point rather than an omission: CIniFile
// allocates from the CRT heap (std::vector, FString), never through the engine arena, because
// it runs before MakeThisPrimaryInstance does.

using frt::FString;
using frt::FStringView;
using frt::config::CIniFile;
namespace platform = frt::platform;


namespace
{
// Under a directory with a non-ASCII name: the files go through UTF-16 paths all the way.
std::filesystem::path MakeTempPath (FStringView Name)
{
	return std::filesystem::temp_directory_path() / FStringView("FrtIniFileTest Тест 😀").ToPath() / Name.ToPath();
}


// The file's bytes as stored; empty when it cannot be read.
FString ReadFile (const std::filesystem::path& Path)
{
	return platform::ReadFileText(Path, frt::ETextFileEncoding::Raw).value_or(FString());
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

	EXPECT_FALSE(ReadFile(path).Contains('\r'));

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


TEST(IniFileTest, NonAsciiNamesAndValuesRoundTripAsUtf8)
{
	const std::filesystem::path path = MakeTempPath("Налаштування 漢字.ini");

	CIniFile written;
	written.Set("Гра", "Назва", "Привіт, світе! 漢字テスト 😀👍🏽");
	written.Set("Гра", "Шлях", "C:\\Ігри\\Тест");
	ASSERT_TRUE(written.SaveToFile(path));

	// UTF-8 without a BOM: what Premake/project.lua reads and writes too.
	EXPECT_EQ(ReadFile(path), "[Гра]\nНазва=Привіт, світе! 漢字テスト 😀👍🏽\nШлях=C:\\Ігри\\Тест\n");

	CIniFile read;
	ASSERT_TRUE(read.LoadFromFile(path));
	ASSERT_NE(read.Find("Гра", "Назва"), nullptr);
	EXPECT_EQ(*read.Find("Гра", "Назва"), "Привіт, світе! 漢字テスト 😀👍🏽");
	ASSERT_NE(read.Find("Гра", "Шлях"), nullptr);
	EXPECT_EQ(*read.Find("Гра", "Шлях"), "C:\\Ігри\\Тест");

	std::filesystem::remove(path);
}


TEST(IniFileTest, CaseIsIgnoredForAsciiLettersOnly)
{
	// Premake/project.lua compares names with Lua's string.lower, which lowers A-Z and nothing
	// else: both sides must agree on which names are one name.
	CIniFile ini;
	ini.Parse("[Секція]\nКЛЮЧ=upper\nключ=lower\nKey=ascii\n");

	ASSERT_NE(ini.Find("Секція", "КЛЮЧ"), nullptr);
	EXPECT_EQ(*ini.Find("Секція", "КЛЮЧ"), "upper");
	ASSERT_NE(ini.Find("Секція", "ключ"), nullptr);
	EXPECT_EQ(*ini.Find("Секція", "ключ"), "lower");
	ASSERT_NE(ini.Find("Секція", "KEY"), nullptr);
	EXPECT_EQ(*ini.Find("Секція", "KEY"), "ascii");
	EXPECT_EQ(ini.Find("СЕКЦІЯ", "Key"), nullptr);
	ASSERT_EQ(ini.GetSections().size(), 1u);
	EXPECT_EQ(ini.GetSections()[0].Entries.size(), 3u);
}


TEST(IniFileTest, TrimsAsciiWhitespaceOnly)
{
	// Lua's %s again: a no-break space or an ideographic space belongs to the value.
	CIniFile ini;
	ini.Parse("[Text]\nNbsp = value\xC2\xA0\nIdeographic=\xE3\x80\x80value\n");

	ASSERT_NE(ini.Find("Text", "Nbsp"), nullptr);
	EXPECT_EQ(*ini.Find("Text", "Nbsp"), "value\xC2\xA0");
	ASSERT_NE(ini.Find("Text", "Ideographic"), nullptr);
	EXPECT_EQ(*ini.Find("Text", "Ideographic"), "\xE3\x80\x80value");

	// Written unquoted, since loading keeps them, and read back unchanged.
	const std::filesystem::path path = MakeTempPath("nbsp.ini");
	ASSERT_TRUE(ini.SaveToFile(path));
	EXPECT_EQ(ReadFile(path), "[Text]\nNbsp=value\xC2\xA0\nIdeographic=\xE3\x80\x80value\n");

	CIniFile read;
	ASSERT_TRUE(read.LoadFromFile(path));
	ASSERT_NE(read.Find("Text", "Nbsp"), nullptr);
	EXPECT_EQ(*read.Find("Text", "Nbsp"), "value\xC2\xA0");
	ASSERT_NE(read.Find("Text", "Ideographic"), nullptr);
	EXPECT_EQ(*read.Find("Text", "Ideographic"), "\xE3\x80\x80value");

	std::filesystem::remove(path);
}


TEST(IniFileTest, FilesAreReadAsStoredLikeTheBuildTools)
{
	// Premake/project.lua's ini.load reads the same project descriptors and registries as the
	// bytes stored, less a UTF-8 BOM. A file the engine understood and Lua did not would open
	// in the editor and then fail to build.
	const std::filesystem::path withBom = MakeTempPath("utf8bom.ini");
	ASSERT_TRUE(platform::WriteFileText(withBom, "\xEF\xBB\xBF[Гра]\r\nНазва=Тест 😀\r\n"));

	CIniFile utf8;
	ASSERT_TRUE(utf8.LoadFromFile(withBom));
	ASSERT_NE(utf8.Find("Гра", "Назва"), nullptr);
	EXPECT_EQ(*utf8.Find("Гра", "Назва"), "Тест 😀");

	// What Notepad saves as "Unicode": a BOM, then UTF-16LE. Lua finds no [Гра] in it, so
	// neither does the engine. The wide text is a named local: a range over ToWide(...).View()
	// would outlive the temporary it views.
	const frt::FWideString wide = frt::ToWide("[Гра]\r\nНазва=Тест 😀\r\n");
	FString bytes = "\xFF\xFE";
	for (const wchar_t unit : wide.View())
	{
		bytes.AppendChar(static_cast<char>(unit & 0xFF));
		bytes.AppendChar(static_cast<char>(unit >> 8));
	}
	const std::filesystem::path utf16Path = MakeTempPath("utf16.ini");
	ASSERT_TRUE(platform::WriteFileText(utf16Path, bytes));

	CIniFile utf16;
	ASSERT_TRUE(utf16.LoadFromFile(utf16Path));
	EXPECT_EQ(utf16.Find("Гра", "Назва"), nullptr);

	std::filesystem::remove(withBom);
	std::filesystem::remove(utf16Path);
}


TEST(IniFileTest, TextThatIsNotUtf8IsKeptByteForByte)
{
	// A file another tool saved in an ANSI code page: nothing is replaced, so a save loses
	// nothing, and whoever takes the value as a path finds it is none.
	CIniFile ini;
	ini.Parse("[Paths]\nHome=C:\\Users\\J\xF6rg\n");

	ASSERT_NE(ini.Find("Paths", "Home"), nullptr);
	EXPECT_EQ(*ini.Find("Paths", "Home"), "C:\\Users\\J\xF6rg");
	EXPECT_FALSE(ini.Find("Paths", "Home")->TryToPath().has_value());
}
