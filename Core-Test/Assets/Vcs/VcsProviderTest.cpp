// The VCS providers against real scratch repositories (design §12.9, §15 I26): the Git provider is busy while any of the
// files §12.9 names exists in the git dir it resolved, and that holds for a worktree and a submodule; a case-only Move of a
// tracked file is a rename to git, and every other move stages nothing; refused or failed moves leave an asset and its
// meta together; committed facts and status round-trip non-ASCII names and survive an unborn HEAD; a read-only file fails
// CheckOut without being written, partly written or having its attribute cleared; and `None` asks nothing of anybody. A
// `.git` that cannot be used is None and one warning. The recording fake other pieces test against is checked here too.
// One small repository per suite (no test writes more than a handful of files). Skipped without git on PATH.
#include <gtest/gtest.h>

#include "TestCommon.h"
#include "TestProcess.h"
#include "Assets/Vcs/GitTestUtils.h"
#include "Assets/Vcs/RecordingVcsProvider.h"
#include "Log/LogCapture.h"
#include "Platform/RecycleBinTestUtils.h"

#include <filesystem>
#include <random>
#include <set>
#include <system_error>
#include <vector>

#include "Assets/Vcs/GitVcsProvider.h"
#include "Assets/Vcs/NoneVcsProvider.h"
#include "Assets/Vcs/VcsProvider.h"
#include "Containers/String.h"
#include "Platform/FileSystem.h"
#include "Platform/Platform.h"
#include "Platform/Process.h"

using namespace frt;
using namespace frt::assets;


namespace
{
std::filesystem::path gGit;
std::filesystem::path gDir;     // everything of this suite
std::filesystem::path gRepo;    // a repository with one commit
std::filesystem::path gMount;   // a folder below its top level, with a non-ASCII name

void SetReadOnly (const std::filesystem::path& File, bool bReadOnly)
{
	const std::filesystem::perms write = std::filesystem::perms::owner_write | std::filesystem::perms::group_write | std::filesystem::perms::others_write;
	std::filesystem::permissions(File, write, bReadOnly ? std::filesystem::perm_options::remove : std::filesystem::perm_options::add);
}

[[nodiscard]] uint32 CountWarnings (const test::CLogCapture& Capture)
{
	uint32 warnings = 0u;
	for (const test::CLogCapture::SEntry& entry : Capture.GetEntries())
	{
		warnings += entry.Level == log::ELevel::Warning ? 1u : 0u;
	}
	return warnings;
}

class CScopedPath
{
public:
	explicit CScopedPath (FStringView Value)
		: Saved(platform::GetEnvironmentValue("PATH").value_or(FString()))
	{
		platform::SetEnvironmentValue("PATH", Value);
	}

	~CScopedPath ()
	{
		platform::SetEnvironmentValue("PATH", Saved);
	}

private:
	FString Saved;
};

class VcsProviderTest : public ::testing::Test
{
protected:
	static void SetUpTestSuite ()
	{
		gGit = platform::FindOnPath("git");
		if (gGit.empty())
		{
			return;
		}
		gDir = std::filesystem::temp_directory_path() / FString::Format("FRT Версії 版本 {}", platform::GetCurrentProcessId()).ToPath();
		std::error_code ec;
		std::filesystem::remove_all(gDir, ec);
		gRepo = gDir / FStringView("Сховище 仓库").ToPath();
		gMount = gRepo / FStringView("Вміст 内容").ToPath();
		std::filesystem::create_directories(gMount, ec);
		ASSERT_EQ(test::RunGit(gGit, gRepo, { "init", "-q" }).ExitCode, 0);
		ASSERT_TRUE(platform::WriteFileText(gRepo / FStringView("Поза.txt").ToPath(), "beside the mount\n"));
		ASSERT_TRUE(platform::WriteFileText(gMount / "Base.png", "base\n"));
		ASSERT_TRUE(platform::WriteFileText(gMount / ".gitignore", "*.ign\n"));
		ASSERT_EQ(test::RunGit(gGit, gRepo, { "add", "--all" }).ExitCode, 0);
		ASSERT_EQ(test::RunGit(gGit, gRepo, { "commit", "-q", "-m", "Base" }).ExitCode, 0);
	}

	static void TearDownTestSuite ()
	{
		if (!gDir.empty())
		{
			std::error_code ec;
			// Git marks its objects read-only, and remove_all cannot delete those on Windows otherwise.
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
		ASSERT_TRUE(Git.Detect(FString::FromPath(gMount)));
	}

	void TearDown () override
	{
		if (!gGit.empty())
		{
			RunGit({ "reset", "-q", "--hard" });
			RunGit({ "clean", "-qffd" });
		}
	}

	static test::SProcessResult RunGit (std::vector<FString> Arguments)
	{
		return test::RunGit(gGit, gRepo, std::move(Arguments));
	}

	[[nodiscard]] static FString Mounted (FStringView Relative)
	{
		return FString::FromPath(gMount / Relative.ToPath());
	}

	static void Write (FStringView Relative, FStringView Text = "x\n")
	{
		std::error_code ec;
		std::filesystem::create_directories((gMount / Relative.ToPath()).parent_path(), ec);
		ASSERT_TRUE(platform::WriteFileText(gMount / Relative.ToPath(), Text));
	}

	static void CommitAll ()
	{
		ASSERT_EQ(RunGit({ "add", "--all" }).ExitCode, 0);
		ASSERT_EQ(RunGit({ "commit", "-q", "-m", "A test's files" }).ExitCode, 0);
	}

	// The porcelain status lines of the mount, as a person would read them.
	[[nodiscard]] static FString StatusLines ()
	{
		return RunGit({ "status", "--porcelain" }).Output;
	}

	[[nodiscard]] static std::vector<FString> DirectoryNames (const std::filesystem::path& Directory)
	{
		std::vector<FString> names;
		for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(Directory))
		{
			names.push_back(FString::FromPath(entry.path().filename()));
		}
		return names;
	}

