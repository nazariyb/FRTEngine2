// FRT.bat assets before AssetTool runs (Premake/tools.lua, the assets action; design §13.1): Premake's own
// --help and --version never take the run over - they reach AssetTool like every other option - and a
// failure of the action itself keeps AssetTool's contract, its exit code and, with --json, AssetTool's
// error object as the one line of standard output ({"error":...,"exitCode":N}, control characters in the
// message escaped), so a script reading the stream sees why it ended. Run as FRT.bat runs it - the
// engine's vendored premake5.exe - on a scratch copy of premake5.lua and Premake/ in a directory with a
// non-ASCII name, on paths that fail before any build of AssetTool would start. FRT.bat new (design §13.3) on the same
// scratch copy, with a throwaway template: its rename of the template's name keeps each source and its meta a pair
// (X.png and X.png.meta, a name that contains the template's name too), copies a meta's bytes as they are but for LF,
// and carries the template's Schemas/Migrations.yml over, so the new project is not behind the engine (MIG001).
#include <gtest/gtest.h>

#include "TestCommon.h"
#include "TestProcess.h"

#include <filesystem>
#include <system_error>
#include <vector>

#include "EnginePaths.h"
#include "Assets/Meta/Migrations.h"
#include "Containers/String.h"
#include "Platform/FileSystem.h"
#include "Platform/Platform.h"
#include "Platform/Process.h"

using namespace frt;


namespace
{
class AssetsCommandTest : public ::testing::Test
{
protected:
	void SetUp () override
	{
		Premake = paths::GetEngineDir() / "ThirdParty" / "premake-5.0.0-beta2-windows" / "premake5.exe";
		ASSERT_TRUE(platform::Stat(Premake).has_value()) << FString::FromPath(Premake).CStr();

		Dir = std::filesystem::temp_directory_path() / FString::Format("FRT Команда 资产 {}", platform::GetCurrentProcessId()).ToPath();
		std::error_code ec;
		std::filesystem::remove_all(Dir, ec);
		Engine = Dir / FStringView("Рушій").ToPath();
		ASSERT_TRUE(std::filesystem::create_directories(Engine, ec)) << FString::FromUtf8Lossy(ec.message()).CStr();
		std::filesystem::copy_file(paths::GetEngineDir() / "premake5.lua", Engine / "premake5.lua", ec);
		ASSERT_FALSE(ec) << FString::FromUtf8Lossy(ec.message()).CStr();
		std::filesystem::copy(paths::GetEngineDir() / "Premake", Engine / "Premake", std::filesystem::copy_options::recursive, ec);
		ASSERT_FALSE(ec) << FString::FromUtf8Lossy(ec.message()).CStr();
	}

	void TearDown () override
	{
		if (!Dir.empty())
		{
			std::error_code ec;
			std::filesystem::remove_all(Dir, ec);
		}
	}

	test::SProcessResult RunAssets (std::vector<FString> Arguments) const
	{
		Arguments.insert(Arguments.begin(), { "--file=" + FString::FromPath(Engine / "premake5.lua"), "assets" });
		return test::RunProcess(Premake, Arguments, Engine);
	}

	[[nodiscard]] FString MissingProject () const
	{
		return FString::FromPath(Dir / FStringView("Немає.frtproject").ToPath());
	}

	// The file's bytes as text ("" when it cannot be read).
	static FString ReadBytes (const std::filesystem::path& Path)
	{
		const std::optional<std::vector<uint8>> bytes = platform::ReadFileBytesUncached(Path);
		return bytes.has_value() ? FString(FStringView(reinterpret_cast<const char*>(bytes->data()), static_cast<uint32>(bytes->size()))) : FString();
	}

	static void Write (const std::filesystem::path& Path, FStringView Bytes)
	{
		ASSERT_TRUE(platform::WriteFileBytes(Path, std::span(reinterpret_cast<const uint8*>(Bytes.GetData()), Bytes.Len())))
			<< FString::FromPath(Path).CStr();
	}

