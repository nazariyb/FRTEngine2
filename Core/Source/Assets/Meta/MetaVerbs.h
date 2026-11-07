#pragma once

#include <filesystem>
#include <span>
#include <vector>

#include "Core.h"
#include "CoreTypes.h"
#include "Assets/Meta/AssetRepairs.h"
#include "Assets/Meta/OpJournal.h"
#include "Containers/String.h"
#include "Diagnostics/Diagnostics.h"


/**
 * The writing meta verbs of AssetTool (design §13.2, §14.8; edits [8], [33], [66]), in Core so that Core-Test runs each
 * against a real registry, the recording provider and a killed child, and AssetTool only parses and prints. Each takes the
 * registry of a tool host, opened with its own SLifecycleDesc (a writer that repairs: bRepairs) over the run's own mount
 * set, /Project and its plugins or /Engine, never both, and acts through F4's CAssetRepairs::RunNow, so a meta a verb
 * writes is derived and journalled exactly as an automatic repair's would be. They are commands a person runs: no delays,
 * no VCS hold, AutoRepairMetas and OrphanPairing ignored; DerivedAutoGuids still picks the GUID kind (edit [33]).
 *
 *   fix / validate --fix   RunNow with SafeFixKinds (Validation.h): no function of its own here
 *   new-meta <file...>     NewMetas: a meta now for each file a person names, an existing meta being an error
 *   migrate metas          MigrateMetas: a meta for every included file of the mounts that has none, then the record
 *   resave --metas         PlanResaveMetas / ResaveMetas: every non-canonical meta rewritten canonically
 *
 * `scan` writes no meta (edit [66]): it opens no lifecycle.
 */
namespace frt::assets
{
class CAssetRegistry;
class CAssetWriter;
class CRegistryView;

/**
 * new-meta: every path must name a file of a class that gets a meta (an asset or a raw file) with no meta in any spelling,
 * in a mount this registry writes; otherwise nothing is written and Problems say why (errors). Then a meta for each, through
 * RunNow limited to those files with CreateMeta alone, so it gets the derived GUID automatic creation would give it. A file
 * that the lifecycle would rather pair with an orphan meta, or that has a known identity, gets the meta its "create" fix-it
 * offers (the record's GUID for a lost meta, a new identity otherwise): the person asked for a meta.
 */
struct SNewMetasResult
{
	std::vector<FString> Created;        // mount paths of the sources, in the order given
	std::vector<SDiagnostic> Problems;   // refusals (nothing written), and what RunNow reported
	bool bRefused = false;               // a path was refused: nothing was written
};

[[nodiscard]] FRT_CORE_API SNewMetasResult NewMetas (CAssetRegistry& Registry, std::span<const FString> MountPaths);

/**
 * migrate metas (§14.8 step 1): CreateMeta for every file of Mounts that gets a meta and has none, with reproducible GUIDs
 * (MakeCreateMetaGuid over the mount's salt, its folded mount path and its content), so two people who migrate one commit
 * write the same bytes. A file with a meta, broken or not, is skipped and never reminted. When no file of the mounts is
 * left without a meta, the step records itself in Root's Schemas/Migrations.yml (RecordMigration, through Provider's
 * CheckOut). A dry run writes nothing and lists what it would create. Remaining: the files still without a meta (a
 * read-only file, a Git LFS pointer, a file an orphan meta matches, a lost meta: `fix` or `new-meta` settles each), which
 * keep the step unrecorded. A Root whose Schemas/Migrations.yml cannot be read (malformed, or a newer engine's) is refused
 * before anything is planned, a dry run's too: the step could never record itself there (Error says why).
 */
struct SMigrateMetasResult
{
	SRepairReport Report;                // RunNow's: Planned is what was (or, in a dry run, would be) created
	std::vector<FString> Remaining;      // sources still without a meta, in the registry's order
	bool bRecorded = false;              // the step is in Migrations.yml now (also when it already was)
	bool bWasRecorded = false;           // it was before this run
	FString Error;                       // Root's Migrations.yml cannot be read (nothing done), or the record not written
};

[[nodiscard]] FRT_CORE_API SMigrateMetasResult MigrateMetas (CAssetRegistry& Registry, std::span<const MountId> Mounts,
	const std::filesystem::path& Root, IVcsProvider* Provider, bool bDryRun);

/**
 * resave --metas: each meta of Mounts that parses and is not canonical, as EmitMeta writes it (unknown keys kept), in ops
 * of at most MaxResaveSteps rewrites through the writer of its mount ("resave-metas"). A broken meta is never rewritten
 * (META004: "keep A, alias B" or a hand fix), and a canonical one is not touched, so nothing is checked out for it.
 */
inline constexpr uint32 MaxResaveSteps = 256u;

struct SResaveMetasResult
{
	std::vector<FString> Rewritten;      // the metas' mount paths
	std::vector<FString> Broken;         // left as they are
	std::vector<FString> Newer;          // written by a newer engine (FormatVersion past this build's): left as they are
	uint32 Canonical = 0u;               // already canonical
	std::vector<SDiagnostic> Problems;   // refusals of the writer or the VCS (VCS002)
};

// The ops, without running them: Out's lists say what each meta is. Reads every meta of Mounts that Writer writes.
[[nodiscard]] FRT_CORE_API std::vector<SAssetOp> PlanResaveMetas (const CAssetWriter& Writer, const CRegistryView& View,
	std::span<const MountId> Mounts, SResaveMetasResult& Out);

// Plans, then runs each op through the writer of its mount and rescans what changed. bDryRun: plans only.
[[nodiscard]] FRT_CORE_API SResaveMetasResult ResaveMetas (CAssetRegistry& Registry, std::span<const MountId> Mounts, bool bDryRun);
}