	CGitVcsProvider Git;
};
}


// ---------------- Detect ----------------

TEST_F(VcsProviderTest, DetectResolvesTheRepositoryFromTheMountRoot)
{
	EXPECT_EQ(Git.GetName(), "Git");
	EXPECT_EQ(platform::GetFileIdentity(Git.GetTopLevel()), platform::GetFileIdentity(gRepo));
	EXPECT_EQ(platform::GetFileIdentity(Git.GetGitDir()), platform::GetFileIdentity(gRepo / ".git"));
	EXPECT_EQ(platform::GetFileIdentity(Git.GetCommonDir()), platform::GetFileIdentity(gRepo / ".git"));
}

TEST_F(VcsProviderTest, MakeVcsProviderFollowsTheSetting)
{
	const test::CLogCapture capture;
	const std::filesystem::path outside = gDir / FStringView("Поза сховищем").ToPath();
	std::filesystem::create_directories(outside);

	EXPECT_EQ(MakeVcsProvider("Auto", FString::FromPath(gMount))->GetName(), "Git");
	EXPECT_EQ(MakeVcsProvider("Git", FString::FromPath(gMount))->GetName(), "Git");
	EXPECT_EQ(MakeVcsProvider("None", FString::FromPath(gMount))->GetName(), "None");
	EXPECT_EQ(MakeVcsProvider("Auto", FString::FromPath(outside))->GetName(), "None");
	EXPECT_EQ(CountWarnings(capture), 0u);

	// Asked for by name and not possible, or not a setting at all: said once, and it still works.
	EXPECT_EQ(MakeVcsProvider("Git", FString::FromPath(outside))->GetName(), "None");
	EXPECT_EQ(CountWarnings(capture), 1u);
	EXPECT_EQ(MakeVcsProvider("Perforce", FString::FromPath(gMount))->GetName(), "Git");
	EXPECT_EQ(CountWarnings(capture), 2u);
}

TEST_F(VcsProviderTest, ADotGitWithoutGitOnPathIsNoneAndOneWarning)
{
	const test::CLogCapture capture;
	const std::filesystem::path outside = gDir / FStringView("Без сховища").ToPath();
	std::filesystem::create_directories(outside);
	const CScopedPath path("");

	std::unique_ptr<IVcsProvider> provider = MakeVcsProvider("Auto", FString::FromPath(gMount));
	EXPECT_EQ(provider->GetName(), "None");
	ASSERT_EQ(CountWarnings(capture), 1u);
	EXPECT_TRUE(capture.GetEntries()[0].Text.Contains("git is not on PATH")) << capture.GetEntries()[0].Text.CStr();

	// No .git, no git: nothing to warn about.
	EXPECT_EQ(MakeVcsProvider("Auto", FString::FromPath(outside))->GetName(), "None");
	EXPECT_EQ(CountWarnings(capture), 1u);
}

TEST_F(VcsProviderTest, ADotGitGitRefusesIsNoneAndAWarningQuotingGit)
{
	const test::CLogCapture capture;
	const std::filesystem::path broken = gDir / FStringView("Зламане").ToPath();
	std::filesystem::create_directories(broken / ".git");

	EXPECT_EQ(MakeVcsProvider("Auto", FString::FromPath(broken))->GetName(), "None");
	ASSERT_EQ(CountWarnings(capture), 1u);
	EXPECT_TRUE(capture.GetEntries()[0].Text.Contains("not a git repository")) << capture.GetEntries()[0].Text.CStr();
}


// ---------------- IsBusy ----------------

TEST_F(VcsProviderTest, BusyWhileAnyOfTheFilesExistsInTheGitDir)
{
	EXPECT_FALSE(Git.IsBusy());
	EXPECT_FALSE(Git.IsRewritingTree());

	const struct { FStringView Name; bool bDirectory; bool bRewriting; } cases[] = {
		{ "index.lock", false, true },
		{ "HEAD.lock", false, true },
		{ "MERGE_HEAD", false, false },
		{ "REBASE_HEAD", false, false },
		{ "CHERRY_PICK_HEAD", false, false },
		{ "rebase-merge", true, false },
		{ "rebase-apply", true, false },
	};
	for (const auto& one : cases)
	{
		SCOPED_TRACE(FString(one.Name).CStr());
		const std::filesystem::path file = Git.GetGitDir() / FStringView(one.Name).ToPath();
		if (one.bDirectory)
		{
			ASSERT_TRUE(std::filesystem::create_directory(file));
		}
		else
		{
			ASSERT_TRUE(platform::WriteFileText(file, ""));
		}
		EXPECT_TRUE(Git.IsBusy());
		EXPECT_EQ(Git.IsRewritingTree(), one.bRewriting) << "an open merge holds repairs, never hot reload";
		std::filesystem::remove(file);
		EXPECT_FALSE(Git.IsBusy());
	}
}

