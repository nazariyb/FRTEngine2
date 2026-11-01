#include "Assets/FileWatcherService.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "Asserts.h"
#include "Assets/FileHash.h"
#include "Config/Config.h"
#include "Containers/StringMap.h"
#include "Jobs/WorkerPool.h"
#include "Log/Log.h"
#include "Platform/FileSystem.h"
#include "Platform/Process.h"
#include "Platform/Thread.h"
#include "Platform/Timestamp.h"
#include "Profiler/CpuProfiler.h"


namespace frt::assets
{
namespace
{
constexpr FStringLiteral LogCategory = "watch";

// Flush's fence files: never told to anyone, wherever they appear.
constexpr FStringLiteral FencePrefix = ".frt-fence-";

// A subscribed directory that does not exist is looked for every RootPollMs, on the owner thread; when
// asking takes longer than this (a share whose server is gone answers after its timeout), the next
// look waits twice as long, up to MaxMissingPollMs.
constexpr uint32 SlowStatMs = 50u;
constexpr uint32 MaxMissingPollMs = 30000u;

// Ticks from Earlier to Later; 0 when Later is not after it (timestamps of events taken in a batch may
// be older than the pump's "now", never the other way round, but nothing here may underflow).
uint64 Elapsed (uint64 Earlier, uint64 Later) noexcept
{
	return Later > Earlier ? Later - Earlier : 0u;
}

FString JoinPath (FStringView Parent, FStringView Child)
{
	if (Parent.IsEmpty())
	{
		return FString(Child);
	}
	if (Child.IsEmpty())
	{
		return FString(Parent);
	}
	FString path(Parent);
	path.AppendChar('/');
	path.Append(Child);
	return path;
}

FStringView GetName (FStringView Path)
{
	const uint32 slash = Path.FindLast('/');
	return slash == FStringView::NotFound ? Path : Path.RightChop(slash + 1u);
}

// How many names a root-relative path has ("a/b/c": 3, "": 0).
uint32 CountNames (FStringView Path)
{
	return Path.IsEmpty() ? 0u : Path.CountOccurrences("/") + 1u;
}

// Path without its first Count names ("a/b/c", 1: "b/c"); empty when it has no more. Paths and their
// keys (folded or not) have the same names in the same places, so this cuts both the same way.
FStringView DropNames (FStringView Path, uint32 Count)
{
	uint32 start = 0u;
	for (uint32 i = 0u; i < Count; ++i)
	{
		const uint32 slash = Path.Find('/', start);
		if (slash == FStringView::NotFound)
		{
			return {};
		}
		start = slash + 1u;
	}
	return Path.RightChop(start);
}

// Key is Prefix or a path below it (both root-relative keys); an empty Prefix holds everything.
bool IsAtOrBelow (FStringView Key, FStringView Prefix)
{
	if (Prefix.IsEmpty())
	{
		return true;
	}
	return Key.StartsWith(Prefix) && (Key.Len() == Prefix.Len() || Key.Get(Prefix.Len()) == '/');
}

bool IsFence (FStringView Path)
{
	return GetName(Path).StartsWith(FStringView(FencePrefix));
}

// The expected-write table's key: the absolute path, generic, as NTFS compares names (MakePathKey).
FString MakeExpectedKey (const std::filesystem::path& Path)
{
	return MakePathKey(FString::FromPath(platform::NormalizePath(Path), EPathStyle::Generic));
}

// Disk checks of one verification task: enough paths that a task outweighs its scheduling, few enough
// that a burst's group spreads over the workers.
constexpr size_t VerifyChunk = 64u;

// How long a Flush waits for the pool to check what its fences cover before it checks inline: long
// enough for an idle pool, short against a pool busy with a scan or a burst of decodes.
constexpr double FlushPoolWaitSeconds = 0.1;

// What the disk says about one path, now: what a settled entry is decided by (Resolve's facts).
struct SDiskFacts
{
	std::optional<platform::SFileStat> Stat;
	platform::EWriterProbe Probe = platform::EWriterProbe::Failed;
	std::optional<hash::SContentHash> Hash;
	bool bProbed = false;
	bool bHashed = false;
};

// One path's disk checks: what to look at (filled on the owner thread) and what was found (filled by
// whoever runs the checks: a worker, or the owner thread without a pool).
struct SVerifyItem
{
	uint64 RootSerial = 0u;
	FString Key;
	FString Path;                       // the entry's path when submitted
	uint64 LastSeen = 0u;               // the entry's when submitted: a later event makes the facts stale
	std::filesystem::path Full;
	bool bProbeIfFile = false;          // a file there is probed for writers
	std::optional<uint64> HashIfSize;   // an expected write's size: a file of that size there is hashed
	SDiskFacts Facts;
};

// Stat, writer probe and (for an expected write of that size) hash, in Resolve's order: a file a
// writer holds, or that went since the stat, is not hashed. Runs on any thread: std, FString and the
// platform layer only.
void GatherFacts (SVerifyItem& Item)
{
	SDiskFacts& facts = Item.Facts;
	facts.Stat = platform::Stat(Item.Full);
	if (!Item.bProbeIfFile || !facts.Stat || facts.Stat->bDirectory)
	{
		return;
	}
	facts.Probe = platform::ProbeFileWriters(Item.Full);
	facts.bProbed = true;
	if (Item.HashIfSize && *Item.HashIfSize == facts.Stat->Size
		&& facts.Probe != platform::EWriterProbe::Held && facts.Probe != platform::EWriterProbe::Missing)
	{
		facts.Hash = HashFileStable(Item.Full);
		facts.bHashed = true;
	}
}

/**
 * The disk checks of one Pump (or one Flush, or one root's events of unknown type), shared between the
 * service and the pool tasks that run them in chunks. A chunk's task finishes its chunk when it is
 * destroyed, run or not: a task the pool drops unrun (its drain) marks the group abandoned, and the
 * service checks those paths again instead of waiting forever.
 */
struct SVerifyGroup
{
	uint64 Id = 0u;
	std::vector<SVerifyItem> Items;

	void Start (uint32 Chunks)
	{
		std::lock_guard lock(Mutex);
		Remaining = Chunks;
	}

	void FinishChunk (bool bRan)
	{
		{
			std::lock_guard lock(Mutex);
			--Remaining;
			bAbandoned = bAbandoned || !bRan;
		}
		Finished.notify_all();
	}

	[[nodiscard]] bool IsDone ()
	{
		std::lock_guard lock(Mutex);
		return Remaining == 0u;
	}

	// Done (every chunk run or dropped) within TimeoutSeconds.
	bool WaitDone (double TimeoutSeconds)
	{
		std::unique_lock lock(Mutex);
		return Finished.wait_for(lock, std::chrono::duration<double>(TimeoutSeconds > 0.0 ? TimeoutSeconds : 0.0),
			[this] { return Remaining == 0u; });
	}

	// Only once done.
	[[nodiscard]] bool IsAbandoned ()
	{
		std::lock_guard lock(Mutex);
		return bAbandoned;
	}

private:
	std::mutex Mutex;
	std::condition_variable Finished;
	uint32 Remaining = 0u;
	bool bAbandoned = false;
};

// Owned by a chunk's work callable: finishes the chunk when the callable dies, which the pool does on
// the worker right after running it, or wherever it drops it unrun.
struct SChunkGuard
{
	std::shared_ptr<SVerifyGroup> Group;
	bool bRan = false;

	explicit SChunkGuard (std::shared_ptr<SVerifyGroup> InGroup) noexcept
		: Group(std::move(InGroup))
	{}

	SChunkGuard (SChunkGuard&& Other) noexcept
		: Group(std::move(Other.Group))
		, bRan(Other.bRan)
	{}

	SChunkGuard (const SChunkGuard&) = delete;
	SChunkGuard& operator= (const SChunkGuard&) = delete;
	SChunkGuard& operator= (SChunkGuard&&) = delete;

	~SChunkGuard ()
	{
		if (Group)
		{
			Group->FinishChunk(bRan);
		}
	}
};

// Submits Group's checks to Pool in chunks; the number of tasks.
uint32 SubmitGroup (jobs::CWorkerPool& Pool, const std::shared_ptr<SVerifyGroup>& Group, jobs::EWorkPriority Priority)
{
	const size_t count = Group->Items.size();
	const uint32 chunks = static_cast<uint32>((count + VerifyChunk - 1u) / VerifyChunk);
	Group->Start(chunks);
	for (size_t begin = 0u; begin < count; begin += VerifyChunk)
	{
		const size_t end = begin + VerifyChunk < count ? begin + VerifyChunk : count;
		Pool.Submit(Priority, jobs::NoCancelGroup, [guard = SChunkGuard(Group), begin, end] () mutable
		{
			for (size_t i = begin; i < end; ++i)
			{
				GatherFacts(guard.Group->Items[i]);
			}
			guard.bRan = true;
		});
	}
	return chunks;
}

}


SFileWatcherSettings SFileWatcherSettings::FromConfig (const config::CConfig& Config)
{
	SFileWatcherSettings settings;
	settings.bEnabled = Config.Get("Assets", "Watch", settings.bEnabled);
	settings.VcsSettleMs = Config.Get("AssetsEditor", "VcsSettleMs", settings.VcsSettleMs);
	return settings;
}


/**
 * The service's state. Paths inside are relative to their root, '/' separated, spelled as the OS
 * reported them; keys are the same paths as NTFS compares names (MakePathKey: its upcase table, not
 * Unicode folding, which merges names NTFS keeps apart) on case-insensitive directories, which is
 * what pending entries, pairing and filters compare.
 */
struct CFileWatcherService::SState
{
	// What happened at one path since it was last told (§5.4 "Stage 3": the level-triggered dirty set).
	enum class EEntry : uint8
	{
		File,               // a file, or a path of unknown type
		DirectoryRemoved,   // a directory removed or moved out (its subtree with it)
		DirectoryMoved,     // a directory renamed or moved within the root, from MovedFrom
		DirectoryAdded,     // a directory that appeared (created, moved or copied in): told through its files
	};

	struct SPending
	{
		FString Path;          // the latest spelling
		FString FirstPath;     // the spelling at the first event of the window (a case-only rename differs)
		FString MovedFrom;     // a paired move's source; empty without one
		uint64 FirstSeen = 0u;
		uint64 LastSeen = 0u;
		uint64 NextProbe = 0u;   // a writer held it: not probed again before this
		uint64 HeldSince = 0u;
		uint32 ProbeFailures = 0u;
		EEntry Type = EEntry::File;
		bool bExistedBefore = false;    // inferred from its first event: anything but a creation
		bool bExistsNow = false;        // from its last event; the stat at settle has the last word
		bool bMovedFromExisted = false; // the move's source was there before the window (else a temporary file)
		bool bMaybeDirectory = false;   // removed without an entry type (no extended information)
		bool bMovedAway = false;        // its file moved to another path, whose entry tells the move; kept so
		                                // that a file landing here afterwards is a Modified (S4's backup rename)
		bool bEnumerate = false;        // a directory appeared here: every file below it is Added when it comes due
		bool bWarnedHeld = false;
		bool bToldUnverified = false;   // the subscriptions that do not verify were told it since its last event
		uint64 Ticket = 0u;             // the verification group its disk checks went to (0: none)
		uint64 Cohort = 0u;             // the directory walk that found it (0: none): never split by the cap
	};

	using FPendingList = std::vector<std::pair<FString, SPending>>;

	enum class ERootState : uint8
	{
		Unarmed,   // watching is off
		Missing,   // the directory does not exist (or cannot be watched yet): looked for every RootPollMs
		Armed,
		Lost,      // the platform lost it; it arms it again by itself and reports an Overflow
	};

	struct SRenamedOld
	{
		FString Path;
		FString Key;
		platform::SFileIdentity Identity;
		uint64 Time = 0u;
		bool bDirectory = false;
		bool bValid = false;
	};

	struct SRoot
	{
		std::filesystem::path Path;   // normalized, as its first subscriber spelled it
		bool bCaseSensitive = false;
		bool bCaseKnown = false;
		ERootState State = ERootState::Unarmed;
		uint32 WatcherId = 0u;
		uint64 NextPoll = 0u;
		uint64 MissingPollTicks = 0u;   // the wait before the next look while Missing (longer while asking is slow)
		bool bRescanPending = false;
		uint64 RescanSince = 0u;
		uint64 LastLossTime = 0u;       // the latest Overflow or RootLost (Flush's fences may be among what it lost)
		bool bArmFailureLogged = false;
		uint32 SubscriptionCount = 0u;
		std::unordered_map<FString, SPending, SStringHash, SStringEqual> Pending;   // by key
		SRenamedOld LastRenamedOld;   // for pairing by adjacency where events carry no identity
		uint64 Serial = 0u;           // names it in work that may outlive it (verification groups)
		// With a pool, events from the first one whose entry type the disk must say (no entry types: FAT,
		// ReFS, a share) wait here, in order, for the Stats that Classifying runs on the pool for the first
		// ClassifyCount of them.
		std::deque<platform::SFileEvent> Deferred;
		std::shared_ptr<SVerifyGroup> Classifying;
		size_t ClassifyCount = 0u;
	};

