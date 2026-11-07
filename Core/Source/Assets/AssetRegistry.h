#pragma once

#include <filesystem>
#include <memory>
#include <span>
#include <vector>

#include "Core.h"
#include "CoreTypes.h"
#include "Assets/AssetId.h"
#include "Assets/Discovery/Mounts.h"
#include "Assets/Pipeline/Rules.h"
#include "Assets/Registry/RegistryStore.h"
#include "Assets/Registry/RegistryTypes.h"
#include "Assets/Registry/RegistryView.h"
#include "Assets/Registry/Snapshot.h"
#include "Containers/String.h"
#include "Functional/FunctionRef.h"
#include "Functional/MulticastDelegate.h"


namespace frt::jobs
{
class CWorkerPool;
}


/**
 * The asset registry as a service (design §5.1, §5.4, §5.9; M3 "registry v0, read-only"): CRegistryStore (the tables,
 * Registry/RegistryStore.h), the process's CMountTable and a CRulesEngine, kept current from the disk - snapshots
 * loaded at start-up, a stat scan per mount, Analyze for what changed, the file watcher's reports afterwards - and
 * persisted by the one process that may write (snapshot, journal, Rules.cache, ProjectId). CEngine owns one in dev
 * hosts (CEngine::GetAssetRegistry; the editor and the dev Launcher, headless too); AssetTool builds its own through
 * this class. Never reachable through SGameContext (the game ABI does not change).
 *
 * The pipeline per scan (a "pass" of one mount, or of one subtree of it):
 *	1. CMountScanner lists the subtree on the worker pool: nothing is opened or hashed. The start-up's scans run
 *	   while the snapshots load.
 *	2. The batch becomes deltas of at most 256 files (or folders) each on a worker (ConvertBatch in the .cpp: the
 *	   removals first, then new folders, then the files in batch order), so that a commit fits a frame's budget
 *	   whatever the subtree's size, each file compared with the view by the scan's Unchanged test (ClassifyKnown,
 *	   ScanTypes.h; byte-exact - done here rather than in the scan, which gets no IKnownFiles, so that the initial
 *	   scans need not wait for the loaded view): folders, changed file rows (ContentHash kept for
 *	   an identity-only change; a file analyzed below that has a record gets its row with its Analyze result
 *	   instead, so nothing persisted pairs a new stat with the old record), removals (known files and folders at
 *	   or below the root that the batch does not hold, outside its failed folders), the Scan and Rules problem
 *	   lists of the subtree (with the rule files' Validation: severities); the RULEFILES rows the batch changed are
 *	   held for step 4. Each file is then one of: analyzed (new, changed, no record, StaleTags, another importer or
 *	   AnalyzeStamp - which covers the folder's Types filter and Validation overrides too, since a record holds its
 *	   sub-entries and problems filtered), re-evaluated in memory (its folder's rule chain changed: committed again
 *	   from the view when its cook rule, chunk or labels changed - never read from disk), or left alone.
 *	3. Analyze runs on workers, a few files per task (AnalyzeFile, the Types filter, Validation, AppendRecordUpsert):
 *	   one delta per task. A file that changed while it was read is retried after the watcher's settle window, three
 *	   times, then committed AnalyzeFailed with a code-0 problem saying so, so a wait always ends.
 *	4. Once every delta of the pass is committed, one last delta: the pass's RULEFILES rows (the mount's
 *	   Config/Assets.yml row included) and RULE002 recomputed for its folders on a worker (companions are known by
 *	   then) under EProblemSource::Unclaimed. The rows go in only when nothing of the pass was dropped (ShutDown cancels
 *	   Analyze work): a pass cut short leaves the old rows, and the next start evaluates that interval again. A commit
 *	   that switches the companion status of a file outside the pass's subtree (a glTF naming a buffer elsewhere)
 *	   queues a pass of that file's folder, which recomputes its RULE002.
 * A file's rule chain counts as changed when its folder or an ancestor has a RulesHash in the batch other than its
 * RULEFILES row (absent = 0, so a rule file added or deleted offline is a change), when its mount's Config/Assets.yml
 * changed (or, for every mount but /Engine, the engine's), or when its snapshot file lost its RULEFILES rows (another
 * RulesBasis: a rebuilt Core.dll with other importer semantics, an edited engine fallback).
 *
 * Threading. One owner thread - the thread that constructs it, which owns the worker pool: the engine's main thread,
 * AssetTool's, a test's. Workers never touch the store: they produce deltas, which the owner commits in Pump (until
 * FrameBudgetMs of commits per call interactively, checked between deltas) or inside the unbounded waits
 * (WaitForInitialScan, ScanPathsSynchronous, Flush). Commits follow the pool's commit order (jobs::ECommitOrder: completion order
 * interactively, strict sequence where a tool sets it), with one pass per mount at a time, so no result can land
 * after a newer one of the same file. The pool's completions and the watcher's callbacks only queue work. Other
 * threads Acquire views and never wait for a commit.
 *
 * Writes (§5.9). Whether the registry writes is decided only by SAssetRegistryDesc::bWriter, which the host sets
 * exactly when it holds the owner root's Owner.lock (ClaimAssetOwnership, Assets/Meta/AssetRepairs.h: the editor through
 * CEngine with SEngineDesc::bAssetWriter; AssetTool: a verb that took the lock; a read-only guest is a reader); the
 * registry never takes a lock and never decides
 * from config. A writer writes its snapshot file and journal (Project: AssetRegistry.snapshot of /Project and its
 * plugins; Engine: Engine.snapshot of /Engine), Rules.cache and ProjectId under its owner root; a reader writes
 * nothing at all - no journal (opening one writes), no ProjectId (ReadProjectId: a root without one starts cold), no
 * Rules.cache, no watcher fences (a writer's Flush writes fence files into its own mounts). A Project owner never writes
 * Engine.snapshot or a journal of /Engine, fences included, even in engine-owner mode (bEngineMetasWritable, which lets
 * the lifecycle repair /Engine's metas and nothing else): engine files changed since the last `FRT.bat assets scan
 * --engine` are analyzed in memory at every start (logged once).
 * Snapshots are written from an acquired view on a worker after a batch of SnapshotBatchRecords records, after
 * SnapshotIdleSeconds without a commit, and at ShutDown (clean exit).
 *
 * The meta lifecycle (M4, §4.7). With SAssetRegistryDesc::Lifecycle the registry owns a CAssetRepairs (GetRepairs):
 * its Start rolls an unfinished operation of the owner root's OpJournal forward before the first scan; every finished
 * pass hands it the paths it touched, and Pump ticks it (SLifecycleDesc::bLive), so the lifecycle runs on a worker
 * against the committed view whenever the registry is quiet, commits its states and problems (EProblemSource::Lifecycle)
 * and, for a writer with bRepairs, applies its repairs through a CAssetWriter on the owner root and rescans what they
 * touched. Only the mounts the writer owns are repaired (Project: /Project and its plugins, and /Engine too through a
 * writer of the engine root with SAssetRegistryDesc::bEngineMetasWritable; Engine: /Engine); every mount is reported.
 *
 * Change notification (§5.9). Panels poll GetRevision() once per frame and re-run their queries when it moved;
 * ForEachChangeSince walks the store's change ring. Engine systems may subscribe to OnAssetsChanged, which fires once
 * per Pump whenever the revision moved - commits, a snapshot adoption and table compaction alike - with the record
 * changes (bRefreshAll when the ring cannot say: an image, a compaction, an overflow). Nobody keeps an SRecordId
 * across revisions.
 *
 * Nothing here allocates from the engine's memory pool: std containers and FString.
 */
