// AssetTool's command line as scripts see it (AssetTool/Source, design §13.1): the exit codes - 0 for
// success, 2 for every usage error, 4 when a run that writes meets a project another process owns -
// the shape of the --json lines (one object per line, stable field names), help's list of verbs,
// newguid's GUIDs, and the writer lock: a read-only run never takes it, and AssetTool's own lock verb
// keeps every other writer out until its input ends. Runs the AssetTool executable beside Core-Test on
// a scratch project in a directory with a non-ASCII name. Release builds no AssetTool: skipped there.
#include <gtest/gtest.h>

#include "TestCommon.h"
#include "TestProcess.h"

#include <filesystem>
#include <optional>
#include <set>
#include <system_error>
#include <vector>

#include "BuildConfig.h"
#include "EnginePaths.h"
#include "Assets/AssetId.h"
#include "Assets/OwnerLock.h"
#include "Containers/String.h"
#include "Platform/FileLock.h"
#include "Platform/Platform.h"
#include "Platform/Process.h"

using namespace frt;


namespace
{
// The C++ initializer newguid prints declares a constant: SGuid is an aggregate.
constexpr assets::SGuid InitializerExample
	{ { 0x9f, 0xc0, 0xd4, 0x01, 0x0b, 0xbf, 0x28, 0xb4, 0x59, 0x40, 0x72, 0xe7, 0x2b, 0x86, 0x55, 0xab } };
static_assert(InitializerExample.IsValid() && InitializerExample.GetVersion() == 2u);

// AssetTool beside this executable, named as the build names it: Core-Test-<config>.exe runs
// AssetTool-<config>.exe.
std::filesystem::path GetAssetToolPath ()
{
	const std::filesystem::path executable = platform::GetExecutablePath();
	const FString stem = FString::FromPath(executable.stem());
	constexpr FStringLiteral prefix = "Core-Test";
	FString name("AssetTool");
	name.Append(stem.View().RightChop(stem.StartsWith(prefix) ? prefix.Len() : stem.Len())).Append(".exe");
	return executable.parent_path() / name.ToPath();
}

std::vector<FStringView> SplitLines (FStringView Text)
{
	std::vector<FStringView> lines;
	for (const FStringView line : Text.Lines())
	{
		lines.push_back(line);
	}
	return lines;
}

bool IsLowerHex (FStringView Text)
{
	for (const char c : Text)
	{
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
		{
			return false;
		}
	}
	return true;
}

// "SGuid { { 0x9f, 0xc0, ... } }" back to the bytes it initializes; nullopt if it is not that shape.
std::optional<assets::SGuid> ParseInitializer (FStringView Text)
{
	constexpr FStringLiteral Prefix = "SGuid { { ";
	constexpr FStringLiteral Suffix = " } }";
	if (!Text.StartsWith(Prefix) || !Text.EndsWith(Suffix))
	{
		return std::nullopt;
	}
	assets::SGuid guid;
	uint32 count = 0u;
	for (const FStringView byte : Text.Mid(Prefix.Len(), Text.Len() - Prefix.Len() - Suffix.Len()).Split(FStringView(", ")))
	{
		if (count == assets::SGuid::Size || byte.Len() != 4u || !byte.StartsWith("0x") || !IsLowerHex(byte.RightChop(2u)))
		{
			return std::nullopt;
		}
		guid.Bytes[count++] = static_cast<uint8>(byte.RightChop(2u).ToUInt32(16u).value_or(0u));
	}
	return count == assets::SGuid::Size ? std::optional<assets::SGuid>(guid) : std::nullopt;
}

// A newguid GUID: 32 lowercase hex digits that parse, version 4 with the RFC variant.
void ExpectFreshGuid (FStringView Text)
{
	EXPECT_EQ(Text.Len(), assets::SGuid::TextLength) << FString(Text).CStr();
	EXPECT_TRUE(IsLowerHex(Text)) << FString(Text).CStr();
	const std::optional<assets::SGuid> guid = assets::SGuid::Parse(Text);
	ASSERT_TRUE(guid.has_value()) << FString(Text).CStr();
	EXPECT_EQ(guid->GetVersion(), 4u);
	EXPECT_TRUE(guid->HasRfcVariant());
	EXPECT_FALSE(guid->HasNumericText());
}


class AssetToolTest : public ::testing::Test
{
protected:
	void SetUp () override
	{
#if FRT_BUILD_RELEASE
		GTEST_SKIP() << "Release builds no AssetTool";
#else
		Exe = GetAssetToolPath();
		ASSERT_TRUE(std::filesystem::exists(Exe)) << FString::FromPath(Exe).CStr();

		Dir = std::filesystem::temp_directory_path() / FString::Format("FRT Ассети 资产 {}", platform::GetCurrentProcessId()).ToPath();
		std::error_code ec;
		std::filesystem::remove_all(Dir, ec);
		ProjectDir = Dir / FStringView("Гра 游戏").ToPath();
		ASSERT_TRUE(std::filesystem::create_directories(ProjectDir, ec)) << FString::FromUtf8Lossy(ec.message()).CStr();
		ProjectFile = ProjectDir / "AssetToolTest.frtproject";
		// The engine by its directory: the scratch project is not inside it.
		ASSERT_TRUE(platform::WriteFileText(ProjectFile, FString::Format("[Project]\nName=AssetToolTest\nEngine={}\n",
			FString::FromPath(paths::GetEngineDir()))));

		// Nothing AssetTool or the hosts write per user lands in the user's own directory.
		PreviousUserDataDir = platform::GetEnvironmentPath("FRT_USER_DATA_DIR");
		ASSERT_TRUE(platform::SetEnvironmentPath("FRT_USER_DATA_DIR", Dir / "UserData"));
#endif
	}