	// A Removed or RenamedOld waiting PairWindowMs for the Added or RenamedNew of the same identity.
	struct SRemoval
	{
		SRoot* Root = nullptr;
		FString Path;
		FString Key;
		platform::SFileIdentity Identity;
		uint64 Time = 0u;
		bool bDirectory = false;
		// A directory's: what was pending below it, re-pathed if the directory turns out to have moved
		// within the root; and the removals its files that had moved in from elsewhere leave behind,
		// pending once it turns out to have gone for good (ExpireRemovals).
		FPendingList Below;
		FPendingList LeftBehind;
	};

	struct SSubscription
	{
		uint64 Id = 0u;   // 0: removed during a delivery, compacted when it ends
		SRoot* Root = nullptr;
		std::filesystem::path Directory;   // normalized, as the subscriber spelled it
		FString SubPath;      // Directory relative to the root
		FString SubKey;
		FString RegionPath;   // SubPath joined with the filter's Prefix: the root-relative region it watches
		FString RegionKey;
		std::vector<FString> Extensions;
		bool bVerify = true;
		FFileChangesCallback Callback;
	};

	// Who a settled change is for: the subscriptions that verify (told what the disk says), those that
	// do not (told what the events say, at once), or both (a Rescan, which no check could change).
	enum class EAudience : uint8
	{
		All,
		Verified,
		Unverified,
	};

	[[nodiscard]] static bool IsFor (EAudience Audience, bool bVerify) noexcept
	{
		return Audience == EAudience::All || (Audience == EAudience::Verified) == bVerify;
	}

	// A change that has settled, before it is cut to each subscription.
	struct SReady
	{
		SRoot* Root = nullptr;
		EAudience Audience = EAudience::All;
		EFileChange Kind = EFileChange::Modified;
		bool bDirectory = false;
		bool bMaybeDirectory = false;
		FString Path;
		FString Key;
		FString OldPath;
		FString OldKey;
	};

	struct SExpected
	{
		hash::SContentHash Hash;
		uint64 Size = 0u;
		uint64 Deadline = 0u;
	};

	// Flush's fence of one root: entries of Root seen no later than Time are told regardless of the
	// windows. Also what Ingest records of every fence event it sees (Name: the file's name).
	struct SFence
	{
		const SRoot* Root = nullptr;
		uint64 Time = 0u;
		FString Name;
	};

	SFileWatcherSettings Settings;
	std::unique_ptr<platform::IFileWatcher> Watcher;
	const uint64 OwnerThread;

	uint64 SettleTicks = 0u;
	uint64 MaxLatencyTicks = 0u;
	uint64 RemoveGraceTicks = 0u;
	uint64 PairTicks = 0u;
	uint64 VcsSettleTicks = 0u;
	uint64 VcsPollTicks = 0u;
	uint64 HeldWarningTicks = 0u;
	uint64 RootPollTicks = 0u;
	uint64 SlowStatTicks = 0u;
	uint64 MaxMissingPollTicks = 0u;

	std::vector<std::unique_ptr<SRoot>> Roots;
	std::unordered_map<uint32, SRoot*> RootsByWatcherId;
	std::vector<SSubscription> Subscriptions;
	std::vector<SSubscription> AddedDuringDelivery;
	uint32 DeliveryDepth = 0u;
	uint32 RemovedDuringDelivery = 0u;

	std::vector<SRemoval> Removals;
	std::unordered_map<FString, SExpected, SStringHash, SStringEqual> Expected;
	// Fence events by the root that reported them: a fence in a root that lies inside another is
	// reported by both, and each root's own events are ordered only with each other.
	std::vector<SFence> SeenFences;
	uint64 FenceCounter = 0u;
	// SetEnabled's OnlyUnder (normalized): when not empty, only roots that hold one of these
	// directories or lie below one are armed.
	std::vector<std::filesystem::path> EnabledOnlyUnder;
	std::vector<std::filesystem::path> Mounts;   // AddMount's, normalized

	TUniqueFunction<bool ()> VcsBusy;
	bool bVcsBusy = false;
	uint64 LastVcsPoll = 0u;

	bool bHolding = false;          // a burst, an overflow or a busy VCS: everything waits for quiet
	uint32 ActivityEvents = 0u;     // events since the last SettleMs pause
	uint64 LastEventTime = 0u;

	SFileWatcherStats Stats;
	std::vector<platform::SFileEvent> EventBuffer;

	// SetWorkerPool's; null: disk checks inline.
	jobs::CWorkerPool* Pool = nullptr;
	// Verification groups submitted and not applied yet, oldest first: applied in this order, each whole.
	std::deque<std::shared_ptr<SVerifyGroup>> InFlight;
	uint64 NextGroupId = 1u;
	uint64 NextRootSerial = 1u;
	uint64 NextCohort = 1u;

	SState (const SFileWatcherSettings& InSettings, std::unique_ptr<platform::IFileWatcher> InWatcher)
		: Settings(InSettings)
		, Watcher(std::move(InWatcher))
		, OwnerThread(platform::GetCurrentThreadId())
	{
		SettleTicks = platform::MillisecondsToTimestamp(Settings.SettleMs);
		MaxLatencyTicks = platform::MillisecondsToTimestamp(Settings.MaxLatencyMs);
		RemoveGraceTicks = platform::MillisecondsToTimestamp(Settings.RemoveGraceMs);
		PairTicks = platform::MillisecondsToTimestamp(Settings.PairWindowMs);
		VcsSettleTicks = platform::MillisecondsToTimestamp(Settings.VcsSettleMs);
		VcsPollTicks = platform::MillisecondsToTimestamp(Settings.VcsPollMs);
		HeldWarningTicks = platform::MillisecondsToTimestamp(Settings.HeldWarningMs);
		RootPollTicks = platform::MillisecondsToTimestamp(Settings.RootPollMs);
		SlowStatTicks = platform::MillisecondsToTimestamp(SlowStatMs);
		MaxMissingPollTicks = platform::MillisecondsToTimestamp(MaxMissingPollMs);
	}

	void CheckOwner () const
	{
		frt_assert(platform::GetCurrentThreadId() == OwnerThread)
	}

	[[nodiscard]] FString MakeKey (const SRoot& Root, FStringView Path) const
	{
		return Root.bCaseSensitive ? FString(Path) : MakePathKey(Path);
	}

	[[nodiscard]] std::filesystem::path MakeFullPath (const SRoot& Root, FStringView Path) const
	{
		return Path.IsEmpty() ? Root.Path : (Root.Path / Path.ToPath()).make_preferred();
	}

	// ---------------- Roots ----------------

	SRoot* FindRoot (const std::filesystem::path& Directory, FString* OutRelative)
	{
		SRoot* best = nullptr;
		for (const std::unique_ptr<SRoot>& root : Roots)
		{
			std::optional<FString> relative = platform::GetRelativePath(Directory, root->Path, root->bCaseSensitive);
			if (relative && (best == nullptr || CountNames(*relative) < CountNames(*OutRelative)))
			{
				best = root.get();
				*OutRelative = std::move(*relative);
			}
		}
		return best;
	}

	// The outermost mount that holds Directory (normalized), with Directory relative to it, or null.
	const std::filesystem::path* FindMount (const std::filesystem::path& Directory, FString* OutRelative) const
	{
		const std::filesystem::path* best = nullptr;
		for (const std::filesystem::path& mount : Mounts)
		{
			std::optional<FString> relative = platform::GetRelativePath(Directory, mount);
			if (relative && (best == nullptr || CountNames(*relative) > CountNames(*OutRelative)))
			{
				best = &mount;
				*OutRelative = std::move(*relative);
			}
		}
		return best;
	}

	// Root and Directory overlap: one holds the other (both normalized).
	[[nodiscard]] static bool Overlaps (const SRoot& Root, const std::filesystem::path& Directory)
	{
		return platform::GetRelativePath(Directory, Root.Path, Root.bCaseSensitive).has_value()
			|| platform::GetRelativePath(Root.Path, Directory, Root.bCaseSensitive).has_value();
	}

	// Watching is on for Root: on, and Root within SetEnabled's OnlyUnder when it named directories.
	[[nodiscard]] bool IsWatchingOn (const SRoot& Root) const
	{
		return Settings.bEnabled
			&& (EnabledOnlyUnder.empty()
				|| std::any_of(EnabledOnlyUnder.begin(), EnabledOnlyUnder.end(), [&Root] (const std::filesystem::path& Directory)
					{
						return Overlaps(Root, Directory);
					}));
	}

	// Arms Root if watching is on for it and its directory exists; else leaves it Missing (looked for
	// later) or Unarmed. True when it is armed now.
	bool ArmRoot (SRoot& Root, uint64 Now)
	{
		if (!IsWatchingOn(Root))
		{
			Root.State = ERootState::Unarmed;
			return false;
		}
		const uint64 statStart = platform::ReadTimestamp();
		const std::optional<platform::SFileStat> stat = platform::Stat(Root.Path);
		if (!stat || !stat->bDirectory)
		{
			Root.State = ERootState::Missing;
			const bool bSlow = Elapsed(statStart, platform::ReadTimestamp()) >= SlowStatTicks;
			const uint64 doubled = (Root.MissingPollTicks > RootPollTicks ? Root.MissingPollTicks : RootPollTicks) * 2u;
			Root.MissingPollTicks = !bSlow ? RootPollTicks : doubled < MaxMissingPollTicks ? doubled : MaxMissingPollTicks;
			Root.NextPoll = Now + Root.MissingPollTicks;
			return false;
		}
		if (!Watcher)
		{
			Watcher = platform::CreateFileWatcher(Settings.Watcher);
		}
		FString error;
		const uint32 id = Watcher->Arm(Root.Path, &error);
		if (id == 0u)
		{
			if (!Root.bArmFailureLogged)
			{
				log::Warning(LogCategory, "cannot watch {}: {}; trying again every {} ms", FString::FromPath(Root.Path), error,
					Settings.RootPollMs);
				Root.bArmFailureLogged = true;
			}
			Root.State = ERootState::Missing;
			Root.NextPoll = Now + RootPollTicks;
			return false;
		}
		Root.WatcherId = id;
		Root.State = ERootState::Armed;
		Root.bArmFailureLogged = false;
		RootsByWatcherId[id] = &Root;
		if (!Root.bCaseKnown)
		{
			if (const std::optional<platform::SVolumeInfo> volume = platform::QueryVolumeInfo(Root.Path))
			{
				Root.bCaseSensitive = volume->bCaseSensitive;
			}
			Root.bCaseKnown = true;
			RefreshSubscriptionKeys(Root);
		}
		log::Verbose(LogCategory, "watching {}", FString::FromPath(Root.Path));
		return true;
	}

	void DisarmRoot (SRoot& Root)
	{
		if (Root.WatcherId != 0u)
		{
			if (Watcher)
			{
				Watcher->Disarm(Root.WatcherId);
			}
			RootsByWatcherId.erase(Root.WatcherId);
			Root.WatcherId = 0u;
		}
		Root.State = ERootState::Unarmed;
		Root.Pending.clear();
		Root.bRescanPending = false;
		Root.LastRenamedOld = {};
		Root.Deferred.clear();
		Root.Classifying.reset();
		Root.ClassifyCount = 0u;
		std::erase_if(Removals, [&Root] (const SRemoval& Removal) { return Removal.Root == &Root; });
	}

	// Roots without subscriptions go (never during a delivery: SReady entries point at them).
	void ReleaseUnusedRoots ()
	{
		if (DeliveryDepth > 0u)
		{
			return;
		}
		for (size_t i = Roots.size(); i-- > 0u;)
		{
			if (Roots[i]->SubscriptionCount == 0u)
			{
				DisarmRoot(*Roots[i]);
				Roots.erase(Roots.begin() + static_cast<std::ptrdiff_t>(i));
			}
		}
	}

	// Subscribed directories that did not exist (or could not be watched) yet.
	void PollRoots (uint64 Now)
	{
		for (const std::unique_ptr<SRoot>& root : Roots)
		{
			if (root->State == ERootState::Missing && Now >= root->NextPoll && ArmRoot(*root, Now))
			{
				// It appeared: whatever is in it is news to its subscribers.
				MarkRescan(*root, Now, false);
			}
		}
	}

	void MarkRescan (SRoot& Root, uint64 Time, bool bHold)
	{
		Root.bRescanPending = true;
		Root.RescanSince = Root.RescanSince > Time ? Root.RescanSince : Time;
		if (bHold)
		{
			StartHolding();
		}
	}

	void StartHolding ()
	{
		if (!bHolding)
		{
			bHolding = true;
			++Stats.Bursts;
		}
	}

	// ---------------- Subscriptions ----------------

	void ComputeSubscriptionKeys (SSubscription& Subscription, FStringView Prefix) const
	{
		Subscription.SubKey = MakeKey(*Subscription.Root, Subscription.SubPath);
		Subscription.RegionPath = JoinPath(Subscription.SubPath, Prefix);
		Subscription.RegionKey = MakeKey(*Subscription.Root, Subscription.RegionPath);
	}