TEST_F(VcsProviderTest, ALockHeldTooLongWarnsOnceAndIsNeverDeleted)
{
	const test::CLogCapture capture;
	const std::filesystem::path lock = Git.GetGitDir() / "index.lock";
	ASSERT_TRUE(platform::WriteFileText(lock, ""));

	// Under the default limit: nothing to say yet.
	EXPECT_TRUE(Git.IsBusy());
	EXPECT_EQ(CountWarnings(capture), 0u);

	Git.SetStaleLockWarningMsForTest(0u);
	EXPECT_TRUE(Git.IsBusy());
	EXPECT_TRUE(Git.IsRewritingTree());
	EXPECT_TRUE(Git.IsBusy());
	ASSERT_EQ(CountWarnings(capture), 1u);
	EXPECT_TRUE(capture.GetEntries()[0].Text.Contains("index.lock")) << capture.GetEntries()[0].Text.CStr();
	EXPECT_TRUE(std::filesystem::exists(lock));

	// An open merge is not a stale lock, however long it stays.
	std::filesystem::remove(lock);
	const std::filesystem::path merge = Git.GetGitDir() / "MERGE_HEAD";
	ASSERT_TRUE(platform::WriteFileText(merge, ""));
	EXPECT_TRUE(Git.IsBusy());
	EXPECT_EQ(CountWarnings(capture), 1u);
	std::filesystem::remove(merge);
}


// ---------------- Worktrees and submodules ----------------

TEST_F(VcsProviderTest, AWorktreeResolvesItsOwnGitDirAndIsBusyOnItsOwn)
{
	const std::filesystem::path tree = gDir / FStringView("Робоче дерево").ToPath();
	ASSERT_EQ(RunGit({ "worktree", "add", "-q", "--detach", FString::FromPath(tree) }).ExitCode, 0);

	CGitVcsProvider worktree;
	ASSERT_TRUE(worktree.Detect(FString::FromPath(tree / gMount.filename())));
	EXPECT_EQ(platform::GetFileIdentity(worktree.GetTopLevel()), platform::GetFileIdentity(tree));
	EXPECT_NE(platform::GetFileIdentity(worktree.GetGitDir()), platform::GetFileIdentity(gRepo / ".git"));
	EXPECT_EQ(platform::GetFileIdentity(worktree.GetCommonDir()), platform::GetFileIdentity(gRepo / ".git"));

	// Each work tree has its own index, so a commit in one does not hold the other.
	ASSERT_TRUE(platform::WriteFileText(Git.GetGitDir() / "index.lock", ""));
	EXPECT_TRUE(Git.IsBusy());
	EXPECT_FALSE(worktree.IsBusy());
	std::filesystem::remove(Git.GetGitDir() / "index.lock");
	ASSERT_TRUE(platform::WriteFileText(worktree.GetGitDir() / "index.lock", ""));
	EXPECT_TRUE(worktree.IsBusy());
	EXPECT_FALSE(Git.IsBusy());
	std::filesystem::remove(worktree.GetGitDir() / "index.lock");

	std::vector<bool> committed;
	const FString file = FString::FromPath(tree / gMount.filename() / "Base.png");
	EXPECT_EQ(worktree.GetCommittedFacts(std::span(&file, 1u), committed).Error, EVcsError::None);
	EXPECT_TRUE(committed[0]);

	EXPECT_EQ(RunGit({ "worktree", "remove", "--force", FString::FromPath(tree) }).ExitCode, 0);
}

TEST_F(VcsProviderTest, ASubmoduleIsARepositoryOfItsOwn)
{
	const std::filesystem::path source = gDir / FStringView("Підмодуль").ToPath();
	std::filesystem::create_directories(source);
	ASSERT_EQ(test::RunGit(gGit, source, { "init", "-q" }).ExitCode, 0);
	ASSERT_TRUE(platform::WriteFileText(source / "Sub.png", "sub\n"));
	ASSERT_EQ(test::RunGit(gGit, source, { "add", "--all" }).ExitCode, 0);
	ASSERT_EQ(test::RunGit(gGit, source, { "commit", "-q", "-m", "Sub" }).ExitCode, 0);
	const test::SProcessResult added = RunGit({ "-c", "protocol.file.allow=always", "submodule", "add", "-q", FString::FromPath(source), "Модуль" });
	ASSERT_EQ(added.ExitCode, 0) << added.Errors.CStr();

	const std::filesystem::path root = gRepo / FStringView("Модуль").ToPath();
	CGitVcsProvider submodule;
	ASSERT_TRUE(submodule.Detect(FString::FromPath(root)));
	EXPECT_EQ(platform::GetFileIdentity(submodule.GetTopLevel()), platform::GetFileIdentity(root));
	EXPECT_NE(platform::GetFileIdentity(submodule.GetGitDir()), platform::GetFileIdentity(gRepo / ".git"));
	EXPECT_TRUE(FString::FromPath(submodule.GetGitDir(), EPathStyle::Generic).Contains(".git/modules/"));

	ASSERT_TRUE(platform::WriteFileText(submodule.GetGitDir() / "index.lock", ""));
	EXPECT_TRUE(submodule.IsBusy());
	EXPECT_FALSE(Git.IsBusy());
	std::filesystem::remove(submodule.GetGitDir() / "index.lock");

	const FString file = FString::FromPath(root / "Sub.png");
	std::vector<bool> committed;
	EXPECT_EQ(submodule.GetCommittedFacts(std::span(&file, 1u), committed).Error, EVcsError::None);
	EXPECT_TRUE(committed[0]);
}


// ---------------- Move ----------------

