#pragma once

#include <span>
#include <utility>
#include <vector>

#include "Core.h"
#include "CoreTypes.h"
#include "Assets/Meta/LifecycleTypes.h"
#include "Assets/Meta/Safety.h"
#include "Assets/Registry/RegistryTypes.h"
#include "Assets/Vcs/VcsProvider.h"
#include "Containers/String.h"
#include "Diagnostics/Diagnostics.h"


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
 */
namespace frt::assets
{
/**
 * SAssetRegistryDesc::Lifecycle: null means no lifecycle at all (metas are read when present, a metaless file keeps its
 * provisional id, no META problems, nothing written). bRepairs false reports only (--ci, guests, read-only hosts); true
 * applies repairs on the owner.
 *
 * A mount with no provider here, or a null one, is None, with no process spawned, but only once the mount's provider is
 * settled: a duplicate resolved before detection would go by smallest path instead of committed facts and re-GUID the wrong
 * file. A tool host passes complete pointers. A live host detects on a worker (MakeVcsProvider), so it sets
 * bProvidersPending and calls CAssetRepairs::SetProvider for each mount, null for None; every run holds (bVcsHold) until
 * the last call.
 */
struct SLifecycleDesc
{
	std::vector<std::pair<MountId, IVcsProvider*>> Providers;
	SAssetEditorSettings Settings;
	FString ProjectSalt;              // SProjectDescriptor::GetAssetSalt(), for GetMountAssetSalt; unused by an Engine-owner run
	bool bRepairs = false;
	bool bProvidersPending = false;
};

/**
 * What RunNow may apply. RunLifecycle proposes everything it sees, so each verb names its own Kinds, and an action of another
 * kind stays a proposal: `migrate metas` and `new-meta` take CreateMeta alone; `fix` and `validate --fix` take CreateMeta,
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

/**
 * Owned by CAssetRegistry when SAssetRegistryDesc::Lifecycle is set. Every member runs on the owner thread, because
 * CFileWatcherService::ExpectWrite must and provider spawns and flushed writes must not: the writes run on a worker.
 *
 * RunNow is for tool hosts only and blocks until it has applied. ApplyFix blocks the same way in a tool host. In a live host
 * it registers the expected writes, submits the op and returns a report with only Planned filled; the outcome arrives as the
 * next delta's problems (VCS002 when refused, the fixed problem gone otherwise) and in the registry's counters.
 */
class FRT_CORE_API CAssetRepairs
{
public:
	// A live host's detection finishing (SLifecycleDesc::bProvidersPending); Provider is null for None.
	void SetProvider (MountId Mount, IVcsProvider* Provider);
	// Live: after a pass has committed, never during one. Dirty: the mount paths it touched.
	void OnPassCommitted (std::span<const FString> Dirty);
	// Live: acts once the last result's WakeAtMs has passed, or when the VCS hold (polled while anything is pending) has
	// ended. A test passes its own clock.
	void Tick (uint64 NowMs);
	// AssetTool verbs: synchronous, with no delays.
	[[nodiscard]] SRepairReport RunNow (const SRepairOptions& Options);
	// Fix-its are not stored on a diagnostic: the panel and `validate --fix` ask here, and apply with ApplyFix.
	[[nodiscard]] std::vector<SLifecycleAction> GetFixIts (const SDiagnostic& Problem) const;
	[[nodiscard]] SRepairReport ApplyFix (const SLifecycleAction& FixIt);
};
}