	// The root's case sensitivity became known: keys made before used the default.
	void RefreshSubscriptionKeys (SRoot& Root)
	{
		const auto refresh = [this, &Root] (SSubscription& Subscription)
		{
			if (Subscription.Root == &Root)
			{
				const FString prefix(DropNames(Subscription.RegionPath, CountNames(Subscription.SubPath)));
				ComputeSubscriptionKeys(Subscription, prefix);
			}
		};
		std::for_each(Subscriptions.begin(), Subscriptions.end(), refresh);
		std::for_each(AddedDuringDelivery.begin(), AddedDuringDelivery.end(), refresh);
	}

	[[nodiscard]] static bool MatchesExtension (const SSubscription& Subscription, FStringView Path)
	{
		if (Subscription.Extensions.empty())
		{
			return true;
		}
		const FStringView name = GetName(Path);
		for (const FString& extension : Subscription.Extensions)
		{
			if (name.EndsWith<ESearchCase::IgnoreCase>(extension))
			{
				return true;
			}
		}
		return false;
	}

	[[nodiscard]] static bool MatchesFile (const SSubscription& Subscription, FStringView Key, FStringView Path)
	{
		return IsAtOrBelow(Key, Subscription.RegionKey) && MatchesExtension(Subscription, Path);
	}

	// Some live subscription of the root that verifies (bVerify) or does not would be told about this
	// entry: only entries some verifying one would be told are stat'ed, probed and hashed.
	[[nodiscard]] bool IsRelevant (const SRoot& Root, const SPending& Entry, FStringView Key, bool bVerify) const
	{
		const FString movedKey = Entry.MovedFrom.IsEmpty() ? FString() : MakeKey(Root, Entry.MovedFrom);
		for (const SSubscription& subscription : Subscriptions)
		{
			if (subscription.Id == 0u || subscription.Root != &Root || subscription.bVerify != bVerify)
			{
				continue;
			}
			// A removal of unknown type may have been a directory, at or below the region or holding it.
			if (MatchesFile(subscription, Key, Entry.Path) || (!movedKey.IsEmpty() && MatchesFile(subscription, movedKey, Entry.MovedFrom))
				|| (Entry.bMaybeDirectory && (IsAtOrBelow(subscription.RegionKey, Key) || IsAtOrBelow(Key, subscription.RegionKey))))
			{
				return true;
			}
		}
		return false;
	}

	// Some live subscription of the root verifies (bVerify) or does not.
	[[nodiscard]] bool HasAudience (const SRoot& Root, bool bVerify) const
	{
		return std::any_of(Subscriptions.begin(), Subscriptions.end(), [&Root, bVerify] (const SSubscription& Subscription)
		{
			return Subscription.Id != 0u && Subscription.Root == &Root && Subscription.bVerify == bVerify;
		});
	}

	// ---------------- Events ----------------

	// Event is about an entry the disk must name: no entry type, and a kind after which the entry exists
	// (Ingest's fallback). Fences never need it.
	[[nodiscard]] static bool NeedsClassification (const platform::SFileEvent& Event)
	{
		return Event.EntryType == platform::EFileEntryType::Unknown
			&& (Event.Kind == platform::EFileEventKind::Added || Event.Kind == platform::EFileEventKind::RenamedNew
				|| Event.Kind == platform::EFileEventKind::Modified)
			&& !IsFence(Event.Path);
	}

	// Takes the platform's events. With a pool (and not bInline: Flush), an event that needs the disk to
	// say what its entry is waits with everything after it in its root, in order, for a Stat on the pool;
	// the next call ingests what has been classified. Inline, they are ingested now.
	void PumpEvents (bool bInline = false)
	{
		if (!Watcher)
		{
			return;
		}
		const bool bDefer = Pool != nullptr && !bInline;
		IngestDeferred(!bDefer);
		EventBuffer.clear();
		Watcher->TakeEvents(EventBuffer);
		for (platform::SFileEvent& event : EventBuffer)
		{
			const auto rootIt = RootsByWatcherId.find(event.RootId);
			if (bDefer && rootIt != RootsByWatcherId.end() && (!rootIt->second->Deferred.empty() || NeedsClassification(event)))
			{
				if (event.Timestamp == 0u)
				{
					event.Timestamp = platform::ReadTimestamp();   // when it arrived, not when it is ingested
				}
				rootIt->second->Deferred.push_back(std::move(event));
				continue;
			}
			Ingest(event, std::nullopt);
		}
		if (bDefer)
		{
			SubmitClassifications();
		}
		// Removals older than the pair window can no longer pair: by the newest event, and by the clock,
		// so that a removal no event follows - a directory removed last, whose left-behind removals
		// wait for its expiry - is not kept until something unrelated happens. The clock's bound leaves
		// one more pair window for events the watcher thread read (and stamped) but has not queued yet,
		// and for events that wait for their classification (their other half may be among them).
		const uint64 now = platform::ReadTimestamp();
		const uint64 byEvents = LastEventTime > PairTicks ? LastEventTime - PairTicks - 1u : 0u;
		uint64 byClock = now > 2u * PairTicks ? now - 2u * PairTicks : 0u;
		for (const std::unique_ptr<SRoot>& root : Roots)
		{
			if (!root->Deferred.empty())
			{
				const uint64 first = root->Deferred.front().Timestamp;
				const uint64 bound = first > PairTicks ? first - PairTicks - 1u : 0u;
				byClock = bound < byClock ? bound : byClock;
			}
		}
		const uint64 expireUpTo = byEvents > byClock ? byEvents : byClock;
		if (expireUpTo > 0u)
		{
			ExpireRemovals(expireUpTo);
		}
	}

	// Ingests each root's deferred events, in order, as far as their entry types are known: classified
	// by its finished group, or - bInline - by a Stat now. A group the pool dropped unrun is submitted
	// again (SubmitClassifications).
	void IngestDeferred (bool bInline)
	{
		for (const std::unique_ptr<SRoot>& rootPtr : Roots)
		{
			SRoot& root = *rootPtr;
			if (root.Deferred.empty())
			{
				continue;
			}
			size_t classified = 0u;
			if (root.Classifying && root.Classifying->IsDone())
			{
				classified = root.Classifying->IsAbandoned() ? 0u : root.ClassifyCount;
			}
			const std::vector<SVerifyItem>* results = classified > 0u ? &root.Classifying->Items : nullptr;
			size_t next = 0u;   // the next result
			size_t taken = 0u;
			// Ingest may not touch Deferred; events are moved out first, as far as they can go.
			while (taken < root.Deferred.size())
			{
				const platform::SFileEvent& event = root.Deferred[taken];
				std::optional<bool> bDirectory;
				if (NeedsClassification(event))
				{
					if (taken < classified)
					{
						const SDiskFacts& facts = (*results)[next++].Facts;
						bDirectory = facts.Stat && facts.Stat->bDirectory;
					}
					else if (!bInline)
					{
						break;
					}
				}
				const platform::SFileEvent copy = event;
				++taken;
				Ingest(copy, bDirectory);
			}
			root.Deferred.erase(root.Deferred.begin(), root.Deferred.begin() + static_cast<std::ptrdiff_t>(taken));
			if (root.Classifying && (root.Classifying->IsDone() || bInline))
			{
				// Used up, dropped, or overtaken by the inline Stats (its late results are ignored).
				root.Classifying.reset();
				root.ClassifyCount = 0u;
			}
			else if (root.Classifying)
			{
				root.ClassifyCount = taken <= root.ClassifyCount ? root.ClassifyCount - taken : 0u;
			}
		}
	}

	// Roots with deferred events and no classification running: their unknown entries are stat'ed on
	// the pool, one group per root.
	void SubmitClassifications ()
	{
		for (const std::unique_ptr<SRoot>& rootPtr : Roots)
		{
			SRoot& root = *rootPtr;
			if (root.Deferred.empty() || root.Classifying)
			{
				continue;
			}
			auto group = std::make_shared<SVerifyGroup>();
			group->Id = NextGroupId++;
			for (const platform::SFileEvent& event : root.Deferred)
			{
				if (NeedsClassification(event))
				{
					SVerifyItem item;
					item.Full = MakeFullPath(root, event.Path);
					group->Items.push_back(std::move(item));
				}
			}
			root.ClassifyCount = root.Deferred.size();
			root.Classifying = group;
			Stats.VerifyTasks += SubmitGroup(*Pool, group, jobs::EWorkPriority::High);
		}
	}

	// Removals (of Only's root, or of every root) seen at Time or earlier will not pair any more: a
	// directory's leaves its left-behind removals pending.
	void ExpireRemovals (uint64 Time, const SRoot* Only = nullptr)
	{
		for (size_t i = 0u; i < Removals.size();)
		{
			SRemoval& removal = Removals[i];
			if (removal.Time > Time || (Only != nullptr && removal.Root != Only))
			{
				++i;
				continue;
			}
			ReinsertLeftBehind(*removal.Root, removal.LeftBehind);
			Removals.erase(Removals.begin() + static_cast<std::ptrdiff_t>(i));
		}
	}

	// A directory gone for good: the removals its files that had moved in from elsewhere in the root
	// left behind are pending now. A source whose own entry is still pending remembers that its file
	// moved away (bMovedAway, dropped at settle because the destination tells the move); the
	// destination went with the directory, so the source's removal is told after all.
	static void ReinsertLeftBehind (SRoot& Root, FPendingList& LeftBehind)
	{
		for (auto& [key, entry] : LeftBehind)
		{
			// try_emplace moves neither the key nor the entry when the key is there already.
			const auto [it, bInserted] = Root.Pending.try_emplace(std::move(key), std::move(entry));
			if (!bInserted)
			{
				it->second.bMovedAway = false;
				MarkChanged(it->second, entry.LastSeen);
			}
		}
		LeftBehind.clear();
	}

	// KnownDirectory: what the disk said the entry of an event of unknown type is (classified on the
	// pool); nullopt asks it now where that is needed.
	void Ingest (const platform::SFileEvent& Event, std::optional<bool> KnownDirectory)
	{
		++Stats.Events;
		const auto rootIt = RootsByWatcherId.find(Event.RootId);
		if (rootIt == RootsByWatcherId.end())
		{
			return;   // disarmed meanwhile
		}
		SRoot& root = *rootIt->second;
		const uint64 time = Event.Timestamp != 0u ? Event.Timestamp : platform::ReadTimestamp();

		// Activity: more than BurstEvents events without a SettleMs pause is a VCS operation.
		if (Elapsed(LastEventTime, time) > SettleTicks)
		{
			ActivityEvents = 0u;
		}
		++ActivityEvents;
		LastEventTime = time > LastEventTime ? time : LastEventTime;
		if (!bHolding && ActivityEvents > Settings.BurstEvents)
		{
			StartHolding();
			log::Info(LogCategory, "more than {} changes at once (a checkout or a sync?): waiting for {} ms of quiet", Settings.BurstEvents,
				Settings.VcsSettleMs);
		}

		switch (Event.Kind)
		{
		case platform::EFileEventKind::Overflow:
			++Stats.Overflows;
			root.State = ERootState::Armed;
			root.LastLossTime = time > root.LastLossTime ? time : root.LastLossTime;
			if (!root.bRescanPending)
			{
				log::Info(LogCategory, "{}: changes were missed; rescanning it once the tree is quiet", FString::FromPath(root.Path));
			}
			MarkRescan(root, time, true);
			return;
		case platform::EFileEventKind::RootLost:
			++Stats.RootsLost;
			root.State = ERootState::Lost;
			root.LastLossTime = time > root.LastLossTime ? time : root.LastLossTime;
			log::Warning(LogCategory, "{} went away; watching it again when it is back", FString::FromPath(root.Path));
			MarkRescan(root, time, false);
			return;
		default:
			break;
		}

		if (IsFence(Event.Path))
		{
			if (Event.Kind == platform::EFileEventKind::Added || Event.Kind == platform::EFileEventKind::RenamedNew)
			{
				SeenFences.push_back({ &root, time, FString(GetName(Event.Path)) });
			}
			return;
		}

		bool bDirectory = Event.EntryType == platform::EFileEntryType::Directory;
		if (Event.EntryType == platform::EFileEntryType::Unknown
			&& (Event.Kind == platform::EFileEventKind::Added || Event.Kind == platform::EFileEventKind::RenamedNew
				|| Event.Kind == platform::EFileEventKind::Modified))
		{
			// Without entry types (ReadDirectoryChangesW on FAT, ReFS or a share) the disk says what the
			// entry is. A directory that appeared is enumerated below; a directory's own Modified is its
			// times, changed by an entry inside that reports itself, so it is dropped here as the platform
			// layer drops it where it knows the type - else every edit would rescan its parent directory.
			if (KnownDirectory)
			{
				bDirectory = *KnownDirectory;
			}
			else
			{
				const std::optional<platform::SFileStat> stat = platform::Stat(MakeFullPath(root, Event.Path));
				bDirectory = stat && stat->bDirectory;
			}
			if (bDirectory && Event.Kind == platform::EFileEventKind::Modified)
			{
				return;
			}
		}
		else if (Event.EntryType == platform::EFileEntryType::Unknown)
		{
			// A removal: the disk cannot say what is gone, but the window may know - a directory that
			// moved or appeared here within it.
			const auto known = root.Pending.find(MakeKey(root, Event.Path));
			bDirectory = known != root.Pending.end() && known->second.Type != EEntry::File;
		}
		if (bDirectory)
		{
			IngestDirectory(root, Event, time);
		}
		else
		{
			IngestFile(root, Event, time);
		}
	}

