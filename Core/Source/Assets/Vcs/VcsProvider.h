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
 * never by the runtime or a package; `--ci` and guests call only IsBusy, GetCommittedFacts and StatusOf.
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
	virtual SVcsResult GetStaged (std::vector<FString>& OutFiles) = 0;     // validate --staged (added at M4); None: empty
	// The file watcher's hold (added at M4), narrower than IsBusy so that an open merge holds repairs but never hot reload.
	// Git: index.lock or HEAD.lock only.
	virtual bool IsRewritingTree () const { return IsBusy(); }
};

/**
 * The provider of one mount. Setting is [AssetsEditor] VcsProvider: "None", "Git", or "Auto", which tries each provider's
 * Detect and falls back to None. Spawns a process when it detects: call it on a worker.
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
