// AssetTool's command-line rules that hold for every verb (AssetTool/Source/Commandlet.h, design §13.1): an
// option is either one the host reads (-Run=, -Project=, --json, --ci, --engine, --help, -?, -Section:Key=Value),
// in the form it reads it, or one the verb declares - anything else is a usage error, so a typo or a negative
// number never runs a verb in a mode nobody asked for - and the mount a run acts on is /Engine with --engine,
// else /Project when there is a project, else none. Compiled into Core-Test from AssetTool's sources, so it runs
// in every configuration, Release included; AssetToolTest.cpp runs the executable.
#include <gtest/gtest.h>

#include "TestCommon.h"

#include <filesystem>
#include <optional>
#include <vector>

#include "Containers/String.h"

#include "Commandlet.h"

using namespace frt;
using assettool::CCommandletContext;
using assettool::FindUnknownOption;
using assettool::SCommandlet;


namespace
{
constexpr FStringLiteral GcOptions[] = { "max-gb", "fix" };
constexpr SCommandlet Gc { "gc", nullptr, "gc [--max-gb=<n>] [--fix]: a stand-in", false, GcOptions };
constexpr SCommandlet Plain { "plain", nullptr, "plain: a verb with no options of its own", false };

std::optional<FString> FirstUnknown (std::vector<FString> CommandLine, const SCommandlet* Commandlet = &Plain)
{
	return FindUnknownOption(CommandLine, Commandlet);
}
}


TEST(CommandletTest, TheHostsOptionsAreKnownInEverySpelling)
{
	const std::vector<FString> known =
	{
		"--json", "-json", "--JSON", "-Json", "--ci", "-CI", "--engine", "-Engine", "--help", "-help", "-?", "--?",
		"-Run=NewGuid", "--run=x", "-RUN=", "-Project=C:\\Гра\\Гра.frtproject", "--project=Templates/Blank", "-Project=",
		"-Assets:ReadOnly=1", "--Assets:ReadOnly=true", "-Automation:CapturePath=C:/Captures=1",
	};
	for (const FString& option : known)
	{
		EXPECT_EQ(FirstUnknown({ option }), std::nullopt) << option.CStr();
		EXPECT_EQ(FirstUnknown({ option }, nullptr), std::nullopt) << option.CStr();
	}
}

TEST(CommandletTest, ArgumentsThatAreNoOptionsAreNeverUnknown)
{
	// Positional: a verb, a number, a mount path, a project file, text with '=' or ':'.
	EXPECT_EQ(FirstUnknown({ "newguid", "3", "/Project/Content/Rock.png", "/json", "Гра.frtproject", "a=b", "C:\\x" }), std::nullopt);
	EXPECT_EQ(FirstUnknown({}), std::nullopt);
}

TEST(CommandletTest, OtherOptionsAreUnknown)
{
	const std::vector<FString> unknown =
	{
		"--jsno", "--cii", "-3", "-1.5", "-", "--", "---json", "--json=1", "-ci=0", "-engine=x", "-Project", "--Run",
		"-Assets:ReadOnly", "-:Key=1", "-Assets:=1", "-=1", "--projec=x", "-Projects=x", "--max-gb=2",
	};
	for (const FString& option : unknown)
	{
		EXPECT_EQ(FirstUnknown({ option }), option) << option.CStr();
	}
	// The first unknown one, in command-line order.
	EXPECT_EQ(FirstUnknown({ "newguid", "--json", "-3", "--jsno" }), FString("-3"));
}

TEST(CommandletTest, AVerbsOwnOptionsAreKnownOnlyForIt)
{
	for (const FString& option : { FString("--max-gb=2"), FString("-max-gb=2"), FString("--MAX-GB"), FString("-Fix"),
		FString("--fix=all") })
	{
		EXPECT_EQ(FirstUnknown({ option }, &Gc), std::nullopt) << option.CStr();
		EXPECT_EQ(FirstUnknown({ option }, &Plain), option) << option.CStr();
		EXPECT_EQ(FirstUnknown({ option }, nullptr), option) << option.CStr();
	}
	EXPECT_EQ(FirstUnknown({ "--max-gb=2", "--json", "--fixx" }, &Gc), FString("--fixx"));
}

TEST(CommandletTest, TheMountIsTheEngineWithEngineElseTheProject)
{
	CCommandletContext context;
	EXPECT_TRUE(context.GetMountName().IsEmpty());
	EXPECT_TRUE(context.GetMountRoot().empty());

	context.EngineRoot = FStringView("C:\\Рушій").ToPath();
	EXPECT_TRUE(context.GetMountName().IsEmpty());
	EXPECT_TRUE(context.GetMountRoot().empty());

	context.ProjectRoot = FStringView("D:\\Гра 游戏").ToPath();
	EXPECT_EQ(context.GetMountName(), "/Project");
	EXPECT_TRUE(context.GetMountRoot() == context.ProjectRoot);

	// --engine: /Engine, whatever project there is.
	context.bEngineOwner = true;
	EXPECT_EQ(context.GetMountName(), "/Engine");
	EXPECT_TRUE(context.GetMountRoot() == context.EngineRoot);
	context.ProjectRoot.clear();
	EXPECT_EQ(context.GetMountName(), "/Engine");
	EXPECT_TRUE(context.GetMountRoot() == context.EngineRoot);
}
