// The pre-commit hook and its switch, self-tested on a scratch repository (design §13.3, §15 I24), run the way git runs it:
// a good commit goes through with a new source left to `validate --staged`; every check reports in one failed commit, with
// each of its messages, so no check hides another; the hook only asks git and AssetTool when it should (BUILD003 only in an
// engine checkout and only for staged asset sources, and a missing AssetTool is a message, not a refusal). The hook is LF
// whatever core.autocrlf says, or bash could not read its first line. `FRT.bat setup --hooks` sets core.hooksPath and
// nothing else, `=off` clears it, relative for an engine inside the clone. A failing commit costs a bash and a few git
// processes, so one commit asserts many messages. Skipped without git on PATH.
#include <gtest/gtest.h>

#include "TestCommon.h"
#include "TestProcess.h"
#include "Assets/Vcs/GitTestUtils.h"

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
constexpr FStringLiteral GuidA = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr FStringLiteral GuidB = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
constexpr FStringLiteral GuidC = "cccccccccccccccccccccccccccccccc";

[[nodiscard]] FString MetaText (FStringView Guid)
{
	return FString::Format("FormatVersion: 1\nGuid: {}\n", Guid);
}

std::filesystem::path gGit;
std::filesystem::path gDir;
std::filesystem::path gRepo;
FString gBase;

class GitHooksTest : public ::testing::Test
{
protected:
	static void SetUpTestSuite ()
	{
		gGit = platform::FindOnPath("git");
		if (gGit.empty())
		{
			return;
		}
		gDir = std::filesystem::temp_directory_path() / FString::Format("FRT Гачки 钩子 {}", platform::GetCurrentProcessId()).ToPath();
		std::error_code ec;
		std::filesystem::remove_all(gDir, ec);
		gRepo = gDir / FStringView("Сховище").ToPath();
		std::filesystem::create_directories(gRepo / "Content", ec);
		ASSERT_EQ(test::RunGit(gGit, gRepo, { "init", "-q" }).ExitCode, 0);
		Write("Content/Base.png", "base\n");
		Write("Content/Base.png.meta", MetaText(GuidA));
		Write("Content/Гра.png", "base\n");
		Write("Content/Гра.png.meta", MetaText(GuidB));
		Write("README.md", "readme\n");
		ASSERT_EQ(test::RunGit(gGit, gRepo, { "add", "--all" }).ExitCode, 0);
		ASSERT_EQ(test::RunGit(gGit, gRepo, { "commit", "-q", "-m", "Base" }).ExitCode, 0);
		gBase = test::RunGit(gGit, gRepo, { "rev-parse", "HEAD" }).Output.TrimEnd();

		// A copy of the hook outside any engine, so that nothing finds an AssetTool beside it.
		const std::filesystem::path hooks = gDir / "Hooks" / "Tools" / "GitHooks";
		std::filesystem::create_directories(hooks, ec);
		std::filesystem::copy_file(paths::GetEngineDir() / "Tools" / "GitHooks" / "pre-commit", hooks / "pre-commit", ec);
		ASSERT_FALSE(ec) << FString::FromUtf8Lossy(ec.message()).CStr();
		ASSERT_EQ(test::RunGit(gGit, gRepo, { "config", "core.hooksPath", FString::FromPath(hooks, EPathStyle::Generic) }).ExitCode, 0);
	}

	static void TearDownTestSuite ()
	{
		if (!gDir.empty())
		{
			std::error_code ec;
			for (const std::filesystem::directory_entry& entry : std::filesystem::recursive_directory_iterator(gDir, ec))
			{
				std::error_code ignored;
				std::filesystem::permissions(entry.path(), std::filesystem::perms::owner_write, std::filesystem::perm_options::add, ignored);
			}
			std::filesystem::remove_all(gDir, ec);
		}
	}

	void SetUp () override
	{
		if (gGit.empty())
		{
			GTEST_SKIP() << "no git.exe on PATH";
		}
		ASSERT_EQ(RunGit({ "reset", "-q", "--hard", gBase }).ExitCode, 0);
		ASSERT_EQ(RunGit({ "clean", "-qfd" }).ExitCode, 0);
	}

	static test::SProcessResult RunGit (std::vector<FString> Arguments)
	{
		return test::RunGit(gGit, gRepo, std::move(Arguments));
	}

	static void Write (FStringView Relative, FStringView Text = "x\n")
	{
		std::error_code ec;
		std::filesystem::create_directories((gRepo / Relative.ToPath()).parent_path(), ec);
		ASSERT_TRUE(platform::WriteFileText(gRepo / Relative.ToPath(), Text)) << FString(Relative).CStr();
	}

