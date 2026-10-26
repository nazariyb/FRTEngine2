#pragma once

#include <filesystem>
#include <memory>
#include <span>
#include <vector>

#include "Core.h"
#include "CoreTypes.h"
#include "Containers/String.h"
#include "Containers/StringMap.h"
#include "Functional/MulticastDelegate.h"
#include "Functional/UniqueFunction.h"
#include "Hash/StableHash.h"
#include "Platform/FileWatcher.h"
#include "Platform/Platform.h"


namespace frt::config
{
class CConfig;
}

namespace frt::jobs
{
class CWorkerPool;
}


/**
 * The one file watcher of dev builds (design §5.4 "The watcher", §9.1, §15 I14): every system that
 * reacts to files changing on disk - materials, input actions, shader includes, the editor's INI files,
 * later the asset registry - subscribes here instead of polling timestamps. Portable code over
 * platform::IFileWatcher (Platform/FileWatcher.h), which only reads the OS's reports; everything that
 * turns those reports into "these files changed" is here, the same on every platform.
 *
 * Level-triggered. An event never says what happened, only where to look: it marks a path dirty, and
 * when the path has settled, its state on disk now (stat, writer probe, hash for expected writes)
 * decides what subscribers are told. That is why a lost event costs a rescan, never a wrong answer,
 * and why the rules below can coalesce freely (UE's FFileCache idea, without its time thresholds).
 *
 * The rules (§5.4's table, with the measured Windows facts of gap-watch-meta §1.3 behind each):
 *	- Settling. A path is told after SettleMs (200) without events, at most MaxLatencyMs (2000) after
 *	  its first one; a removal waits RemoveGraceMs (1000) longer, so a delete and re-create - an atomic
 *	  save, git's unlink and write of every file it checks out - is one Modified.
 *	- Coalescing per path within the window: Removed then Added is Modified; Added then Removed is
 *	  nothing (a temporary file); a rename of a temporary file over a path is a Modified of that path.
 *	- Moves pair by identity: a Removed and an Added (a move between directories) or a RenamedOld and
 *	  a RenamedNew with the same file identity within PairWindowMs (150) are one Moved; without
 *	  identities, a RenamedNew pairs with the RenamedOld right before it. A case-only rename is a Moved
 *	  whose two paths differ in case only.
 *	- Directories: one event stands for a whole subtree. A directory moved in (or created, or restored
 *	  from the Recycle Bin) is enumerated with platform::EnumerateDirectory and every file in it is
 *	  Added - when its entry comes due, not when its event arrives (a burst may still be arriving, and a
 *	  Rescan makes the walk moot), once with the directories below it, and only where a subscription
 *	  watches; one renamed or moved within the root (a case-only rename included) re-paths what is
 *	  pending below it and is told as one directory Moved from where it was when the window began (a
 *	  chain a -> b -> c is one Moved from a); one removed or moved out is one directory Removed, which
 *	  stands for everything below it (UE's RemoveAssetPath leaves ghosts there) - at a, if it had moved
 *	  from a within the window - followed by the file's Added when a file took its place within the
 *	  window. Where the platform gives no entry types, the disk is asked, and a directory's own Modified
 *	  (its times, changed by an entry inside) is dropped; the disk cannot say what a removed entry was,
 *	  so a removal of unknown type is told as a directory Removed (everything at or below the path,
 *	  true of a file too) to every subscription whose region holds it or lies below it, and a rename
 *	  whose new half is a directory is a directory Moved.
 *	- Bursts. More than BurstEvents (256) events without a SettleMs pause is a VCS operation (a
 *	  checkout, a sync): everything is held until the whole tree has been quiet for VcsSettleMs
 *	  ([AssetsEditor] VcsSettleMs, 1500), then told at once, so no subscriber ever sees a half-updated
 *	  tree. A VCS provider's IsBusy (M4, §12.9) plugs in through SetVcsBusyQuery: polled every
 *	  VcsPollMs while anything is pending, and while it says busy everything is held the same way.
 *	- Overflow, or a root lost and back: events were missed, so the root is told as one Rescan once
 *	  the tree is quiet, standing for everything below it.
 *	- Writers. A file that still exists is probed with platform::ProbeFileWriters before it is told;
 *	  while a writer holds it, it waits (100 ms, doubling up to MaxProbeBackoffMs), and after
 *	  HeldWarningMs a warning names it once.
 *	- Self-writes. ExpectWrite registers the hash and size a write of the caller's own will leave; when
 *	  the path settles with exactly those bytes, nothing is told. A different hash (someone else wrote
 *	  meanwhile) is told as usual.
 *	- Fences. Flush(Directory) creates ".frt-fence-<pid>-<n>" in each root that watches the directory,
 *	  waits for each root's own fence event - events of one root arrive in order, so everything before
 *	  it has arrived - and tells everything older at once, ignoring the windows (Bitsquid's fence,
 *	  watchman's cookies). Tests, commandlets that write then read, and bulk editor operations call it.
 *	  Fence names are never told.
 *
 * Subscriptions are explicit, keyed by directory and filter: Subscribe(Directory, {Prefix,
 * Extensions}, Callback). The service arms one platform root per mount (AddMount: the engine's and
 * the project's Content, the project's Config) that holds a subscribed directory, and one per
 * subscribed directory outside every mount that no root already contains (a subscription inside a
 * root shares it; a directory subscribed before one above it keeps its own root, so roots may
 * overlap, and Flush fences all of them). Mounts keep roots few and high: on Windows an open
 * directory handle keeps every directory above it from being renamed or moved, so a root per loaded
 * file's folder would pin the folders between it and the mount. A root is disarmed when its last
 * subscription goes, and a root directory that does not exist yet is looked for every RootPollMs (it
 * is armed, and told as a Rescan, when it appears). Callbacks run on the owner thread, inside Pump or
 * Flush, at most once per call each, with the subscription's changes sorted by path; a callback may
 * subscribe, unsubscribe (itself included) and register expected writes, but not Pump or Flush.
 *
 * Verification off the owner thread (§5.4 "Verification is level-triggered", §5.9). With a worker
 * pool (SetWorkerPool: CEngine's), the disk checks that decide what a settled path is - its Stat, the
 * writer probe, the hash an expected write is compared with, and the Stat that says what an event of
 * unknown entry type was about - run on the pool, in one group of tasks per Pump; a later Pump applies
 * the group whole (a group is never told in part, so a burst's batch stays one batch), and a path that
 * changed again meanwhile is checked again. Without a pool (tests, tool hosts) they run inline, as
 * they always did. Flush stays synchronous either way: it waits for the checks of everything its
 * fences cover (resolving them inline if the pool cannot get to them before its timeout) and tells
 * them before it returns.
 *
 * Subscriptions that do not verify (SWatchFilter::bVerify false: the asset registry, which stats,
 * reads and retries files itself) are told a settled path as soon as it settles, with the kind its
 * events say, without any disk check: no writer probe (a file a writer still holds is told, and told
 * again when the writer's close reports it), no self-write check (an expected write's echo is told
 * too). Where the events cannot say what is there now - a directory removed and something back in its
 * place - they are told a Rescan of the path.
 *
 * A cap on how many settled paths one Pump resolves (SFileWatcherSettings::MaxChangesPerPump) keeps a
 * folder dropped into a mount from costing one frame everything at once; it never splits a held
 * release (a burst, an overflow, a busy VCS ending), a Flush, or the files found below one directory
 * that appeared, so no subscriber is told part of a tree.
 *
 * Owner thread: the thread that constructed the service (CEngine's main thread); every member asserts
 * it. Nothing here allocates from the engine's memory pool: std containers and FString, on the CRT
 * heap; the work on the pool uses nothing else. Dev builds only: CEngine owns one in Debug and
 * Development (Engine.h, GetFileWatcher), pumped in BeginFrame; Release builds watch nothing.
 */