	SPending& Touch (SRoot& Root, const FString& Key, FStringView Path, bool bExistedIfNew, uint64 Time)
	{
		auto [it, bInserted] = Root.Pending.try_emplace(Key);
		SPending& entry = it->second;
		if (bInserted)
		{
			entry.FirstPath = FString(Path);
			entry.bExistedBefore = bExistedIfNew;
			entry.FirstSeen = Time;
		}
		entry.Path = FString(Path);
		MarkChanged(entry, Time);
		return entry;
	}

	// Something happened at Entry's path at Time: it settles again, the subscriptions that do not verify
	// are told it again, and disk checks made before are stale.
	static void MarkChanged (SPending& Entry, uint64 Time)
	{
		Entry.LastSeen = Time > Entry.LastSeen ? Time : Entry.LastSeen;
		Entry.bToldUnverified = false;
		Entry.Ticket = 0u;
	}

	// The removal (or rename's old half) the Added or RenamedNew Event completes: same identity within
	// the pair window, else - without identities - the RenamedOld right before a RenamedNew. Its index
	// in Removals, or nullopt; for adjacency, Removals.size() (the root's LastRenamedOld).
	std::optional<size_t> FindMoveSource (const SRoot& Root, const platform::SFileEvent& Event, uint64 Time) const
	{
		if (Event.Identity.IsValid())
		{
			for (size_t i = Removals.size(); i-- > 0u;)
			{
				const SRemoval& removal = Removals[i];
				if (removal.Root == &Root && removal.Identity == Event.Identity && Elapsed(removal.Time, Time) <= PairTicks)
				{
					return i;
				}
			}
			return std::nullopt;
		}
		if (Event.Kind == platform::EFileEventKind::RenamedNew && Root.LastRenamedOld.bValid && !Root.LastRenamedOld.Identity.IsValid()
			&& Elapsed(Root.LastRenamedOld.Time, Time) <= PairTicks)
		{
			return Removals.size();
		}
		return std::nullopt;
	}

	// The source of a paired move, taken out of Removals (and the root's adjacency slot).
	SRemoval TakeMoveSource (SRoot& Root, size_t Index)
	{
		SRemoval source;
		if (Index == Removals.size())
		{
			source.Root = &Root;
			source.Path = std::move(Root.LastRenamedOld.Path);
			source.Key = std::move(Root.LastRenamedOld.Key);
			source.Time = Root.LastRenamedOld.Time;
			source.bDirectory = Root.LastRenamedOld.bDirectory;
		}
		else
		{
			source = std::move(Removals[Index]);
			Removals.erase(Removals.begin() + static_cast<std::ptrdiff_t>(Index));
		}
		Root.LastRenamedOld = {};
		return source;
	}

	void IngestFile (SRoot& Root, const platform::SFileEvent& Event, uint64 Time)
	{
		const FString key = MakeKey(Root, Event.Path);
		switch (Event.Kind)
		{
		case platform::EFileEventKind::Added:
		case platform::EFileEventKind::RenamedNew:
		{
			const std::optional<size_t> sourceIndex = FindMoveSource(Root, Event, Time);
			SPending& entry = Touch(Root, key, Event.Path, false, Time);
			entry.bExistsNow = true;
			if (sourceIndex)
			{
				const SRemoval source = TakeMoveSource(Root, *sourceIndex);
				if (source.Key != key)
				{
					TransferMove(Root, key, source);
				}
			}
			if (Event.Kind == platform::EFileEventKind::RenamedNew)
			{
				Root.LastRenamedOld = {};
			}
			break;
		}
		case platform::EFileEventKind::Removed:
		case platform::EFileEventKind::RenamedOld:
		{
			SPending& entry = Touch(Root, key, Event.Path, true, Time);
			entry.bExistsNow = false;
			entry.bMaybeDirectory = entry.bMaybeDirectory || Event.EntryType == platform::EFileEntryType::Unknown;
			SRemoval removal;
			removal.Root = &Root;
			removal.Path = Event.Path;
			removal.Key = key;
			removal.Identity = Event.Identity;
			removal.Time = Time;
			if (Event.Kind == platform::EFileEventKind::RenamedOld)
			{
				Root.LastRenamedOld = { Event.Path, key, Event.Identity, Time, false, true };
			}
			if (Event.Identity.IsValid())
			{
				Removals.push_back(std::move(removal));
			}
			break;
		}
		case platform::EFileEventKind::Modified:
		{
			SPending& entry = Touch(Root, key, Event.Path, true, Time);
			entry.bExistsNow = true;
			break;
		}
		default:
			break;
		}
	}

	// The entry at DestinationKey is where Source's file went: it takes over the source's history (a
	// chain a -> b -> c is one move from a), and the source's entry stays only to remember that its
	// path held a file before (bMovedAway: dropped at settle unless a file lands there again).
	void TransferMove (SRoot& Root, const FString& DestinationKey, const SRemoval& Source)
	{
		FString from = Source.Path;
		bool bExisted = true;
		const auto sourceIt = Root.Pending.find(Source.Key);
		if (sourceIt != Root.Pending.end())
		{
			SPending& sourceEntry = sourceIt->second;
			bExisted = sourceEntry.bExistedBefore;
			if (!sourceEntry.MovedFrom.IsEmpty())
			{
				from = sourceEntry.MovedFrom;
				bExisted = sourceEntry.bMovedFromExisted;
				sourceEntry.MovedFrom.Clear();
			}
			if (!sourceEntry.bExistsNow)
			{
				sourceEntry.bMovedAway = true;
			}
		}

		SPending& destination = Root.Pending[DestinationKey];
		if (MakeKey(Root, from) == DestinationKey)
		{
			// Moved away and back (a -> b -> a): the file is where it was.
			destination.MovedFrom.Clear();
			destination.bExistedBefore = destination.bExistedBefore || bExisted;
			return;
		}
		destination.MovedFrom = std::move(from);
		destination.bMovedFromExisted = bExisted;
	}

	void IngestDirectory (SRoot& Root, const platform::SFileEvent& Event, uint64 Time)
	{
		const FString key = MakeKey(Root, Event.Path);
		switch (Event.Kind)
		{
		case platform::EFileEventKind::Removed:
		case platform::EFileEventKind::RenamedOld:
		{
			// Gone with everything below it: one record stands for the subtree. What was pending below
			// it waits in the removal, in case the directory turns out to have moved within the root.
			SRemoval removal;
			removal.Root = &Root;
			removal.Path = Event.Path;
			removal.Key = key;
			removal.Identity = Event.Identity;
			removal.Time = Time;
			removal.bDirectory = true;
			ExtractBelow(Root, key, removal.Below, removal.LeftBehind);
			SPending& entry = Touch(Root, key, Event.Path, true, Time);
			if (entry.Type == EEntry::File)
			{
				entry.MovedFrom.Clear();   // a file's history; a directory that moved here keeps where it came from
			}
			entry.Type = EEntry::DirectoryRemoved;
			entry.bExistsNow = false;
			if (Event.Kind == platform::EFileEventKind::RenamedOld)
			{
				Root.LastRenamedOld = { Event.Path, key, Event.Identity, Time, true, true };
			}
			if (Event.Identity.IsValid())
			{
				Removals.push_back(std::move(removal));
			}
			else
			{
				// It can never pair: gone for good.
				ReinsertLeftBehind(Root, removal.LeftBehind);
			}
			break;
		}
		case platform::EFileEventKind::Added:
		case platform::EFileEventKind::RenamedNew:
		{
			const std::optional<size_t> sourceIndex = FindMoveSource(Root, Event, Time);
			if (sourceIndex)
			{
				const bool bAdjacent = *sourceIndex == Removals.size();
				SRemoval source = TakeMoveSource(Root, *sourceIndex);
				if (!source.bDirectory && bAdjacent)
				{
					// Without entry types the old half went in as a file (the disk could not say what was
					// gone); the new half is a directory, so the old one was too.
					source.bDirectory = true;
					ExtractBelow(Root, source.Key, source.Below, source.LeftBehind);
				}
				if (source.bDirectory)
				{
					MoveDirectory(Root, std::move(source), Event.Path, key, Time);
					break;
				}
			}
			if (Event.Kind == platform::EFileEventKind::RenamedNew)
			{
				Root.LastRenamedOld = {};
			}
			// Created, moved in from outside, restored: its subtree reported nothing by itself, and is
			// enumerated when the entry comes due (EnumerateAppeared) - not now: a burst may still be
			// arriving, and a Rescan may make it moot.
			const auto existing = Root.Pending.find(key);
			if (existing == Root.Pending.end())
			{
				SPending& entry = Touch(Root, key, Event.Path, false, Time);
				entry.Type = EEntry::DirectoryAdded;
				entry.bExistsNow = true;
				entry.bEnumerate = true;
				break;
			}
			SPending& entry = existing->second;
			if (entry.Type == EEntry::DirectoryRemoved)
			{
				// Removed and back within the window: whatever was below it may differ (a Rescan).
				entry.bExistsNow = true;
			}
			MarkChanged(entry, Time);
			entry.bEnumerate = true;
			break;
		}
		default:
			break;   // a directory's own times: its children report themselves
		}
	}

	// The directory Source removed is at Path (Key) now, renamed or moved within the root - also when only
	// its name's case changed: what was pending below it moves along, and it is one Moved from where it
	// was when the window began (a chain a -> b -> c is one move from a, as TransferMove makes it for
	// files; back where it began, spelled the same, the directory itself is told nothing).
	void MoveDirectory (SRoot& Root, SRemoval&& Source, FStringView Path, const FString& Key, uint64 Time)
	{
		FString from = Source.Path;
		bool bEnumerate = false;   // it had appeared within the window and is not enumerated yet
		const auto sourceIt = Root.Pending.find(Source.Key);
		if (sourceIt != Root.Pending.end())
		{
			if (sourceIt->second.Type != EEntry::File && !sourceIt->second.MovedFrom.IsEmpty())
			{
				from = sourceIt->second.MovedFrom;
			}
			bEnumerate = sourceIt->second.bEnumerate;
			Root.Pending.erase(sourceIt);
		}
		RepathInto(Root, std::move(Source.Below), Source.Path, Key, Path);
		if (from == Path && !bEnumerate)
		{
			return;
		}
		SPending& entry = Touch(Root, Key, Path, false, Time);
		entry.Type = from == Path ? EEntry::DirectoryAdded : EEntry::DirectoryMoved;
		entry.MovedFrom = from == Path ? FString() : std::move(from);
		entry.bExistsNow = true;
		entry.bEnumerate = bEnumerate;
	}

	// Takes what is pending below the directory at Key out of the root into OutBelow: the directory's own
	// record will stand for all of it. A file that had moved in from elsewhere in the root leaves a
	// removal behind where it came from (OutLeftBehind), told if the directory is gone for good.
	void ExtractBelow (SRoot& Root, FStringView Key, FPendingList& OutBelow, FPendingList& OutLeftBehind)
	{
		for (auto it = Root.Pending.begin(); it != Root.Pending.end();)
		{
			if (it->first == Key || !IsAtOrBelow(it->first, Key))
			{
				++it;
				continue;
			}
			const SPending& entry = it->second;
			if (!entry.MovedFrom.IsEmpty() && entry.bMovedFromExisted)
			{
				FString sourceKey = MakeKey(Root, entry.MovedFrom);
				if (!IsAtOrBelow(sourceKey, Key))
				{
					SPending removed;
					removed.Path = entry.MovedFrom;
					removed.FirstPath = entry.MovedFrom;
					removed.FirstSeen = entry.FirstSeen;
					removed.LastSeen = entry.LastSeen;
					removed.bExistedBefore = true;
					OutLeftBehind.emplace_back(std::move(sourceKey), std::move(removed));
				}
			}
			OutBelow.emplace_back(it->first, std::move(it->second));
			it = Root.Pending.erase(it);
		}
	}

	// Entries that were pending below the directory OldPath, now below NewPath (whose key is NewKey). A
	// move that began inside the directory is re-pathed too: told after the directory's own Moved (keys
	// sort below it), its source is where the directory's move put it.
	void RepathInto (SRoot& Root, FPendingList&& Entries, FStringView OldPath, FStringView NewKey, FStringView NewPath)
	{
		const uint32 oldNames = CountNames(OldPath);
		const FString oldDirectoryKey = MakeKey(Root, OldPath);
		for (auto& [oldKey, entry] : Entries)
		{
			FString newKey = JoinPath(NewKey, DropNames(oldKey, oldNames));
			entry.Path = JoinPath(NewPath, DropNames(entry.Path, oldNames));
			entry.FirstPath = JoinPath(NewPath, DropNames(entry.FirstPath, oldNames));
			if (!entry.MovedFrom.IsEmpty() && IsAtOrBelow(MakeKey(Root, entry.MovedFrom), oldDirectoryKey))
			{
				entry.MovedFrom = JoinPath(NewPath, DropNames(entry.MovedFrom, oldNames));
			}
			Root.Pending.insert_or_assign(std::move(newKey), std::move(entry));
		}
	}

