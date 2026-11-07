#pragma once

#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "Core.h"
#include "CoreTypes.h"
#include "Enum.h"
#include "Assets/AssetId.h"
#include "Assets/Meta/MetaTypes.h"
#include "Assets/Meta/Safety.h"
#include "Assets/Registry/RegistryTypes.h"
#include "Containers/String.h"
#include "Diagnostics/Diagnostics.h"
#include "Hash/StableHash.h"
#include "Platform/FileSystem.h"


/**
 * The meta lifecycle's interface (design §4.6, §4.7): every transition is RunLifecycle, a pure function of these facts,
 * with no I/O and no clock (time is an input), so a table of facts and expected results tests every arrow without a file.
 *
 * The hard part of a pure function here is that the facts it needs are not free. Content hashes are lazy (§5.4) and
 * committed facts cost a process spawn, so the function asks instead of reading: a run that returns NeedContentHash or
 * bNeedCommitted proposes no repair for those paths, and the caller fetches on a worker and calls again. Sources and
 * Metas hold only what a batch dirtied plus the rows not Paired, so a quiet warm start does no work, and WakeAtMs says
 * when a delay runs out, so the caller never polls.
 *
 * One run covers one mount: Sources, Metas, Tombstones, Salt and Safety are that mount's, because /Engine and /Project have
 * different salts and each mount has its own switches. A batch across mounts would mint an /Engine meta salted with the
 * project's name, which differs from what `fix --engine` derives. LiveGuids alone spans every mount, so a GUID live in
 * another mount is still a duplicate and still re-rolls.
 *
 * Units. Every Ms here is on one clock, wall-clock milliseconds since the Unix epoch, into which the caller converts a
 * persisted time (FileTimeToLifecycleMs). A test may pass any clock that keeps them comparable, starting above 0, since 0
 * in MetaGoneMs means "not gone". Differences go through GetElapsedMs, because a persisted time can lie ahead of NowMs and
 * an unsigned difference would then read as long past every grace. Every path is a mount path.
 *
 * §4.7's ErrorMissingMeta is not a state of its own: it is EMetaState::PendingMeta plus an error-severity META001.
 * MissingMeta is a lost meta (META008).
 *
 * Derived GUIDs are pure, so RunLifecycle computes them itself (MakeCreateMetaGuid, MakeDedupeGuid in MetaPaths.h),
 * re-rolling over LiveGuids and what it proposed in this run. With DerivedAutoGuids off an action's Guid stays invalid
 * and the planner calls SGuid::NewRandom: the function stays pure.
 */
