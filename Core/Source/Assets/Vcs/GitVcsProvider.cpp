#include "Assets/Vcs/GitVcsProvider.h"

#include <chrono>
#include <optional>
#include <utility>

#include "Assets/Vcs/VcsFileOps.h"
#include "Containers/StringMap.h"
#include "Log/Log.h"
#include "Platform/FileSystem.h"
#include "Unicode/DisplayText.h"


namespace frt::assets
{
namespace
{
// Longer than any command here takes on a healthy repository; a hung git must not hold a worker for good.
constexpr uint32 GitTimeoutMs = 120000u;

[[nodiscard]] FStringView AsText (const std::vector<uint8>& Bytes) noexcept
{
	return FStringView(reinterpret_cast<const char*>(Bytes.data()), static_cast<uint32>(Bytes.size()));
}

// A `.git` file or directory at or above Directory: a repository git could not, or would not, use.
[[nodiscard]] bool HasDotGitAbove (const std::filesystem::path& Directory)
{
	for (std::filesystem::path current = Directory; ; current = current.parent_path())
	{
		if (platform::Stat(current / ".git").has_value())
		{
			return true;
		}
		if (current == current.parent_path())
		{
			return false;
		}
	}
}

[[nodiscard]] FStringView FirstLine (FStringView Text) noexcept
{
	return Text.TrimStartAndEnd().SplitFirst('\n').Left.TrimEnd();
}

// What failed, with git's words. Git says index.lock when another git holds the index: a wait, not a failure.
[[nodiscard]] SVcsResult MakeFailure (FStringView Command, const platform::SToolProcessResult& Run)
{
	if (!Run.bStarted)
	{
		return { EVcsError::Failed, Run.Stderr };
	}
	if (Run.bTimedOut)
	{
		return { EVcsError::Failed, FString::Format("git {} did not finish and was stopped", Command) };
	}
	return { Run.Stderr.View().Contains("index.lock") ? EVcsError::Busy : EVcsError::Failed,
		FString::Format("git {} failed: {}", Command, FirstLine(Run.Stderr)) };
}

// Porcelain v2 fields are separated by one space, and only the last, the path, may hold more.
[[nodiscard]] FStringView SkipFields (FStringView Entry, uint32 Count) noexcept
{
	for (uint32 field = 0u; field < Count; ++field)
	{
		Entry = Entry.SplitFirst(' ').Right;
	}
	return Entry;
}

[[nodiscard]] EVcsState ToVcsState (char Index, char WorkTree) noexcept
{
	if (Index == 'D' || WorkTree == 'D')
	{
		return EVcsState::Deleted;
	}
	if (Index == 'A' || Index == 'R' || Index == 'C')
	{
		return EVcsState::Added;
	}
	if (Index != '.' || WorkTree != '.')
	{
		return EVcsState::Modified;
	}
	return EVcsState::Clean;
}

// Whether a folder above Name was listed as one untracked or ignored entry, its name with a trailing '/'. A file's own
// name never ends with '/', so Listed holds both and only folders match.
template <typename TMap>
[[nodiscard]] bool IsInUntrackedFolder (const FString& Name, const TMap& Listed)
{
	const FStringView name = Name.View();
	for (uint32 at = 0u; at < name.Len(); ++at)
	{
		if (name[at] == '/' && Listed.contains(FString(name.Left(at + 1u))))
		{
			return true;
		}
	}
	return false;
}

// Always quoted: a name comes with the repository, and unquoted `A&calc.png` pasted into cmd starts a second command. A
// Windows name cannot hold '"'.
[[nodiscard]] FString QuoteForShell (const FString& File)
{
	return FString::Format("\"{}\"", File);
}

// One directory, as git spelled it and as Detect resolved it: the same text below, or the same directory on disk (a subst
// drive or a junction spells it otherwise).
[[nodiscard]] bool IsSameDirectory (const std::filesystem::path& Spelled, const std::filesystem::path& Resolved)
{
	const std::optional<FString> relative = platform::GetRelativePath(Spelled, Resolved);
	if (relative.has_value())
	{
		return relative->IsEmpty();
	}
	const std::optional<platform::SFileIdentity> spelled = platform::GetFileIdentity(Spelled);
	return spelled.has_value() && spelled->IsValid() && spelled == platform::GetFileIdentity(Resolved);
}
}


bool CGitVcsProvider::Detect (const FString& InMountRoot)
{
	MountRoot = platform::NormalizePath(InMountRoot.ToPath());
	const std::filesystem::path git = platform::FindOnPath("git");
	if (git.empty())
	{
		if (HasDotGitAbove(MountRoot))
		{
			log::Warning(log::Assets, "'{}' is in a git repository but git is not on PATH: no version control for it.",
				unicode::ToDisplayText(FString::FromPath(MountRoot)));
		}
		return false;
	}

	Git = git;
	const platform::SToolProcessResult run = RunGit({ "rev-parse", "--path-format=absolute", "--show-toplevel", "--git-dir",
		"--git-common-dir", "--show-prefix" });
	std::vector<FStringView> lines;
	if (run.bStarted && !run.bTimedOut && run.ExitCode == 0)
	{
		for (const FStringView line : AsText(run.Stdout).Split('\n'))
		{
			lines.push_back(line.TrimEnd("\r"));
		}
	}
	// Absolute, or git is older than --path-format (2.31) and printed the option back.
	if (lines.size() < 3u || !lines[0].ToPath().is_absolute() || !lines[1].ToPath().is_absolute()
		|| !lines[2].ToPath().is_absolute())
	{
		Git.clear();
		if (HasDotGitAbove(MountRoot))
		{
			log::Warning(log::Assets, "git cannot use the repository of '{}', so it has no version control: {}",
				unicode::ToDisplayText(FString::FromPath(MountRoot)),
				run.Stderr.IsEmpty() ? FStringView("git 2.31 or later is needed") : FirstLine(run.Stderr));
		}
		return false;
	}

	TopLevel = lines[0].ToPath();
	GitDir = lines[1].ToPath();
	CommonDir = lines[2].ToPath();
	Prefix = lines.size() > 3u ? FString(lines[3]) : FString();
	IndexLock = GitDir / "index.lock";
	HeadLock = GitDir / "HEAD.lock";
	CommitMessage = GitDir / "COMMIT_EDITMSG";
	// What `git status` calls an operation in progress, by the same files. REBASE_HEAD is not one: a rebase is open while
	// rebase-merge/ or rebase-apply/ is there, and git can leave REBASE_HEAD behind after one that ended, which would hold
	// every repair for good with nothing said. sequencer/todo is a cherry-pick or revert of several commits between steps.
	OtherBusyFiles = { GitDir / "MERGE_HEAD", GitDir / "CHERRY_PICK_HEAD", GitDir / "REVERT_HEAD", GitDir / "rebase-merge",
		GitDir / "rebase-apply", GitDir / "sequencer" / "todo" };

	// A pre-commit hook hands its tool GIT_INDEX_FILE and names the repository committing (Tools/GitHooks/pre-commit). That
	// index is the committing repository's alone: git in any other (an Engine/ submodule, a shared engine, another worktree)
	// would read it without a word, as an empty index or as somebody else's. Without the hook's word the variable is the
	// person's, and git's own rules apply.
	UnsetEnvironment.clear();
	const std::optional<FString> committing = platform::GetEnvironmentValue("FRT_HOOK_TOPLEVEL");
	if (committing.has_value() && !committing->IsEmpty() && !IsSameDirectory(committing->ToPath(), TopLevel))
	{
		UnsetEnvironment.emplace_back("GIT_INDEX_FILE");
	}
	return true;
}

// `git commit -a` and `git commit <paths>` take index.lock before their pre-commit hook and keep it while the person writes
// the message, which git asks for in a COMMIT_EDITMSG it writes once the hook passed. From then to its end a commit writes
// no working file, so a COMMIT_EDITMSG no older than the lock is a commit waiting for its message: busy for repairs, which
// must not write a meta in the middle of a commit, but no rewrite to hold hot reload for. A checkout, merge or rebase step
// takes a new index.lock, newer than any COMMIT_EDITMSG before it, and HEAD.lock is always a ref update in progress.
CGitVcsProvider::ELockHeld CGitVcsProvider::CheckLocks () const
{
	const std::optional<platform::SFileStat> index = platform::Stat(IndexLock);
	const bool bHead = platform::Stat(HeadLock).has_value();
	if (!index.has_value() && !bHead)
	{
		LockHeldSinceMs = 0u;
		bLockWarned = false;
		return ELockHeld::None;
	}
	ELockHeld held = ELockHeld::Rewriting;
	if (!bHead)
	{
		const std::optional<platform::SFileStat> message = platform::Stat(CommitMessage);
		if (message.has_value() && message->LastWriteTime >= index->LastWriteTime)
		{
			held = ELockHeld::CommitMessage;
		}
	}

	const uint64 nowMs = static_cast<uint64>(std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count()) + 1u;
	uint64 since = 0u;
	if (LockHeldSinceMs.compare_exchange_strong(since, nowMs))
	{
		since = nowMs;
	}
	if (nowMs - since >= LockWarningMs && !bLockWarned.exchange(true))
	{
		const FString file = unicode::ToDisplayText(FString::FromPath(index.has_value() ? IndexLock : HeadLock));
		if (held == ELockHeld::CommitMessage)
		{
			log::Warning(log::Assets, "'{}' has been held for {} s by a git commit waiting for its message, and repairs wait "
				"until it ends. If no git is running, the lock is stale: delete it.", file, (nowMs - since) / 1000u);
		}
		else
		{
			log::Warning(log::Assets, "'{}' has been there for {} s, and everything that waits for git is waiting. A git "
				"command may still be running; if none is, the lock is stale: delete it.", file, (nowMs - since) / 1000u);
		}
	}
	return held;
}

bool CGitVcsProvider::IsRewritingTree () const
{
	return !Git.empty() && CheckLocks() == ELockHeld::Rewriting;
}

bool CGitVcsProvider::IsBusy () const
{
	if (Git.empty())
	{
		return false;
	}
	if (CheckLocks() != ELockHeld::None)
	{
		return true;
	}
	// An open merge, rebase, cherry-pick or revert stays for as long as the person takes, unlike the two locks.
	for (const std::filesystem::path& file : OtherBusyFiles)
	{
		if (platform::Stat(file).has_value())
		{
			return true;
		}
	}
	return false;
}

SVcsResult CGitVcsProvider::CheckOut (std::span<const FString> Files)
{
	return CheckOutFiles(Files);
}

SVcsResult CGitVcsProvider::MoveOne (const SVcsMove& Move) const
{
	if (!IsCaseOnlyRename(Move))
	{
		return MoveFilePlain(Move);
	}
	FString from;
	FString to;
	if (!MakeMountRelative(Move.From, from) || !MakeMountRelative(Move.To, to))
	{
		return MoveFilePlain(Move);
	}
	const platform::SToolProcessResult tracked = RunGit({ "ls-files", "--error-unmatch", "--", from });
	if (!tracked.bStarted || tracked.bTimedOut || (tracked.ExitCode != 0 && tracked.ExitCode != 1))
	{
		return MakeFailure("ls-files", tracked);
	}
	if (tracked.ExitCode == 1)
	{
		return MoveFilePlain(Move);
	}
	// -f: git's own "is it the same file in another spelling" folds ASCII only, so it refuses `текстура` to `Текстура` as
	// "destination exists". RunMoves has made sure the target is this very file or absent, so -f overwrites nothing.
	const platform::SToolProcessResult moved = RunGit({ "mv", "-f", "--", from, to });
	if (!moved.bStarted || moved.bTimedOut || moved.ExitCode != 0)
	{
		return MakeFailure("mv", moved);
	}
	return {};
}

SVcsResult CGitVcsProvider::Move (std::span<const SVcsMove> Moves)
{
	return RunMoves(Moves, [this] (const SVcsMove& One) { return MoveOne(One); });
}

SVcsResult CGitVcsProvider::Delete (std::span<const FString> Files)
{
	return TrashFiles(Files);
}

SVcsResult CGitVcsProvider::GetCommittedFacts (std::span<const FString> Files, std::vector<bool>& OutCommitted)
{
	OutCommitted.assign(Files.size(), false);
	if (Files.empty())
	{
		return {};
	}
	if (Git.empty())
	{
		return { EVcsError::Failed, "git provider used before Detect" };
	}

	// Below the working directory only, and -z so that non-ASCII names arrive as they are, not quoted.
	const platform::SToolProcessResult listed = RunGit({ "ls-tree", "-r", "-z", "--full-name", "--name-only", "HEAD" });
	if (!listed.bStarted || listed.bTimedOut)
	{
		return MakeFailure("ls-tree", listed);
	}
	if (listed.ExitCode != 0)
	{
		// Asked only now, so the usual path spawns one process.
		const platform::SToolProcessResult head = RunGit({ "rev-parse", "--verify", "-q", "HEAD" });
		return head.bStarted && !head.bTimedOut && head.ExitCode != 0 ? SVcsResult {} : MakeFailure("ls-tree", listed);
	}

	FStringSetIgnoreCase committed;
	for (const FStringView name : AsText(listed.Stdout).Split('\0', EStringSplit::SkipEmpty))
	{
		committed.emplace(name);
	}
	for (size_t file = 0u; file < Files.size(); ++file)
	{
		const FString name = MakeGitName(Files[file]);
		OutCommitted[file] = !name.IsEmpty() && committed.contains(name);
	}
	return {};
}

SVcsResult CGitVcsProvider::StatusOf (std::span<const FString> Files, std::vector<SVcsStatus>& Out)
{
	Out.assign(Files.size(), SVcsStatus {});
	if (Files.empty())
	{
		return {};
	}
	if (Git.empty())
	{
		return { EVcsError::Failed, "git provider used before Detect" };
	}

	// --no-renames: a rename is its two files, and each has a state. --ignored with --untracked-files=all lists each ignored
	// file by itself. The paths are below the top level, whatever the working directory.
	const platform::SToolProcessResult run = RunGit({ "status", "--porcelain=v2", "-z", "--no-renames", "--untracked-files=all",
		"--ignored", "--", "." });
	if (!run.bStarted || run.bTimedOut || run.ExitCode != 0)
	{
		return MakeFailure("status", run);
	}

	// A case-only rename lists both spellings, so the exact one answers first; the fold is for a file renamed outside git.
	TStringMap<EVcsState> exact;
	TStringMapIgnoreCase<EVcsState> folded;
	const auto record = [&] (FStringView Path, EVcsState State)
	{
		exact.insert_or_assign(FString(Path), State);
		folded.insert_or_assign(FString(Path), State);
	};
	bool bOriginalPath = false;   // what follows a rename entry, were one ever reported
	// Even with --untracked-files=all, a repository inside this one, never committed here, is one `?` or `!` entry: its folder,
	// with a trailing '/'. Every file below it is as untracked as the folder.
	bool bUntrackedFolders = false;
	for (const FStringView entry : AsText(run.Stdout).Split('\0', EStringSplit::SkipEmpty))
	{
		if (bOriginalPath)
		{
			bOriginalPath = false;
			continue;
		}
		switch (entry.First())
		{
		case '1':
			record(SkipFields(entry, 8u), ToVcsState(entry[2], entry[3]));
			break;
		case '2':
			record(SkipFields(entry, 9u), ToVcsState(entry[2], entry[3]));
			bOriginalPath = true;
			break;
		case 'u':
			record(SkipFields(entry, 10u), EVcsState::Conflicted);
			break;
		case '?':
		case '!':
			record(SkipFields(entry, 1u), EVcsState::Untracked);
			bUntrackedFolders = bUntrackedFolders || entry.EndsWith('/');
			break;
		default:
			break;
		}
	}

	for (size_t file = 0u; file < Files.size(); ++file)
	{
		const FString name = MakeGitName(Files[file]);
		if (name.IsEmpty())
		{
			continue;
		}
		if (const auto found = exact.find(name); found != exact.end())
		{
			Out[file].State = found->second;
		}
		else if (const auto foundFolded = folded.find(name); foundFolded != folded.end())
		{
			Out[file].State = foundFolded->second;
		}
		else if (bUntrackedFolders && IsInUntrackedFolder(name, folded))
		{
			Out[file].State = EVcsState::Untracked;
		}
		else if (platform::Stat(Files[file].ToPath()).has_value())
		{
			Out[file].State = EVcsState::Clean;
		}
	}
	return {};
}

SVcsResult CGitVcsProvider::GetStaged (std::vector<FString>& OutFiles)
{
	OutFiles.clear();
	if (Git.empty())
	{
		return { EVcsError::Failed, "git provider used before Detect" };
	}
	// --relative: below the working directory only, and named from it. On an unborn HEAD, everything staged is listed.
	const platform::SToolProcessResult run = RunGit({ "diff", "--cached", "--name-only", "-z", "--no-renames", "--relative" });
	if (!run.bStarted || run.bTimedOut || run.ExitCode != 0)
	{
		return MakeFailure("diff --cached", run);
	}
	for (const FStringView name : AsText(run.Stdout).Split('\0', EStringSplit::SkipEmpty))
	{
		OutFiles.push_back(FString::FromPath((MountRoot / name.ToPath()).make_preferred()));
	}
	return {};
}

FString CGitVcsProvider::GetRestoreCommand (const FString& File) const
{
	return FString::Format("git checkout -- {}", QuoteForShell(File));
}

FString CGitVcsProvider::GetAddCommand (const FString& File) const
{
	return FString::Format("git add {}", QuoteForShell(File));
}

platform::SToolProcessResult CGitVcsProvider::RunGit (std::vector<FString> Arguments) const
{
	// --no-optional-locks: a look at the tree never takes index.lock, so FRT never makes git, or its own IsBusy, see a lock.
	// --literal-pathspecs: a name with [ or * in it is that name. core.fsmonitor off: the daemon it starts would be in this
	// call's job, and die with it.
	std::vector<FString> arguments = { "--no-optional-locks", "--literal-pathspecs", "-c", "core.fsmonitor=false" };
	arguments.insert(arguments.end(), std::make_move_iterator(Arguments.begin()), std::make_move_iterator(Arguments.end()));
	platform::SToolProcessDesc desc;
	desc.Executable = Git;
	desc.Arguments = std::move(arguments);
	desc.WorkingDir = MountRoot;
	desc.TimeoutMs = GitTimeoutMs;
	desc.UnsetEnvironment = UnsetEnvironment;
	return platform::RunToolProcess(desc);
}

FString CGitVcsProvider::MakeGitName (const FString& File) const
{
	FString relative;
	return MakeMountRelative(File, relative) ? Prefix + relative : FString();
}

bool CGitVcsProvider::MakeMountRelative (const FString& File, FString& Out) const
{
	std::optional<FString> relative = platform::GetRelativePath(File.ToPath(), MountRoot);
	if (!relative.has_value() || relative->IsEmpty())
	{
		return false;
	}
	Out = std::move(*relative);
	return true;
}
}
