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
 *	- Removes apply first (FolderRemoves, FileRemoves, RecordRemoves, RuleFileRemoves), then upserts in the
 *	  order of the members below (Mounts, FolderUpserts, FileUpserts, RuleFileUpserts, RecordUpserts,
 *	  Problems). A delta is atomic: one revision, one change-ring entry per record touched; readers see all
 *	  of it or none of it. Every SRecordId, NameId and string offset is assigned at commit: no delta carries
 *	  one, so a worker never needs to know the store's state.
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
struct SMountUpsert    { FString Name; bool bCaseSensitive = false; };   // "Engine", "Project", "Plugin.X"; idempotent
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
	ERecordFlags Flags{}; ECookRule CookRule = ECookRule::Default; uint16 Chunk = 0; std::vector<FString> RuleLabels;
	FString PrimaryName;                             // empty unless ERecordFlags::Primary
	EImportStatus ImportStatus = EImportStatus::NotAnalyzed; uint64 AnalyzeStamp = 0;
	std::vector<STagDecl> Tags; std::vector<SSubEntryDecl> SubEntries; std::vector<SEdgeDecl> Edges;
	std::vector<FString> Companions;                 // mount paths this record's importer reads besides the file (glTF .bin)
};
enum class EProblemSource : uint8 { Scan, Rules, Analyze, Registry };
struct SProblemsSet { FString Subject; EProblemSource Source = EProblemSource::Scan; std::vector<SDiagnostic> Diagnostics; };  // replaces that pair's list; empty clears
struct SRegistryDelta
{
	uint64 Sequence = 0;
	std::vector<SMountUpsert> Mounts;
	std::vector<FString> FolderRemoves;              // subtree: folders, files, records, rule files, problems at or below
	std::vector<FString> FileRemoves;                // removes the file's record too
	std::vector<FString> RecordRemoves;
	std::vector<SRuleFileUpsert> RuleFileRemoves;
	std::vector<SFolderUpsert> FolderUpserts;
	std::vector<SFileUpsert> FileUpserts;
	std::vector<SRuleFileUpsert> RuleFileUpserts;
	std::vector<SRecordUpsert> RecordUpserts;
	std::vector<SProblemsSet> Problems;

	// Nothing to apply (Sequence aside).
	[[nodiscard]] bool IsEmpty () const noexcept
	{
		return Mounts.empty() && FolderRemoves.empty() && FileRemoves.empty() && RecordRemoves.empty()
			&& RuleFileRemoves.empty() && FolderUpserts.empty() && FileUpserts.empty() && RuleFileUpserts.empty()
			&& RecordUpserts.empty() && Problems.empty();
	}
};
}
