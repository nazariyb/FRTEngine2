#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "Core.h"
#include "CoreTypes.h"
#include "Assets/Meta/LifecycleTypes.h"
#include "Assets/Meta/OpJournal.h"
#include "Assets/Meta/Safety.h"
#include "Assets/Registry/RegistryTypes.h"
#include "Assets/Vcs/VcsProvider.h"
#include "Containers/String.h"
#include "Diagnostics/Diagnostics.h"
#include "Functional/FunctionRef.h"
#include "Platform/FileLock.h"


namespace frt::jobs
{
class CWorkerPool;
}


/**
 * The path from the lifecycle's proposals to the writer (design §4.7, §12.9), named once so that the live editor, the
 * verbs and the problems panel do not each rebuild the fact gathering and the planning:
 *
 *   view + memory -> SLifecycleFacts, one per mount -> one RunLifecycle per mount (fetching what it asks for on a worker,
 *     then again) -> States and Problems as one delta -> each repair planned into an SAssetOp, except that all ReGuid
 *     actions of one result are planned into one SAssetOp (LifecycleTypes.h says why) -> CAssetWriter::Run on a worker, one
 *     op at a time per owner root -> a rescan of the touched folders
 *
 * It runs after a pass has committed, never during one, because it reads the committed view: companions are known only
 * then. A refused repair stays a proposal and is not retried until an event on that path or an explicit fix, so a
 * read-only file cannot spin. Everything else is level-triggered: an echo's scan sees the bytes the registry already
 * holds and proposes nothing.
 *
 * Where each fact comes from (Lifecycle.h's contract, edit [28]): stat, class and content hash from the source's FILES
 * row (a hash the lifecycle asks for is computed on the worker, kept in memory per stat and committed into the row);
 * RecordGuid from the record, unless the row is PendingMeta (a provisional id); bMetaOnDisk and the meta's spelling from
 * the Meta row in the same folder whose name folds to the source's + ".meta"; the GUID of an orphan or broken meta read
 * from its file on the worker (few, and nothing of them is stored); LiveGuids from every meta but one beside a file the
 * rules exclude (not an asset, so no member of a duplicate group); VerifiedCopies from METAS whose hash is the source's
 * MetaHash; tombstones from the view; bWritable from the mounts this process writes and the read-only attribute of the
 * file and its meta; every Ms (LastEventMs, MetaGoneMs, FirstSeenMs) from the time of the first Tick after the pass that
 * touched the path (never persisted: a restart counts from then, and an untracked time means the delay is over; of the
 * initial scan only the paths whose time decides something are kept);
 * bLfsPointer from the first bytes of a file under 1024 bytes, read only when it is hashed; Committed from the provider,
 * asked only when a run sets bNeedCommitted. Besides the lifecycle's own problems a run reports VCS001 for a meta whose
 * bytes hold CRLF, VCS002 for a repair the VCS refused and VCS003 for a meta created here that StatusOf calls untracked
 * beside a tracked source, all under EProblemSource::Lifecycle with the rule files' Validation: overrides applied.
 */