namespace frt::assets
{
// NowMs - ThenMs, or 0 when ThenMs is later (a clock set back): a delay then runs long, never short.
[[nodiscard]] constexpr uint64 GetElapsedMs (uint64 NowMs, uint64 ThenMs) noexcept
{
	return NowMs > ThenMs ? NowMs - ThenMs : 0u;
}

// A file time (100 ns ticks since 1601-01-01 UTC, the unit of SFileStat and STombstone::RemovedFileTime) as the lifecycle's
// clock. Rounds down; a time before 1970 is 0.
[[nodiscard]] constexpr uint64 FileTimeToLifecycleMs (int64 FileTime) noexcept
{
	constexpr int64 UnixEpochFileTicks = 116444736000000000ll;
	return FileTime > UnixEpochFileTicks ? static_cast<uint64>((FileTime - UnixEpochFileTicks) / 10000) : 0u;
}

// A file of a kind that gets a meta, as the registry and the caller's memory know it.
struct SLifecycleSource
{
	FString MountPath;
	EFileClass Class = EFileClass::Asset;
	platform::SFileIdentity Identity;
	std::optional<hash::SContentHash> Content;   // nullopt: not hashed
	SGuid RecordGuid;                            // invalid: no record yet, or a provisional id
	EMetaState Stored = EMetaState::PendingMeta;
	bool bMetaOnDisk = false;
	bool bMetaCaseDiffers = false;
	FString MetaSpelling;                        // when bMetaCaseDiffers: the meta's mount path as the listing spells it
	bool bLfsPointer = false;
	bool bWritable = false;                      // the mount is owned, this process is the writer, the file is not read-only
	uint64 LastEventMs = 0u;
	uint64 MetaGoneMs = 0u;                      // 0: not gone
};

/**
 * A meta whose source is not simply paired with it: an orphan, a broken one, one a batch just dirtied.
 *
 * bSourceExcluded: the listing holds the source but a rule excludes it, so this is not an orphan: no META002 and no pairing.
 * The scan's flag goes no further than this: E4 commits such a Meta row as Paired (one beside no file at all is Orphan), and
 * RunValidation raises META007 for a Paired Meta row whose source has no FILES row or has class Excluded.
 */
struct SLifecycleMeta
{
	FString MountPath;
	FString SourcePath;
	EMetaParseState Parse = EMetaParseState::Ok;
	SGuid Guid;
	std::vector<SGuid> SalvagedGuids;
	platform::SFileIdentity Identity;
	uint64 FirstSeenMs = 0u;
	bool bWritable = false;
	bool bSourceExcluded = false;
};

struct SLifecycleTombstone
{
	SGuid Guid;
	FString LastPath;
	hash::SContentHash Hash;
	platform::SFileIdentity Identity;
	uint64 RemovedMs = 0u;                       // FileTimeToLifecycleMs(STombstone::RemovedFileTime)
};

struct SLiveGuid
{
	SGuid Guid;
	FString MetaPath;
};

struct SLifecycleFacts
{
	uint64 NowMs = 0u;
	SSafetySwitches Safety;
	SAssetEditorSettings Times;
	FString Salt;
	// A person ran it: no delays; AutoRepairMetas ignored. DerivedAutoGuids still picks the GUID kind, and OrphanPairing and
	// AutoResolveDuplicates still decide: off, a relink or a duplicate stays a fix-it, even for `fix`.
	bool bCommand = false;
	// The provider is busy, unsettled, or not yet known (SLifecycleDesc::bProvidersPending): nothing automatic is proposed. A
	// held run's WakeAtMs says nothing about the hold, which usually ends with no file event (index.lock lies outside the
	// content): the caller polls while anything is pending, and runs again when it ends.
	bool bVcsHold = false;
	std::span<const SLifecycleSource> Sources;
	std::span<const SLifecycleMeta> Metas;
	std::span<const SLifecycleTombstone> Tombstones;
	std::span<const SLiveGuid> LiveGuids;
	std::span<const SGuid> VerifiedCopies;                  // METAS entries whose hash equals the source's MetaHash
	std::optional<std::span<const FString>> Committed;      // the metas (mount paths) the VCS has committed; nullopt: not asked yet
};

/**
 * What the lifecycle proposes.
 *
 * Every loser of one run's duplicates is a ReGuid carrying the whole batch's map, and they are proposed together, because a
 * loser can refer to another loser. The planner puts them in one SAssetOp (AssetRepairs.h), so a crash between two losers'
 * rewrites is rolled forward rather than rerun: rerun, the second run would see fewer duplicates and a smaller map, and a
 * remaining loser would silently keep its reference to the original asset. In M4 the map applies to the losers' metas only
 * (SubAssets.Remap), since no file holds `$asset` until M5, which uses the same map. A loser keeps no Aliases, FormerPaths
 * or FormerNames: they belong to the GUID it gives up, which the winner keeps.
 *
 * MoveMeta and FixMetaCase are apart because `fix` applies a case fix and never a relink.
 */
enum class ELifecycleAction : uint8 { CreateMeta, RestoreMeta, MoveMeta, FixMetaCase, ReGuid, KeepAliasB };

/**
 * One flat struct, not a variant: it plans and logs simply, and one type runs from automatic repair to fix-it button.
 * Path and ToPath are mount paths, and which file they name depends on Kind:
 *
 *   CreateMeta, RestoreMeta   Path: the source; the meta goes to GetMetaPath(Path)
 *   MoveMeta                  Path: the meta as spelled on disk; ToPath: the meta's new path (orphan pairing, relink)
 *   FixMetaCase               Path: SLifecycleSource::MetaSpelling; ToPath: GetMetaPath(source)
 *   ReGuid                    Path: the loser's meta; Guid: its new GUID; Remap: the whole batch's map
 *   KeepAliasB                Path: the meta to rewrite; Guid: the GUID it keeps; Aliases: the ones that resolve to it
 */
struct SLifecycleAction
{
	ELifecycleAction Kind = ELifecycleAction::CreateMeta;
	FString Path;
	FString ToPath;
	SGuid Guid;                                       // invalid with DerivedAutoGuids off: the planner calls NewRandom
	std::vector<SGuid> Aliases;
	std::vector<std::pair<SGuid, SGuid>> Remap;       // old GUID, new GUID
};

struct SLifecycleState
{
	FString MountPath;
	EMetaState State = EMetaState::PendingMeta;
};

struct SLifecycleResult
{
	std::vector<SLifecycleAction> Repairs;            // applied automatically, on the owner
	std::vector<SLifecycleAction> FixIts;             // what a switched-off or unsafe repair becomes
	std::vector<SLifecycleState> States;
	std::vector<SDiagnostic> Problems;                // EProblemSource::Lifecycle
	std::vector<FString> NeedContentHash;
	bool bNeedCommitted = false;                      // ask about every SLiveGuid::MetaPath of this run's duplicate groups
	uint64 WakeAtMs = ~0ull;                          // when a delay runs out; never: ~0
};

[[nodiscard]] FRT_CORE_API SLifecycleResult RunLifecycle (const SLifecycleFacts& Facts);
}


FRT_DECLARE_ENUM_REFLECTION(
	frt::assets::ELifecycleAction,
	FRT_ENUM_ENTRY(frt::assets::ELifecycleAction, CreateMeta),
	FRT_ENUM_ENTRY(frt::assets::ELifecycleAction, RestoreMeta),
	FRT_ENUM_ENTRY(frt::assets::ELifecycleAction, MoveMeta),
	FRT_ENUM_ENTRY(frt::assets::ELifecycleAction, FixMetaCase),
	FRT_ENUM_ENTRY(frt::assets::ELifecycleAction, ReGuid),
	FRT_ENUM_ENTRY(frt::assets::ELifecycleAction, KeepAliasB));