namespace frt::assets
{
class CAssetRepairs;
class CFileWatcherService;
class CRegistryJournal;
struct SLifecycleDesc;

// Which owner root's files a writer writes, and which mounts it holds.
enum class EAssetRegistryOwner : uint8
{
	Project,   // /Engine, /Project and the plugins; writes AssetRegistry.snapshot (+ journal), Rules.cache and ProjectId
	           // under the project; reads Engine.snapshot and never writes or journals /Engine
	Engine,    // /Engine alone, no project (`FRT.bat assets scan --engine`, a subset of M4's engine-owner mode); writes
	           // Engine.snapshot (+ journal) under <engine>/Local/Registry, ProjectId and Rules.cache under <engine>/Local/Assets
};

enum class EScanState : uint8
{
	NotStarted,
	LoadingSnapshots,   // the snapshots are read and adopted on a worker
	Scanning,           // the initial scan of some mount is not committed yet
	Ready,              // the initial scan is complete: every mount scanned, analyzed, RULE002 known
	ShutDown,
};

struct SAssetRegistryDesc
{
	// The mounts (CMountTable::Resolve); the registry keeps the pointer. Engine owner: a table without a project.
	std::shared_ptr<const CMountTable> Mounts;
	// Scans, conversions, Analyze and snapshot writes run here; its owner thread is the registry's.
	jobs::CWorkerPool* Pool = nullptr;
	EAssetRegistryOwner Owner = EAssetRegistryOwner::Project;
	// The host holds the owner root's Owner.lock (see the class comment): only then is anything written.
	bool bWriter = false;
	// Engine-owner mode of a Project-owner writer (§4.7, ClaimAssetOwnership in Assets/Meta/AssetRepairs.h): the host holds
	// <engine>/Local/Assets/Owner.lock too, so the lifecycle repairs metas in /Engine through a second CAssetWriter on the
	// engine root. Nothing more: no Engine.snapshot, no journal of /Engine, no fences there (only --engine runs, Owner
	// Engine, write those). Ignored without bWriter, without Lifecycle and for an Engine owner.
	bool bEngineMetasWritable = false;
	// The file watcher to subscribe to (every mount's content root and rule-file directory); null or disabled: none.
	CFileWatcherService* Watcher = nullptr;
	// Default (empty Target) in every host, so the editor, headless hosts and AssetTool share one RulesBasis. Tests
	// change the semantics hash with a Target.
	SRulesOptions RulesOptions;
	// Load the snapshot files at start-up (false: a cold start, as if there were none; tests).
	bool bLoadSnapshots = true;
	uint32 SnapshotBatchRecords = 1000u;
	double SnapshotIdleSeconds = 60.0;
	// A writer writes its snapshot at ShutDown (false: what was committed stays in the journal alone, as after a
	// crash; tests of the journal).
	bool bSnapshotAtShutDown = true;
	// Commits per Pump interactively; the waits commit without a limit.
	double FrameBudgetMs = 2.0;
	// bSourceChanged retries (after RetryDelayMs each) before a file is committed AnalyzeFailed.
	uint32 SourceChangedRetries = 3u;
	uint32 RetryDelayMs = 200u;
	// Files per Analyze task (one delta each).
	uint32 AnalyzeBatchFiles = 16u;
	// The meta lifecycle (Assets/Meta/AssetRepairs.h); the registry keeps the pointer and owns a CAssetRepairs for it
	// (GetRepairs). Null: none, nothing is reported or written (tests and benchmarks that predate it). Every host passes
	// one: CEngine's live, repairing for the writer and reporting otherwise; each AssetTool run its own (RegistryRun.h).
	const SLifecycleDesc* Lifecycle = nullptr;
};

// What the registry did since construction.
struct SAssetRegistryCounters
{
	uint64 Analyzed = 0u;       // AnalyzeFile calls (retries included)
	uint64 Hashed = 0u;         // content hashes computed (the importers that read a whole file hash it in that pass)
	uint64 ReEvaluated = 0u;    // records whose rule outputs were resolved again without Analyze
	uint64 MetasRead = 0u;      // metas whose bytes a conversion read (changed ones, and those of files worked on)
	uint64 Committed = 0u;      // deltas committed
	uint64 Scanned = 0u;        // files listed by scans
	uint64 Passes = 0u;         // scans completed (whole mounts and subtrees)
	uint64 SnapshotsWritten = 0u;
	uint64 Compactions = 0u;
	uint64 RepairsApplied = 0u; // lifecycle repairs written (an action each: a meta created, restored, moved, re-GUIDed)
	uint64 RepairsRefused = 0u; // lifecycle repairs the VCS or the writer refused (VCS002): kept as proposals
	double CommitMs = 0.0;      // owner-thread time of the commits, journal appends included
};

// One record of an SAssetChangeSet: its id and mount path ("#label" for a sub-entry) when the change was made.
struct SAssetChange
{
	SAssetId Id;
	FString MountPath;
};

/**
 * What changed since the last OnAssetsChanged (§5.9). Added, Removed and Modified come from the store's change ring
 * (asset records; a sub-entry's change is its record's); a record whose id changed in place (its file's meta appeared,
 * a GUID edited) is a Removed of the old id and an Added of the new one. Moved: an id that left a path it held before
 * the window and holds, at the end of it, a path it did not hold (from M4 the meta's GUID, so a file renamed or moved
 * with its meta), listed once with its new path and in neither Added nor Removed (AddRecordChanges); a record added and
 * removed within one window, a path that went and came back, and a file without a meta (its id is path-derived) stay
 * Added and Removed. The store does not tell tag and edge edits apart from other edits, so TagsChanged and
 * DependenciesChanged stay empty and every edit is Modified. bRefreshAll: the ring cannot list the changes (a snapshot
 * adopted, tables compacted, the ring overflowed): re-run every query. bScanCompleted: the initial scan completed in
 * this Pump.
 */
struct SAssetChangeSet
{
	std::vector<SAssetChange> Added;
	std::vector<SAssetChange> Removed;
	std::vector<SAssetChange> Moved;
	std::vector<SAssetChange> Modified;
	std::vector<SAssetChange> TagsChanged;
	std::vector<SAssetChange> DependenciesChanged;
	bool bRefreshAll = false;
	bool bScanCompleted = false;
	uint64 Revision = 0u;       // the revision the set brings a listener to