	static void Stage (std::vector<FString> Files)
	{
		Files.insert(Files.begin(), { "add", "--" });
		const test::SProcessResult added = RunGit(std::move(Files));
		ASSERT_EQ(added.ExitCode, 0) << added.Errors.CStr();
	}

	// An index entry with no file behind it: the only way to stage a name this file system would not allow.
	static void StageName (FStringView Name)
	{
		const FString blob = RunGit({ "rev-parse", "HEAD:README.md" }).Output.TrimEnd();
		const test::SProcessResult added = RunGit({ "-c", "core.protectNTFS=false", "update-index", "--add", "--cacheinfo",
			FString::Format("100644,{},{}", blob, Name) });
		ASSERT_EQ(added.ExitCode, 0) << added.Errors.CStr();
	}

	static test::SProcessResult Commit (std::vector<FString> Options = {})
	{
		Options.insert(Options.end(), { "commit", "-q", "-m", "A test's commit" });
		return RunGit(std::move(Options));
	}
};
}


TEST_F(GitHooksTest, TheHookIsLfWhateverCoreAutocrlfSays)
{
	const FString text = platform::ReadFileText(paths::GetEngineDir() / "Tools" / "GitHooks" / "pre-commit", ETextFileEncoding::Raw).value_or(FString());
	ASSERT_FALSE(text.IsEmpty());
	EXPECT_TRUE(text.StartsWith("#!/usr/bin/env bash\n"));
	EXPECT_FALSE(text.Contains('\r')) << "bash cannot read a first line that ends in a CR";
}

TEST_F(GitHooksTest, AGoodCommitGoesThroughAndEveryPairMovesTogether)
{
	// A pair; a new source without a meta, which only validate --staged can judge (it knows companions); a renamed pair; a
	// deleted pair; a non-ASCII pair; a meta edited, its source tracked; files that are not content at all.
	Write("Content/New.png");
	Write("Content/New.png.meta", MetaText(GuidC));
	Write("Content/Model.bin");
	ASSERT_EQ(RunGit({ "mv", "--", "Content/Base.png", "Content/Renamed.png" }).ExitCode, 0);
	ASSERT_EQ(RunGit({ "mv", "--", "Content/Base.png.meta", "Content/Renamed.png.meta" }).ExitCode, 0);
	ASSERT_EQ(RunGit({ "rm", "-q", "--", "Content/Гра.png", "Content/Гра.png.meta" }).ExitCode, 0);
	Write("Source/Code.cpp");
	Write("Source/Not content.meta", "no Guid here\n");
	Stage({ "Content/New.png", "Content/New.png.meta", "Content/Model.bin", "Source/Code.cpp", "Source/Not content.meta" });

	const test::SProcessResult commit = Commit();
	EXPECT_EQ(commit.ExitCode, 0) << commit.Output.CStr() << commit.Errors.CStr();
	EXPECT_EQ(commit.Errors, "") << "nothing to say: not a project, so check 6 is not FRT's to run";
}