TEST_F(VcsProviderTest, ACaseOnlyMoveOfATrackedPairIsARenameToGit)
{
	Write("Case/tex.png");
	Write("Case/tex.png.meta");
	// Git's own test for "the same file in another spelling" folds ASCII only, so a plain `git mv` refuses this pair.
	Write("Case/текстура 纹理.png");
	Write("Case/текстура 纹理.png.meta");
	CommitAll();

	const SVcsMove moves[] = { { Mounted("Case/tex.png"), Mounted("Case/Tex.png") }, { Mounted("Case/tex.png.meta"), Mounted("Case/Tex.png.meta") },
		{ Mounted("Case/текстура 纹理.png"), Mounted("Case/Текстура 纹理.png") }, { Mounted("Case/текстура 纹理.png.meta"), Mounted("Case/Текстура 纹理.png.meta") } };
	const SVcsResult result = Git.Move(moves);
	EXPECT_EQ(result.Error, EVcsError::None) << result.Message.CStr();

	const FString status = RunGit({ "-c", "core.quotePath=false", "status", "--porcelain" }).Output;
	EXPECT_TRUE(status.Contains("R  ")) << status.CStr();
	for (const FStringView name : { "Case/Tex.png\"\n", "Case/Tex.png.meta\"\n", "Case/Текстура 纹理.png\"\n", "Case/Текстура 纹理.png.meta\"\n" })
	{
		EXPECT_TRUE(status.Contains(name)) << FString(name).CStr() << " not in:\n" << status.CStr();
	}
	std::set<FString> names;
	for (FString& name : DirectoryNames(gMount / "Case"))
	{
		names.insert(std::move(name));
	}
	EXPECT_EQ(names, (std::set<FString> { "Tex.png", "Tex.png.meta", "Текстура 纹理.png", "Текстура 纹理.png.meta" }));
}

TEST_F(VcsProviderTest, ACaseOnlyMoveOfAnUntrackedFileStagesNothing)
{
	Write("Untracked/new.png");
	const SVcsMove move = { Mounted("Untracked/new.png"), Mounted("Untracked/New.png") };
	const SVcsResult result = Git.Move(std::span(&move, 1u));
	EXPECT_EQ(result.Error, EVcsError::None) << result.Message.CStr();

	EXPECT_EQ(RunGit({ "diff", "--cached", "--name-only" }).Output, "");
	EXPECT_EQ(DirectoryNames(gMount / "Untracked"), std::vector<FString> { "New.png" });
}

TEST_F(VcsProviderTest, GitMvRefusedOverIndexLockIsBusyAndTheBatchMovesBack)
{
	Write("Lock/tracked.png");
	CommitAll();
	Write("Lock/plain.png");
	const std::filesystem::path lock = Git.GetGitDir() / "index.lock";
	ASSERT_TRUE(platform::WriteFileText(lock, ""));

	const SVcsMove moves[] = { { Mounted("Lock/plain.png"), Mounted("Lock/Elsewhere/plain.png") }, { Mounted("Lock/tracked.png"), Mounted("Lock/Tracked.png") } };
	const SVcsResult result = Git.Move(moves);
	std::filesystem::remove(lock);

	EXPECT_EQ(result.Error, EVcsError::Busy) << result.Message.CStr();
	EXPECT_TRUE(result.Message.Contains("index.lock")) << result.Message.CStr();
	EXPECT_TRUE(std::filesystem::exists(gMount / "Lock" / "plain.png"));
	EXPECT_FALSE(std::filesystem::exists(gMount / "Lock" / "Elsewhere" / "plain.png"));
	std::set<FString> names;
	for (FString& name : DirectoryNames(gMount / "Lock"))
	{
		names.insert(std::move(name));
	}
	EXPECT_TRUE(names.contains("tracked.png")) << "the tracked file kept its spelling";
}

TEST_F(VcsProviderTest, APlainMoveStagesNothingAndMakesItsFolder)
{
	Write("Move/a.png");
	Write("Move/a.png.meta");
	CommitAll();

	const SVcsMove moves[] = { { Mounted("Move/a.png"), Mounted("Move/Глибше/b.png") }, { Mounted("Move/a.png.meta"), Mounted("Move/Глибше/b.png.meta") } };
	const SVcsResult result = Git.Move(moves);
	EXPECT_EQ(result.Error, EVcsError::None) << result.Message.CStr();
	EXPECT_TRUE(std::filesystem::exists(gMount / FStringView("Move/Глибше/b.png.meta").ToPath()));
	EXPECT_EQ(RunGit({ "diff", "--cached", "--name-only" }).Output, "");
}

TEST_F(VcsProviderTest, AMoveThatWouldBeRefusedRefusesBeforeAnythingMoves)
{
	Write("Refuse/a.png");
	Write("Refuse/a.png.meta");
	Write("Refuse/other.png.meta", "another asset's meta\n");

	const SVcsMove occupied[] = { { Mounted("Refuse/a.png"), Mounted("Refuse/other.png") }, { Mounted("Refuse/a.png.meta"), Mounted("Refuse/other.png.meta") } };
	SVcsResult result = Git.Move(occupied);
	EXPECT_EQ(result.Error, EVcsError::Failed);
	EXPECT_TRUE(result.Message.Contains("other.png.meta")) << result.Message.CStr();
	EXPECT_TRUE(std::filesystem::exists(gMount / "Refuse" / "a.png"));
	EXPECT_FALSE(std::filesystem::exists(gMount / "Refuse" / "other.png"));
	EXPECT_EQ(platform::ReadFileText(gMount / "Refuse" / "other.png.meta").value_or(FString()), "another asset's meta\n");

	const SVcsMove missing[] = { { Mounted("Refuse/a.png"), Mounted("Refuse/b.png") }, { Mounted("Refuse/Немає.meta"), Mounted("Refuse/b.png.meta") } };
	result = Git.Move(missing);
	EXPECT_EQ(result.Error, EVcsError::Failed);
	EXPECT_TRUE(result.Message.Contains("Немає.meta")) << result.Message.CStr();
	EXPECT_TRUE(std::filesystem::exists(gMount / "Refuse" / "a.png"));

	// A file moved to where it is, is where it should be.
	const SVcsMove same = { Mounted("Refuse/a.png"), Mounted("Refuse/a.png") };
	EXPECT_EQ(Git.Move(std::span(&same, 1u)).Error, EVcsError::None);
}