	[[nodiscard]] bool IsEmpty () const noexcept
	{
		return Added.empty() && Removed.empty() && Moved.empty() && Modified.empty() && TagsChanged.empty() && DependenciesChanged.empty()
			&& !bRefreshAll && !bScanCompleted;
	}
};

// Appends the change ring's entries of one broadcast window (oldest first, CRegistryStore::ForEachChangeSince) to Out's
// Added, Removed, Modified and Moved, as SAssetChangeSet says: per id, the k-th path it left (by its last Removed) and
// the k-th path it came to (by its first Added) are one move.
FRT_CORE_API void AddRecordChanges (std::span<const SRecordChange> Changes, SAssetChangeSet& Out);

// The start-up as the hosts log it (`[assets] initial scan: ...`).
struct SInitialScanReport
{
	bool bComplete = false;
	uint32 Files = 0u;            // FILES rows when it completed
	uint32 Records = 0u;          // asset records when it completed
	double LoadMs = 0.0;          // snapshot read + adoption (on a worker)
	double ScanMs = 0.0;          // the mounts' stat scans (workers), summed
	double ConvertMs = 0.0;       // batches made deltas (workers), summed
	double CommitMs = 0.0;        // commits and journal appends (the owner thread)
	double UnclaimedMs = 0.0;     // RULE002 recomputed (workers, summed; skipped where no file, record or rule changed)
	double TotalMs = 0.0;         // Start to complete
	uint64 Analyzed = 0u;         // during the initial scan
	uint64 Hashed = 0u;
	uint64 ReEvaluated = 0u;
	uint64 EngineAnalyzed = 0u;   // of Analyzed, files of /Engine (Project owner: Engine.snapshot is behind the disk)
};

class FRT_CORE_API CAssetRegistry
{
public:
	explicit CAssetRegistry (SAssetRegistryDesc Desc);
	// ShutDown must have run (asserted): the destructor does not wait for work.
	~CAssetRegistry ();
	CAssetRegistry (const CAssetRegistry&) = delete;
	CAssetRegistry& operator= (const CAssetRegistry&) = delete;