	// Every file at or below Directory (root-relative) becomes an Added entry of Cohort: a directory that
	// moved in reports nothing for its children (S13), and they are resolved together.
	void EnumerateInto (SRoot& Root, FStringView Directory, uint64 Time, uint64 Cohort)
	{
		std::vector<FString> stack;
		stack.emplace_back(Directory);
		while (!stack.empty())
		{
			const FString directory = std::move(stack.back());
			stack.pop_back();
			++Stats.DirectoriesEnumerated;
			platform::EnumerateDirectory(MakeFullPath(Root, directory), [&] (const platform::SDirectoryEntry& Entry)
			{
				FString path = JoinPath(directory, Entry.Name);
				if (IsFence(path))
				{
					return;
				}
				if (Entry.Stat.bDirectory)
				{
					if (!Entry.bReparsePoint)
					{
						stack.push_back(std::move(path));
					}
					return;
				}
				SPending& entry = Touch(Root, MakeKey(Root, path), path, false, Time);
				entry.bExistsNow = true;
				entry.Cohort = Cohort;
			});
		}
	}

	// Some live subscription of the root watches at or below Key, or a region that holds it.
	[[nodiscard]] bool IsWatched (const SRoot& Root, FStringView Key) const
	{
		return std::any_of(Subscriptions.begin(), Subscriptions.end(), [&Root, Key] (const SSubscription& Subscription)
		{
			return Subscription.Id != 0u && Subscription.Root == &Root
				&& (IsAtOrBelow(Key, Subscription.RegionKey) || IsAtOrBelow(Subscription.RegionKey, Key));
		});
	}

	// Directories that appeared, enumerated once their entries come due (by IsDue's rule), not when
	// their events arrived: during a burst or a hold that would walk a tree that is still arriving, and
	// walk it again for every directory below that reports itself. Each due directory is walked once;
	// one below another that is walked goes with it, and so does what is pending below it with an
	// enumeration of its own; directories no subscription watches are not walked at all. The files
	// found are Added entries as old as the directory's, so they come due with it.
	void EnumerateAppeared (SRoot& Root, uint64 Now, bool bHold, bool bReleaseAll, const SFence* Fence)
	{
		struct SDue
		{
			FString Key;
			FString Path;
			uint64 Time = 0u;
		};
		std::vector<SDue> due;
		for (auto& [key, entry] : Root.Pending)
		{
			if (!entry.bEnumerate || !IsDue(entry, Now, bHold, bReleaseAll, Fence))
			{
				continue;
			}
			entry.bEnumerate = false;
			if (entry.Type != EEntry::DirectoryRemoved || entry.bExistsNow)
			{
				due.push_back({ key, entry.Path, entry.LastSeen });
			}
		}
		if (due.empty())
		{
			return;
		}
		std::sort(due.begin(), due.end(), [] (const SDue& A, const SDue& B) { return A.Key < B.Key; });
		std::vector<SDue> walk;
		for (SDue& directory : due)
		{
			// Sorted, the directories above one come before it.
			const bool bBelowAnother = std::any_of(walk.begin(), walk.end(), [&directory] (const SDue& Above)
			{
				return IsAtOrBelow(directory.Key, Above.Key);
			});
			if (!bBelowAnother && IsWatched(Root, directory.Key))
			{
				walk.push_back(std::move(directory));
			}
		}
		for (auto& [key, entry] : Root.Pending)
		{
			if (entry.bEnumerate && std::any_of(walk.begin(), walk.end(), [&key] (const SDue& Directory) { return IsAtOrBelow(key, Directory.Key); }))
			{
				entry.bEnumerate = false;
			}
		}
		for (const SDue& directory : walk)
		{
			EnumerateInto(Root, directory.Path, directory.Time, NextCohort++);
		}
	}

	// ---------------- Settling ----------------

	[[nodiscard]] bool IsSettled (const SPending& Entry, uint64 Now) const
	{
		const bool bGone = Entry.Type == EEntry::DirectoryRemoved ? !Entry.bExistsNow : Entry.bExistedBefore && !Entry.bExistsNow;
		const uint64 needed = SettleTicks + (bGone ? RemoveGraceTicks : 0u);
		return Elapsed(Entry.LastSeen, Now) >= needed || Elapsed(Entry.FirstSeen, Now) >= MaxLatencyTicks;
	}

	// The entry is resolved now: its fence covers it, or nothing is held and it settled (or the hold
	// just ended).
	[[nodiscard]] bool IsDue (const SPending& Entry, uint64 Now, bool bHold, bool bReleaseAll, const SFence* Fence) const
	{
		return (Fence != nullptr && Entry.LastSeen <= Fence->Time) || (!bHold && (bReleaseAll || IsSettled(Entry, Now)));
	}

	[[nodiscard]] bool HasPending () const
	{
		for (const std::unique_ptr<SRoot>& root : Roots)
		{
			if (!root->Pending.empty() || root->bRescanPending || !root->Deferred.empty())
			{
				return true;
			}
		}
		// A directory removal still waiting to pair holds removals that become pending when it expires.
		return std::any_of(Removals.begin(), Removals.end(), [] (const SRemoval& Removal) { return !Removal.LeftBehind.empty(); });
	}

	// Whether everything waits: a burst or an overflow until VcsSettleMs of quiet, a busy VCS until it
	// is not and then the same quiet. True when nothing may be told now; ReleaseAll says the hold just
	// ended (everything pending goes at once).
	bool UpdateHold (uint64 Now, bool& OutReleaseAll)
	{
		OutReleaseAll = false;
		if (VcsBusy && Elapsed(LastVcsPoll, Now) >= VcsPollTicks && HasPending())
		{
			LastVcsPoll = Now;
			bVcsBusy = VcsBusy();
			if (bVcsBusy)
			{
				StartHolding();
			}
		}
		if (!bHolding)
		{
			return false;
		}
		if (bVcsBusy || Elapsed(LastEventTime, Now) < VcsSettleTicks)
		{
			return true;
		}
		bHolding = false;
		ActivityEvents = 0u;
		OutReleaseAll = true;
		return false;
	}

	enum class EOutcome : uint8
	{
		Tell,
		Drop,
		Keep,
		TellAndResolveAgain,
	};

	// The disk checks a settled entry needs, to run inline or on the pool: its Stat, the writer probe if
	// a file is there (also where a directory was removed: a file in its place is resolved as one next),
	// and the hash if the caller expects a write of that file's size there.
	[[nodiscard]] SVerifyItem MakeVerifyItem (const SRoot& Root, const FString& Key, const SPending& Entry) const
	{
		SVerifyItem item;
		item.RootSerial = Root.Serial;
		item.Key = Key;
		item.Path = Entry.Path;
		item.LastSeen = Entry.LastSeen;
		item.Full = MakeFullPath(Root, Entry.Path);
		item.bProbeIfFile = Entry.Type == EEntry::File || Entry.Type == EEntry::DirectoryRemoved;
		if (item.bProbeIfFile && !Expected.empty())
		{
			const auto expected = Expected.find(MakeExpectedKey(item.Full));
			if (expected != Expected.end())
			{
				item.HashIfSize = expected->second.Size;
			}
		}
		return item;
	}

	// A file entry's change, by whether a file is there now (the disk's answer for the subscriptions
	// that verify, the events' for the others): false when there is nothing to tell.
	bool DecideFile (const SRoot& Root, const SPending& Entry, bool bExists, SReady& Out) const
	{
		if (Entry.bMovedAway && !bExists)
		{
			return false;   // where its file went tells the move
		}
		const bool bMoved = !Entry.MovedFrom.IsEmpty() && Entry.bMovedFromExisted;
		if (bExists && bMoved)
		{
			Out.Kind = EFileChange::Moved;
			Out.OldPath = Entry.MovedFrom;
		}
		else if (bExists && Entry.bExistedBefore)
		{
			Out.Kind = EFileChange::Modified;
			if (Entry.FirstPath != Entry.Path)
			{
				// Its name's case changed (a case-only rename, git restoring the index's case).
				Out.Kind = EFileChange::Moved;
				Out.OldPath = Entry.FirstPath;
			}
		}
		else if (bExists)
		{
			Out.Kind = EFileChange::Added;
		}
		else if (Entry.bExistedBefore)
		{
			Out.Kind = EFileChange::Removed;
			Out.bMaybeDirectory = Entry.bMaybeDirectory;
		}
		else if (bMoved)
		{
			// Moved here, then deleted: gone from where it was.
			Out.Kind = EFileChange::Removed;
			Out.Path = Entry.MovedFrom;
			Out.Key = MakeKey(Root, Entry.MovedFrom);
		}
		else
		{
			return false;   // born and gone within the window: a temporary file
		}
		if (!Out.OldPath.IsEmpty())
		{
			Out.OldKey = MakeKey(Root, Out.OldPath);
		}
		return true;
	}

	// What the subscriptions that do not verify are told of a settled entry: what its events say, no
	// disk check. Where the events cannot say what is at a path now - a directory removed, and something
	// back in its place - a Rescan of the path stands for it. Appends no change, one or two.
	void DecideFromEvents (SRoot& Root, const FString& Key, const SPending& Entry, std::vector<SReady>& Out) const
	{
		SReady change;
		change.Root = &Root;
		change.Audience = EAudience::Unverified;
		change.Path = Entry.Path;
		change.Key = Key;
		switch (Entry.Type)
		{
		case EEntry::DirectoryAdded:
			return;   // its files tell it (EnumerateAppeared made them entries)
		case EEntry::DirectoryMoved:
			change.bDirectory = true;
			change.Kind = EFileChange::Moved;
			change.OldPath = Entry.MovedFrom;
			change.OldKey = MakeKey(Root, Entry.MovedFrom);
			Out.push_back(std::move(change));
			return;
		case EEntry::DirectoryRemoved:
			change.bDirectory = true;
			if (!Entry.MovedFrom.IsEmpty())
			{
				// It had moved here within the window, then went on or away: gone from where it was.
				SReady gone = change;
				gone.Kind = EFileChange::Removed;
				gone.Path = Entry.MovedFrom;
				gone.Key = MakeKey(Root, Entry.MovedFrom);
				Out.push_back(std::move(gone));
			}
			if (Entry.bExistsNow)
			{
				change.Kind = EFileChange::Rescan;
				Out.push_back(std::move(change));
			}
			else if (Entry.MovedFrom.IsEmpty())
			{
				change.Kind = EFileChange::Removed;
				Out.push_back(std::move(change));
			}
			return;
		case EEntry::File:
			break;
		}
		if (DecideFile(Root, Entry, Entry.bExistsNow, change))
		{
			Out.push_back(std::move(change));
		}
	}