namespace frt::assets
{
struct SFileWatcherSettings
{
	// [Assets] Watch (default 1): whether anything is watched. Off, subscriptions are kept but no root is
	// armed and nothing is ever told (scripted runs, which must not reload behind a capture's back).
	bool bEnabled = true;
	uint32 SettleMs = 200u;
	uint32 MaxLatencyMs = 2000u;
	uint32 RemoveGraceMs = 1000u;
	uint32 PairWindowMs = 150u;
	uint32 BurstEvents = 256u;
	// [AssetsEditor] VcsSettleMs (default 1500).
	uint32 VcsSettleMs = 1500u;
	uint32 VcsPollMs = 100u;
	uint32 ProbeBackoffMs = 100u;
	uint32 MaxProbeBackoffMs = 2000u;
	uint32 HeldWarningMs = 10000u;
	uint32 RootPollMs = 1000u;
	// How many settled paths one Pump resolves; the rest are resolved by the next ones, oldest first. A
	// held release, a Flush and the files found below one directory that appeared are never split.
	uint32 MaxChangesPerPump = 4096u;
	// For the platform watcher the service creates (buffer size, thread name, lost-root polling).
	platform::SFileWatcherDesc Watcher;

	// The defaults with [Assets] Watch and [AssetsEditor] VcsSettleMs from Config.
	[[nodiscard]] FRT_CORE_API static SFileWatcherSettings FromConfig (const config::CConfig& Config);
};

enum class EFileChange : uint8
{
	Added,      // Path exists and did not before (created, copied or moved in from outside the root)
	Modified,   // Path existed before and exists now: written, replaced by an atomic save, checked out again
	Removed,    // Path is gone (with bDirectory: everything at or below it)
	Moved,      // what was at OldPath is at Path now (with bDirectory: everything below it moved along)
	Rescan,     // events at or below Path were missed (bDirectory is set): compare what you hold with the
	            // disk and reload what differs - never reload everything blindly
};

struct SFileChange
{
	EFileChange Kind = EFileChange::Modified;
	// The change is about a directory and everything below it (Removed, Moved, Rescan of a subtree).
	// Also set when a directory record covers the subscription's whole filter: then Path is the
	// subscription's directory joined with its filter Prefix.
	bool bDirectory = false;
	// Absolute. Spelled with the subscription's own spelling of its directory, then the names as the OS
	// reported them, so a subscriber that keys files by the paths it loaded them from finds them.
	std::filesystem::path Path;
	// Moved only: where it was.
	std::filesystem::path OldPath;
	// Path relative to the subscription's directory, '/' separated, UTF-8; empty when Path is that
	// directory or lies outside it (the far side of a move).
	FString RelativePath;
};

/**
 * Which changes of a subscription's directory it is told. A file change matches when its path (or a
 * Moved's old path) is at or below Prefix and its name ends with one of Extensions; a directory
 * change matches when its subtree and Prefix overlap, whatever the extensions. A file moved across the
 * edge of the filter is, to this subscription, an Added (moved into it) or a Removed (moved out of it;
 * not told at all when another change of the old path is told in the same call, as when ReplaceFileW
 * renames the old file to a backup name and the new one into its place).
 */
struct SWatchFilter
{
	// Relative to the subscription's directory, '/' separated: a subdirectory ("Materials") or one file
	// ("EditorInput.ini"). Empty: everything below the directory.
	FString Prefix;
	// Name endings, compared ignoring case (".frtmat.yml"). Empty: any name.
	std::vector<FString> Extensions;
	// Told only what the disk confirms (the default): a file once no writer holds it, never the echo of
	// an expected write. False: told every settled path at once, with the kind its events say and no
	// disk check (see the class comment) - for a subscriber that compares what it holds with the disk
	// itself.
	bool bVerify = true;
};

using FFileChangesCallback = TUniqueFunction<void (std::span<const SFileChange>)>;

/**
 * For subscribers that keep files they loaded: whether the file at File may hold other bytes now
 * than before Change, so that its loader should look again - compare the write time it loaded with
 * the file's now, and read it only when they differ (level-triggered, as the service is). True when
 * Change's Path is File (Added, Modified, or a Moved onto it), or when Change is about a directory
 * (Rescan, a directory Moved) that File lies below. Never for Removed, nor for a Moved's OldPath:
 * the file is gone from there, and a loader keeps what it has. Paths compare as the service compares
 * them (absolute, normalized, names ignoring case).
 */
[[nodiscard]] FRT_CORE_API bool IsFileAffected (const SFileChange& Change, const std::filesystem::path& File);

/**
 * IsFileAffected for many files at once: a subscriber that keeps many files indexes a callback's
 * changes once and asks for each of its files in one hash lookup (and a prefix test per directory
 * change), instead of comparing every file with every change. The same answers as IsFileAffected.
 */
class FRT_CORE_API CAffectedFiles
{
public:
	explicit CAffectedFiles (std::span<const SFileChange> Changes);

