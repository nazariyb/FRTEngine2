#include "Assets/FileWatcherService.h"

#include <algorithm>
#include <optional>
#include <system_error>
#include <unordered_map>
#include <utility>

#include "Asserts.h"
#include "Config/Config.h"
#include "Containers/StringMap.h"
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

std::optional<hash::SContentHash> HashFileStable (const std::filesystem::path& Path)
{
	hash::CContentHasher hasher;
	const platform::EStableRead read = platform::ReadFileChunksStable(Path, [&hasher] (std::span<const uint8> Bytes) { hasher.Update(Bytes); });
	if (read != platform::EStableRead::Stable)
	{
		return std::nullopt;
	}
	return hasher.Finalize();
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
		FFileChangesCallback Callback;
	};

	// A change that has settled, before it is cut to each subscription.
	struct SReady
	{
		SRoot* Root = nullptr;
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

	// Some live subscription of the root would be told about this entry: only those are stat'ed,
	// probed and hashed.
	[[nodiscard]] bool IsRelevant (const SRoot& Root, const SPending& Entry, FStringView Key) const
	{
		const FString movedKey = Entry.MovedFrom.IsEmpty() ? FString() : MakeKey(Root, Entry.MovedFrom);
		for (const SSubscription& subscription : Subscriptions)
		{
			if (subscription.Id == 0u || subscription.Root != &Root)
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

	// ---------------- Events ----------------

	void PumpEvents ()
	{
		if (!Watcher)
		{
			return;
		}
		EventBuffer.clear();
		Watcher->TakeEvents(EventBuffer);
		for (const platform::SFileEvent& event : EventBuffer)
		{
			Ingest(event);
		}
		// Removals older than the pair window can no longer pair: by the newest event, and by the clock,
		// so that a removal no event follows - a directory removed last, whose left-behind removals
		// wait for its expiry - is not kept until something unrelated happens. The clock's bound leaves
		// one more pair window for events the watcher thread read (and stamped) but has not queued yet.
		const uint64 now = platform::ReadTimestamp();
		const uint64 byEvents = LastEventTime > PairTicks ? LastEventTime - PairTicks - 1u : 0u;
		const uint64 byClock = now > 2u * PairTicks ? now - 2u * PairTicks : 0u;
		const uint64 expireUpTo = byEvents > byClock ? byEvents : byClock;
		if (expireUpTo > 0u)
		{
			ExpireRemovals(expireUpTo);
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
				it->second.LastSeen = entry.LastSeen > it->second.LastSeen ? entry.LastSeen : it->second.LastSeen;
			}
		}
		LeftBehind.clear();
	}

	void Ingest (const platform::SFileEvent& Event)
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
			const std::optional<platform::SFileStat> stat = platform::Stat(MakeFullPath(root, Event.Path));
			bDirectory = stat && stat->bDirectory;
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
		entry.LastSeen = Time > entry.LastSeen ? Time : entry.LastSeen;
		return entry;
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
			entry.LastSeen = Time > entry.LastSeen ? Time : entry.LastSeen;
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

	// Every file at or below Directory (root-relative) becomes an Added entry: a directory that moved in
	// reports nothing for its children (S13).
	void EnumerateInto (SRoot& Root, FStringView Directory, uint64 Time)
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
			EnumerateInto(Root, directory.Path, directory.Time);
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
			if (!root->Pending.empty() || root->bRescanPending)
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

	// What a settled entry turns out to be, by the disk now: Tell (Out filled), Drop (nothing to tell,
	// or a self-write), Keep (a writer holds it, or it changed under the probe: wait),
	// TellAndResolveAgain (Out filled, and the entry, changed into another, is resolved once more).
	EOutcome Resolve (SRoot& Root, const FString& Key, SPending& Entry, uint64 Now, SReady& Out)
	{
		Out.Root = &Root;
		Out.Path = Entry.Path;
		Out.Key = Key;
		const std::filesystem::path full = MakeFullPath(Root, Entry.Path);

		if (Entry.Type == EEntry::DirectoryRemoved)
		{
			const std::optional<platform::SFileStat> stat = platform::Stat(full);
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
			const std::optional<platform::SFileStat> stat = platform::Stat(full);
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

		if (!IsRelevant(Root, Entry, Key))
		{
			return EOutcome::Drop;
		}
		const std::optional<platform::SFileStat> stat = platform::Stat(full);
		if (stat && stat->bDirectory)
		{
			// A directory where a file event was (a file system without entry types, or a file replaced
			// by a directory): what is below it is unknown.
			Out.Kind = EFileChange::Rescan;
			Out.bDirectory = true;
			return EOutcome::Tell;
		}
		const bool bExists = stat.has_value();
		if (Entry.bMovedAway && !bExists)
		{
			return EOutcome::Drop;   // where its file went tells the move
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
			return EOutcome::Drop;   // born and gone within the window: a temporary file
		}
		if (!Out.OldPath.IsEmpty())
		{
			Out.OldKey = MakeKey(Root, Out.OldPath);
		}
		if (!bExists)
		{
			return EOutcome::Tell;
		}

		// A writer may still be at it (Modified arrives at close, usually; S3, S19).
		switch (platform::ProbeFileWriters(full))
		{
		case platform::EWriterProbe::Held:
			BackOff(Entry, full, Now);
			return EOutcome::Keep;
		case platform::EWriterProbe::Missing:
			// Gone since the stat: settle again.
			Entry.bExistsNow = false;
			Entry.LastSeen = Now;
			return EOutcome::Keep;
		default:
			break;
		}

		// The echo of a write of the caller's own: exactly the bytes it said it would write.
		const auto expected = Expected.find(MakeExpectedKey(full));
		if (expected != Expected.end())
		{
			const bool bMatch = stat->Size == expected->second.Size && HashFileStable(full) == std::optional(expected->second.Hash);
			Expected.erase(expected);
			if (bMatch)
			{
				++Stats.SelfWritesSuppressed;
				return EOutcome::Drop;
			}
		}
		return EOutcome::Tell;
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

	// Everything that has settled (or that one of Fences covers), resolved against the disk, in root
	// order and sorted by key within each root.
	std::vector<SReady> CollectReady (uint64 Now, std::span<const SFence> Fences)
	{
		std::vector<SReady> ready;
		bool bReleaseAll = false;
		const bool bHold = UpdateHold(Now, bReleaseAll);
		for (const std::unique_ptr<SRoot>& rootPtr : Roots)
		{
			SRoot& root = *rootPtr;
			const auto fenceIt = std::find_if(Fences.begin(), Fences.end(), [&root] (const SFence& Fence) { return Fence.Root == &root; });
			const SFence* fence = fenceIt != Fences.end() ? &*fenceIt : nullptr;
			const bool bFenced = fence != nullptr;
			const size_t firstOfRoot = ready.size();
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
					// One Rescan of the root stands for everything pending in it.
					SReady rescan;
					rescan.Root = &root;
					rescan.Kind = EFileChange::Rescan;
					rescan.bDirectory = true;
					ready.push_back(std::move(rescan));
					++Stats.Rescans;
					root.bRescanPending = false;
					root.Pending.clear();
				}
				continue;
			}

			EnumerateAppeared(root, Now, bHold, bReleaseAll, fence);
			for (auto it = root.Pending.begin(); it != root.Pending.end();)
			{
				SPending& entry = it->second;
				const bool bForced = bFenced && entry.LastSeen <= fence->Time;
				const bool bDue = IsDue(entry, Now, bHold, bReleaseAll, fence);
				if (!bDue || (!bForced && Now < entry.NextProbe))
				{
					++it;
					continue;
				}
				SReady change;
				switch (Resolve(root, it->first, entry, Now, change))
				{
				case EOutcome::Tell:
					ready.push_back(std::move(change));
					it = root.Pending.erase(it);
					break;
				case EOutcome::Drop:
					it = root.Pending.erase(it);
					break;
				case EOutcome::Keep:
					++it;
					break;
				case EOutcome::TellAndResolveAgain:
					ready.push_back(std::move(change));
					break;   // the same entry again, changed so that it does not ask for this forever
				}
			}
			// Stable: two changes of one path (a directory removed, a file in its place) keep their order.
			std::stable_sort(ready.begin() + static_cast<std::ptrdiff_t>(firstOfRoot), ready.end(),
				[] (const SReady& A, const SReady& B) { return A.Key < B.Key; });
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
			else if (std::none_of(All.begin(), All.end(), [&Ready] (const SReady& Other)
				{
					return Other.Root == Ready.Root && !Other.bDirectory && Other.Key == Ready.OldKey;
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
		Deliver(CollectReady(now, {}));
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
		state.PumpEvents();
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
	state.Deliver(state.CollectReady(now, settled));
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
