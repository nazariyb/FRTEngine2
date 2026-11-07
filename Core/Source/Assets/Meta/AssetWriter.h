#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "Core.h"
#include "CoreTypes.h"
#include "Assets/Meta/OpJournal.h"
#include "Assets/Registry/RegistryTypes.h"
#include "Assets/Vcs/VcsProvider.h"
#include "Containers/String.h"
#include "Functional/FunctionRef.h"
#include "Functional/UniqueFunction.h"
#include "Hash/StableHash.h"


/**
 * The one way an asset and its meta are written (design §4.6, §4.7, §12.9): one op at a time, journalled before the first
 * disk touch, through the VCS provider as one operation. A refused call fails the op before any byte is written (VCS002).
 *
 * One instance and one log per owner root, because a project and /Engine are separate roots. Providers are per mount, and
 * /Project and every plugin share one owner root while a plugin can be its own work tree (a submodule), so one log serves N
 * providers: Run splits each provider call by the mount of each path, and a case-only `git mv` runs in the right repository.
 * A Move whose ends have different providers is refused with VCS002.
 *
 * It makes no watcher call and is safe on a worker. ExpectWrite must be called on the watcher's owner thread, so the caller
 * does that from ForEachExpectedWrite before submitting the op. That serves verifying subscribers only: the registry
 * subscribes without verifying and sees the write as a Changed stat, so an echo costs it one folder rescan and no repair.
 *
 * What Run does with an op, in order:
 * 0. Settles the journal: an unfinished op is rolled forward (Op is refused when that fails), and anything else the log
 *    holds, a finished op or a torn tail, is emptied, because Begin appends and a reader stops at a torn tail. So a host
 *    that only calls Run never loses an op to an earlier crash. When that roll-forward changed a file, Op is not run: it
 *    was planned against the disk as it was before, so it is given back stale (SAssetWriteReport::bStale) for its caller
 *    to plan again. Two ops of one ReGuid set would otherwise run in turn, the second planned narrower from a half-done
 *    first, and its bytes would replace the full remap the first one finished.
 * 1. Refuses it without the writer lock, with a path outside this owner root's mounts, or with a Move across providers.
 *    A Move whose file is missing or whose target is taken by another file is refused here too, before anything moves.
 *    So is an op that touches a path in two roles (each path is moved away or trashed, or moved to and then written at
 *    most once, or written; folded): each step is idempotent, but a swap or chain, or a write of a file that a later
 *    step moves, would replay wrongly. A meta swap is therefore two WriteFiles, whose bytes are in hand, never Moves.
 *    A WriteFile whose frame is past the journal's limit (FitsOpJournalFrame) is refused, and so is any CopyFile until
 *    duplicate (M11) settles its replay. A WriteFile whose target no longer holds what its planner read (its Prior,
 *    OpJournal.h, looked at under the name the file has before the op), or a create's of a meta whose source is gone,
 *    makes the whole op stale: nothing is asked or logged, and its caller plans again from what is there now. Half an
 *    op is never run on the first try: the losers of one ReGuid set reference each other.
 *    Within each run of consecutive writes, metas go before sources (a crash between them then leaves an orphan meta,
 *    which pairs, rather than a metaless source, which the lifecycle would give a new identity). A WriteFile whose target
 *    already holds its bytes, and which no earlier step moves, is dropped: a file is rewritten only when its canonical
 *    bytes change. Nothing left: done, with nothing logged and no provider asked.
 * 2. CheckOut, one call per provider, of every file it writes as it is named before the op (so a meta that a Move
 *    carries and the op then rewrites is checked out at its old path) and of that file's pair (asset or meta) where the
 *    pair exists or is written too. A refusal fails the op with nothing written.
 * 3. The journal's Begin, flushed.
 * 4. The steps in order. Consecutive Moves go to the provider as one call per provider, asset and meta in one span, and
 *    consecutive Trashes as one Delete. Every step is idempotent, as OpJournal.h says, and so is the op by step 1's
 *    one-role rule, so a replay is the same code. Right before each WriteFile its Prior is checked again (the first run
 *    and a replay alike): a target that changed meanwhile, a created meta whose source is gone, is skipped with a
 *    warning and reported (bStale), never written over. A create goes in with WriteFileNew, so a meta that lands
 *    between the check and the write is kept too. No write creates a folder: one renamed or deleted since the plan is
 *    not brought back. A first run that skipped a step so fails stale when it is done, like one stale before step 2:
 *    the rest of it is written and finished, and its caller plans again from what is there now, so nothing counts it
 *    as applied.
 * 5. Add, one call per provider, of the files the op created and their pairs, leaving out what step 4 skipped.
 * 6. The journal's End, which empties it.
 * A step that fails before anything changed gives the op up (the log is emptied); one that fails after leaves the op in
 * the log, and RollForward finishes it, through the provider as well.
 *
 * Edit [31]'s TakeOwnership(Root) (the Owner.lock, then RollForward, shared by CEngine and AssetTool) is F4's, which wires
 * the hosts; since Run settles the journal itself, it is what lets a host finish an op at start-up rather than at its
 * first write, not a condition of safety.
 */