	// What a settled entry turns out to be for the subscriptions that verify, by Item's disk checks:
	// Tell (Out filled), Drop (nothing to tell, or a self-write), Keep (a writer holds it, it changed
	// under the probe, or it needs a check Item lacks: wait), TellAndResolveAgain (Out filled, and the
	// entry, changed into another, is decided once more by the same checks).
	EOutcome Decide (SRoot& Root, const FString& Key, SPending& Entry, uint64 Now, const SVerifyItem& Item, SReady& Out)
	{
		const std::optional<platform::SFileStat>& stat = Item.Facts.Stat;
		Out.Root = &Root;
		Out.Audience = EAudience::Verified;
		Out.Path = Entry.Path;
		Out.Key = Key;

		if (Entry.Type == EEntry::DirectoryRemoved)
		{
			Out.bDirectory = true;
			if (!Entry.MovedFrom.IsEmpty())
			{
				// It had moved here within the window (a -> b), then went on or away: gone from where it
				// was. Whatever is at its path now is resolved after that, as a path nobody knew.
				Out.Kind = EFileChange::Removed;
				Out.Path = Entry.MovedFrom;
				Out.Key = MakeKey(Root, Entry.MovedFrom);
				Entry.MovedFrom.Clear();
				return stat ? EOutcome::TellAndResolveAgain : EOutcome::Tell;
			}
			Out.Kind = stat && stat->bDirectory ? EFileChange::Rescan : EFileChange::Removed;
			if (stat && !stat->bDirectory)
			{
				// A file took the directory's place (git switching a path from a directory on one branch to
				// a file on another): the directory is told gone, then the path is resolved as the file
				// that is there now - an Added, or the Moved of a file that came from elsewhere in the root
				// - with the writer probe and the self-write check every file gets.
				Entry.Type = EEntry::File;
				Entry.FirstPath = Entry.Path;
				Entry.bExistedBefore = false;
				Entry.bExistsNow = true;
				Entry.bMaybeDirectory = false;
				Entry.bMovedAway = false;
				return EOutcome::TellAndResolveAgain;
			}
			return EOutcome::Tell;
		}
		if (Entry.Type == EEntry::DirectoryAdded)
		{
			return EOutcome::Drop;   // its files tell it (EnumerateAppeared made them entries)
		}
		if (Entry.Type == EEntry::DirectoryMoved)
		{
			Out.bDirectory = true;
			if (stat && stat->bDirectory)
			{
				Out.Kind = EFileChange::Moved;
				Out.OldPath = Entry.MovedFrom;
				Out.OldKey = MakeKey(Root, Entry.MovedFrom);
			}
			else
			{
				// Moved, then gone from its new place too.
				Out.Kind = EFileChange::Removed;
				Out.Path = Entry.MovedFrom;
				Out.Key = MakeKey(Root, Entry.MovedFrom);
			}
			return EOutcome::Tell;
		}

		if (!IsRelevant(Root, Entry, Key, true))
		{
			return EOutcome::Drop;
		}
		if (stat && stat->bDirectory)
		{
			// A directory where a file event was (a file system without entry types, or a file replaced
			// by a directory): what is below it is unknown.
			Out.Kind = EFileChange::Rescan;
			Out.bDirectory = true;
			return EOutcome::Tell;
		}
		const bool bExists = stat.has_value();
		if (!DecideFile(Root, Entry, bExists, Out))
		{
			return EOutcome::Drop;
		}
		if (!bExists)
		{
			return EOutcome::Tell;
		}
		if (!Item.Facts.bProbed)
		{
			Entry.NextProbe = 0u;   // not checked as a file: checked again
			return EOutcome::Keep;
		}

		// A writer may still be at it (Modified arrives at close, usually; S3, S19).
		switch (Item.Facts.Probe)
		{
		case platform::EWriterProbe::Held:
			BackOff(Entry, Item.Full, Now);
			return EOutcome::Keep;
		case platform::EWriterProbe::Missing:
			// Gone since the stat: settle again.
			Entry.bExistsNow = false;
			MarkChanged(Entry, Now);
			return EOutcome::Keep;
		default:
			break;
		}

		// The echo of a write of the caller's own: exactly the bytes it said it would write.
		const auto expected = Expected.find(MakeExpectedKey(Item.Full));
		if (expected != Expected.end())
		{
			const bool bSameSize = stat->Size == expected->second.Size;
			if (bSameSize && !Item.Facts.bHashed)
			{
				Entry.NextProbe = 0u;   // expected only after the checks ran: hashed in the next ones
				return EOutcome::Keep;
			}
			const bool bMatch = bSameSize && Item.Facts.Hash == std::optional(expected->second.Hash);
			Expected.erase(expected);
			if (bMatch)
			{
				++Stats.SelfWritesSuppressed;
				return EOutcome::Drop;
			}
		}
		return EOutcome::Tell;
	}

	// Decides Entry by Item's checks, again for as long as it changes into another; appends what to tell
	// to Out. True when the entry is done (told or dropped), false when it waits.
	bool ApplyFacts (SRoot& Root, const FString& Key, SPending& Entry, uint64 Now, const SVerifyItem& Item, std::vector<SReady>& Out)
	{
		for (;;)
		{
			SReady change;
			switch (Decide(Root, Key, Entry, Now, Item, change))
			{
			case EOutcome::Tell:
				Out.push_back(std::move(change));
				return true;
			case EOutcome::Drop:
				return true;
			case EOutcome::Keep:
				return false;
			case EOutcome::TellAndResolveAgain:
				Out.push_back(std::move(change));
				break;   // the same entry again, changed so that it does not ask for this forever
			}
		}
	}

	// The disk checks now, on the owner thread, then ApplyFacts.
	bool ResolveInline (SRoot& Root, const FString& Key, SPending& Entry, uint64 Now, std::vector<SReady>& Out)
	{
		SVerifyItem item = MakeVerifyItem(Root, Key, Entry);
		GatherFacts(item);
		return ApplyFacts(Root, Key, Entry, Now, item, Out);
	}

	void BackOff (SPending& Entry, const std::filesystem::path& Full, uint64 Now)
	{
		++Stats.WriterBackoffs;
		++Entry.ProbeFailures;
		if (Entry.HeldSince == 0u)
		{
			Entry.HeldSince = Now;
		}
		const uint32 shift = Entry.ProbeFailures - 1u < 16u ? Entry.ProbeFailures - 1u : 16u;
		const uint64 backoffMs = static_cast<uint64>(Settings.ProbeBackoffMs) << shift;
		const uint32 waitMs = static_cast<uint32>(backoffMs < Settings.MaxProbeBackoffMs ? backoffMs : Settings.MaxProbeBackoffMs);
		Entry.NextProbe = Now + platform::MillisecondsToTimestamp(waitMs);
		if (!Entry.bWarnedHeld && Elapsed(Entry.HeldSince, Now) >= HeldWarningTicks)
		{
			log::Warning(LogCategory, "{} is still open for writing in another process; waiting for it to finish", FString::FromPath(Full));
			Entry.bWarnedHeld = true;
		}
	}

	// ---------------- Verification on the pool ----------------

	// Entry's disk checks are in a group that has not been applied yet.
	[[nodiscard]] bool IsAwaiting (const SPending& Entry) const
	{
		return Entry.Ticket != 0u && std::any_of(InFlight.begin(), InFlight.end(), [&Entry] (const std::shared_ptr<SVerifyGroup>& Group)
		{
			return Group->Id == Entry.Ticket;
		});
	}

	[[nodiscard]] SRoot* FindRootBySerial (uint64 Serial) const
	{
		for (const std::unique_ptr<SRoot>& root : Roots)
		{
			if (root->Serial == Serial)
			{
				return root.get();
			}
		}
		return nullptr;
	}

	[[nodiscard]] size_t GetRootIndex (const SRoot* Root) const
	{
		for (size_t i = 0u; i < Roots.size(); ++i)
		{
			if (Roots[i].get() == Root)
			{
				return i;
			}
		}
		return Roots.size();
	}

	// Ready[First, end) in root order, by key within each root. Stable: two changes of one path (a
	// directory removed, a file in its place) keep their order.
	void SortReady (std::vector<SReady>& Ready, size_t First) const
	{
		std::stable_sort(Ready.begin() + static_cast<std::ptrdiff_t>(First), Ready.end(), [this] (const SReady& A, const SReady& B)
		{
			if (A.Root != B.Root)
			{
				return GetRootIndex(A.Root) < GetRootIndex(B.Root);
			}
			return A.Key < B.Key;
		});
	}

	enum class EApply : uint8
	{
		Facts,    // the group's checks are whole: decide by them
		Inline,   // they are not (a Flush could not wait for them): check again now, inline
		Forget,   // they are not: the entries are checked again when the next Pump finds them due
	};

	// One group's entries - those still waiting for it, unchanged since - decided, their changes
	// appended to Out in root order and by key.
	void ApplyGroup (SVerifyGroup& Group, uint64 Now, EApply Mode, std::vector<SReady>& Out)
	{
		const size_t first = Out.size();
		for (const SVerifyItem& item : Group.Items)
		{
			SRoot* root = FindRootBySerial(item.RootSerial);
			if (root == nullptr)
			{
				continue;   // unsubscribed or disabled meanwhile
			}
			const auto it = root->Pending.find(item.Key);
			if (it == root->Pending.end() || it->second.Ticket != Group.Id)
			{
				continue;   // told or forgotten meanwhile (a Rescan), or changed again: checked again
			}
			SPending& entry = it->second;
			entry.Ticket = 0u;
			if (Mode == EApply::Forget || entry.LastSeen != item.LastSeen || entry.Path != item.Path)
			{
				continue;
			}
			const bool bDone = Mode == EApply::Inline ? ResolveInline(*root, it->first, entry, Now, Out)
				: ApplyFacts(*root, it->first, entry, Now, item, Out);
			if (bDone)
			{
				root->Pending.erase(it);
			}
		}
		SortReady(Out, first);
	}

	// The paths in the groups in flight (stale ones included: they still occupy the pool).
	[[nodiscard]] size_t CountInFlight () const
	{
		size_t count = 0u;
		for (const std::shared_ptr<SVerifyGroup>& group : InFlight)
		{
			count += group->Items.size();
		}
		return count;
	}

	// The per-Pump cap (MaxChangesPerPump; 0 counts as 1).
	[[nodiscard]] size_t GetCap () const
	{
		return Settings.MaxChangesPerPump > 0u ? Settings.MaxChangesPerPump : 1u;
	}

	// Pump: applies the finished groups in flight, oldest first, each whole, up to the first that is
	// still running, and no more once Budget paths have been applied (the group that reaches it is
	// applied whole). A group the pool's drain dropped is forgotten: its entries are checked again when
	// the next Pump finds them due. Returns the paths applied.
	size_t ApplyFinished (uint64 Now, std::vector<SReady>& Out, size_t Budget)
	{
		size_t applied = 0u;
		while (!InFlight.empty() && applied < Budget)
		{
			const std::shared_ptr<SVerifyGroup> group = InFlight.front();
			if (!group->IsDone())
			{
				break;
			}
			InFlight.pop_front();
			const bool bWhole = !group->IsAbandoned();
			applied += bWhole ? group->Items.size() : 0u;
			ApplyGroup(*group, Now, bWhole ? EApply::Facts : EApply::Forget, Out);
		}
		return applied;
	}

	// Flush: its own group, waited for up to WaitSeconds, then applied by its checks, or - still running,
	// or dropped by the pool's drain - resolved inline (which reads only what the owner wrote into the
	// items, never the facts a worker may still be writing). The older groups stay in flight for a
	// later Pump: what of theirs the fences cover was taken into this one, and their stale items are
	// skipped when they are applied.
	void ApplyFlushGroup (const std::shared_ptr<SVerifyGroup>& Group, uint64 Now, double WaitSeconds, std::vector<SReady>& Out)
	{
		const bool bDone = Group->WaitDone(WaitSeconds);
		std::erase(InFlight, Group);
		ApplyGroup(*Group, Now, bDone && !Group->IsAbandoned() ? EApply::Facts : EApply::Inline, Out);
	}

	// ---------------- Settling and resolving ----------------

	// One settled entry a Pump may resolve.
	struct SCandidate
	{
		const FString* Key = nullptr;
		SPending* Entry = nullptr;
		bool bWhole = false;   // of a held release or under a fence: taken whatever the cap
		bool bTaken = false;
	};

	// Takes every candidate, or at most Budget of them: the whole ones, then the oldest (by first event,
	// then key), then the rest of every directory walk one of those came from.
	void SelectCandidates (std::vector<std::vector<SCandidate>>& Candidates, size_t Total, size_t Budget)
	{
		const size_t cap = Budget;
		std::vector<SCandidate*> rest;
		size_t taken = 0u;
		for (std::vector<SCandidate>& ofRoot : Candidates)
		{
			for (SCandidate& candidate : ofRoot)
			{
				candidate.bTaken = Total <= cap || candidate.bWhole;
				taken += candidate.bTaken ? 1u : 0u;
				if (!candidate.bTaken)
				{
					rest.push_back(&candidate);
				}
			}
		}
		if (rest.empty())
		{
			return;
		}
		std::sort(rest.begin(), rest.end(), [] (const SCandidate* A, const SCandidate* B)
		{
			if (A->Entry->FirstSeen != B->Entry->FirstSeen)
			{
				return A->Entry->FirstSeen < B->Entry->FirstSeen;
			}
			return *A->Key < *B->Key;
		});
		std::unordered_set<uint64> cohorts;
		for (SCandidate* candidate : rest)
		{
			if (taken >= cap)
			{
				break;
			}
			candidate->bTaken = true;
			++taken;
			if (candidate->Entry->Cohort != 0u)
			{
				cohorts.insert(candidate->Entry->Cohort);
			}
		}
		bool bLeft = false;
		for (SCandidate* candidate : rest)
		{
			if (!candidate->bTaken && candidate->Entry->Cohort != 0u && cohorts.contains(candidate->Entry->Cohort))
			{
				candidate->bTaken = true;
			}
			bLeft = bLeft || !candidate->bTaken;
		}
		if (bLeft)
		{
			++Stats.CappedPumps;
		}
	}