namespace frt::assets
{
class CAssetWriter;
class CFileWatcherService;
class CMountTable;
class CRegistryView;
class CRulesEngine;
struct SRegistryDelta;

/**
 * SAssetRegistryDesc::Lifecycle: null means no lifecycle at all (metas are read when present, a metaless file keeps its
 * provisional id, no META problems, nothing written). bRepairs false reports only (--ci, guests, read-only hosts); true
 * applies repairs on the owner.
 *
 * A mount with no provider here, or a null one, is None, with no process spawned, but only once the mount's provider is
 * settled: a duplicate resolved before detection would go by smallest path instead of committed facts and re-GUID the wrong
 * file. A tool host passes complete pointers. A live host detects on a worker (CVcsProviderSet::DetectAsync), so it sets
 * bProvidersPending and calls CAssetRepairs::SetProvider for each mount it did not list, null for None; every run holds
 * (bVcsHold) until the last call.
 *
 * bLive: the registry ticks the repairs from its Pump with the wall clock (GetLifecycleNowMs), as the editor does. Off
 * (tool hosts, and tests that pass their own clock to Tick or Settle), only RunNow, ApplyFix and explicit ticks act.
 */
struct SLifecycleDesc
{
	std::vector<std::pair<MountId, IVcsProvider*>> Providers;
	SAssetEditorSettings Settings;
	FString ProjectSalt;              // SProjectDescriptor::GetAssetSalt(), for GetMountAssetSalt; unused by an Engine-owner run
	bool bRepairs = false;
	bool bProvidersPending = false;
	bool bLive = false;
};

/**
 * What RunNow may apply. RunLifecycle proposes everything it sees, so each verb names its own Kinds, and an action of another
 * kind is reported as a repair this run cannot make (its problem, with the action as its fix-it): `migrate metas` and `new-meta` take CreateMeta alone; `fix` and `validate --fix` take CreateMeta,
 * RestoreMeta, FixMetaCase and ReGuid, never MoveMeta (a relink) or KeepAliasB. With AutoResolveDuplicates off RunLifecycle
 * makes a duplicate a fix-it rather than a repair, so that ReGuid never reaches `fix`.
 */
struct SRepairOptions
{
	std::vector<FString> Paths;              // mount paths; empty: every mount the run owns
	std::vector<ELifecycleAction> Kinds;     // empty: every kind RunLifecycle proposes
	bool bCommand = true;
	bool bDryRun = false;
};

struct SRepairReport
{
	std::vector<SLifecycleAction> Planned;
	uint32 Applied = 0u;
	uint32 Refused = 0u;
	std::vector<SDiagnostic> Problems;
};

// What the runtime holds now (Core-Test; the Stats panel may show it).
struct SAssetRepairsStats
{
	uint64 Runs = 0u;              // lifecycle runs whose result was committed
	uint32 TrackedEvents = 0u;     // paths whose event time is kept (the delays' clocks)
};

/**
 * What CAssetRepairs needs of the registry that owns it (CAssetRegistry implements it in AssetRegistry.cpp). Owner thread,
 * every member.
 */
class IAssetRepairsHost
{
public:
	virtual ~IAssetRepairsHost () = default;
	[[nodiscard]] virtual const CRegistryView& GetLiveView () const = 0;
	[[nodiscard]] virtual std::shared_ptr<const CRegistryView> AcquireView () const = 0;
	// The initial scan is complete and nothing is queued, scanning, analyzing, retrying or buffered: repairs act on the
	// final tree only then (§4.7 "a quiet, idle tick").
	[[nodiscard]] virtual bool IsQuiet () const = 0;
	// Commits Delta at once (only while quiet: nothing queued can be older than it).
	virtual void CommitNow (SRegistryDelta&& Delta) = 0;
	// Queues a scan of the folder of each path (a file's folder; a folder itself), as watcher events do.
	virtual void RequestRescan (std::span<const FString> MountPaths) = 0;
	// Scans them and waits until they are committed (CAssetRegistry::ScanPathsSynchronous).
	virtual bool ScanNow (std::span<const FString> MountPaths) = 0;
	// The registry's own pump loop (completions, commits, retries) until Done; false on the timeout (negative: none).
	virtual bool WaitUntil (TFunctionRef<bool ()> Done, double TimeoutSeconds) = 0;
	// SAssetRegistryCounters::RepairsApplied / RepairsRefused.
	virtual void CountRepairs (uint32 Applied, uint32 Refused) = 0;
};

struct SAssetRepairsDesc
{
	const SLifecycleDesc* Lifecycle = nullptr;   // not null; kept
	const CMountTable* Mounts = nullptr;
	const CRulesEngine* Rules = nullptr;
	jobs::CWorkerPool* Pool = nullptr;
	CFileWatcherService* Watcher = nullptr;      // null or disabled: no ExpectWrite, no busy query
	IAssetRepairsHost* Host = nullptr;
	// The mounts this process writes, all under OwnerRoot (Project owner: /Project and its plugins; Engine owner: /Engine).
	// Empty, or bWriter false: a reader, which reports and writes nothing.
	std::vector<MountId> OwnedMounts;
	std::filesystem::path OwnerRoot;
	bool bWriter = false;                        // the host holds OwnerRoot's Owner.lock
};

/**
 * Owned by CAssetRegistry when SAssetRegistryDesc::Lifecycle is set. Every member runs on the owner thread, because
 * CFileWatcherService::ExpectWrite must and provider spawns and flushed writes must not: fact gathering, the lifecycle
 * runs, the hashing, the planning and the writes run on a worker, one job at a time.
 *
 * Live (Tick): a run starts when a pass touched something, a delay ran out (WakeAtMs) or a VCS hold ended, and only while
 * the registry is quiet. Its result is committed when it arrives if the registry is still quiet at the revision the run
 * read; otherwise it is dropped and run again, so a result never lands on a tree it did not see. The repairs of one result
 * are one proposal set: their ops run in one worker job, after IsBusy is asked once more and the expected writes are
 * registered with the watcher. While a provider IsBusy, and for VcsSettleMs after (WaitForVcs), until the providers are
 * settled, and while the watcher holds events back (a burst, an overflow), runs report without repairing; IsBusy is polled
 * every 100 ms only while that hold keeps something pending. A fix-it is a command: no hold applies to its op.
 *
 * RunNow is for tool hosts only and blocks until it has applied. ApplyFix blocks the same way in a tool host. In a live host
 * it registers the expected writes, submits the op and returns a report with only Planned filled; the outcome arrives as the
 * next delta's problems (VCS002 when refused, the fixed problem gone otherwise) and in the registry's counters.
 */
class FRT_CORE_API CAssetRepairs
{
public:
	explicit CAssetRepairs (SAssetRepairsDesc Desc);
	~CAssetRepairs ();
	CAssetRepairs (const CAssetRepairs&) = delete;
	CAssetRepairs& operator= (const CAssetRepairs&) = delete;

