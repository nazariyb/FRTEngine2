#pragma once

#include <span>
#include <vector>

#include "Core.h"
#include "CoreTypes.h"
#include "Assets/Meta/AssetWriter.h"
#include "Assets/Meta/MetaTypes.h"
#include "Assets/Meta/OpJournal.h"
#include "Assets/Vcs/VcsProvider.h"
#include "Containers/String.h"
#include "Diagnostics/Diagnostics.h"


/**
 * M4's commands that need only the operation journal and the asset writer (design §4.6, §12.6): create a meta, move or
 * rename assets (case-only included), delete assets. They live in Core, not in AssetTool, so that the verbs, the lifecycle's
 * repairs and the problems panel are thin callers of one implementation, and Core-Test can run each against the recording
 * provider and in a child process it kills. Duplicate, duplicate folder, consolidate, replace references, extract
 * materials and CSV conversion are M11's and M5's; the lifecycle's own repairs (restore, re-GUID, relink, keep A alias B)
 * are planned in F4.
 *
 * Each command is a plan and a run. Plan* reads what it must (which files exist, the meta whose FormerPaths a move
 * extends) and returns the op, refusing with reasons rather than half-planning: a live host registers the op's expected
 * writes on the owner thread (ForEachExpectedWrite) and runs it on a worker. The one-call forms plan and run at once, for
 * tool hosts and tests. Commands a person runs ignore the Safety switches (§4.7): they always write their metas.
 *
 * Every path is a mount path in a mount of the writer's owner root, as the folder spells it on disk. Problems follow the
 * diagnostics conventions: Subject the mount path, File relative to the owner root, absolute paths only in Message.
 */
namespace frt::assets
{
struct SAssetCommandResult
{
	SVcsResult Vcs;                      // None: done (also when there was nothing to do)
	std::vector<SDiagnostic> Problems;   // why it was refused (errors; VCS002 when the VCS refused), and warnings
	SAssetOp Op;                         // what was planned; empty when it was refused before planning finished
	SAssetWriteReport Report;

	[[nodiscard]] FRT_CORE_API bool IsOk () const noexcept;
};

/**
 * A meta for a file that has none. Meta is what it holds: a template's settings and labels, or just the defaults; an
 * invalid Guid becomes SGuid::NewRandom, which the plan pre-allocates. The lifecycle passes the GUID it derived
 * (MakeCreateMetaGuid) when DerivedAutoGuids is on. An existing meta, in any spelling, refuses: it is never reminted.
 */
struct SCreateMetaRequest
{
	FString Source;
	SAssetMeta Meta;
};

/**
 * One asset to move or rename. Its meta goes along in the same provider call, and the old path is appended to the meta's
 * FormerPaths (path lookups and path references resolve through it, §4.6); a case-only rename records nothing there, since
 * every lookup folds case. Companions are files the importer declares beside the source (a glTF's .bin), moved in the same
 * call; which they are, and their new names, is the caller's to say, because it knows the record. Referrers are not
 * rewritten.
 */
struct SAssetMove
{
	FString From;
	FString To;
	std::vector<SVcsMove> Companions;    // mount paths
};

// One asset to delete: its source, its meta and the companions given, in one provider Delete (the Recycle Bin under None
// and Git). An orphan meta alone may be deleted by naming the meta.
struct SAssetDelete
{
	FString Source;
	std::vector<FString> Companions;
};

[[nodiscard]] FRT_CORE_API bool PlanCreateMeta (const CAssetWriter& Writer, const SCreateMetaRequest& Request, SAssetOp& OutOp,
	std::vector<SDiagnostic>& OutProblems);
// Every move in one op: a folder's worth moves as one operation.
[[nodiscard]] FRT_CORE_API bool PlanMoveAssets (const CAssetWriter& Writer, std::span<const SAssetMove> Moves, SAssetOp& OutOp,
	std::vector<SDiagnostic>& OutProblems);
[[nodiscard]] FRT_CORE_API bool PlanDeleteAssets (const CAssetWriter& Writer, std::span<const SAssetDelete> Deletes, SAssetOp& OutOp,
	std::vector<SDiagnostic>& OutProblems);

// Runs a planned op. A refusal by the VCS is VCS002 on the op's first asset; nothing was written then, unless Report says
// otherwise (a refused Add, after the files were written, leaves the op for RollForward).
[[nodiscard]] FRT_CORE_API SAssetCommandResult RunAssetOp (CAssetWriter& Writer, SAssetOp Op);

[[nodiscard]] FRT_CORE_API SAssetCommandResult CreateMeta (CAssetWriter& Writer, const SCreateMetaRequest& Request);
[[nodiscard]] FRT_CORE_API SAssetCommandResult MoveAssets (CAssetWriter& Writer, std::span<const SAssetMove> Moves);
[[nodiscard]] FRT_CORE_API SAssetCommandResult DeleteAssets (CAssetWriter& Writer, std::span<const SAssetDelete> Deletes);
}