	// Everything that has settled (or that one of Fences covers), resolved, in root order and sorted by
	// key within each root: told at once to the subscriptions that do not verify, and for the others
	// checked against the disk - inline without a pool, else in one group submitted to it at High
	// priority (a few stats a path, and what a hot reload waits for), applied by a later Pump. At most
	// Budget entries besides the whole ones (SelectCandidates). OutFlushGroup (Flush): the fenced entries
	// an older group is checking are taken too, and the group is handed back for Flush to wait for.
	std::vector<SReady> CollectReady (uint64 Now, std::span<const SFence> Fences, size_t Budget,
		std::shared_ptr<SVerifyGroup>* OutFlushGroup = nullptr)
	{
		const bool bFlush = OutFlushGroup != nullptr;
		std::vector<SReady> ready;
		bool bReleaseAll = false;
		const bool bHold = UpdateHold(Now, bReleaseAll);
		if (std::none_of(Roots.begin(), Roots.end(), [] (const std::unique_ptr<SRoot>& Root) { return !Root->Pending.empty() || Root->bRescanPending; }))
		{
			return ready;   // the common frame: nothing happened
		}

		std::vector<std::vector<SCandidate>> candidates(Roots.size());
		std::vector<uint8> rescans(Roots.size(), 0u);
		size_t total = 0u;
		for (size_t r = 0u; r < Roots.size(); ++r)
		{
			SRoot& root = *Roots[r];
			const auto fenceIt = std::find_if(Fences.begin(), Fences.end(), [&root] (const SFence& Fence) { return Fence.Root == &root; });
			const SFence* fence = fenceIt != Fences.end() ? &*fenceIt : nullptr;
			const bool bFenced = fence != nullptr;
			if (bHold && !bFenced)
			{
				continue;   // nothing of it is due: not even worth a walk
			}

			if (root.bRescanPending)
			{
				const bool bDue = (bFenced && root.RescanSince <= fence->Time)
					|| (!bHold && (bReleaseAll || Elapsed(root.RescanSince, Now) >= SettleTicks));
				if (bDue)
				{
					// One Rescan of the root stands for everything pending in it (checks in flight included).
					rescans[r] = 1u;
					++Stats.Rescans;
					root.bRescanPending = false;
					root.Pending.clear();
				}
				continue;
			}

			EnumerateAppeared(root, Now, bHold, bReleaseAll, fence);
			for (auto& [key, entry] : root.Pending)
			{
				const bool bForced = bFenced && entry.LastSeen <= fence->Time;
				if (!IsDue(entry, Now, bHold, bReleaseAll, fence) || (!bForced && Now < entry.NextProbe))
				{
					continue;
				}
				if (IsAwaiting(entry) && !(bFlush && bForced))
				{
					continue;
				}
				candidates[r].push_back({ &key, &entry, bReleaseAll || bForced, false });
				++total;
			}
		}
		SelectCandidates(candidates, total, Budget);

		std::shared_ptr<SVerifyGroup> group;
		std::vector<FString> done;
		for (size_t r = 0u; r < Roots.size(); ++r)
		{
			SRoot& root = *Roots[r];
			const size_t firstOfRoot = ready.size();
			if (rescans[r] != 0u)
			{
				SReady rescan;
				rescan.Root = &root;
				rescan.Kind = EFileChange::Rescan;
				rescan.bDirectory = true;
				ready.push_back(std::move(rescan));
				continue;
			}
			if (candidates[r].empty())
			{
				continue;
			}
			const bool bAnyVerifying = HasAudience(root, true);
			const bool bAnyUnverified = HasAudience(root, false);
			done.clear();
			for (const SCandidate& candidate : candidates[r])
			{
				if (!candidate.bTaken)
				{
					continue;
				}
				const FString& key = *candidate.Key;
				SPending& entry = *candidate.Entry;
				if (!entry.bToldUnverified)
				{
					entry.bToldUnverified = true;
					if (bAnyUnverified && (entry.Type != EEntry::File || IsRelevant(root, entry, key, false)))
					{
						DecideFromEvents(root, key, entry, ready);
					}
				}
				const bool bVerify = bAnyVerifying && entry.Type != EEntry::DirectoryAdded
					&& (entry.Type != EEntry::File || IsRelevant(root, entry, key, true));
				if (!bVerify)
				{
					done.push_back(key);
				}
				else if (Pool != nullptr)
				{
					if (!group)
					{
						group = std::make_shared<SVerifyGroup>();
						group->Id = NextGroupId++;
					}
					entry.Ticket = group->Id;
					group->Items.push_back(MakeVerifyItem(root, key, entry));
				}
				else if (ResolveInline(root, key, entry, Now, ready))
				{
					done.push_back(key);
				}
			}
			for (const FString& key : done)
			{
				root.Pending.erase(key);
			}
			SortReady(ready, firstOfRoot);
		}
		if (group)
		{
			InFlight.push_back(group);
			Stats.VerifyTasks += SubmitGroup(*Pool, group, jobs::EWorkPriority::High);
			if (bFlush)
			{
				*OutFlushGroup = group;
			}
		}
		return ready;
	}

	// ---------------- Delivery ----------------

	// Path (root-relative) as the subscriber spells it: below its directory, the directory it gave joined
	// with the names below it; elsewhere in the root, the root's path.
	std::filesystem::path ToSubscriberPath (const SSubscription& Subscription, FStringView Path, FString* OutRelative) const
	{
		const FString key = MakeKey(*Subscription.Root, Path);
		if (IsAtOrBelow(key, Subscription.SubKey))
		{
			const FStringView below = DropNames(Path, CountNames(Subscription.SubPath));
			if (OutRelative != nullptr)
			{
				*OutRelative = FString(below);
			}
			return below.IsEmpty() ? Subscription.Directory : (Subscription.Directory / below.ToPath()).make_preferred();
		}
		return MakeFullPath(*Subscription.Root, Path);
	}

	SFileChange Compose (const SSubscription& Subscription, EFileChange Kind, bool bDirectory, FStringView Path, FStringView OldPath) const
	{
		SFileChange change;
		change.Kind = Kind;
		change.bDirectory = bDirectory;
		change.Path = ToSubscriberPath(Subscription, Path, &change.RelativePath);
		if (!OldPath.IsEmpty())
		{
			change.OldPath = ToSubscriberPath(Subscription, OldPath, nullptr);
		}
		return change;
	}

	// Ready, cut to Subscription's region (see SWatchFilter), appended to Out when it matches. All is the
	// whole batch Ready belongs to.
	void AppendMatches (const SSubscription& Subscription, const SReady& Ready, const std::vector<SReady>& All,
		std::vector<SFileChange>& Out) const
	{
		if (!IsFor(Ready.Audience, Subscription.bVerify))
		{
			return;
		}
		const FString& region = Subscription.RegionKey;
		const bool bMove = Ready.Kind == EFileChange::Moved;
		if (Ready.bDirectory)
		{
			if (IsAtOrBelow(Ready.Key, region) || (bMove && IsAtOrBelow(Ready.OldKey, region)))
			{
				Out.push_back(Compose(Subscription, Ready.Kind, true, Ready.Path, Ready.OldPath));
			}
			else if (bMove && IsAtOrBelow(region, Ready.OldKey))
			{
				// The whole region moved along with a directory above it.
				const FString moved = JoinPath(Ready.Path, DropNames(Subscription.RegionPath, CountNames(Ready.OldPath)));
				Out.push_back(Compose(Subscription, EFileChange::Moved, true, moved, Subscription.RegionPath));
			}
			else if (IsAtOrBelow(region, Ready.Key))
			{
				// A record about a subtree that holds the whole region (a removed parent, a rescanned root, a
				// directory moved over it): told about the region as a whole.
				Out.push_back(Compose(Subscription, bMove ? EFileChange::Rescan : Ready.Kind, true, Subscription.RegionPath, {}));
			}
			return;
		}
		if (Ready.bMaybeDirectory)
		{
			// A removal of unknown type (no entry types: a share, FAT, ReFS) may have been a directory:
			// everything at or below the path is gone - true of a file as well - whatever the filter.
			if (IsAtOrBelow(Ready.Key, region))
			{
				Out.push_back(Compose(Subscription, Ready.Kind, true, Ready.Path, {}));
			}
			else if (IsAtOrBelow(region, Ready.Key))
			{
				Out.push_back(Compose(Subscription, Ready.Kind, true, Subscription.RegionPath, {}));
			}
			return;
		}
		const bool bNewMatches = MatchesFile(Subscription, Ready.Key, Ready.Path);
		const bool bOldMatches = !Ready.OldKey.IsEmpty() && MatchesFile(Subscription, Ready.OldKey, Ready.OldPath);
		if (bMove && bNewMatches != bOldMatches)
		{
			// A move across the edge of the filter is, to this subscriber, an addition or a removal - and
			// no removal at all when something else is told at the old path in the same batch (S4: the
			// old file renamed to a backup name, the new one renamed into its place).
			if (bNewMatches)
			{
				Out.push_back(Compose(Subscription, EFileChange::Added, false, Ready.Path, {}));
			}
			else if (std::none_of(All.begin(), All.end(), [&Ready, &Subscription] (const SReady& Other)
				{
					return Other.Root == Ready.Root && !Other.bDirectory && Other.Key == Ready.OldKey && IsFor(Other.Audience, Subscription.bVerify);
				}))
			{
				Out.push_back(Compose(Subscription, EFileChange::Removed, false, Ready.OldPath, {}));
			}
			return;
		}
		if (bNewMatches || bOldMatches)
		{
			Out.push_back(Compose(Subscription, Ready.Kind, false, Ready.Path, Ready.OldPath));
		}
	}

	void Deliver (const std::vector<SReady>& Ready)
	{
		if (Ready.empty())
		{
			return;
		}
		++DeliveryDepth;
		std::vector<SFileChange> changes;
		// Subscriptions neither grows nor shrinks during a delivery: adds wait in AddedDuringDelivery,
		// removes only clear the id. Indexed: a callback's Subscribe never moves the vector.
		const size_t count = Subscriptions.size();
		for (size_t i = 0u; i < count; ++i)
		{
			if (Subscriptions[i].Id == 0u)
			{
				continue;
			}
			changes.clear();
			for (const SReady& ready : Ready)
			{
				if (ready.Root == Subscriptions[i].Root)
				{
					AppendMatches(Subscriptions[i], ready, Ready, changes);
				}
			}
			if (!changes.empty())
			{
				++Stats.Deliveries;
				Stats.Changes += changes.size();
				Subscriptions[i].Callback(std::span<const SFileChange>(changes));
			}
		}
		--DeliveryDepth;
		if (DeliveryDepth == 0u)
		{
			FinishDelivery();
		}
	}

	void FinishDelivery ()
	{
		std::vector<SSubscription> removed;
		if (RemovedDuringDelivery > 0u)
		{
			for (size_t i = Subscriptions.size(); i-- > 0u;)
			{
				if (Subscriptions[i].Id == 0u)
				{
					removed.push_back(std::move(Subscriptions[i]));
					Subscriptions.erase(Subscriptions.begin() + static_cast<std::ptrdiff_t>(i));
				}
			}
			RemovedDuringDelivery = 0u;
		}
		for (SSubscription& added : AddedDuringDelivery)
		{
			Subscriptions.push_back(std::move(added));
		}
		AddedDuringDelivery.clear();
		ReleaseUnusedRoots();
		// The removed callbacks are destroyed last, with the lists consistent.
	}