// ---------------- What git knows ----------------

TEST_F(VcsProviderTest, CommittedFactsAndStatusRoundTripEveryName)
{
	const FStringView names[] = { "Гра 游戏/Текстура 😀.png", "Bracket[1].png", "With space.png" };
	std::vector<FString> committed;
	for (const FStringView name : names)
	{
		Write(name);
		committed.push_back(Mounted(name));
	}
	CommitAll();
	Write("Гра 游戏/Новий 新.png");

	std::vector<FString> files = committed;
	files.push_back(Mounted("Гра 游戏/Новий 新.png"));
	files.push_back(FString::FromPath(gRepo / FStringView("Поза.txt").ToPath()));   // committed, but not this mount's
	std::vector<bool> facts;
	const SVcsResult factsResult = Git.GetCommittedFacts(files, facts);
	EXPECT_EQ(factsResult.Error, EVcsError::None) << factsResult.Message.CStr();
	EXPECT_EQ(facts, (std::vector<bool> { true, true, true, false, false }));

	std::vector<SVcsStatus> status;
	const SVcsResult statusResult = Git.StatusOf(files, status);
	EXPECT_EQ(statusResult.Error, EVcsError::None) << statusResult.Message.CStr();
	ASSERT_EQ(status.size(), files.size());
	EXPECT_EQ(status[0].State, EVcsState::Clean);
	EXPECT_EQ(status[1].State, EVcsState::Clean);
	EXPECT_EQ(status[2].State, EVcsState::Clean);
	EXPECT_EQ(status[3].State, EVcsState::Untracked);
	EXPECT_EQ(status[4].State, EVcsState::Untracked);

	// A file renamed in another case outside git is still the committed file.
	const FString otherCase = Mounted("with SPACE.png");
	EXPECT_EQ(Git.GetCommittedFacts(std::span(&otherCase, 1u), facts).Error, EVcsError::None);
	EXPECT_TRUE(facts[0]);

	// An empty batch is no reason to run git, and asks nothing.
	EXPECT_EQ(Git.GetCommittedFacts({}, facts).Error, EVcsError::None);
	EXPECT_TRUE(facts.empty());
	EXPECT_EQ(Git.StatusOf({}, status).Error, EVcsError::None);
	EXPECT_TRUE(status.empty());
}

TEST_F(VcsProviderTest, StatusTellsEveryStateApart)
{
	Write("State/Clean.png");
	Write("State/Modified.png");
	Write("State/ModifiedStaged.png");
	Write("State/Deleted.png");
	Write("State/DeletedStaged.png");
	Write("State/Conflict.png");
	CommitAll();
	Write("State/Modified.png", "changed\n");
	Write("State/ModifiedStaged.png", "changed\n");
	std::filesystem::remove(gMount / "State" / "Deleted.png");
	Write("State/Added.png");
	Write("State/Untracked.png");
	Write("State/Ignored.ign");
	const FString mountName = FString::FromPath(gMount.filename());
	const FString state = mountName + "/State/";
	ASSERT_EQ(RunGit({ "add", "--", state + "ModifiedStaged.png", state + "Added.png" }).ExitCode, 0);
	ASSERT_EQ(RunGit({ "rm", "-q", "--", state + "DeletedStaged.png" }).ExitCode, 0);

	// Three stages of one path, which is what a merge conflict leaves in the index.
	const FString blob = RunGit({ "rev-parse", "HEAD:" + state + "Clean.png" }).Output.TrimEnd();
	ASSERT_TRUE(platform::WriteFileText(Git.GetGitDir() / "stages.txt", FString::Format("0 {0}\t{1}Conflict.png\n100644 {2} 1\t{1}Conflict.png\n100644 {2} 2\t{1}Conflict.png\n100644 {2} 3\t{1}Conflict.png\n",
		"0000000000000000000000000000000000000000", state, blob)));
	const test::SProcessResult stages = RunGit({ "-c", "alias.stages=!git update-index --index-info < .git/stages.txt", "stages" });
	ASSERT_EQ(stages.ExitCode, 0) << stages.Errors.CStr();
	std::filesystem::remove(Git.GetGitDir() / "stages.txt");

	// A repository inside this one, never committed here, seen and ignored: git lists each as one folder with a trailing /.
	Write("State/Nested/Inner.png");
	Write("State/Held.ign/Inner.png");
	Write("State/Nested.png");
	ASSERT_EQ(test::RunGit(gGit, gMount / "State" / "Nested", { "init", "-q" }).ExitCode, 0);
	ASSERT_EQ(test::RunGit(gGit, gMount / "State" / "Held.ign", { "init", "-q" }).ExitCode, 0);

	const struct { FStringView Name; EVcsState State; } cases[] = {
		{ "State/Clean.png", EVcsState::Clean },
		{ "State/Modified.png", EVcsState::Modified },
		{ "State/ModifiedStaged.png", EVcsState::Modified },
		{ "State/Added.png", EVcsState::Added },
		{ "State/Deleted.png", EVcsState::Deleted },
		{ "State/DeletedStaged.png", EVcsState::Deleted },
		{ "State/Conflict.png", EVcsState::Conflicted },
		{ "State/Untracked.png", EVcsState::Untracked },
		{ "State/Ignored.ign", EVcsState::Untracked },
		{ "State/Never.png", EVcsState::Untracked },
		{ "State/Nested/Inner.png", EVcsState::Untracked },
		{ "State/nested/inner.png", EVcsState::Untracked },
		{ "State/Held.ign/Inner.png", EVcsState::Untracked },
		{ "State/Nested.png", EVcsState::Untracked },
	};
	std::vector<FString> files;
	for (const auto& one : cases)
	{
		files.push_back(Mounted(one.Name));
	}
	std::vector<SVcsStatus> status;
	const SVcsResult result = Git.StatusOf(files, status);
	EXPECT_EQ(result.Error, EVcsError::None) << result.Message.CStr();
	ASSERT_EQ(status.size(), files.size());
	for (size_t i = 0u; i < files.size(); ++i)
	{
		EXPECT_EQ(status[i].State, cases[i].State) << files[i].CStr() << " is " << static_cast<int32>(status[i].State);
	}
}

