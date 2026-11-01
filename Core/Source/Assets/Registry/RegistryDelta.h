#pragma once

#include <optional>
#include <vector>

#include "CoreTypes.h"
#include "Assets/AssetId.h"
#include "Assets/Registry/RegistryTypes.h"
#include "Containers/String.h"
#include "Diagnostics/Diagnostics.h"
#include "Hash/StableHash.h"
#include "Platform/FileSystem.h"


/**
 * The delta contract (design §5.4, §5.9): what every registry worker produces - a scan batch converted to
 * rows, a rules re-evaluation, an Analyze result - and the only way anything changes the registry store.
 * Workers build deltas with std containers and FString only (never the engine's memory pool); the owner
 * thread commits them in Sequence order.
 *
 * Semantics (CRegistryStore implements and tests them):
 *	- Removes apply first (FolderRemoves, FileRemoves, RecordRemoves, RuleFileRemoves, ProblemClears), then
 *	  upserts in the order of the members below (Mounts, FolderUpserts, FileUpserts, RuleFileUpserts,
 *	  RecordUpserts, Problems). A delta is atomic: one revision, one change-ring entry per record touched;
 *	  readers see all of it or none of it. Every SRecordId, NameId and string offset is assigned at commit:
 *	  no delta carries one, so a worker never needs to know the store's state.
 *	- MountIds are not the store's to choose: every SMountUpsert carries the id CMountTable assigned (the
 *	  single source of truth, design MountId rule), and the store adopts it as given. Upserting a known name
 *	  with a different id, or a known id with a different name, is a contract violation: a debug assert, a
 *	  code-0 Registry problem on the mount root, and that mount upsert is ignored. So every MountId F3 hands
 *	  to the store, a snapshot writer or an export (WriteSnapshot, ExportImage, DumpText) is a CMountTable id.
 *	- Problems are lists per (Subject, Source). A producer that rescans or re-evaluates a subtree clears
 *	  its source there first (ProblemClears: every list of that source whose Subject is the MountPath or
 *	  lies below it), then sets what it found (Problems), so a problem that is no longer reported does not
 *	  survive. Which source a diagnostic goes under:
 *	    Scan     - the scan batch's PATH001-003, unlistable folders, skipped links (Subject: the file or
 *	               folder);
 *	    Rules    - RULE001 (always, also when the scan's oracle raised it) and the rules engine's other
 *	               diagnostics, Subject = the folder of the rule file. A mount root holds two rule files
 *	               (the mount's Config/Assets.yml and the content root's Assets.yml) under one Subject: the
 *	               producer sends both files' diagnostics in that one list (File tells them apart); every
 *	               CRulesEngine entry point that reports for a mount root returns both (Rules.h, LoadMountRules);
 *	    Unclaimed - RULE002 (ReportUnclaimedFiles), Subject = the folder. Its own source because it is computed
 *	               at another time than RULE001 (after Analyze and commit, once companions are known): the
 *	               producer clears and sets it only when it recomputes a folder's unclaimed files, so neither
 *	               list erases the other;
 *	    Analyze  - an importer's diagnostics for its file (Subject: the file, or file#label);
 *	    Registry - what the store itself finds at commit (REF006, PATH001 on edges, contract violations). Their
 *	               severities are fixed: a rule file's Validation: overrides do not reach them in M3;
 *	    Lifecycle - the meta lifecycle's META* and VCS00x (Assets/Meta/LifecycleTypes.h, M4), cleared and set per
 *	               subject. A rule file's Validation: can override them.
 *	- An edge with an invalid Target resolves at commit by TargetPath, through the record path index under
 *	  the portable fold (§5.2). No match: the edge is kept with EDepFlags::Unresolved against an unresolved
 *	  slot keyed by the folded path (or by the GUID for a Target id nothing has), and is rebound - reverse
 *	  edges included - in the commit that registers a matching record. So the final graph never depends on
 *	  Analyze or commit order. A hit whose spelling differs from the on-disk one keeps the edge and adds
 *	  REF006 (info, the on-disk spelling in the message). A folded key that matches two records (PATH001)
 *	  stays Unresolved and adds PATH001.
 *	- A FILES row named in any committed record's Companions has class Companion (and no record of its
 *	  own); it returns to Unclaimed when no record names it any more. Order-independent, like unresolved
 *	  edges.
 *	- There is no move operation in M3: a watcher Moved becomes removes plus a subtree rescan (ids are
 *	  path-derived, MakeProvisionalGuid).
 *	- No user callback runs inside Commit.
 *
 * Paths are mount paths (RegistryTypes.h) in on-disk spelling. Mount names are "Engine", "Project",
 * "Plugin.<Name>".
 */