	std::filesystem::path Premake;
	std::filesystem::path Dir;
	std::filesystem::path Engine;
};
}


TEST_F(AssetsCommandTest, PremakesHelpAndVersionNeverTakeTheRunOver)
{
	for (const FStringView option : { FStringView("--help"), FStringView("--version") })
	{
		SCOPED_TRACE(FString(option).CStr());
		// The action runs, so the project it is named fails it: AssetTool's usage error, not Premake's text.
		const test::SProcessResult run = RunAssets({ "newguid", FString(option), "--json", "--project=" + MissingProject() });
		EXPECT_EQ(run.ExitCode, 2) << run.Output.CStr() << run.Errors.CStr();
		EXPECT_FALSE(run.Output.Contains("Premake")) << run.Output.CStr();
		EXPECT_FALSE(run.Output.Contains("Usage:")) << run.Output.CStr();
		EXPECT_TRUE(run.Output.StartsWith("{\"error\":\"")) << run.Output.CStr();
	}
}

// FRT.bat new from a template "Tiny": Tiny.png and Tiny.png.meta become Game.png and Game.png.meta, TinyRock's pair
// GameRock's, each meta byte for byte (its text names the template and is not rewritten; CRLF becomes LF, as for every
// hashed text file of a new project), and the template's Schemas/Migrations.yml comes along: the project is not behind.
// One Premake run with --no-generate (no solution, no build); git, when on PATH, makes the repository it always does.
TEST_F(AssetsCommandTest, NewKeepsEverySourceAndItsMetaAPairThroughTheTemplateRename)
{
	const std::filesystem::path previousUserData = platform::GetEnvironmentPath("FRT_USER_DATA_DIR");
	// The engine it registers and the recent project it adds go to a scratch user directory, put back at the end.
	ASSERT_TRUE(platform::SetEnvironmentPath("FRT_USER_DATA_DIR", Dir / "User"));
	struct SRestore
	{
		std::filesystem::path Saved;
		~SRestore () { platform::SetEnvironmentPath("FRT_USER_DATA_DIR", Saved); }
	} restore { previousUserData };

	Write(Engine / "Engine.frtengine", "[Engine]\nName=FrtNewTestEngine\n");
	std::error_code ec;
	std::filesystem::create_directories(Engine / "Templates" / "Common", ec);
	std::filesystem::copy_file(paths::GetEngineDir() / "Templates" / "Common" / ".gitattributes", Engine / "Templates" / "Common" / ".gitattributes", ec);
	ASSERT_FALSE(ec) << FString::FromUtf8Lossy(ec.message()).CStr();
	const std::filesystem::path tiny = Engine / "Templates" / "Tiny";
	constexpr FStringView Png = "PNG\r\nnot an image: bytes that must arrive as they are\r\n";
	constexpr FStringView Meta = "FormatVersion: 1\nGuid: 5d1e7a3b9c0f42d68e1a2b3c4d5e6f70\nLabels:\n  - Tiny\n";
	constexpr FStringView RockMeta = "FormatVersion: 1\r\nGuid: 3f2a9c4e7b1d4e0f9a8b6c5d4e3f2a1b\r\n";
	constexpr FStringView Migrations = "FormatVersion: 1\nApplied:\n  - metas\n";
	Write(tiny / "Template.ini", "[Template]\nDescription=A template for this test\n");
	Write(tiny / "Tiny.frtproject", "[Project]\nName=Tiny\n");
	Write(tiny / "Content" / "Tiny.png", Png);
	Write(tiny / "Content" / "Tiny.png.meta", Meta);
	Write(tiny / "Content" / "Rocks" / "TinyRock.png", Png);
	Write(tiny / "Content" / "Rocks" / "TinyRock.png.meta", RockMeta);
	Write(tiny / "Schemas" / "Migrations.yml", Migrations);
	ASSERT_FALSE(HasFatalFailure());

	const std::filesystem::path location = Dir / FStringView("Проєкти").ToPath();
	const test::SProcessResult created = test::RunProcess(Premake, std::vector<FString> { "--file=" + FString::FromPath(Engine / "premake5.lua"),
		"new", "--name=Game", "--template=Tiny", "--location=" + FString::FromPath(location), "--no-generate" }, Engine);
	ASSERT_EQ(created.ExitCode, 0) << created.Output.CStr() << created.Errors.CStr();

	const std::filesystem::path game = location / "Game";
	EXPECT_EQ(ReadBytes(game / "Content" / "Game.png"), Png);
	EXPECT_EQ(ReadBytes(game / "Content" / "Game.png.meta"), Meta) << "a meta's text is copied, never templated";
	EXPECT_EQ(ReadBytes(game / "Content" / "Rocks" / "GameRock.png"), Png);
	EXPECT_EQ(ReadBytes(game / "Content" / "Rocks" / "GameRock.png.meta"), "FormatVersion: 1\nGuid: 3f2a9c4e7b1d4e0f9a8b6c5d4e3f2a1b\n");
	for (const std::filesystem::path& stale : { game / "Content" / "Tiny.png", game / "Content" / "Tiny.png.meta", game / "Content" / "Rocks" / "TinyRock.png.meta",
		game / "Tiny.frtproject", game / "Template.ini" })
	{
		EXPECT_FALSE(std::filesystem::exists(stale, ec)) << FString::FromPath(stale).CStr();
	}
	EXPECT_TRUE(std::filesystem::exists(game / "Game.frtproject", ec));
	EXPECT_EQ(ReadBytes(assets::GetMigrationsPath(game)), Migrations);
	const std::optional<SDiagnostic> gap = assets::FindMigrationGap(game, "/Project", false);
	EXPECT_FALSE(gap.has_value()) << "a new project is not behind: " << (gap ? gap->Message.CStr() : "");
	EXPECT_FALSE(std::filesystem::exists(game / "Game.sln", ec)) << "--no-generate";
}