	void TearDown () override
	{
		if (!Dir.empty())
		{
			platform::SetEnvironmentPath("FRT_USER_DATA_DIR", PreviousUserDataDir);
			std::error_code ec;
			std::filesystem::remove_all(Dir, ec);
		}
	}

	// AssetTool with Arguments, in WorkingDir (default: the scratch directory, which holds no project).
	test::SProcessResult Run (const std::vector<FString>& Arguments, const std::filesystem::path& WorkingDir = {}) const
	{
		return test::RunProcess(Exe, Arguments, WorkingDir.empty() ? Dir : WorkingDir);
	}

	[[nodiscard]] FString ProjectArgument () const
	{
		return "-Project=" + FString::FromPath(ProjectFile);
	}

	std::filesystem::path Exe;
	std::filesystem::path Dir;
	std::filesystem::path ProjectDir;
	std::filesystem::path ProjectFile;
	std::filesystem::path PreviousUserDataDir;
};
}


TEST_F(AssetToolTest, HelpListsEveryVerb)
{
	const test::SProcessResult help = Run({ "help" });
	ASSERT_EQ(help.ExitCode, 0) << help.Errors.CStr();
	EXPECT_TRUE(help.Output.Contains("help [verb]:")) << help.Output.CStr();
	EXPECT_TRUE(help.Output.Contains("newguid [n]:")) << help.Output.CStr();
	EXPECT_TRUE(help.Output.Contains("lock:")) << help.Output.CStr();
	EXPECT_TRUE(help.Output.Contains("Exit codes: 0 success, 1 problems found, 2 usage error, 3 internal error or crash, 4 the mount"))
		<< help.Output.CStr();
	EXPECT_EQ(Run({ "--help" }).Output, help.Output);

	// --json: one object per verb, in table order, with stable fields.
	const test::SProcessResult json = Run({ "help", "--json" });
	ASSERT_EQ(json.ExitCode, 0) << json.Errors.CStr();
	const std::vector<FStringView> lines = SplitLines(json.Output);
	ASSERT_EQ(lines.size(), 3u) << json.Output.CStr();
	EXPECT_TRUE(lines[0].StartsWith("{\"verb\":\"help\",\"usage\":\"help [verb]: ")) << json.Output.CStr();
	EXPECT_TRUE(lines[0].EndsWith("\",\"writes\":false}")) << json.Output.CStr();
	EXPECT_TRUE(lines[1].StartsWith("{\"verb\":\"newguid\",\"usage\":\"newguid [n]: ")) << json.Output.CStr();
	EXPECT_TRUE(lines[2].StartsWith("{\"verb\":\"lock\",")) << json.Output.CStr();
	EXPECT_TRUE(lines[2].EndsWith("\",\"writes\":true}")) << json.Output.CStr();

	const test::SProcessResult one = Run({ "help", "NewGuid" });
	ASSERT_EQ(one.ExitCode, 0) << one.Errors.CStr();
	EXPECT_EQ(SplitLines(one.Output).size(), 1u) << one.Output.CStr();
	EXPECT_TRUE(one.Output.Contains("newguid [n]:")) << one.Output.CStr();
}

