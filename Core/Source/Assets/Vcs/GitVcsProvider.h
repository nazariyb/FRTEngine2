#pragma once

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

#include "Assets/Vcs/VcsProvider.h"
#include "Platform/Process.h"


/**
 * Git (design §12.9), run as `git.exe` found on PATH alone, one blocking process per call that needs it. FRT is not a git
 * client: it never commits, syncs or reverts, and stages exactly one kind of change, a case-only rename of a tracked file,
 * which `core.ignorecase=true` would otherwise hide. New files are left for the user to stage.
 *
 * Every git command runs from the mount root (while that does not exist, from the nearest folder above it), so a mount in a
 * submodule or a worktree talks to its own repository, and never compares a path git prints with one FRT holds: a path is
 * the mount-relative name plus the mount's prefix below the top level, which git itself reports, so a subst or mapped drive
 * or a junction in the path cannot make them disagree. Each command lists the whole mount once (`ls-tree`, `status`) and the
 * batch is looked up in the result, so no command line ever grows with the batch. A repository committed inside the mount
 * (a submodule under Content/) is its own: git lists its gitlink and never a file below it, so committed facts and status
 * of those files are asked of a provider of that repository, made on first use, and its locks make this one busy.
 *
 * IsBusy, IsRewritingTree and the path getters only stat or read what Detect resolved: they spawn nothing and are safe on
 * the owner thread. Every other call that asks git blocks on a process, so it belongs on a worker. All are thread-safe after
 * Detect, which happens once, before the provider is shared.
 */
namespace frt::assets
{
class FRT_CORE_API CGitVcsProvider final : public IVcsProvider
{
public:
	// True when MountRoot is in a work tree. False leaves this provider unusable (the caller falls back to None), and warns
	// once if a `.git` is there all the same: no git on PATH, or git refusing it (dubious ownership), quoting git's reason.
	// A mount root that does not exist yet is in the repository of the nearest folder above it, and git runs from there
	// until the root appears.
	// Under a pre-commit hook (FRT_HOOK_TOPLEVEL set), only the provider of the committing repository keeps the hook's
	// GIT_INDEX_FILE; every other one's git runs without it, and its GetStaged lists nothing: that repository's staged
	// files are not in this commit, and its own commit's hook checks them.
	bool Detect (const FString& MountRoot) override;
	bool IsBusy () const override;
	bool IsRewritingTree () const override;
	SVcsResult CheckOut (std::span<const FString> Files) override;
	SVcsResult Add (std::span<const FString> Files) override { (void)Files; return {}; }
	SVcsResult Move (std::span<const SVcsMove> Moves) override;
	SVcsResult Delete (std::span<const FString> Files) override;
	// In HEAD, under the portable fold, so a file renamed in another case outside git still counts. An unborn HEAD
	// (`FRT.bat new` commits nothing) means nothing is committed, not an error.
	SVcsResult GetCommittedFacts (std::span<const FString> Files, std::vector<bool>& OutCommitted) override;
	// A file git ignores is Untracked: it will not travel with the commit, which is what a status is asked for. A file git
	// does not list and that is not on disk is Untracked too.
	SVcsResult StatusOf (std::span<const FString> Files, std::vector<SVcsStatus>& Out) override;
	SVcsResult GetStaged (std::vector<FString>& OutFiles) override;
	[[nodiscard]] FStringView GetName () const override { return "Git"; }
	[[nodiscard]] FString GetRestoreCommand (const FString& File) const override;
	[[nodiscard]] FString GetAddCommand (const FString& File) const override;

	// What Detect resolved, absolute. The git dir is this work tree's own: a worktree's or a submodule's, not the common one.
	[[nodiscard]] const std::filesystem::path& GetTopLevel () const { return TopLevel; }
	[[nodiscard]] const std::filesystem::path& GetGitDir () const { return GitDir; }
	[[nodiscard]] const std::filesystem::path& GetCommonDir () const { return CommonDir; }

	// A crash during a git command leaves index.lock behind, and every hold would then last for ever without a word. Held
	// this long without a break logs one warning naming the file. It never deletes it: only a person knows no git is running.
	static constexpr uint32 StaleLockWarningMs = 30000u;
	void SetStaleLockWarningMsForTest (uint32 Ms) { LockWarningMs = Ms; }

private:
	// Which of the two locks git holds: none, one that rewrites the tree, or index.lock kept by a commit that waits for its
	// message (CheckLocks says how it tells). Stats only, and warns once of a lock held too long.
	enum class ELockHeld : uint8 { None, Rewriting, CommitMessage };
	[[nodiscard]] ELockHeld CheckLocks () const;
	// The mount root, or while it does not exist the nearest folder above it that did at Detect (git cannot start in a folder
	// that is not there). RunGit runs there; RunGitIn where it is told.
	[[nodiscard]] std::filesystem::path GetWorkingDir () const;
	[[nodiscard]] platform::SToolProcessResult RunGit (std::vector<FString> Arguments) const;
	[[nodiscard]] platform::SToolProcessResult RunGitIn (const std::filesystem::path& WorkingDir, std::vector<FString> Arguments) const;
	[[nodiscard]] SVcsResult MoveOne (const SVcsMove& Move) const;
	// The name below the top level git would print for File; empty when File is not under this mount.
	[[nodiscard]] FString MakeGitName (const FString& File) const;
	// `git mv` needs names relative to where it runs; false when a file is not under this mount.
	[[nodiscard]] bool MakeMountRelative (const FString& File, FString& Out) const;
	// The provider of the repository committed at Gitlink (its git name, "/"-ended), made on first use; null, after one
	// warning, when it is not checked out.
	[[nodiscard]] CGitVcsProvider* FindNested (const FString& Gitlink);
	// Takes from InOutOwn the files below one of Gitlinks and asks each one's provider about them, Ask(provider, indices).
	template <typename TAsk>
	[[nodiscard]] SVcsResult AskNested (std::span<const FString> Files, std::span<const FString> Gitlinks,
		std::vector<size_t>& InOutOwn, TAsk&& Ask);

	struct SNested
	{
		FString Gitlink;
		std::unique_ptr<CGitVcsProvider> Provider;   // null: not checked out
	};
	mutable std::mutex NestedMutex;   // IsBusy reads Nested on the owner thread while a worker may add to it
	std::vector<SNested> Nested;

	std::filesystem::path Git;        // empty until Detect succeeds
	std::filesystem::path MountRoot;
	std::filesystem::path TopLevel;
	std::filesystem::path GitDir;
	std::filesystem::path CommonDir;
	FString Prefix;                   // the mount below the top level: "" or "Content/"
	FString MissingTail;              // the part of the prefix that did not exist at Detect, "/"-ended; "" when it all did
	std::filesystem::path WorkingDirWhileMissing;   // the folder above it that did; empty when the mount root existed
	std::vector<std::filesystem::path> OtherBusyFiles;
	std::filesystem::path IndexLock;
	std::filesystem::path HeadLock;
	std::filesystem::path CommitMessage;   // COMMIT_EDITMSG
	std::vector<FString> UnsetEnvironment;   // GIT_INDEX_FILE when a hook gave it for another repository (Detect)
	bool bAnotherRepositoryIsCommitting = false;   // and then GetStaged lists nothing
	uint32 LockWarningMs = StaleLockWarningMs;
	mutable std::atomic<uint64> LockHeldSinceMs = 0u;   // 0: not held at the last look
	mutable std::atomic<bool> bLockWarned = false;
};
}