	// The registry's start, before its first scan commits: an operation the writer's journal holds unfinished is rolled
	// forward through the providers known now (TakeOwnership did it already when the host called it).
	void Start ();
	// The registry's initial scan is complete: the first run covers every row not Paired (a warm start too).
	void OnInitialScanComplete ();
	// Waits for the job in flight and leaves the watcher's busy query unset. Before the registry's own shut-down waits.
	void ShutDown ();

	// A live host's detection finishing (SLifecycleDesc::bProvidersPending); Provider is null for None.
	void SetProvider (MountId Mount, IVcsProvider* Provider);
	// Live: after a pass has committed, never during one. Dirty: the mount paths it touched. bInitial (a pass of the initial
	// scan): only the paths whose arrival time a run needs are kept, not every Paired file of the tree.
	void OnPassCommitted (std::span<const FString> Dirty, bool bInitial = false);
	// A pass read changed rule files, or re-evaluated a mount (its rules changed): Safety switches and Validation severities
	// may differ, though no file did, so the lifecycle runs again.
	void OnRulesChanged ();
	// Live: acts once the last result's WakeAtMs has passed, or when the VCS hold (polled while anything is pending) has
	// ended. A test passes its own clock.
	void Tick (uint64 NowMs);
	// Ticks at NowMs and waits until nothing more is due at that time: no run or op in flight, the rescans committed, no run
	// asked for. For tests and tool hosts that drive the live path with their own clock. False on the timeout.
	bool Settle (uint64 NowMs, double TimeoutSeconds = 30.0);
	// AssetTool verbs: synchronous, with no delays.
	[[nodiscard]] SRepairReport RunNow (const SRepairOptions& Options);
	// Fix-its are not stored on a diagnostic: the panel and `validate --fix` ask here, and apply with ApplyFix.
	[[nodiscard]] std::vector<SLifecycleAction> GetFixIts (const SDiagnostic& Problem) const;
	[[nodiscard]] SRepairReport ApplyFix (const SLifecycleAction& FixIt);