TEST_F(AssetToolTest, NewGuidPrintsFreshGuidsAsMetaTextAndCppInitializers)
{
	const test::SProcessResult text = Run({ "newguid" });
	ASSERT_EQ(text.ExitCode, 0) << text.Errors.CStr();
	const std::vector<FStringView> lines = SplitLines(text.Output);
	ASSERT_EQ(lines.size(), 1u) << text.Output.CStr();
	const SStringSplit guidAndInitializer = lines[0].SplitFirst(FStringView("  "));
	ASSERT_TRUE(guidAndInitializer) << text.Output.CStr();
	ExpectFreshGuid(guidAndInitializer.Left);
	EXPECT_EQ(ParseInitializer(guidAndInitializer.Right), assets::SGuid::Parse(guidAndInitializer.Left)) << text.Output.CStr();

	// --json, the -Run= spelling, any case: {"guid":"<32 hex>","cpp":"SGuid { { ... } }"} per line, all different.
	const test::SProcessResult json = Run({ "-Run=NewGuid", "5", "--json" });
	ASSERT_EQ(json.ExitCode, 0) << json.Errors.CStr();
	const std::vector<FStringView> objects = SplitLines(json.Output);
	ASSERT_EQ(objects.size(), 5u) << json.Output.CStr();
	std::set<FString> seen;
	for (const FStringView object : objects)
	{
		constexpr FStringLiteral Start = "{\"guid\":\"";
		constexpr FStringLiteral Middle = "\",\"cpp\":\"";
		constexpr FStringLiteral End = "\"}";
		ASSERT_TRUE(object.StartsWith(Start) && object.EndsWith(End)) << FString(object).CStr();
		const FStringView guid = object.Mid(Start.Len(), assets::SGuid::TextLength);
		ExpectFreshGuid(guid);
		const FStringView rest = object.RightChop(Start.Len() + assets::SGuid::TextLength);
		ASSERT_TRUE(rest.StartsWith(Middle)) << FString(object).CStr();
		const FStringView initializer = rest.Mid(Middle.Len(), rest.Len() - Middle.Len() - End.Len());
		EXPECT_EQ(ParseInitializer(initializer), assets::SGuid::Parse(guid)) << FString(object).CStr();
		EXPECT_TRUE(seen.insert(FString(guid)).second) << "twice: " << FString(guid).CStr();
	}
	EXPECT_TRUE(json.Errors.IsEmpty()) << json.Errors.CStr();
}

