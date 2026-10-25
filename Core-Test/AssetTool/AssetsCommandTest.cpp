// FRT.bat assets before AssetTool runs (Premake/tools.lua, the assets action; design §13.1): Premake's own
// --help and --version never take the run over - they reach AssetTool like every other option - and a
// failure of the action itself keeps AssetTool's contract, its exit code and, with --json, AssetTool's
// error object as the one line of standard output ({"error":...,"exitCode":N}, control characters in the
// message escaped), so a script reading the stream sees why it ended. Run as FRT.bat runs it - the
// engine's vendored premake5.exe - on a scratch copy of premake5.lua and Premake/ in a directory with a
// non-ASCII name, on paths that fail before any build of AssetTool would start.
#include <gtest/gtest.h>

#include "TestCommon.h"
#include "TestProcess.h"

#include <filesystem>
#include <system_error>
#include <vector>

#include "EnginePaths.h"
#include "Containers/String.h"
#include "Platform/FileSystem.h"
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