	[[nodiscard]] bool Contains (const std::filesystem::path& File) const;
	// No change could affect a file (all of them removals).
	[[nodiscard]] bool IsEmpty () const noexcept;

private:
	FStringSet Files;                    // path keys (MakePathKey) of the files changed
	std::vector<FString> Directories;    // path keys of the directories changed: Rescan, a directory Moved
};

// Counts at the moment of the call; the totals count since construction.
struct SFileWatcherStats
{
	uint32 Roots = 0u;                // platform roots armed now
	uint32 Subscriptions = 0u;
	uint32 PendingPaths = 0u;         // marked dirty, not told yet
	uint32 VerifyingPaths = 0u;       // disk checks on the worker pool, not applied yet
	bool bHolding = false;            // a burst, an overflow or a busy VCS holds everything until quiet
	uint64 Events = 0u;               // taken from the platform watcher
	uint64 Overflows = 0u;
	uint64 RootsLost = 0u;
	uint64 Bursts = 0u;               // times everything was held for VcsSettleMs of quiet
	uint64 Rescans = 0u;              // Rescan changes made (one per root or directory)
	uint64 DirectoriesEnumerated = 0u;
	uint64 Deliveries = 0u;           // subscriber callbacks run
	uint64 Changes = 0u;              // changes handed to them, summed over callbacks
	uint64 SelfWritesSuppressed = 0u;
	uint64 WriterBackoffs = 0u;       // probes that found a writer
	uint64 VerifyTasks = 0u;          // disk-check tasks submitted to the worker pool
	uint64 CappedPumps = 0u;          // Pumps that left settled paths to the next (MaxChangesPerPump)
};

class FRT_CORE_API CFileWatcherService
{
public:
	// The calling thread becomes the owner. Watcher: the platform watcher to use (tests pass a fake);
	// null creates platform::CreateFileWatcher(Settings.Watcher) when the first root is armed, so a
	// service nobody subscribes to - or a disabled one - starts no thread and opens no handle.
	explicit CFileWatcherService (const SFileWatcherSettings& Settings = {}, std::unique_ptr<platform::IFileWatcher> Watcher = nullptr);
	// Ends every subscription (destroys the callbacks) and disarms everything. Owner thread.
	~CFileWatcherService ();