TEST_F(AssetToolTest, UsageErrorsExitWithTwo)
{
	const test::SProcessResult noVerb = Run({});
	EXPECT_EQ(noVerb.ExitCode, 2);
	EXPECT_TRUE(noVerb.Errors.Contains("No verb")) << noVerb.Errors.CStr();
	EXPECT_TRUE(noVerb.Output.IsEmpty()) << noVerb.Output.CStr();

	const test::SProcessResult unknown = Run({ "frobnicate" });
	EXPECT_EQ(unknown.ExitCode, 2);
	EXPECT_TRUE(unknown.Errors.Contains("No verb 'frobnicate'")) << unknown.Errors.CStr();

	// With --json the error is an object on stdout too, for a script that reads only that.
	const test::SProcessResult unknownJson = Run({ "frobnicate", "--json" });
	EXPECT_EQ(unknownJson.ExitCode, 2);
	EXPECT_TRUE(unknownJson.Output.StartsWith("{\"error\":\"No verb 'frobnicate'")) << unknownJson.Output.CStr();
	EXPECT_TRUE(unknownJson.Output.Contains("\"exitCode\":2}")) << unknownJson.Output.CStr();

	const std::vector<std::vector<FString>> badArguments =
	{
		{ "newguid", "abc" }, { "newguid", "0" }, { "newguid", "10001" }, { "newguid", "1", "2" }, { "help", "nosuch" },
		{ "help", "a", "b" }, { ProjectArgument(), "lock", "extra" },
	};
	for (const std::vector<FString>& arguments : badArguments)
	{
		const test::SProcessResult result = Run(arguments);
		EXPECT_EQ(result.ExitCode, 2) << arguments.back().CStr() << ": " << result.Errors.CStr();
		EXPECT_FALSE(result.Errors.IsEmpty()) << arguments.back().CStr();
	}

	// A verb that writes needs a mount: no project here, no --engine.
	const test::SProcessResult noProject = Run({ "lock" });
	EXPECT_EQ(noProject.ExitCode, 2);
	EXPECT_TRUE(noProject.Errors.Contains("no project")) << noProject.Errors.CStr();

	// A project that is not there; --engine together with a project.
	const test::SProcessResult missing = Run({ "-Project=" + FString::FromPath(Dir / "Немає.frtproject"), "newguid" });
	EXPECT_EQ(missing.ExitCode, 2);
	EXPECT_TRUE(missing.Errors.Contains("Немає.frtproject")) << missing.Errors.CStr();
	EXPECT_EQ(Run({ "--engine", ProjectArgument(), "lock" }).ExitCode, 2);

	// Verbs that write nothing need no project.
	EXPECT_EQ(Run({ "newguid" }).ExitCode, 0);
	EXPECT_EQ(Run({ "--engine", "newguid" }).ExitCode, 0);
}

// The design's message and code for a second writer: this test process owns the project, AssetTool's
// lock verb (which writes, as far as the lock is concerned) is refused and names it. Read-only runs
// never get that far, and verbs that write nothing never ask.
TEST_F(AssetToolTest, AWritingRunExitsWithFourWhileAnotherProcessOwnsTheProject)
{
	FString error;
	const std::optional<platform::CExclusiveFileLock> owned = assets::TryTakeOwnerLock(ProjectDir, nullptr, &error);
	ASSERT_TRUE(owned.has_value()) << error.CStr();
	const FString owner = assets::GetThisProcessAsOwner().Describe();

	const test::SProcessResult refused = Run({ ProjectArgument(), "lock" });
	EXPECT_EQ(refused.ExitCode, 4) << refused.Errors.CStr();
	EXPECT_TRUE(refused.Errors.Contains(owner + " owns /Project: close it, or run with --ci")) << refused.Errors.CStr();
	EXPECT_TRUE(refused.Output.IsEmpty()) << refused.Output.CStr();

	const test::SProcessResult refusedJson = Run({ ProjectArgument(), "lock", "--json" });
	EXPECT_EQ(refusedJson.ExitCode, 4);
	const std::vector<FStringView> lines = SplitLines(refusedJson.Output);
	ASSERT_EQ(lines.size(), 1u) << refusedJson.Output.CStr();
	EXPECT_TRUE(lines[0].StartsWith("{\"error\":\"")) << refusedJson.Output.CStr();
	EXPECT_TRUE(lines[0].Contains("\"exitCode\":4,\"owner\":\"" + owner + "\",\"mount\":\"/Project\"}")) << refusedJson.Output.CStr();

	// Read-only by request: no lock, so no 4 (lock itself has nothing to do then).
	const test::SProcessResult ci = Run({ ProjectArgument(), "lock", "--ci" });
	EXPECT_EQ(ci.ExitCode, 2) << ci.Errors.CStr();
	EXPECT_TRUE(ci.Errors.Contains("read-only")) << ci.Errors.CStr();
	EXPECT_EQ(Run({ ProjectArgument(), "lock", "-Assets:ReadOnly=1" }).ExitCode, 2);

	// newguid writes nothing: it runs beside the owner.
	EXPECT_EQ(Run({ ProjectArgument(), "newguid" }).ExitCode, 0);
}