	/**
	 * Starts the work and returns at once (§5.4): the mount rule files read, the watcher subscriptions made (their
	 * changes buffered until the initial scan is complete, then replayed), the snapshots read and adopted on a worker,
	 * then a scan of every mount. Pump or a wait carries it on. Owner thread; once.
	 */
	void Start ();
	/**
	 * Ends the work (§5.9 clean exit): unsubscribes from the watcher, cancels queued Analyze work, waits for the scans
	 * and the snapshot load that cannot be cancelled, commits what arrived, writes the exit snapshot (a writer) and
	 * closes the journal. Owner thread; before the worker pool's drain, which would drop the snapshot write unrun.
	 */
	void ShutDown ();

	// Commits what the workers produced, at most FrameBudgetMs of it, then fires OnAssetsChanged. Once per frame
	// (CEngine::BeginFrame, right after the worker completions).
	void Pump ();

	/**
	 * Waits until the initial scan is complete - the registry's own pump loop (pool completions, commits without a
	 * limit, retry deadlines), never a bare CWorkerPool::PumpUntil, which neither commits nor waits for a retry timer.
	 * False when TimeoutSeconds (negative: none) ran out first.
	 */
	bool WaitForInitialScan (double TimeoutSeconds = -1.0);
	/**
	 * What the hosts call before their first BuildWorld (§5.4 policies: game code never sees "unknown"): waits for the
	 * initial scan when it is not complete, with a progress line while it waits, then logs
	 * `[assets] initial scan: <files> files, <records> records, <ms> ms; BuildWorld waited <ms> ms (analyzed <a>,
	 * hashed <h>)`. Returns the time it waited, in milliseconds.
	 */
	double WaitBeforeBuildWorld ();
	[[nodiscard]] bool IsInitialScanComplete () const noexcept;
	[[nodiscard]] EScanState GetScanState () const noexcept;
	[[nodiscard]] const SInitialScanReport& GetInitialScanReport () const noexcept;
	// The snapshot files as the start-up loaded them (empty before the load completed, and for a cold start).
	[[nodiscard]] std::span<const SSnapshotFileReport> GetSnapshotReports () const noexcept;

