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
 * §4.7's ErrorMissingMeta is not a state of its own: it is EMetaState::PendingMeta plus an error-severity META001.
 * MissingMeta is a lost meta (META008).
 *
 * Derived GUIDs are pure, so RunLifecycle computes them itself (MakeCreateMetaGuid, MakeDedupeGuid in MetaPaths.h),
 * re-rolling over LiveGuids and what it proposed in this run. With DerivedAutoGuids off an action's Guid stays invalid
 * and the planner calls SGuid::NewRandom: the function stays pure.
 */
namespace frt::assets
{
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
	bool bLfsPointer = false;
	bool bWritable = false;                      // the mount is owned, this process is the writer, the file is not read-only
	uint64 LastEventMs = 0u;
	uint64 MetaGoneMs = 0u;                      // 0: not gone
};

// A meta whose source is not simply paired with it: an orphan, a broken one, one a batch just dirtied.
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
};

struct SLifecycleTombstone
{
	SGuid Guid;
	FString LastPath;
	hash::SContentHash Hash;
	platform::SFileIdentity Identity;
	uint64 RemovedMs = 0u;
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
	bool bCommand = false;    // a person ran it: no delays, switches ignored (DerivedAutoGuids still picks the GUID kind)
	bool bVcsHold = false;    // the provider is busy, or unsettled: nothing automatic is proposed
	std::span<const SLifecycleSource> Sources;
	std::span<const SLifecycleMeta> Metas;
	std::span<const SLifecycleTombstone> Tombstones;
	std::span<const SLiveGuid> LiveGuids;
	std::span<const SGuid> VerifiedCopies;                  // METAS entries whose hash equals the source's MetaHash
	std::optional<std::span<const FString>> Committed;      // nullopt: not asked yet
};

/**
 * What the lifecycle proposes. RemapRefs carries, as data, what a duplicate's losers must rewrite; in M4 the remap applies
 * to the losers' metas only (SubAssets.Remap, Aliases), since no file holds `$asset` until M5.
 */
enum class ELifecycleAction : uint8 { CreateMeta, RestoreMeta, MoveMeta, ReGuid, RemapRefs, KeepAliasB };

// One flat struct, not a variant: it plans and logs simply, and one type runs from automatic repair to fix-it button.
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
	bool bNeedCommitted = false;
	uint64 WakeAtMs = ~0ull;                          // when a delay runs out; never: ~0
};

[[nodiscard]] FRT_CORE_API SLifecycleResult RunLifecycle (const SLifecycleFacts& Facts);
}


FRT_DECLARE_ENUM_REFLECTION(
	frt::assets::ELifecycleAction,
	FRT_ENUM_ENTRY(frt::assets::ELifecycleAction, CreateMeta),
	FRT_ENUM_ENTRY(frt::assets::ELifecycleAction, RestoreMeta),
	FRT_ENUM_ENTRY(frt::assets::ELifecycleAction, MoveMeta),
	FRT_ENUM_ENTRY(frt::assets::ELifecycleAction, ReGuid),
	FRT_ENUM_ENTRY(frt::assets::ELifecycleAction, RemapRefs),
	FRT_ENUM_ENTRY(frt::assets::ELifecycleAction, KeepAliasB));