	CFileWatcherService (const CFileWatcherService&) = delete;
	CFileWatcherService& operator= (const CFileWatcherService&) = delete;

	// ---- subscriptions ----

	// Directory becomes a mount: a later subscription at or below it is watched from a root armed at
	// Directory itself, shared with every other one in it (see the class comment for why). Add mounts
	// before subscribing: a root armed already keeps its place. Directory need not exist yet.
	void AddMount (const std::filesystem::path& Directory);
	template <platform::NarrowText T>
	void AddMount (const T&) = delete;

	// Tells Callback about changes at or below Directory that Filter matches, from now on. Directory need
	// not exist yet. An invalid handle when Callback is empty. Changes made before the call are not told.
	SDelegateHandle Subscribe (const std::filesystem::path& Directory, const SWatchFilter& Filter, FFileChangesCallback Callback);
	template <platform::NarrowText T>
	SDelegateHandle Subscribe (const T&, const SWatchFilter&, FFileChangesCallback) = delete;

	// Ends a subscription: it is not called again, also not later in a delivery that is running. False
	// when the handle names no live subscription. A subscriber that outlives the service drops its
	// handle without calling this (the service's destruction ended the subscription).
	bool Unsubscribe (SDelegateHandle Handle);

	// [Assets] Watch at run time. Off: every root is disarmed and everything pending is forgotten. On:
	// the subscribed directories are armed again, watching from now (nothing that changed while it was
	// off is told). OnlyUnder, when not empty, limits watching to the roots that hold one of these
	// directories or lie below one - also those of later subscriptions - until the next call: a scripted
	// step that watches its own files, and nothing else of the checkout it runs in. Roots that leave
	// the scope are disarmed; roots that stay in it keep watching.
	void SetEnabled (bool bEnabled, std::span<const std::filesystem::path> OnlyUnder = {});
	[[nodiscard]] bool IsEnabled () const noexcept;

