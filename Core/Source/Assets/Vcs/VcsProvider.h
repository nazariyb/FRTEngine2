#pragma once

#include <memory>
#include <span>
#include <vector>

#include "Core.h"
#include "CoreTypes.h"
#include "Enum.h"
#include "Containers/String.h"


/**
 * What FRT needs from a version-control system (design §12.9), and nothing more: FRT is not a VCS client. It needs writes
 * the VCS accepts (Perforce keeps unopened files read-only), an asset and its `.meta` kept together in every add, move and
 * delete, and to know when the VCS is rewriting the tree so automatic repairs wait. Used by the editor and AssetTool only,
 * never by the runtime or a package.
 *
 * `--ci` and guests make only read calls (IsBusy, IsRewritingTree, GetCommittedFacts, StatusOf, GetStaged), never CheckOut,
 * Add, Move or Delete. The pre-commit hook's `validate --staged --ci` does not wait on IsBusy either, because `git commit -a`
 * holds index.lock while the hook runs.
 *
 * Reader verbs that need no VCS pass no provider and run no Detect, and every call that spawns a process runs on a worker:
 * only IsBusy and IsRewritingTree, which are stats, run on the owner thread.
 */
namespace frt::assets
{
enum class EVcsError : uint8 { None, Busy, ReadOnly, LockedByOther, NotLatest, Offline, Failed };
struct SVcsResult { EVcsError Error = EVcsError::None; FString Message; };     // the message names files and owners

enum class EVcsState : uint8 { Untracked, Clean, Modified, Added, Deleted, Conflicted, OpenedByOther, LockedByOther, OutOfDate };
struct SVcsStatus { EVcsState State = EVcsState::Untracked; FString Owner; };   // who holds the lock or the open file
struct SVcsMove   { FString From; FString To; };

class IVcsProvider                                  // one instance per mount; every call takes a batch of absolute paths
{
public:
	virtual ~IVcsProvider () = default;
	virtual bool Detect (const FString& MountRoot) = 0;                    // claims the mount, once at start-up
	virtual bool IsBusy () const = 0;                                      // the VCS is rewriting the tree right now
	virtual SVcsResult CheckOut (std::span<const FString> Files) = 0;      // writable and opened for edit
	virtual SVcsResult Add (std::span<const FString> Files) = 0;
	virtual SVcsResult Move (std::span<const SVcsMove> Moves) = 0;         // case-only renames included
	virtual SVcsResult Delete (std::span<const FString> Files) = 0;
	virtual SVcsResult GetCommittedFacts (std::span<const FString> Files, std::vector<bool>& OutCommitted) = 0;
	virtual SVcsResult StatusOf (std::span<const FString> Files, std::vector<SVcsStatus>& Out) = 0;
	// validate --staged (added at M4). OutFiles: absolute paths, like every other call here, and only those under this
	// provider's mount, whatever form the VCS prints them in (git: relative to a top level that can sit above the mount).
	// None: empty.
	virtual SVcsResult GetStaged (std::vector<FString>& OutFiles) = 0;
	// The file watcher's hold (added at M4), narrower than IsBusy so that an open merge holds repairs but never hot reload.
	// Git: index.lock or HEAD.lock only.
	virtual bool IsRewritingTree () const { return IsBusy(); }
	// Added at M4, for what a person is shown, so that no caller downcasts or hard-codes one VCS's text. The name is what
	// `status` and the Stats panel print per mount: "None", "Git". The commands are never run, only shown and copied, since
	// FRT neither reverts nor stages: restoring File from the VCS (Git: `git checkout -- <file>`) and adding File to it (Git:
	// `git add <file>`, VCS003's hint). Empty: this provider has no such command, so the fix-it is absent, as under None.
	[[nodiscard]] virtual FStringView GetName () const = 0;
	[[nodiscard]] virtual FString GetRestoreCommand (const FString& File) const { (void)File; return {}; }
	[[nodiscard]] virtual FString GetAddCommand (const FString& File) const { (void)File; return {}; }
};

/**
 * The provider of one mount. Setting is [AssetsEditor] VcsProvider: "None", "Git", or "Auto", which tries each provider's
 * Detect and falls back to None. Spawns a process when it detects: call it on a worker. The object exists only once that
 * has returned, so a live host hands it over with CAssetRepairs::SetProvider (SLifecycleDesc::bProvidersPending).
 */
[[nodiscard]] FRT_CORE_API std::unique_ptr<IVcsProvider> MakeVcsProvider (FStringView Setting, const FString& MountRoot);
}


FRT_DECLARE_ENUM_REFLECTION(
	frt::assets::EVcsError,
	FRT_ENUM_ENTRY(frt::assets::EVcsError, None),
	FRT_ENUM_ENTRY(frt::assets::EVcsError, Busy),
	FRT_ENUM_ENTRY(frt::assets::EVcsError, ReadOnly),
	FRT_ENUM_ENTRY(frt::assets::EVcsError, LockedByOther),
	FRT_ENUM_ENTRY(frt::assets::EVcsError, NotLatest),
	FRT_ENUM_ENTRY(frt::assets::EVcsError, Offline),
	FRT_ENUM_ENTRY(frt::assets::EVcsError, Failed));

FRT_DECLARE_ENUM_REFLECTION(
	frt::assets::EVcsState,
	FRT_ENUM_ENTRY(frt::assets::EVcsState, Untracked),
	FRT_ENUM_ENTRY(frt::assets::EVcsState, Clean),
	FRT_ENUM_ENTRY(frt::assets::EVcsState, Modified),
	FRT_ENUM_ENTRY(frt::assets::EVcsState, Added),
	FRT_ENUM_ENTRY(frt::assets::EVcsState, Deleted),
	FRT_ENUM_ENTRY(frt::assets::EVcsState, Conflicted),
	FRT_ENUM_ENTRY(frt::assets::EVcsState, OpenedByOther),
	FRT_ENUM_ENTRY(frt::assets::EVcsState, LockedByOther),
	FRT_ENUM_ENTRY(frt::assets::EVcsState, OutOfDate));
