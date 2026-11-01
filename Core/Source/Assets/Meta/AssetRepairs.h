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
 *   view + memory -> SLifecycleFacts -> RunLifecycle (fetching what it asks for on a worker, then again)
 *     -> States and Problems as one delta -> each repair planned into an SAssetOp -> CAssetWriter::Run on a worker, one op
 *     at a time per owner root -> a rescan of the touched folders
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
 * applies repairs on the owner. A mount with no provider here, or a null one, is None, with no process spawned.
 */
struct SLifecycleDesc
{
	std::vector<std::pair<MountId, IVcsProvider*>> Providers;
	SAssetEditorSettings Settings;
	bool bRepairs = false;
};

struct SRepairOptions
{
	std::vector<FString> Paths;   // empty: every mount the run owns
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

// Owned by CAssetRegistry when SAssetRegistryDesc::Lifecycle is set.
class FRT_CORE_API CAssetRepairs
{
public:
	// Live: after a pass has committed, never during one. Dirty: the mount paths it touched.
	void OnPassCommitted (std::span<const FString> Dirty);
	// Live: acts only once the last result's WakeAtMs has passed. A test passes its own clock.
	void Tick (uint64 NowMs);
	// AssetTool verbs and fix-its: synchronous, with no delays.
	[[nodiscard]] SRepairReport RunNow (const SRepairOptions& Options);
	// Fix-its are not stored on a diagnostic: the panel and `validate --fix` ask here, and apply with ApplyFix.
	[[nodiscard]] std::vector<SLifecycleAction> GetFixIts (const SDiagnostic& Problem) const;
	[[nodiscard]] SRepairReport ApplyFix (const SLifecycleAction& FixIt);
};
}