TEST_F(VcsProviderTest, AnUnbornHeadMeansNothingIsCommitted)
{
	const std::filesystem::path repository = gDir / FStringView("Новий проєкт").ToPath();
	std::filesystem::create_directories(repository / "Content");
	ASSERT_EQ(test::RunGit(gGit, repository, { "init", "-q" }).ExitCode, 0);
	ASSERT_TRUE(platform::WriteFileText(repository / "Content" / "A.png", "a\n"));
	ASSERT_TRUE(platform::WriteFileText(repository / "Content" / "B.png", "b\n"));
	ASSERT_EQ(test::RunGit(gGit, repository, { "add", "--", "Content/A.png" }).ExitCode, 0);

	CGitVcsProvider unborn;
	ASSERT_TRUE(unborn.Detect(FString::FromPath(repository / "Content")));
	const std::vector<FString> files = { FString::FromPath(repository / "Content" / "A.png"), FString::FromPath(repository / "Content" / "B.png") };
	std::vector<bool> facts;
	const SVcsResult factsResult = unborn.GetCommittedFacts(files, facts);
	EXPECT_EQ(factsResult.Error, EVcsError::None) << factsResult.Message.CStr();
	EXPECT_EQ(facts, (std::vector<bool> { false, false }));

	std::vector<SVcsStatus> status;
	EXPECT_EQ(unborn.StatusOf(files, status).Error, EVcsError::None);
	EXPECT_EQ(status[0].State, EVcsState::Added);
	EXPECT_EQ(status[1].State, EVcsState::Untracked);

	std::vector<FString> staged;
	EXPECT_EQ(unborn.GetStaged(staged).Error, EVcsError::None);
	EXPECT_EQ(staged, std::vector<FString> { files[0] });
}

TEST_F(VcsProviderTest, GetStagedListsOnlyThisMountsFilesByAbsolutePath)
{
	Write("Staged/Змінений.png");
	Write("Staged/Gone.png");
	CommitAll();
	Write("Staged/Змінений.png", "changed\n");
	Write("Staged/Unstaged.png");
	ASSERT_TRUE(platform::WriteFileText(gRepo / FStringView("Поза.txt").ToPath(), "changed\n"));
	const FString state = FString::FromPath(gMount.filename()) + "/Staged/";
	ASSERT_EQ(RunGit({ "add", "--", state + "Змінений.png", "Поза.txt" }).ExitCode, 0);
	ASSERT_EQ(RunGit({ "rm", "-q", "--", state + "Gone.png" }).ExitCode, 0);

	std::vector<FString> staged;
	const SVcsResult result = Git.GetStaged(staged);
	EXPECT_EQ(result.Error, EVcsError::None) << result.Message.CStr();
	EXPECT_EQ(std::set<FString>(staged.begin(), staged.end()),
		(std::set<FString> { FString::FromPath(gMount / "Staged" / "Gone.png"), FString::FromPath(gMount / "Staged" / FStringView("Змінений.png").ToPath()) }));
}

// A pre-commit hook hands its tool GIT_INDEX_FILE, relative as git leaves it or absolute (a worktree, `commit -a`). It belongs
// to the committing repository alone: git in any other reads it without a word, as an empty index or as somebody else's.
TEST_F(VcsProviderTest, OnlyTheCommittingRepositoryReadsTheIndexAHookWasGiven)
{
	Write("Hook/Staged.png");
	const FString staged = FString::FromPath(gMount.filename()) + "/Hook/Staged.png";
	ASSERT_EQ(RunGit({ "add", "--", staged }).ExitCode, 0);
	const std::filesystem::path tree = gDir / FStringView("Дерево поруч").ToPath();
	ASSERT_EQ(RunGit({ "worktree", "add", "-q", "--detach", FString::FromPath(tree) }).ExitCode, 0);
	const FString treeMount = FString::FromPath(tree / gMount.filename());
	const FString baseInTree = FString::FromPath(tree / gMount.filename() / "Base.png");

	for (const FString& index : { FString(".git/index"), FString::FromPath(gRepo / ".git" / "index") })
	{
		SCOPED_TRACE(index.CStr());
		platform::SetEnvironmentValue("GIT_INDEX_FILE", index);
		platform::SetEnvironmentValue("FRT_HOOK_TOPLEVEL", FString::FromPath(gRepo, EPathStyle::Generic));

		CGitVcsProvider committing;
		ASSERT_TRUE(committing.Detect(FString::FromPath(gMount)));
		std::vector<FString> files;
		EXPECT_EQ(committing.GetStaged(files).Error, EVcsError::None);
		EXPECT_EQ(files, std::vector<FString> { FString::FromPath(gMount / "Hook" / "Staged.png") });

		CGitVcsProvider other;
		ASSERT_TRUE(other.Detect(treeMount));
		EXPECT_EQ(other.GetStaged(files).Error, EVcsError::None);
		EXPECT_EQ(files, std::vector<FString> {}) << "nothing is staged in the other work tree";
		std::vector<SVcsStatus> status;
		EXPECT_EQ(other.StatusOf(std::span(&baseInTree, 1u), status).Error, EVcsError::None);
		EXPECT_EQ(status[0].State, EVcsState::Clean);

		platform::SetEnvironmentValue("FRT_HOOK_TOPLEVEL", "");
		platform::SetEnvironmentValue("GIT_INDEX_FILE", "");
	}

	// No hook said anything: a person's own GIT_INDEX_FILE is theirs, and git's rules apply.
	platform::SetEnvironmentValue("GIT_INDEX_FILE", FString::FromPath(gRepo / ".git" / "index"));
	CGitVcsProvider plain;
	const bool bDetected = plain.Detect(treeMount);
	std::vector<FString> files;
	const SVcsResult result = plain.GetStaged(files);
	platform::SetEnvironmentValue("GIT_INDEX_FILE", "");
	EXPECT_TRUE(bDetected);
	EXPECT_EQ(result.Error, EVcsError::None);
	EXPECT_EQ(files.size(), 1u);

	EXPECT_EQ(RunGit({ "worktree", "remove", "--force", FString::FromPath(tree) }).ExitCode, 0);
}