namespace frt::assets
{
struct SMountUpsert    { FString Name; bool bCaseSensitive = false; MountId Id = InvalidMountId; };   // "Engine", "Project", "Plugin.X"; idempotent; Id from CMountTable
struct SFolderUpsert   { FString MountPath; };                           // a missing parent is created
struct SFileUpsert
{
	FString MountPath; uint64 Size = 0; int64 MTime = 0; platform::SFileIdentity Identity;
	std::optional<hash::SContentHash> ContentHash;   // nullopt: keep the stored hash (identity-only refresh); zero: changed, not hashed
	EFileClass Class = EFileClass::Unclaimed; EMetaState MetaState = EMetaState::PendingMeta; uint64 MetaHash = 0;
};
struct SRuleFileUpsert { FString MountName; FString FolderMountPath; uint64 RulesHash = 0; };  // empty folder = the mount's Config/Assets.yml
struct STagDecl        { FString Key; STagValue Value; };                // a key of the record type's table; MultiValue keys repeat
struct SSubEntryDecl   { FString Label; uint64 SubId = 0; reflect::TypeId Type = 0; ERecordFlags Flags{}; std::vector<STagDecl> Tags; };
struct SEdgeDecl
{
	uint64 FromSub = 0;      // 0 = the record; else one of its SubIds
	SAssetId Target;         // valid: by id (canonical, then alias index); invalid: by TargetPath
	FString TargetPath;      // mount path, relative URIs already resolved against the source's folder, '..' collapsed; "#label" allowed
	EDepFlags Flags{}; FString FieldPath;
};
struct SRecordUpsert     // replaces the whole record of FileMountPath: tags, labels, sub-entries, edges, companions
{
	FString FileMountPath; SGuid Guid;               // M3: MakeProvisionalGuid(FileMountPath)
	reflect::TypeId Type = 0; FString Importer;      // the store interns importer names into its own u16 table
	ERecordFlags Flags{}; ECookRule CookRule = ECookRule::Auto; uint16 Chunk = 0; std::vector<FString> RuleLabels;
	FString PrimaryName;                             // empty unless ERecordFlags::Primary
	EImportStatus ImportStatus = EImportStatus::NotAnalyzed; uint64 AnalyzeStamp = 0;
	std::vector<STagDecl> Tags; std::vector<SSubEntryDecl> SubEntries; std::vector<SEdgeDecl> Edges;
	std::vector<FString> Companions;                 // mount paths this record's importer reads besides the file (glTF .bin)
};
enum class EProblemSource : uint8 { Scan, Rules, Analyze, Registry, Unclaimed, Lifecycle };   // values are stored: append only
struct SProblemsSet { FString Subject; EProblemSource Source = EProblemSource::Scan; std::vector<SDiagnostic> Diagnostics; };  // replaces that pair's list; empty clears
// Clears every list of Source whose Subject equals MountPath or continues it with '/' or '#' (byte-exact).
struct SProblemsClear { FString MountPath; EProblemSource Source = EProblemSource::Scan; };
struct SRegistryDelta
{
	uint64 Sequence = 0;
	std::vector<SMountUpsert> Mounts;
	std::vector<FString> FolderRemoves;              // subtree: folders, files, records, rule files, problems at or below
	std::vector<FString> FileRemoves;                // removes the file's record too
	std::vector<FString> RecordRemoves;
	std::vector<SRuleFileUpsert> RuleFileRemoves;
	std::vector<SProblemsClear> ProblemClears;       // after the other removes, before every upsert
	std::vector<SFolderUpsert> FolderUpserts;
	std::vector<SFileUpsert> FileUpserts;
	std::vector<SRuleFileUpsert> RuleFileUpserts;
	std::vector<SRecordUpsert> RecordUpserts;
	std::vector<SProblemsSet> Problems;

	// Nothing to apply (Sequence aside).
	[[nodiscard]] bool IsEmpty () const noexcept
	{
		return Mounts.empty() && FolderRemoves.empty() && FileRemoves.empty() && RecordRemoves.empty()
			&& RuleFileRemoves.empty() && ProblemClears.empty() && FolderUpserts.empty() && FileUpserts.empty()
			&& RuleFileUpserts.empty() && RecordUpserts.empty() && Problems.empty();
	}
};
}


FRT_DECLARE_ENUM_REFLECTION(
	frt::assets::EProblemSource,
	FRT_ENUM_ENTRY(frt::assets::EProblemSource, Scan),
	FRT_ENUM_ENTRY(frt::assets::EProblemSource, Rules),
	FRT_ENUM_ENTRY(frt::assets::EProblemSource, Analyze),
	FRT_ENUM_ENTRY(frt::assets::EProblemSource, Registry),
	FRT_ENUM_ENTRY(frt::assets::EProblemSource, Unclaimed),
	FRT_ENUM_ENTRY(frt::assets::EProblemSource, Lifecycle));