TEST_F(AssetsCommandTest, AFailureBeforeAssetToolPrintsItsErrorObjectWithJson)
{
	const test::SProcessResult missing = RunAssets({ "help", "--json", "--project=" + MissingProject() });
	EXPECT_EQ(missing.ExitCode, 2) << missing.Errors.CStr();
	EXPECT_TRUE(missing.Output.StartsWith("{\"error\":\"")) << missing.Output.CStr();
	// One line (Premake's standard output is in text mode: it ends in CRLF, which every reader of lines takes).
	EXPECT_TRUE(missing.Output.EndsWith(",\"exitCode\":2}\r\n")) << missing.Output.CStr();
	EXPECT_FALSE(missing.Output.View().LeftChop(2u).Contains('\n')) << missing.Output.CStr();
	EXPECT_TRUE(missing.Errors.Contains("assets: ")) << missing.Errors.CStr();

	// Every spelling of the switch AssetTool reads, and a control character escaped in the message.
	for (const FStringView json : { FStringView("--json"), FStringView("-JSON"), FStringView("/Json") })
	{
		SCOPED_TRACE(FString(json).CStr());
		const test::SProcessResult broken = RunAssets({ "help", "line\nbreak", FString(json) });
		EXPECT_EQ(broken.ExitCode, 2) << broken.Errors.CStr();
		EXPECT_EQ(broken.Output, "{\"error\":\"an argument holds a line break, which cmd.exe cannot pass on: line\\u000abreak\","
			"\"exitCode\":2}\r\n");
	}

	// Without --json, standard output stays empty: the message goes to standard error alone.
	const test::SProcessResult plain = RunAssets({ "help", "--project=" + MissingProject() });
	EXPECT_EQ(plain.ExitCode, 2);
	EXPECT_TRUE(plain.Output.IsEmpty()) << plain.Output.CStr();
	EXPECT_TRUE(plain.Errors.Contains("assets: ")) << plain.Errors.CStr();
}