	/**
	 * Scans the subtrees at MountPaths (folders or files, as spelled on disk; a file scans its folder) and waits until
	 * they are committed with their Analyze results (§5.4: tools and tests). False on a timeout or a path in no
	 * mount. Waits for the initial scan first.
	 */
	bool ScanPathsSynchronous (std::span<const FString> MountPaths, double TimeoutSeconds = -1.0);
	/**
	 * Everything the disk did before the call is in the registry when it returns true: a writer fences the watcher
	 * for the mounts it owns (CFileWatcherService::Flush of their content and rule-file directories, which writes
	 * fence files there: /Project and its plugins for a Project owner, never /Engine; /Engine for an Engine owner),
	 * then every registry waits until nothing is pending. Of the mounts it does not fence - all of them for a reader -
	 * a registry only waits for what the watcher reported already.
	 */
	bool Flush (double TimeoutSeconds = 30.0);
	// Nothing queued, scanning, analyzing, retrying or buffered: everything reported has been committed.
	[[nodiscard]] bool IsIdle () const noexcept;
	// Writes the snapshot now and waits for it (a writer; false for a reader or a failed write). Tests and AssetTool.
	bool WriteSnapshotNow ();

	// ---- Reads (§5.7) ----
	// The current state; owner thread, valid until the next commit (an empty view while the snapshots load).
	[[nodiscard]] const CRegistryView& Live () const noexcept;
	// The last published view; any thread.
	[[nodiscard]] std::shared_ptr<const CRegistryView> Acquire () const noexcept;
	// The revision of the last commit (any thread; what panels poll once per frame).
	[[nodiscard]] uint64 GetRevision () const noexcept;
	// The store's change ring (false: refresh everything). Owner thread.
	bool ForEachChangeSince (uint64 Revision, TFunctionRef<void (const SRecordChange&)> Visit) const;
	// EExists::Unknown until the initial scan is complete, then the view's answer (§5.7's tri-state).
	[[nodiscard]] EExists Exists (const SAssetId& Id) const;
	// Fires once per Pump (and per round of a wait) whenever the revision moved; owner thread.
	[[nodiscard]] TMulticastDelegate<const SAssetChangeSet&>& OnAssetsChanged () noexcept;

	[[nodiscard]] const SAssetRegistryCounters& GetCounters () const noexcept;
	// The meta lifecycle's runtime (SAssetRegistryDesc::Lifecycle), null without one. Owner thread.
	[[nodiscard]] CAssetRepairs* GetRepairs () noexcept;
	[[nodiscard]] const CMountTable& GetMounts () const noexcept;
	[[nodiscard]] const CRulesEngine& GetRules () const noexcept;
	[[nodiscard]] bool IsWriter () const noexcept;
	[[nodiscard]] EAssetRegistryOwner GetOwner () const noexcept;
	// The snapshot file this registry writes as a writer (and reads as anyone).
	[[nodiscard]] const std::filesystem::path& GetSnapshotPath () const noexcept;

	struct SImpl;

private:
	// Core-Test: breaks the journal as a failed write would; makes a file keep changing while it is analyzed, or a meta
	// unreadable.
	friend struct SAssetRegistryTestAccess;
	// The writer's journal while it is open (null for a reader, and before the snapshots loaded or after ShutDown).
	[[nodiscard]] CRegistryJournal* GetJournal () noexcept;
	// Every Analyze of the file at MountPath reports bSourceChanged, as for a file written to without pause (the
	// retry cap that ends every wait); for a meta's path, every read of the meta changed while read. Owner thread,
	// before Start.
	void SetSourceChangedForTest (FString MountPath);
	// Every read of the meta at MountPath fails as for a file that cannot be opened (no access, an I/O error). Owner
	// thread, before Start.
	void SetMetaUnreadableForTest (FString MountPath);

	std::unique_ptr<SImpl> Impl;
};
}


FRT_DECLARE_ENUM_REFLECTION(
	frt::assets::EAssetRegistryOwner,
	FRT_ENUM_ENTRY(frt::assets::EAssetRegistryOwner, Project),
	FRT_ENUM_ENTRY(frt::assets::EAssetRegistryOwner, Engine));

FRT_DECLARE_ENUM_REFLECTION(
	frt::assets::EScanState,
	FRT_ENUM_ENTRY(frt::assets::EScanState, NotStarted),
	FRT_ENUM_ENTRY(frt::assets::EScanState, LoadingSnapshots),
	FRT_ENUM_ENTRY(frt::assets::EScanState, Scanning),
	FRT_ENUM_ENTRY(frt::assets::EScanState, Ready),
	FRT_ENUM_ENTRY(frt::assets::EScanState, ShutDown));