// lock holds the project until its input ends: a second writer (AssetTool again) is refused meanwhile and
// names it, and once it let go the next one runs. The project is found from the working directory.
TEST_F(AssetToolTest, LockKeepsOtherWritersOutUntilItsInputEnds)
{
	test::CTestProcess holder;
	const FString arguments[] = { FString("lock") };
	FString startError;
	ASSERT_TRUE(holder.Start(Exe, arguments, true, ProjectDir, &startError)) << startError.CStr();
	ASSERT_TRUE(test::WaitUntil([&] { return holder.ReadOutput().Contains("is locked"); }))
		<< "out: " << holder.ReadOutput().CStr() << " err: " << holder.ReadErrors().CStr();
	EXPECT_TRUE(holder.ReadOutput().StartsWith("/Project is locked by this process")) << holder.ReadOutput().CStr();
	EXPECT_TRUE(std::filesystem::exists(assets::GetOwnerLockPath(ProjectDir)));

	const test::SProcessResult refused = Run({ "lock" }, ProjectDir);
	EXPECT_EQ(refused.ExitCode, 4) << refused.Errors.CStr();
	EXPECT_TRUE(refused.Errors.Contains(FString::Format(", pid {}, started ", holder.GetProcessId()))) << refused.Errors.CStr();
	EXPECT_TRUE(refused.Errors.Contains("AssetTool")) << refused.Errors.CStr();
	EXPECT_TRUE(holder.IsRunning());

	holder.CloseInput();
	EXPECT_EQ(holder.Wait(), 0) << holder.ReadErrors().CStr();
	EXPECT_TRUE(holder.ReadOutput().Contains("/Project is unlocked")) << holder.ReadOutput().CStr();
	EXPECT_FALSE(std::filesystem::exists(assets::GetOwnerLockPath(ProjectDir)));

	// Its input ends at once: it locks and lets go.
	const test::SProcessResult next = Run({ "lock", "--json" }, ProjectDir);
	EXPECT_EQ(next.ExitCode, 0) << next.Errors.CStr();
	const std::vector<FStringView> lines = SplitLines(next.Output);
	ASSERT_EQ(lines.size(), 2u) << next.Output.CStr();
	EXPECT_TRUE(lines[0].StartsWith("{\"locked\":\"/Project\",\"lockFile\":\"")) << next.Output.CStr();
	EXPECT_EQ(lines[1], "{\"unlocked\":\"/Project\"}");
}

// A killed lock holder leaves nothing behind that keeps the next writer out.
TEST_F(AssetToolTest, AKilledLockHolderLeavesTheProjectFree)
{
	test::CTestProcess holder;
	const FString arguments[] = { ProjectArgument(), FString("lock") };
	ASSERT_TRUE(holder.Start(Exe, arguments, true));
	ASSERT_TRUE(test::WaitUntil([&] { return holder.ReadOutput().Contains("is locked"); })) << holder.ReadErrors().CStr();
	holder.Kill();

	const test::SProcessResult next = Run({ ProjectArgument(), "lock" });
	EXPECT_EQ(next.ExitCode, 0) << next.Errors.CStr();
}