TEST_F(GitHooksTest, EveryCheckReportsInOneRefusedCommit)
{
	// 1. A meta with no source, and a source deleted without its meta.
	Write("Content/Orphan.png.meta", MetaText("dddddddddddddddddddddddddddddddd"));
	ASSERT_EQ(RunGit({ "rm", "-q", "--cached", "--", "Content/Гра.png" }).ExitCode, 0);
	// 2. A copy of a tracked meta.
	Write("Content/Copy.png");
	Write("Content/Copy.png.meta", MetaText(GuidA));
	// 3. Two lines, none, a dashed one, conflict markers.
	Write("Content/Two.png");
	Write("Content/Two.png.meta", "Guid: eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee\nGuid: ffffffffffffffffffffffffffffffff\n");
	Write("Content/None.png");
	Write("Content/None.png.meta", "FormatVersion: 1\n");
	Write("Content/Dashed.png");
	Write("Content/Dashed.png.meta", "Guid: 11111111-1111-1111-1111-111111111111\n");
	Write("Content/Marked.png");
	Write("Content/Marked.png.meta", "<<<<<<< HEAD\nGuid: 22222222222222222222222222222222\n=======\n>>>>>>> other\n");
	Stage({ "Content/Orphan.png.meta", "Content/Copy.png", "Content/Copy.png.meta", "Content/Two.png", "Content/Two.png.meta", "Content/None.png",
		"Content/None.png.meta", "Content/Dashed.png", "Content/Dashed.png.meta", "Content/Marked.png", "Content/Marked.png.meta" });
	// 4. Collisions in the index, also non-ASCII, also of a folder, and illegal names; and one between index and disk, which
	// core.ignorecase=true lets through: the disk says Disk.PNG, the index disk.png.
	StageName("Content/README.md");
	StageName("Content/readme.MD");
	StageName("Content/ЯЩИК.bin");
	StageName("Content/ящик.bin");
	StageName("Content/Folder/a.bin");
	StageName("Content/FOLDER/b.bin");
	StageName("Content/Bad?Name.bin");
	StageName("Content/Trailing.");
	StageName("Content/AUX.bin");
	Write("Content/disk.png");
	Stage({ "Content/disk.png" });
	std::filesystem::rename(gRepo / "Content" / "disk.png", gRepo / "Content" / "Disk.PNG");

	const test::SProcessResult commit = Commit({ "-c", "core.protectNTFS=false" });
	EXPECT_EQ(commit.ExitCode, 1) << commit.Output.CStr() << commit.Errors.CStr();
	const FString all = commit.Output + commit.Errors;
	for (const FStringView expected : {
		FStringView("META002 Content/Orphan.png.meta: staged without its source"),
		FStringView("META002 Content/Гра.png: staged for delete or rename"),
		FStringView("META003 aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa is in more than one meta"),
		FStringView("Content/Base.png.meta"),
		FStringView("Content/Copy.png.meta"),
		FStringView("META004 Content/Two.png.meta: it has 2 Guid: lines"),
		FStringView("META004 Content/None.png.meta: it has 0 Guid: lines"),
		FStringView("META004 Content/Dashed.png.meta: its Guid: line is not 32"),
		FStringView("META004 Content/Marked.png.meta: it holds conflict markers"),
		FStringView("differ only in case"),
		FStringView("ЯЩИК"),
		FStringView("Content/FOLDER"),
		FStringView("PATH002 Content/Bad?Name.bin"),
		FStringView("PATH002 Content/Trailing."),
		FStringView("PATH002 Content/AUX.bin"),
		FStringView("the index has \"Content/disk.png\", the disk \"Content/Disk.PNG\""),
		FStringView("commit refused"),
	})
	{
		EXPECT_TRUE(all.Contains(expected)) << FString(expected).CStr() << " not in:\n" << all.CStr();
	}
	EXPECT_EQ(RunGit({ "rev-parse", "HEAD" }).Output.TrimEnd(), gBase) << "nothing was committed";
}

TEST_F(GitHooksTest, BuildersAreCheckedOnlyInAnEngineCheckoutAndOnlyForAssetSources)
{
	// An engine checkout, whose check-builders fails, and whose AssetTool is not built.
	Write("Engine.frtengine", "[Engine]\nName=Scratch\n");
	Write("FRT.bat", "@echo BUILD003 Test.Stub\r\n@exit /b 1\r\n");

	Write("Core/Source/Other/Code.cpp");
	Stage({ "Core/Source/Other/Code.cpp" });
	const test::SProcessResult other = Commit();
	EXPECT_EQ(other.ExitCode, 0) << other.Output.CStr() << other.Errors.CStr();
	EXPECT_TRUE((other.Output + other.Errors).Contains("quick validate skipped: AssetTool is not built")) << other.Output.CStr();

	Write("Core/Source/Assets/Code.cpp");
	Stage({ "Core/Source/Assets/Code.cpp" });
	const test::SProcessResult assets = Commit();
	EXPECT_EQ(assets.ExitCode, 1) << assets.Output.CStr() << assets.Errors.CStr();
	EXPECT_TRUE((assets.Output + assets.Errors).Contains("BUILD003 Test.Stub")) << assets.Output.CStr() << assets.Errors.CStr();
}

// The engine's own hook, so it finds the engine's AssetTool when this configuration built one. Until AssetTool has
// validate (G4 replaces this test with a staged META004 refused through it), its usage error is a message, not a refusal.
TEST_F(GitHooksTest, AToolWithoutTheVerbIsAMessageNotARefusal)
{
	Write("Game.frtproject", "[Project]\nName=Game\n");
	Write("Content/New.png");
	Stage({ "Content/New.png" });

	const FString hooks = FString::FromPath(paths::GetEngineDir() / "Tools" / "GitHooks", EPathStyle::Generic);
	const test::SProcessResult commit = Commit({ "-c", "core.hooksPath=" + hooks });
	EXPECT_EQ(commit.ExitCode, 0) << commit.Output.CStr() << commit.Errors.CStr();
	EXPECT_TRUE((commit.Output + commit.Errors).Contains("quick validate skipped")) << commit.Output.CStr() << commit.Errors.CStr();
}

