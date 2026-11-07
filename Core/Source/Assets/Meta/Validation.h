#pragma once

#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "Core.h"
#include "CoreTypes.h"
#include "Assets/Meta/LifecycleTypes.h"
#include "Assets/Registry/RegistryTypes.h"
#include "Containers/String.h"
#include "Diagnostics/Diagnostics.h"


/**
 * `validate` (design §13.2, §13.4), in Core so that AssetTool's verb and the editor's Asset problems panel are one
 * implementation over one view (edits [33], [66]): the panel calls RunValidation on a worker, the verb prints it.
 *
 * What it returns, for the mounts and paths asked for:
 *	- every problem the registry holds, whatever its source: Scan (PATH001-003), Rules (RULE001), Unclaimed (RULE002),
 *	  Analyze (REF007, an importer's failure), Registry (REF001, REF006) and Lifecycle (META001-004, 006, 008, 009,
 *	  VCS001-004 as the lifecycle's run committed them: a caller wanting those runs CAssetRepairs::RunNow first);
 *	- the rules only `validate` runs (§13.4 "V"), which read files the registry does not keep:
 *	  META005  a meta that parses but is not canonical (`resave --metas` rewrites it), plus a meta's unknown keys and a
 *	           FormatVersion newer than this engine's, as warnings with no code (§13.4 has none: they fail `--ci`);
 *	  META007  a meta beside a file the rules exclude (a rule file included) or beside one of a class that takes no meta
 *	           (a companion, an unclaimed file);
 *	  a meta's settings that the effective importer has no field for, that are no value of their field, and an
 *	  `Importer:` pin this build does not know (ResolveFile ignores them, so the rules' value stands): warnings, no code;
 *	  VCS001   CRLF in a hashed FRT text file the lifecycle does not read: a rule file, a `.yml`, `.frt`, `.csv`, `.ini`,
 *	           `.frtinputmap` or `.frtinputaction` source (metas are the lifecycle's). Not with bQuick, except for Paths;
 *	  VCS003   a pair the VCS tracks differently, one StatusOf per provider over every pair in scope ("commit it");
 *	  MIG001   a repository root of the scope whose Schemas/Migrations.yml lacks a step this engine requires.
 *	MIG002 has no data in M4: the legacy patterns it names go with steps that arrive in M7 and M9a.
 *
 * Each V-only problem carries Subject (the mount path as spelled), File (relative to its owner root) and the rule files'
 * `Validation:` severity for its folder when Rules is given. The list is deduplicated by code and subject, and sorted by
 * subject (portable fold, then bytes), then code. Reads files and asks the providers: run it on a worker.
 */
namespace frt::assets
{
class CMountTable;
class CRegistryView;
class CRulesEngine;
class IVcsProvider;

struct SValidateOptions
{
	const CMountTable* Mounts = nullptr;              // required: what mount paths are on disk
	const CRulesEngine* Rules = nullptr;              // the Validation: overrides of the V-only rules; null: none
	std::vector<MountId> Scope;                       // the mounts validated; empty: every mount
	// Only these mount paths (--staged, or the paths a person named): a problem is in when its subject is at or below one,
	// or is the meta or the source of one. MIG001 is the root's, so it is reported whatever the paths. nullopt: everything.
	std::optional<std::vector<FString>> Paths;
	bool bQuick = false;                              // no reads of text sources but those in Paths (no hashing anywhere)
	std::vector<std::pair<MountId, IVcsProvider*>> Providers;   // VCS003; absent, null or None: no status asked
};

FRT_CORE_API void RunValidation (const CRegistryView& View, const SValidateOptions& Options, std::vector<SDiagnostic>& Out);

struct SValidationCounts
{
	uint32 Errors = 0u;
	uint32 Warnings = 0u;
	uint32 Infos = 0u;
};

[[nodiscard]] FRT_CORE_API SValidationCounts CountValidation (std::span<const SDiagnostic> Problems) noexcept;

// `validate`'s verdict: no error; with --ci also no warning (infos are allowed: REF007 stays until M7).
[[nodiscard]] constexpr bool IsValidationClean (const SValidationCounts& Counts, bool bCi) noexcept
{
	return Counts.Errors == 0u && (!bCi || Counts.Warnings == 0u);
}

/**
 * The repairs `fix` and `validate --fix` apply, one list for both (edit [8]): create a meta, restore a verified copy, fix a
 * meta's case, and the deterministic dedupe (which RunLifecycle proposes only while Safety.AutoResolveDuplicates is on).
 * Never a relink of a suggestion (MoveMeta), "keep A, alias B" or a hand re-GUID: those are a person's fix-its, and a
 * non-canonical meta is `resave --metas`'s.
 */
inline constexpr ELifecycleAction SafeFixKinds[] =
{
	ELifecycleAction::CreateMeta, ELifecycleAction::RestoreMeta, ELifecycleAction::FixMetaCase, ELifecycleAction::ReGuid,
};
}