// ---------------- CheckOut, Add and the commands ----------------

TEST_F(VcsProviderTest, AReadOnlyFileFailsCheckOutAndIsLeftAlone)
{
	Write("ReadOnly/Locked.png", "locked\n");
	Write("ReadOnly/Open.png", "open\n");
	const std::filesystem::path locked = gMount / "ReadOnly" / "Locked.png";
	SetReadOnly(locked, true);

	CNoneVcsProvider none;
	const std::vector<FString> files = { Mounted("ReadOnly/Open.png"), Mounted("ReadOnly/Locked.png"), Mounted("ReadOnly/Not there yet.png") };
	for (IVcsProvider* provider : std::initializer_list<IVcsProvider*> { &Git, &none })
	{
		SCOPED_TRACE(FString(provider->GetName()).CStr());
		const SVcsResult result = provider->CheckOut(files);
		EXPECT_EQ(result.Error, EVcsError::ReadOnly);
		EXPECT_TRUE(result.Message.Contains("Locked.png")) << result.Message.CStr();
		EXPECT_FALSE(result.Message.Contains("Open.png")) << result.Message.CStr();
		EXPECT_TRUE(platform::Stat(locked)->bReadOnly) << "never cleared silently";
	}

	// A write that came anyway fails too, and leaves neither a partial file nor a temporary one.
	EXPECT_FALSE(platform::WriteFileAtomic(locked, FStringView("written").AsBytes(), false));
	EXPECT_EQ(platform::ReadFileText(locked).value_or(FString()), "locked\n");
	EXPECT_EQ(DirectoryNames(gMount / "ReadOnly").size(), 2u);
	SetReadOnly(locked, false);

	const std::vector<FString> writable = { Mounted("ReadOnly/Open.png"), Mounted("ReadOnly/Not there yet.png") };
	EXPECT_EQ(Git.CheckOut(writable).Error, EVcsError::None);
	EXPECT_EQ(none.CheckOut(writable).Error, EVcsError::None);
}

TEST_F(VcsProviderTest, AddLeavesNewFilesForTheUserToStage)
{
	Write("Add/New.png");
	const FString file = Mounted("Add/New.png");
	EXPECT_EQ(Git.Add(std::span(&file, 1u)).Error, EVcsError::None);
	EXPECT_EQ(RunGit({ "diff", "--cached", "--name-only" }).Output, "");
}

TEST_F(VcsProviderTest, DeleteGoesToTheRecycleBinAndAGoneFileIsDone)
{
	std::random_device random;
	const FString stem = FString::Format("FRT Core-Test видалити {:08x}", random());
	const FString first = stem + " 1.png";
	const FString second = stem + " 2.png";
	Write(FString::Format("Delete/{}", first));
	Write(FString::Format("Delete/{}", second));
	const std::filesystem::path folder = gMount / "Delete";
	CNoneVcsProvider none;

	const FString gitFile = FString::FromPath(folder / first.ToPath());
	const FString noneFile = FString::FromPath(folder / second.ToPath());
	const std::vector<FString> gitFiles = { gitFile, Mounted("Delete/Вже немає.png") };
	const SVcsResult gitResult = Git.Delete(gitFiles);
	EXPECT_EQ(gitResult.Error, EVcsError::None) << gitResult.Message.CStr();
	const SVcsResult noneResult = none.Delete(std::span(&noneFile, 1u));
	EXPECT_EQ(noneResult.Error, EVcsError::None) << noneResult.Message.CStr();
	EXPECT_FALSE(std::filesystem::exists(gitFile.ToPath()));
	EXPECT_FALSE(std::filesystem::exists(noneFile.ToPath()));
	EXPECT_EQ(test::PurgeFromRecycleBin(stem, folder), 2u);
}

