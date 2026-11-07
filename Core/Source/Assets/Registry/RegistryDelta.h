#pragma once

#include <optional>
#include <span>
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
 *	- Removes apply first (FolderRemoves, FileRemoves, RecordRemoves, RuleFileRemoves, ProblemClears,
 *	  TombstoneRemoves), then upserts (Mounts, FolderUpserts, FileUpserts, RuleFileUpserts, RecordUpserts),
 *	  then MetaCopyRemoves and Tombstones, then Problems and MetaCopies. A delta is atomic: one revision, one change-ring entry per record touched;
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
 *	- A FILES row of class Meta is a `.meta` (§3.2, M4): it has no record, is never a companion and never
 *	  Unclaimed, and the view's file count leaves it out. A source row's SFileUpsert::Meta of nullopt keeps the
 *	  stored state and hash, so a stat-only refresh never turns a Paired source back into PendingMeta.
 *	- There is no move operation: a watcher Moved becomes removes plus a subtree rescan. Identity follows the meta
 *	  (M4): the rescan's record carries the meta's GUID, so a remove and an add of one GUID is a move to listeners
 *	  (SAssetChangeSet::Moved). Two records with one GUID (a copy, before the lifecycle resolves it) are kept; lookups
 *	  by id answer the one whose file comes first in CompareFileOrder (the smallest folded path), never an assert.
 *	- Identity state (§5.3, M4) is keyed by GUID and travels in the same deltas: TombstoneRemoves apply with the
 *	  removes, MetaCopyRemoves and Tombstones after RecordUpserts and MetaCopies last. METAS keeps one copy per GUID (a
 *	  later copy replaces it), TOMBSTONES one tombstone per GUID (the latest), and a record upsert of a GUID drops that
 *	  GUID's tombstone, so a removal and an add of one GUID in one delta (a move) leaves none. A Tombstone or a
 *	  MetaCopyRemove of a GUID that a live record holds once the delta's records are in is ignored (another copy of
 *	  the asset, a pass on another mount that committed first, a journal part replayed out of order): no live GUID
 *	  has a tombstone and its copy stays, whatever order commits arrive in, as the bulk adoption builds it. The store
 *	  itself never writes either: the producer that removes a file or changes a record's GUID sends them, with its
 *	  time, without asking whether another record still holds the GUID.
 *	- A record's Aliases resolve to it (the alias index, §5.2): {B, 0} makes every {B, s} resolve to {this record, s}
 *	  (a consolidated GUID, the meta's Aliases), {own GUID, s} the one sub-entry whose label's MakeSubId is s (a
 *	  sub-asset renamed in the DCC: the sub-entry keeps the old label's id, SubAssets.Renamed). Its FormerPaths
 *	  (mount paths, the meta's) resolve path edges and FindByPath when no record has the path under the portable
 *	  fold. An edge resolved through either keeps its target and adds REF006 (info) to its record's Registry list.
 *	- No user callback runs inside Commit.
 *
 * Paths are mount paths (RegistryTypes.h) in on-disk spelling. Mount names are "Engine", "Project",
 * "Plugin.<Name>".
 */
namespace frt::assets
{
struct SMountUpsert    { FString Name; bool bCaseSensitive = false; MountId Id = InvalidMountId; };   // "Engine", "Project", "Plugin.X"; idempotent; Id from CMountTable
struct SFolderUpsert   { FString MountPath; };                           // a missing parent is created
/**
 * A FILES row's meta columns (§5.2): State, and Hash = Xxh3_64 of the meta's raw bytes (MetaFile.h HashMetaBytes; 0 for
 * none). A source row holds its meta's (Paired, PendingMeta, Broken from a scan; the lifecycle's other states), a Meta
 * row (EFileClass::Meta) its own pairing (Paired, Orphan or Broken).
 */
struct SFileMetaState
{
	EMetaState State = EMetaState::PendingMeta; uint64 Hash = 0;
	bool operator== (const SFileMetaState&) const = default;
};
struct SFileUpsert
{
	FString MountPath; uint64 Size = 0; int64 MTime = 0; platform::SFileIdentity Identity;
	std::optional<hash::SContentHash> ContentHash;   // nullopt: keep the stored hash (identity-only refresh); zero: changed, not hashed
	EFileClass Class = EFileClass::Unclaimed;
	std::optional<SFileMetaState> Meta;              // nullopt: keep the stored values (as ContentHash's nullopt); a new row starts PendingMeta
};
struct SRuleFileUpsert { FString MountName; FString FolderMountPath; uint64 RulesHash = 0; };  // empty folder = the mount's Config/Assets.yml
struct STagDecl        { FString Key; STagValue Value; };                // a key of the record type's table; MultiValue keys repeat
struct SSubEntryDecl   { FString Label; uint64 SubId = 0; reflect::TypeId Type = 0; ERecordFlags Flags{}; std::vector<STagDecl> Tags; };

// The aliases a record's sub-entries imply (§5.2 alias index): {Guid, MakeSubId(label)} of each sub-entry that keeps
// another label's id (SubAssets.Renamed), pinned rows (ERecordFlags::PinnedId: M5's) aside. Every producer of a record
// (an Analyze, a re-evaluation from the view) appends them before the meta's own Aliases.
inline void AppendRenamedSubAliases (const SGuid& Guid, std::span<const SSubEntryDecl> Subs, std::vector<SAssetId>& Out)
{
	for (const SSubEntryDecl& sub : Subs)
	{
		const uint64 labelId = MakeSubId(sub.Label);
		if (sub.SubId != 0u && sub.SubId != labelId && !HasAnyFlags(sub.Flags, ERecordFlags::PinnedId))
		{
			Out.push_back(SAssetId { Guid, labelId });
		}
	}
}
struct SEdgeDecl
{
	uint64 FromSub = 0;      // 0 = the record; else one of its SubIds
	SAssetId Target;         // valid: by id (canonical, then alias index); invalid: by TargetPath
	FString TargetPath;      // mount path, relative URIs already resolved against the source's folder, '..' collapsed; "#label" allowed
	EDepFlags Flags{}; FString FieldPath;
};
struct SRecordUpsert     // replaces the whole record of FileMountPath: tags, labels, sub-entries, edges, companions
{
	FString FileMountPath; SGuid Guid;               // the meta's; MakeProvisionalGuid(FileMountPath) only for a file with none yet
	reflect::TypeId Type = 0; FString Importer;      // the store interns importer names into its own u16 table
	ERecordFlags Flags{}; ECookRule CookRule = ECookRule::Auto; uint16 Chunk = 0; std::vector<FString> RuleLabels;
	std::vector<FString> AuthoredLabels;             // the meta's Labels (LABELS without the rule flag), beside RuleLabels
	FString PrimaryName;                             // empty unless ERecordFlags::Primary
	EImportStatus ImportStatus = EImportStatus::NotAnalyzed; uint64 AnalyzeStamp = 0;
	std::vector<STagDecl> Tags; std::vector<SSubEntryDecl> SubEntries; std::vector<SEdgeDecl> Edges;
	std::vector<FString> Companions;                 // mount paths this record's importer reads besides the file (glTF .bin)
	std::vector<SAssetId> Aliases;                   // ids that resolve to this record: {B,0} consolidated, {own GUID,XXH3(new label)} renamed sub-asset
	std::vector<FString> FormerPaths;                // the meta's: mount paths path lookups and path edges still resolve through
};
/**
 * METAS (§5.3): the raw bytes of a meta's last Ok parse, verified by Hash (MetaFile.h HashMetaBytes, the source row's
 * MetaHash) and restored byte for byte by the lifecycle. SourcePath is the source's mount path, which decides the
 * snapshot file and journal the copy goes to.
 */
struct SMetaCopy { SGuid Guid; uint64 Hash = 0u; std::vector<uint8> Bytes; FString SourcePath; };
/**
 * TOMBSTONES (§4.6, §5.3): what is left of a removed asset with a meta-given identity, for the lifecycle's grace period,
 * its orphan pairing and dangling-reference reports. RemovedFileTime: 100 ns ticks since 1601 (SFileStat's unit), the
 * producer's clock. Pruned at snapshot write after 30 days.
 */
struct STombstone { SGuid Guid; FString LastPath; hash::SContentHash Hash; platform::SFileIdentity Identity; int64 RemovedFileTime = 0; };
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
	std::vector<SMetaCopy> MetaCopies;               // replace that GUID's copy
	std::vector<SGuid> MetaCopyRemoves;
	std::vector<STombstone> Tombstones;              // replace that GUID's tombstone
	std::vector<SGuid> TombstoneRemoves;

	// Nothing to apply (Sequence aside).
	[[nodiscard]] bool IsEmpty () const noexcept
	{
		return Mounts.empty() && FolderRemoves.empty() && FileRemoves.empty() && RecordRemoves.empty()
			&& RuleFileRemoves.empty() && ProblemClears.empty() && FolderUpserts.empty() && FileUpserts.empty()
			&& RuleFileUpserts.empty() && RecordUpserts.empty() && Problems.empty() && MetaCopies.empty()
			&& MetaCopyRemoves.empty() && Tombstones.empty() && TombstoneRemoves.empty();
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