	void Pump ()
	{
		CheckOwner();
		frt_assert(DeliveryDepth == 0u)
		SeenFences.clear();   // only a Flush waits for its fence, inside the Flush
		PumpEvents();
		const uint64 now = platform::ReadTimestamp();
		PollRoots(now);
		std::erase_if(Expected, [now] (const auto& Item) { return Item.second.Deadline <= now; });
		// The checks earlier Pumps sent to the pool, then what has settled since: one delivery. The cap
		// counts both, and the checks still in flight: a slow pool holds changes back instead of
		// piling them into one Pump.
		std::vector<SReady> ready;
		const size_t cap = GetCap();
		const size_t busy = ApplyFinished(now, ready, cap) + CountInFlight();
		std::vector<SReady> collected = CollectReady(now, {}, busy < cap ? cap - busy : 0u);
		ready.insert(ready.end(), std::make_move_iterator(collected.begin()), std::make_move_iterator(collected.end()));
		SortReady(ready, 0u);
		Deliver(ready);
	}
};


CFileWatcherService::CFileWatcherService (const SFileWatcherSettings& Settings, std::unique_ptr<platform::IFileWatcher> Watcher)
	: State(std::make_unique<SState>(Settings, std::move(Watcher)))
{}

CFileWatcherService::~CFileWatcherService ()
{
	State->CheckOwner();
	for (const std::unique_ptr<SState::SRoot>& root : State->Roots)
	{
		State->DisarmRoot(*root);
	}
}

SDelegateHandle CFileWatcherService::Subscribe (const std::filesystem::path& Directory, const SWatchFilter& Filter,
	FFileChangesCallback Callback)
{
	SState& state = *State;
	state.CheckOwner();
	if (!Callback)
	{
		return {};
	}

	SState::SSubscription subscription;
	subscription.Id = detail::NewDelegateHandleId();
	subscription.Directory = platform::NormalizePath(Directory);
	subscription.Extensions = Filter.Extensions;
	subscription.bVerify = Filter.bVerify;
	subscription.Callback = std::move(Callback);

	FString relative;
	SState::SRoot* root = state.FindRoot(subscription.Directory, &relative);
	if (root == nullptr)
	{
		// A new root: at the mount that holds the directory, else at the directory itself.
		relative.Clear();
		const std::filesystem::path* mount = state.FindMount(subscription.Directory, &relative);
		auto newRoot = std::make_unique<SState::SRoot>();
		newRoot->Path = mount != nullptr ? *mount : subscription.Directory;
		newRoot->Serial = state.NextRootSerial++;
		root = newRoot.get();
		state.Roots.push_back(std::move(newRoot));
	}
	subscription.Root = root;
	subscription.SubPath = std::move(relative);
	FString prefix = Filter.Prefix;
	prefix.ReplaceCharInline('\\', '/');
	state.ComputeSubscriptionKeys(subscription, prefix.View().TrimStartAndEnd("/"));
	++root->SubscriptionCount;

	const SDelegateHandle handle{ subscription.Id };
	(state.DeliveryDepth > 0u ? state.AddedDuringDelivery : state.Subscriptions).push_back(std::move(subscription));
	if (root->State == SState::ERootState::Unarmed)
	{
		state.ArmRoot(*root, platform::ReadTimestamp());
	}
	return handle;
}

void CFileWatcherService::AddMount (const std::filesystem::path& Directory)
{
	State->CheckOwner();
	State->Mounts.push_back(platform::NormalizePath(Directory));
}

bool CFileWatcherService::Unsubscribe (SDelegateHandle Handle)
{
	SState& state = *State;
	state.CheckOwner();
	if (!Handle.IsValid())
	{
		return false;
	}
	for (size_t i = 0u; i < state.Subscriptions.size(); ++i)
	{
		SState::SSubscription& subscription = state.Subscriptions[i];
		if (subscription.Id != Handle.Id)
		{
			continue;
		}
		--subscription.Root->SubscriptionCount;
		if (state.DeliveryDepth > 0u)
		{
			// It may be running: destroyed when the delivery ends.
			subscription.Id = 0u;
			++state.RemovedDuringDelivery;
			return true;
		}
		SState::SSubscription removed = std::move(subscription);
		state.Subscriptions.erase(state.Subscriptions.begin() + static_cast<std::ptrdiff_t>(i));
		state.ReleaseUnusedRoots();
		return true;
	}
	for (size_t i = 0u; i < state.AddedDuringDelivery.size(); ++i)
	{
		if (state.AddedDuringDelivery[i].Id == Handle.Id)
		{
			--state.AddedDuringDelivery[i].Root->SubscriptionCount;
			SState::SSubscription removed = std::move(state.AddedDuringDelivery[i]);
			state.AddedDuringDelivery.erase(state.AddedDuringDelivery.begin() + static_cast<std::ptrdiff_t>(i));
			return true;
		}
	}
	return false;
}

void CFileWatcherService::SetEnabled (bool bEnabled, std::span<const std::filesystem::path> OnlyUnder)
{
	SState& state = *State;
	state.CheckOwner();
	state.Settings.bEnabled = bEnabled;
	state.EnabledOnlyUnder.clear();
	if (!bEnabled)
	{
		for (const std::unique_ptr<SState::SRoot>& root : state.Roots)
		{
			state.DisarmRoot(*root);
		}
		state.Removals.clear();
		state.bHolding = false;
		state.ActivityEvents = 0u;
		return;
	}
	for (const std::filesystem::path& directory : OnlyUnder)
	{
		state.EnabledOnlyUnder.push_back(platform::NormalizePath(directory));
	}
	// Roots armed already stay armed (watching them did not stop); the others are armed when they are
	// in scope now, and disarmed when they left it.
	const uint64 now = platform::ReadTimestamp();
	for (const std::unique_ptr<SState::SRoot>& root : state.Roots)
	{
		if (!state.IsWatchingOn(*root))
		{
			if (root->State != SState::ERootState::Unarmed)
			{
				state.DisarmRoot(*root);
			}
		}
		else if (root->State == SState::ERootState::Unarmed)
		{
			state.ArmRoot(*root, now);
		}
	}
}

bool CFileWatcherService::IsEnabled () const noexcept
{
	return State->Settings.bEnabled;
}

void CFileWatcherService::SetWorkerPool (jobs::CWorkerPool* Pool)
{
	State->CheckOwner();
	State->Pool = Pool;
}

void CFileWatcherService::Pump ()
{
	FRT_CPU_SCOPE("Assets.FileWatcher");
	State->Pump();
}

bool CFileWatcherService::Flush (const std::filesystem::path& Directory, double TimeoutSeconds)
{
	SState& state = *State;
	state.CheckOwner();
	frt_assert(state.DeliveryDepth == 0u)
	if (!state.Watcher)
	{
		return false;
	}

	// Every armed root that watches some of Directory - one that holds it, or one below it - gets a
	// fence of its own, in its own directory. Roots overlap when a directory was subscribed before one
	// above it, and each subscription hears only its own root, so a fence in one of them would leave
	// the others' subscribers to their windows.
	struct SWaiting
	{
		SState::SRoot* Root = nullptr;
		FString Name;
		std::filesystem::path File;
		uint64 Written = 0u;   // before the fence was written
		std::optional<uint64> Time;
		bool bByLoss = false;  // the root lost events after the fence was written: its own may be among them
	};
	const std::filesystem::path directory = platform::NormalizePath(Directory);
	std::vector<SWaiting> waiting;
	bool bAllWritten = true;
	for (const std::unique_ptr<SState::SRoot>& root : state.Roots)
	{
		if (root->State != SState::ERootState::Armed || !SState::Overlaps(*root, directory))
		{
			continue;
		}
		SWaiting fence;
		fence.Root = root.get();
		fence.Name = FString(FencePrefix);
		fence.Name.AppendFormat("{}-{}", platform::GetCurrentProcessId(), ++state.FenceCounter);
		fence.File = root->Path / fence.Name.ToPath();
		fence.Written = platform::ReadTimestamp();
		if (platform::WriteFileBytes(fence.File, {}, false))
		{
			waiting.push_back(std::move(fence));
		}
		else
		{
			bAllWritten = false;
		}
	}
	if (waiting.empty())
	{
		return false;
	}

	// Events of one root arrive in order: once its fence's has, everything before it has too.
	const uint64 frequency = platform::GetTimestampFrequency();
	const uint64 deadline = platform::MakeDeadline(TimeoutSeconds);
	while (true)
	{
		state.PumpEvents(true);   // events of unknown entry type are classified inline here
		bool bAllSeen = true;
		bool bAnyArmed = false;
		for (SWaiting& fence : waiting)
		{
			if (!fence.Time)
			{
				const auto seen = std::find_if(state.SeenFences.begin(), state.SeenFences.end(),
					[&fence] (const SState::SFence& Seen) { return Seen.Root == fence.Root && Seen.Name == fence.Name; });
				if (seen != state.SeenFences.end())
				{
					fence.Time = seen->Time;
				}
				else if (fence.Root->LastLossTime >= fence.Written)
				{
					// An Overflow or a RootLost after the fence was written: the fence's own event may be
					// lost with the rest, and the Rescan the loss marked stands for everything pending in
					// the root - due at once, below, as of the loss.
					fence.Time = fence.Root->LastLossTime;
					fence.bByLoss = true;
				}
			}
			bAllSeen = bAllSeen && fence.Time.has_value();
			bAnyArmed = bAnyArmed || (!fence.Time && fence.Root->State == SState::ERootState::Armed);
		}
		const uint64 now = platform::ReadTimestamp();
		if (bAllSeen || !bAnyArmed || now >= deadline)
		{
			break;
		}
		const double remaining = static_cast<double>(deadline - now) / static_cast<double>(frequency);
		state.Watcher->WaitForEvents(remaining < 0.05 ? remaining : 0.05);
	}

	const uint64 now = platform::ReadTimestamp();
	std::vector<SState::SFence> settled;
	bool bAllFenced = bAllWritten;
	for (SWaiting& fence : waiting)
	{
		if (fence.Time)
		{
			if (fence.bByLoss && fence.Root->LastLossTime > *fence.Time)
			{
				fence.Time = fence.Root->LastLossTime;   // lost more while the others were awaited
			}
			// Both halves of a move arrive together: a removal older than the fence will not pair any more.
			state.ExpireRemovals(*fence.Time, fence.Root);
			settled.push_back({ fence.Root, *fence.Time, {} });
		}
		bAllFenced = bAllFenced && fence.Time.has_value();
	}
	state.PollRoots(now);
	std::erase_if(state.Expected, [now] (const auto& Item) { return Item.second.Deadline <= now; });
	// With a pool, the checks of what the fences cover go to it in one group of their own, at the front
	// of its queue; Flush waits a moment for that group (not for the older ones: what of theirs it
	// covers is in it) and resolves inline what the pool has not got to by then: synchronous all the
	// same, one delivery.
	const size_t inFlight = state.CountInFlight();
	const size_t cap = state.GetCap();
	std::shared_ptr<SVerifyGroup> group;
	std::vector<SState::SReady> ready = state.CollectReady(now, settled, inFlight < cap ? cap - inFlight : 0u, &group);
	if (group)
	{
		const uint64 waitStart = platform::ReadTimestamp();
		const double remaining = waitStart < deadline ? static_cast<double>(deadline - waitStart) / static_cast<double>(frequency) : 0.0;
		state.ApplyFlushGroup(group, now, remaining < FlushPoolWaitSeconds ? remaining : FlushPoolWaitSeconds, ready);
	}
	state.SortReady(ready, 0u);
	state.Deliver(ready);
	for (const SWaiting& fence : waiting)
	{
		platform::RemoveFile(fence.File);
	}
	return bAllFenced;
}

void CFileWatcherService::ExpectWrite (const std::filesystem::path& Path, const hash::SContentHash& Hash, uint64 Size, uint64 Deadline)
{
	State->CheckOwner();
	State->Expected.insert_or_assign(MakeExpectedKey(Path), SState::SExpected{ Hash, Size, Deadline });
}

void CFileWatcherService::ExpectWrite (const std::filesystem::path& Path, std::span<const uint8> Bytes, double TimeoutSeconds)
{
	ExpectWrite(Path, hash::HashContent(Bytes), Bytes.size(), platform::MakeDeadline(TimeoutSeconds));
}

void CFileWatcherService::SetVcsBusyQuery (TUniqueFunction<bool ()> IsBusy)
{
	State->CheckOwner();
	State->VcsBusy = std::move(IsBusy);
	State->bVcsBusy = false;
}

SFileWatcherStats CFileWatcherService::GetStats () const
{
	const SState& state = *State;
	SFileWatcherStats stats = state.Stats;
	stats.Subscriptions = 0u;
	for (const SState::SSubscription& subscription : state.Subscriptions)
	{
		stats.Subscriptions += subscription.Id != 0u ? 1u : 0u;
	}
	stats.Subscriptions += static_cast<uint32>(state.AddedDuringDelivery.size());
	for (const std::unique_ptr<SState::SRoot>& root : state.Roots)
	{
		stats.Roots += root->State == SState::ERootState::Armed || root->State == SState::ERootState::Lost ? 1u : 0u;
		stats.PendingPaths += static_cast<uint32>(root->Pending.size());
	}
	for (const std::shared_ptr<SVerifyGroup>& group : state.InFlight)
	{
		stats.VerifyingPaths += static_cast<uint32>(group->Items.size());
	}
	stats.bHolding = state.bHolding;
	return stats;
}

bool CFileWatcherService::IsIdle () const
{
	return !State->bHolding && !State->HasPending();
}

const SFileWatcherSettings& CFileWatcherService::GetSettings () const noexcept
{
	return State->Settings;
}

namespace
{
// A loaded file's path in the form changes are spelled (absolute, normal), made so only when it is not
// absolute already; then its key.
FString MakeFileKey (const std::filesystem::path& File)
{
	const std::filesystem::path path = File.is_absolute() ? File.lexically_normal() : platform::NormalizePath(File);
	return MakePathKey(FString::FromPath(path, EPathStyle::Generic));
}
}

bool IsFileAffected (const SFileChange& Change, const std::filesystem::path& File)
{
	if (Change.Kind == EFileChange::Removed)
	{
		return false;
	}
	// Change.Path is absolute and normal already: the service spells it so.
	const FString fileKey = MakeFileKey(File);
	const FString changeKey = MakePathKey(FString::FromPath(Change.Path, EPathStyle::Generic));
	return Change.bDirectory ? IsAtOrBelow(fileKey, changeKey) : fileKey == changeKey;
}

CAffectedFiles::CAffectedFiles (std::span<const SFileChange> Changes)
{
	for (const SFileChange& change : Changes)
	{
		if (change.Kind == EFileChange::Removed)
		{
			continue;
		}
		FString key = MakePathKey(FString::FromPath(change.Path, EPathStyle::Generic));
		if (change.bDirectory)
		{
			Directories.push_back(std::move(key));
		}
		else
		{
			Files.insert(std::move(key));
		}
	}
}

bool CAffectedFiles::Contains (const std::filesystem::path& File) const
{
	if (IsEmpty())
	{
		return false;
	}
	const FString key = MakeFileKey(File);
	return Files.contains(key)
		|| std::any_of(Directories.begin(), Directories.end(), [&key] (const FString& Directory) { return IsAtOrBelow(key, Directory); });
}

bool CAffectedFiles::IsEmpty () const noexcept
{
	return Files.empty() && Directories.empty();
}
}