TEST_F(GitHooksTest, SetupHooksSetsCoreHooksPathAndNothingElse)
{
	// A scratch engine inside a project, as FRT.bat runs it. No Engine.frtengine yet: if --hooks fell through to the rest of
	// setup, it would fail.
	const std::filesystem::path premake = paths::GetEngineDir() / "ThirdParty" / "premake-5.0.0-beta2-windows" / "premake5.exe";
	const std::filesystem::path embedded = gDir / FStringView("Вбудований проєкт").ToPath();
	const std::filesystem::path engine = embedded / "Engine";
	std::error_code ec;
	std::filesystem::create_directories(engine, ec);
	std::filesystem::copy_file(paths::GetEngineDir() / "premake5.lua", engine / "premake5.lua", std::filesystem::copy_options::overwrite_existing, ec);
	ASSERT_FALSE(ec) << FString::FromUtf8Lossy(ec.message()).CStr();
	std::filesystem::copy(paths::GetEngineDir() / "Premake", engine / "Premake",
		std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, ec);
	ASSERT_FALSE(ec) << FString::FromUtf8Lossy(ec.message()).CStr();
	ASSERT_EQ(test::RunGit(gGit, engine, { "init", "-q" }).ExitCode, 0);

	const auto setup = [&] (std::vector<FString> Options)
	{
		Options.insert(Options.begin(), { "--file=" + FString::FromPath(engine / "premake5.lua"), "setup" });
		return test::RunProcess(premake, Options, engine);
	};
	const auto hooksPath = [&] (const std::filesystem::path& Clone = {})
	{
		return test::RunGit(gGit, Clone.empty() ? engine : Clone, { "config", "--local", "--get", "core.hooksPath" }).Output.TrimEnd();
	};

	const test::SProcessResult on = setup({ "--hooks" });
	EXPECT_EQ(on.ExitCode, 0) << on.Output.CStr() << on.Errors.CStr();
	EXPECT_EQ(hooksPath(), "Tools/GitHooks") << on.Output.CStr();
	EXPECT_FALSE(std::filesystem::exists(engine / "FRTEngine2.sln")) << "--hooks generates nothing";

	const test::SProcessResult off = setup({ "--hooks=off" });
	EXPECT_EQ(off.ExitCode, 0) << off.Output.CStr() << off.Errors.CStr();
	EXPECT_EQ(hooksPath(), "");
	EXPECT_EQ(setup({ "--hooks=off" }).ExitCode, 0) << "already off is fine";

	const test::SProcessResult wrong = setup({ "--hooks=maybe" });
	EXPECT_NE(wrong.ExitCode, 0);
	EXPECT_EQ(hooksPath(), "");

	// A project's clone. An engine inside it is named relatively, so the clone can move; a shared one has only its absolute path.
	ASSERT_TRUE(platform::WriteFileText(engine / "Engine.frtengine", "[Engine]\nName=Scratch\nVersion=1\n"));
	ASSERT_TRUE(platform::WriteFileText(embedded / "Game.frtproject", "[Project]\nName=Game\nEngine=Engine\n"));
	ASSERT_EQ(test::RunGit(gGit, embedded, { "init", "-q" }).ExitCode, 0);
	const test::SProcessResult inside = setup({ "--hooks", "--project=" + FString::FromPath(embedded / "Game.frtproject") });
	EXPECT_EQ(inside.ExitCode, 0) << inside.Output.CStr() << inside.Errors.CStr();
	EXPECT_EQ(hooksPath(embedded), "Engine/Tools/GitHooks") << inside.Output.CStr();

	const std::filesystem::path shared = gDir / FStringView("Спільний проєкт").ToPath();
	std::filesystem::create_directories(shared, ec);
	const FString engineName = FString::Format("../{}/Engine", FString::FromPath(embedded.filename()));
	ASSERT_TRUE(platform::WriteFileText(shared / "Other.frtproject", FString::Format("[Project]\nName=Other\nEngine={}\n", engineName)));
	ASSERT_EQ(test::RunGit(gGit, shared, { "init", "-q" }).ExitCode, 0);
	const test::SProcessResult outside = setup({ "--hooks", "--project=" + FString::FromPath(shared / "Other.frtproject") });
	EXPECT_EQ(outside.ExitCode, 0) << outside.Output.CStr() << outside.Errors.CStr();
	EXPECT_TRUE(std::filesystem::path(hooksPath(shared).ToPath()).is_absolute()) << hooksPath(shared).CStr();
	EXPECT_TRUE(hooksPath(shared).EndsWith("Engine/Tools/GitHooks")) << hooksPath(shared).CStr();
}
