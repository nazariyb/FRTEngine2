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
using assettool::FindVerb;
using assettool::GetOptionArguments;
using assettool::GetPositionalArguments;
using assettool::SCommandlet;


namespace
{
constexpr FStringLiteral GcOptions[] = { "max-gb", "fix" };
constexpr SCommandlet Gc { "gc", nullptr, "gc [--max-gb=<n>] [--fix]: a stand-in", false, GcOptions };
constexpr SCommandlet Plain { "plain", nullptr, "plain: a verb with no options of its own", false };
constexpr FStringLiteral QueryOptions[] = { "limit", "rows" };
constexpr SCommandlet Query { "query", nullptr, "query \"<text>\": a stand-in that takes text", false, QueryOptions, true };

std::optional<FString> FirstUnknown (std::vector<FString> CommandLine, const SCommandlet* Commandlet = &Plain)
{
	return FindUnknownOption(CommandLine, Commandlet);
}

std::vector<FString> Positional (std::vector<FString> CommandLine, const SCommandlet* Commandlet)
{
	return GetPositionalArguments(CommandLine, Commandlet);
}

std::optional<FString> Verb (std::vector<FString> CommandLine)
{
	return FindVerb(CommandLine);
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
		"--jsno", "--cii", "-3", "-1.5", "-", "---json", "--json=1", "-ci=0", "-engine=x", "-Project", "--Run",
		"-Assets:ReadOnly", "-:Key=1", "-Assets:=1", "-=1", "--projec=x", "-Projects=x", "--max-gb=2",
		// Whitespace in a section or key: text, never a config override.
		"-Assets:Read Only=1", "-label:Debug Width=5", "--Assets :ReadOnly=1",
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

TEST(CommandletTest, TheVerbAndItsArgumentsAreThePositionalArguments)
{
	// -Run= names the verb, else the first positional argument; options, the project and "--" are none.
	EXPECT_EQ(Verb({ "--json", "Гра.frtproject", "newguid", "3" }), FString("newguid"));
	EXPECT_EQ(Positional({ "--json", "Гра.frtproject", "newguid", "3", "/Project/Rock.png" }, &Plain),
		(std::vector<FString> { "3", "/Project/Rock.png" }));
	EXPECT_EQ(Verb({ "-Run=NewGuid", "3" }), FString("NewGuid"));
	EXPECT_EQ(Positional({ "-Run=NewGuid", "3" }, &Plain), std::vector<FString> { "3" });
	EXPECT_EQ(Verb({ "--json" }), std::nullopt);
	EXPECT_EQ(Verb({}), std::nullopt);
}

// "--" ends the options: every later argument is positional, whatever it starts with, and no option is read there.
TEST(CommandletTest, AfterTheEndOfOptionsEveryArgumentIsPositional)
{
	const std::vector<FString> commandLine = { "--json", "refs", "--", "-Odd", "--jsno", "--", "-Run=x" };
	EXPECT_EQ(GetOptionArguments(commandLine).size(), 2u);
	EXPECT_EQ(FirstUnknown(commandLine), std::nullopt);
	EXPECT_EQ(Verb(commandLine), FString("refs"));
	EXPECT_EQ(Positional(commandLine, &Plain), (std::vector<FString> { "-Odd", "--jsno", "--", "-Run=x" }));
	// The verb itself may follow it, and a project file names the project there too.
	EXPECT_EQ(Verb({ "--", "query", "-json" }), FString("query"));
	EXPECT_EQ(Positional({ "--", "query", "-json", "Гра.frtproject" }, &Query), std::vector<FString> { "-json" });
	// Before it, options are options as ever.
	EXPECT_EQ(FirstUnknown({ "--jsno", "--", "x" }), FString("--jsno"));
	EXPECT_EQ(GetOptionArguments(std::vector<FString> { "a", "-b" }).size(), 2u);
}

// A verb that takes text (query) keeps a term with one dash that no option is - a negated term, alone or at the start of
// one quoted text - as an argument; two dashes, and the options the host and the verb read, are still options.
TEST(CommandletTest, AVerbThatTakesTextKeepsItsNegatedTerms)
{
	for (const FString& text : { FString("-label:Debug"), FString("-label:Debug type:Model"), FString("-type=frt.Texture"), FString("-\"assault rifle\""),
		FString("-label:Debug Width=5"), FString("-Assets:ReadOnly"), FString("-3"), FString("-") })
	{
		EXPECT_EQ(FirstUnknown({ "query", text }, &Query), std::nullopt) << text.CStr();
		EXPECT_EQ(Positional({ "query", text }, &Query), std::vector<FString> { text }) << text.CStr();
		// Any other verb refuses it, as before.
		EXPECT_EQ(FirstUnknown({ "plain", text }, &Plain), text) << text.CStr();
		EXPECT_TRUE(Positional({ "plain", text }, &Plain).empty()) << text.CStr();
	}
	const std::vector<FString> commandLine = { "-label:Debug", "query", "--json", "-limit=3", "type:Model", "-Assets:ReadOnly=1", "-Rows", "-is:unused" };
	EXPECT_EQ(FirstUnknown(commandLine, &Query), std::nullopt);
	EXPECT_EQ(Verb(commandLine), FString("query"));
	EXPECT_EQ(Positional(commandLine, &Query), (std::vector<FString> { "-label:Debug", "type:Model", "-is:unused" }));
	EXPECT_EQ(FirstUnknown({ "query", "x", "--limt=3" }, &Query), FString("--limt=3"));
	EXPECT_EQ(FirstUnknown({ "query", "x", "--label:Debug" }, &Query), FString("--label:Debug"));
	EXPECT_EQ(Positional({ "-Run=query", "-label:Debug", "x" }, &Query), (std::vector<FString> { "-label:Debug", "x" }));
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