	// The pool the disk checks run on (see the class comment); null runs them inline on the owner
	// thread. The pool must outlive the service, or be replaced (null included) before it goes; checks
	// already submitted finish on it, or are checked again when its drain drops them.
	void SetWorkerPool (jobs::CWorkerPool* Pool);

	// ---- the frame ----

	// Takes the platform's events, applies the rules above and calls the subscribers whose changes have
	// settled. Once per frame (CEngine::BeginFrame); cheap when nothing happened.
	void Pump ();

	// The fence: creates a fence file in every armed root that watches some of Directory (one that
	// holds it, or lies below it), waits for each root's fence event, all within TimeoutSeconds, then
	// tells everything of those roots that arrived before their fences, whatever the windows (files a
	// writer still holds stay pending). A root that reports a loss (Overflow, RootLost) after its fence
	// was written may have lost the fence's event with the rest: the loss stands for it, and the Rescan
	// it marked is told at once. True when every fence was seen or stood for so; false at once when
	// Directory is not watched (no subscription, disabled, or not armed).
	bool Flush (const std::filesystem::path& Directory, double TimeoutSeconds = 5.0);
	template <platform::NarrowText T>
	bool Flush (const T&, double = 5.0) = delete;

	// ---- self-writes ----

	// The caller is about to write Path with bytes of this hash and size (hash::HashContent): when Path
	// settles holding exactly them, its change is not told. Forgotten at Deadline (a platform::
	// ReadTimestamp value, platform::MakeDeadline) or when the path settles, whichever comes first. A second call
	// for the same path replaces the first.
	void ExpectWrite (const std::filesystem::path& Path, const hash::SContentHash& Hash, uint64 Size, uint64 Deadline);
	// The same, hashing Bytes, forgotten after TimeoutSeconds.
	void ExpectWrite (const std::filesystem::path& Path, std::span<const uint8> Bytes, double TimeoutSeconds = 10.0);
	template <platform::NarrowText T>
	void ExpectWrite (const T&, const hash::SContentHash&, uint64, uint64) = delete;
	template <platform::NarrowText T>
	void ExpectWrite (const T&, std::span<const uint8>, double = 10.0) = delete;

	// ---- the VCS seam (M4) ----

	// IVcsProvider::IsBusy of the mounts' provider (§12.9): while it returns true, everything is held,
	// and after it clears, until VcsSettleMs of quiet. Null removes it (settle windows alone).
	void SetVcsBusyQuery (TUniqueFunction<bool ()> IsBusy);

	// ---- state ----

	[[nodiscard]] SFileWatcherStats GetStats () const;
	// Nothing pending, no rescan waiting and nothing held: everything that arrived has been told.
	[[nodiscard]] bool IsIdle () const;
	[[nodiscard]] const SFileWatcherSettings& GetSettings () const noexcept;

private:
	struct SState;
	std::unique_ptr<SState> State;
};
}