	// A run or an op is in flight on a worker.
	[[nodiscard]] bool IsWorking () const noexcept;
	// The writer of the owner root (null for a reader): Core-Test's crash points.
	[[nodiscard]] CAssetWriter* GetWriter () noexcept;
	// The provider a mount uses now (null: None, or not settled yet). `status` and the Stats panel name it per mount.
	[[nodiscard]] IVcsProvider* GetProvider (MountId Mount) const noexcept;
	[[nodiscard]] SAssetRepairsStats GetStats () const noexcept;

	struct SImpl;

private:
	std::unique_ptr<SImpl> Impl;
};

// The lifecycle's clock: wall-clock milliseconds since the Unix epoch (LifecycleTypes.h's units).
[[nodiscard]] FRT_CORE_API uint64 GetLifecycleNowMs () noexcept;

/**
 * Plans Actions into one op: a single action, or every ReGuid of one result together (LifecycleTypes.h says why). With
 * DerivedAutoGuids off an action's Guid is invalid: a ReGuid gets SGuid::NewRandom, and every stand-in in every Remap of
 * the set (the derived value each action's first pair names) is replaced by what was minted for it; a CreateMeta gets its
 * random GUID from PlanCreateMeta. Reads what it must (the metas a ReGuid rewrites, the METAS copy a restore writes, the
 * folder listings), so it runs on a worker. False, with the reasons in OutProblems, when nothing could be planned.
 *
 *   CreateMeta    PlanCreateMeta (never over an existing meta in any spelling)
 *   RestoreMeta   WriteFile of the verified METAS copy, byte for byte, where no meta exists
 *   MoveMeta      Move of the orphan meta onto the new source's meta path (a relink): its bytes stay as they are
 *   FixMetaCase   Move of the meta to its source's spelling (a case-only rename: `git mv` for a tracked meta)
 *   ReGuid        WriteFile of each loser's meta with its new GUID, the map applied to its Aliases and to every `$asset`
 *                 under SubAssets (Remap), emitted canonically; a loser whose meta no longer parses is left out
 *   KeepAliasB    WriteFile of the broken meta as the METAS copy of the kept GUID (or a minimal meta without one), with
 *                 the other GUIDs in its Aliases
 */
[[nodiscard]] FRT_CORE_API bool PlanRepairOp (const CAssetWriter& Writer, const CRegistryView& View, std::span<const SLifecycleAction> Actions,
	SAssetOp& OutOp, std::vector<SDiagnostic>& OutProblems);

// Applies Map (old GUID -> new GUID) to a meta: its Aliases, and the `$asset` of every reference under SubAssets. True when
// anything changed. In M4 the map reaches metas only; documents' `$asset` and `base:` arrive with M5 (edit [73]).
FRT_CORE_API bool ApplyGuidRemap (SAssetMeta& Meta, std::span<const std::pair<SGuid, SGuid>> Map);

/**
 * Edit [31]'s TakeOwnership, shared by CEngine and AssetTool: the owner root's Owner.lock, then the roll-forward of an
 * operation its journal holds unfinished, through Providers (the mounts of Root; absent or null: None), before a registry
 * exists and so before its first scan. A process that does not get the lock rolls nothing forward: a reader or a guest
 * reports an unfinished journal instead (bUnfinished).
 */
struct SOwnership
{
	std::optional<platform::CExclusiveFileLock> Lock;
	FString Owner;               // who holds it, when another process does (SLockOwner::Describe)
	FString Error;               // any other failure: of the lock, or of the roll-forward
	bool bRolledForward = false; // an unfinished operation was finished
	bool bUnfinished = false;    // one is left in the journal (not owned, or the roll-forward failed)
};

[[nodiscard]] FRT_CORE_API SOwnership TakeOwnership (const std::filesystem::path& Root, const CMountTable& Mounts,
	std::span<const std::pair<MountId, IVcsProvider*>> Providers);
}