TEST_F(VcsProviderTest, TheCommandsAPersonIsShownNameTheFile)
{
	EXPECT_EQ(Git.GetRestoreCommand("D:\\Game\\A.png.meta"), "git checkout -- \"D:\\Game\\A.png.meta\"");
	EXPECT_EQ(Git.GetRestoreCommand("D:\\My Game\\A.png.meta"), "git checkout -- \"D:\\My Game\\A.png.meta\"");
	EXPECT_EQ(Git.GetAddCommand("D:\\My Game\\A.png.meta"), "git add \"D:\\My Game\\A.png.meta\"");
	// A name comes with the repository, and pasted without quotes this one would start a second command.
	EXPECT_EQ(Git.GetRestoreCommand("D:\\Game\\A&calc.png.meta"), "git checkout -- \"D:\\Game\\A&calc.png.meta\"");
	EXPECT_EQ(Git.GetAddCommand("D:\\Game\\A&calc.png.meta"), "git add \"D:\\Game\\A&calc.png.meta\"");

	const CNoneVcsProvider none;
	EXPECT_TRUE(none.GetRestoreCommand("A.png.meta").IsEmpty());
	EXPECT_TRUE(none.GetAddCommand("A.png.meta").IsEmpty());
}


// ---------------- None and the fake ----------------

TEST_F(VcsProviderTest, NoneAsksNothingAndWarnsOnceOnACaseOnlyRename)
{
	const test::CLogCapture capture;
	CNoneVcsProvider none;
	EXPECT_TRUE(none.Detect(FString::FromPath(gMount)));
	EXPECT_EQ(none.GetName(), "None");
	EXPECT_FALSE(none.IsBusy());
	EXPECT_FALSE(none.IsRewritingTree());

	Write("None/a.png");
	Write("None/b.png");
	const SVcsMove plain = { Mounted("None/b.png"), Mounted("None/c.png") };
	EXPECT_EQ(none.Move(std::span(&plain, 1u)).Error, EVcsError::None);
	EXPECT_EQ(CountWarnings(capture), 0u);

	const SVcsMove first = { Mounted("None/a.png"), Mounted("None/A.png") };
	EXPECT_EQ(none.Move(std::span(&first, 1u)).Error, EVcsError::None);
	EXPECT_EQ(CountWarnings(capture), 1u);
	const SVcsMove second = { Mounted("None/A.png"), Mounted("None/a.png") };
	EXPECT_EQ(none.Move(std::span(&second, 1u)).Error, EVcsError::None);
	EXPECT_EQ(CountWarnings(capture), 1u);

	const FString file = Mounted("None/a.png");
	std::vector<bool> facts;
	EXPECT_EQ(none.GetCommittedFacts(std::span(&file, 1u), facts).Error, EVcsError::None);
	EXPECT_EQ(facts, std::vector<bool> { false });
	std::vector<SVcsStatus> status;
	EXPECT_EQ(none.StatusOf(std::span(&file, 1u), status).Error, EVcsError::None);
	EXPECT_EQ(status[0].State, EVcsState::Untracked);
	std::vector<FString> staged = { "left over" };
	EXPECT_EQ(none.GetStaged(staged).Error, EVcsError::None);
	EXPECT_TRUE(staged.empty());
}

TEST(RecordingVcsProviderTest, RecordsEachCallAsOneEntryAndRefusesOnDemand)
{
	const std::filesystem::path dir = std::filesystem::temp_directory_path() / FString::Format("FRT Запис 记录 {}", platform::GetCurrentProcessId()).ToPath();
	std::error_code ec;
	std::filesystem::remove_all(dir, ec);
	std::filesystem::create_directories(dir, ec);
	const FString asset = FString::FromPath(dir / "A.png");
	const FString meta = FString::FromPath(dir / "A.png.meta");
	const FString movedAsset = FString::FromPath(dir / "B.png");
	const FString movedMeta = FString::FromPath(dir / "B.png.meta");
	ASSERT_TRUE(platform::WriteFileText(asset.ToPath(), "a"));
	ASSERT_TRUE(platform::WriteFileText(meta.ToPath(), "m"));

	test::CRecordingVcsProvider provider;
	const std::vector<FString> pair = { asset, meta };
	EXPECT_EQ(provider.CheckOut(pair).Error, EVcsError::None);
	const SVcsMove moves[] = { { asset, movedAsset }, { meta, movedMeta } };
	EXPECT_EQ(provider.Move(moves).Error, EVcsError::None);
	EXPECT_TRUE(std::filesystem::exists(movedMeta.ToPath()));

	const std::vector<test::CRecordingVcsProvider::SCall> calls = provider.GetWriteCalls();
	ASSERT_EQ(calls.size(), 2u);
	EXPECT_EQ(calls[0].Name, "CheckOut");
	EXPECT_EQ(calls[0].Files, pair);
	EXPECT_EQ(calls[1].Name, "Move");
	EXPECT_EQ(calls[1].Files, (std::vector<FString> { asset, movedAsset, meta, movedMeta }));

	provider.Refuse("Delete", EVcsError::LockedByOther, "held by someone");
	const std::vector<FString> moved = { movedAsset, movedMeta };
	const SVcsResult refused = provider.Delete(moved);
	EXPECT_EQ(refused.Error, EVcsError::LockedByOther);
	EXPECT_EQ(refused.Message, "held by someone");
	EXPECT_TRUE(std::filesystem::exists(movedAsset.ToPath())) << "a refused call touches nothing";

	provider.Refuse("Delete", EVcsError::None);
	EXPECT_EQ(provider.Delete(moved).Error, EVcsError::None);
	EXPECT_FALSE(std::filesystem::exists(movedAsset.ToPath()));

	provider.SetBusy(true);
	EXPECT_TRUE(provider.IsBusy());
	EXPECT_FALSE(provider.IsRewritingTree());
	provider.SetCommitted(std::span(&asset, 1u));
	std::vector<bool> facts;
	EXPECT_EQ(provider.GetCommittedFacts(pair, facts).Error, EVcsError::None);
	EXPECT_EQ(facts, (std::vector<bool> { true, false }));
	std::filesystem::remove_all(dir, ec);
}