namespace frt::assets
{
class CMountTable;
class CNoneVcsProvider;

struct SAssetWriterDesc
{
	const CMountTable* Mounts = nullptr;
	std::filesystem::path OwnerRoot;
	std::vector<std::pair<MountId, IVcsProvider*>> Providers;   // as SLifecycleDesc's; absent or null: None
	bool bHoldsLock = false;       // false: every op is refused
};

// What one Run or RollForward did, beside its result: what a caller turns into VCS002 and its counters.
struct SAssetWriteReport
{
	bool bRefusedByVcs = false;     // a provider refused a call (VCS002); with bChangedDisk false nothing was written
	bool bChangedDisk = false;      // a step changed a file
	bool bLeftUnfinished = false;   // the op stays in the journal, for RollForward
	// Planned against files that have changed since: plan it again from what is there now. On a first run nothing was
	// written; a step skipped while running (a race, a replay) left its file as it was.
	bool bStale = false;
	uint32 StepsRun = 0u;
	uint32 StepsSkipped = 0u;       // already done (a replay), bytes unchanged, or stale
	std::vector<FString> Changed;          // the mount paths that made it stale (a WriteFile's target, not as planned)
	std::vector<FString> RolledForward;    // Run: the mount paths of the earlier op it finished first, for a rescan
};

class FRT_CORE_API CAssetWriter
{
public:
	explicit CAssetWriter (SAssetWriterDesc Desc);
	~CAssetWriter ();

	CAssetWriter (const CAssetWriter&) = delete;
	CAssetWriter& operator= (const CAssetWriter&) = delete;

	// An op still unfinished in the log is rolled forward first; when that fails, Op is refused, and when that changed a
	// file, Op is given back stale (bStale, RolledForward) without being run. Whatever else the log holds is emptied before
	// Begin; a log that cannot be read or emptied refuses Op.
	[[nodiscard]] SVcsResult Run (const SAssetOp& Op);
	[[nodiscard]] SVcsResult Run (const SAssetOp& Op, SAssetWriteReport& OutReport);
	// Finishes an op the log holds unfinished, through the provider as well, and empties a log that holds nothing unfinished.
	// False when one could not be finished (it stays in the log), or without the writer lock: a read-only process or a guest
	// reports an unfinished journal and never rolls it forward. A step whose file is gone by now (deleted meanwhile) is
	// skipped with a warning: no replay can bring it back, and the lifecycle reconciles what is on disk. So is a WriteFile
	// whose target no longer holds its Prior, or a created meta's whose source is gone (bStale): a meta a pull or a
	// teammate put there after the crash is theirs. The op is finished all the same.
	bool RollForward (FString* OutError = nullptr);
	bool RollForward (SAssetWriteReport& OutReport, FString* OutError = nullptr);

	// The op the log holds unfinished, if any.
	[[nodiscard]] std::optional<SOpJournalEntry> GetUnfinished () const;

	[[nodiscard]] const CMountTable* GetMounts () const noexcept { return Desc.Mounts; }
	[[nodiscard]] const std::filesystem::path& GetOwnerRoot () const noexcept { return Desc.OwnerRoot; }
	[[nodiscard]] bool HoldsLock () const noexcept { return Desc.bHoldsLock; }
	[[nodiscard]] const std::filesystem::path& GetJournalPath () const noexcept { return Journal.GetPath(); }
	// The provider of the mount that holds MountPath (None when it has none); nullptr when MountPath is in no mount of this
	// owner root.
	[[nodiscard]] IVcsProvider* FindProvider (FStringView MountPath) const;

	/**
	 * Kill-mid-command tests (design §16.2 M4): the next Run or RollForward stops once Steps steps are through, with the
	 * journal's Begin on disk and no End, as a crash there would leave it. Steps may cut a run of Moves that would be one
	 * provider call, as a crash between two renames does. Steps equal to the op's step count stops before Add and End.
	 * OnStop, when given, runs at that point instead of returning (a child process blocks there to be killed); without it
	 * Run returns Failed. ~0u turns it off.
	 */
	void SetStopAfterStepForTest (uint32 Steps, TUniqueFunction<void ()> OnStop = {});
	// The next Run calls OnRun once, after it settled the journal and before it looks at the op: what a sync does to the
	// disk between an op's plan and its run.
	void SetBeforeRunForTest (TUniqueFunction<void (const SAssetOp&)> OnRun);

private:
	struct SPlan;

	SAssetWriterDesc Desc;
	std::filesystem::path NormalizedOwnerRoot;
	std::unique_ptr<CNoneVcsProvider> NoneProvider;
	COpJournal Journal;
	uint32 StopAfterStep = ~0u;
	TUniqueFunction<void ()> OnStopForTest;
	TUniqueFunction<void (const SAssetOp&)> OnRunForTest;

	// MountPath's absolute path and provider, when it names something in a mount of this owner root.
	[[nodiscard]] bool Resolve (FStringView MountPath, std::filesystem::path& OutAbsolute, IVcsProvider*& OutProvider) const;
	[[nodiscard]] SVcsResult Prepare (const SAssetOp& Op, bool bReplay, SPlan& Out, SAssetWriteReport& Report) const;
	[[nodiscard]] SVcsResult CheckOutPlanned (const SPlan& Plan, SAssetWriteReport& Report);
	[[nodiscard]] SVcsResult Execute (const SPlan& Plan, SAssetWriteReport& Report);
	[[nodiscard]] SVcsResult StopForTest (SAssetWriteReport& Report);
	[[nodiscard]] FString GetStaleReason (const SAssetOpStep& Step, const std::filesystem::path& Target,
		const std::optional<std::filesystem::path>& Source) const;
};

// The files of Op whose bytes are in hand, to register with CFileWatcherService::ExpectWrite. A Move has none: nothing is
// hashed just to rename it.
FRT_CORE_API void ForEachExpectedWrite (const SAssetOp& Op, TFunctionRef<void (FStringView MountPath, const hash::SContentHash& Hash, uint64 Size)> Visit);
}
