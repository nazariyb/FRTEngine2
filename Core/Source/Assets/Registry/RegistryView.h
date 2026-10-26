#pragma once

#include <optional>
#include <span>
#include <vector>

#include "Core.h"
#include "CoreTypes.h"
#include "Assets/AssetId.h"
#include "Assets/Registry/RegistryTables.h"
#include "Assets/Registry/RegistryTypes.h"
#include "Containers/String.h"
#include "Diagnostics/Diagnostics.h"
#include "Functional/FunctionRef.h"
#include "Hash/StableHash.h"
#include "Platform/FileSystem.h"


/**
 * An immutable snapshot of the asset registry at one revision (design §5.7, §5.9): every read lives here,
 * so one view gives consistent answers across calls. CRegistryStore publishes a new view after every
 * commit; the owner thread reads the live one, other threads acquire the last published one and keep it as
 * long as they like - pages are shared copy-on-write, so a view never changes and holding one never blocks
 * a commit.
 *
 * An SRecordId carries the revision of the view that produced it; every other view rejects it (the
 * accessors return false or nothing), so an id never silently names another record after a later commit.
 * Code that keeps a record across frames keeps its SAssetId (or path) and re-resolves it.
 *
 * Paths are mount paths (RegistryTypes.h); a sub-entry's path is its file's path, '#', and its label.
 * Lookups by path compare under the portable fold (§5.2); the FILES and FOLDERS accessors that say so use
 * the mount's file-system rule. Nothing here touches the disk or the engine's memory pool.
 *
 * Queries (§5.7): Compile, Run and RunText are declared here and implemented by the query engine
 * (Registry/QueryEngine.cpp, which documents their semantics; include QueryEngine.h for SCompiledQuery and
 * SQueryStats). Not here yet: FindPrimary/FindRow (M4/M5), ICatalog (M6). G3 and H3 read the tables through
 * GetTables (RegistryTables.h).
 */
namespace frt::assets
{
struct SAssetQuery;       // AssetQuery.h
struct SCompiledQuery;    // QueryEngine.h
struct SQueryStats;       // QueryEngine.h

// A record copied out of a view; owns its strings.
struct SAssetData
{
	SRecordId Record;
	SAssetId Id;
	SAssetId64 Id64;
	reflect::TypeId Type = reflect::InvalidTypeId;
	uint16 AssetType = 0u;
	ERecordFlags Flags {};
	FString MountPath;                  // "#label" suffix for sub-entries
	FString Name;                       // the file's stem, or the sub-entry's label
	uint64 Revision = 0u;               // the registry revision at which this record last changed
};

/**
 * One edge as a reader sees it. From is the record that declares it (the sub-entry for FromSub != 0 when
 * that sub-entry exists). To is the record it resolves to (the sub-entry named by TargetSub when there is
 * one, else the file's record), invalid while Flags has Unresolved - also when the target is ambiguous
 * under the portable fold (bAmbiguous, PATH001), and invalid for an edge to a companion file (bCompanion: a
 * glTF's buffer, a FILES row with no record), which is satisfied and so carries no Unresolved - `is:broken-ref`
 * and the unresolved counts skip it. The views point into the view's pools.
 */
struct SDependencyView
{
	SRecordId From;
	SRecordId To;
	uint64 FromSub = 0u;
	uint64 TargetSub = 0u;
	EDepFlags Flags {};                 // as declared, plus Unresolved when neither resolved nor a companion
	FStringView TargetPath;             // the path the source wrote ("#label" included); a Guid target's hint
	SAssetId TargetId;                  // a Guid target's id; a resolved Path target's record id and TargetSub
	FStringView FieldPath;
	bool bAmbiguous = false;
	bool bCompanion = false;            // the target is a companion file (no record, To invalid, not Unresolved)
	bool bGuidTarget = false;           // declared by id (TargetId as declared), else by TargetPath
};

struct SMountInfo
{
	FStringView Name;
	MountId Id = InvalidMountId;
	bool bCaseSensitive = false;
	uint32 RootFolder = InvalidRow;
};

struct SFileData
{
	FStringView MountPath;              // on-disk spelling
	MountId Mount = InvalidMountId;
	uint64 Size = 0u;
	int64 MTime = 0;
	platform::SFileIdentity Identity;
	hash::SContentHash ContentHash;     // zero: not hashed
	uint64 MetaHash = 0u;
	EFileClass Class = EFileClass::Unclaimed;
	EMetaState MetaState = EMetaState::PendingMeta;
	SRecordId Record;                   // invalid when the file has no record
	uint32 Folder = InvalidRow;
};

struct SFolderData
{
	FStringView MountPath;
	MountId Mount = InvalidMountId;
	uint32 Parent = InvalidRow;
	uint32 PreorderBegin = 0u;          // the subtree: folders of the mount with PreorderBegin in [Begin, End)
	uint32 PreorderEnd = 0u;
	uint32 RuleFile = InvalidRow;
};

struct SRuleFileData
{
	MountId Mount = InvalidMountId;
	uint32 Folder = InvalidRow;         // InvalidRow: the mount's Config/Assets.yml
	FStringView FolderMountPath;        // empty for the mount's Config/Assets.yml
	uint64 RulesHash = 0u;
};

enum class EDumpFlags : uint8
{
	None = 0,
	NoFolders = 1 << 0,                 // leave out the folder list
	NoProblems = 1 << 1,                // leave out diagnostics
};

[[nodiscard]] constexpr EDumpFlags operator| (EDumpFlags A, EDumpFlags B) noexcept
{
	return static_cast<EDumpFlags>(static_cast<uint8>(A) | static_cast<uint8>(B));
}

[[nodiscard]] constexpr bool HasDumpFlag (EDumpFlags Flags, EDumpFlags Flag) noexcept
{
	return (static_cast<uint8>(Flags) & static_cast<uint8>(Flag)) != 0u;
}


class FRT_CORE_API CRegistryView final
{
public:
	CRegistryView ();
	CRegistryView (const CRegistryView&);
	CRegistryView& operator= (const CRegistryView&);
	~CRegistryView ();

	[[nodiscard]] uint64 GetRevision () const noexcept;
	// Whether the record is in this view and was produced by it (its revision is this view's).
	[[nodiscard]] bool IsValid (SRecordId Record) const noexcept;

	// ---- Records (§5.7) ----
	[[nodiscard]] EExists Exists (const SAssetId& Id) const;                    // Yes or No (a view is complete)
	[[nodiscard]] std::optional<SRecordId> Find (const SAssetId& Id) const;     // canonical, then the alias index
	[[nodiscard]] std::optional<SRecordId> Find (SAssetId64 Id) const;
	// Portable fold (§5.2); "path#label" names a sub-entry (the label matched exactly, then under the fold).
	// A folded path that matches two records (PATH001) fails: nullopt, and OutError says so.
	[[nodiscard]] std::optional<SRecordId> FindByPath (FStringView MountPath, SDiagnostic* OutError = nullptr) const;
	bool GetRecord (SRecordId Record, SAssetData& Out) const;
	// The record's sub-entries (an asset record's), in the order its producer declared them.
	void ForEachSubEntry (SRecordId Record, TFunctionRef<void (SRecordId)> Visit) const;
	// The asset record a sub-entry belongs to (the record itself for an asset record).
	[[nodiscard]] SRecordId GetOwner (SRecordId Record) const noexcept;

	// A tag of the record's type table, by key name (the portable fold). A MultiValue key yields its first
	// value here; ForEachTagValue yields all.
	bool GetTag (SRecordId Record, FStringView TagName, STagValue& Out) const;
	void ForEachTagValue (SRecordId Record, FStringView TagName, TFunctionRef<void (const STagValue&)> Visit) const;
	// Every present tag, in the type's key order (MultiValue keys once per value).
	void ForEachTag (SRecordId Record, TFunctionRef<void (FStringView Key, const STagValue&)> Visit) const;
	// Labels in stored order; bRule: from Assets.yml, else authored (.meta, M4).
	void ForEachLabel (SRecordId Record, TFunctionRef<void (FStringView Label, bool bRule)> Visit) const;
	// The mount paths the record's importer reads besides its file (an asset record's).
	void ForEachCompanion (SRecordId Record, TFunctionRef<void (FStringView MountPath)> Visit) const;
	// The importer that analyzed the record, "" for none.
	[[nodiscard]] FStringView GetImporterName (SRecordId Record) const noexcept;

	// ---- Dependencies (§5.2, §5.7) ----
	// An asset record's dependencies are every edge of its file (FromSub tells the declaring sub-entry); a
	// sub-entry's are the edges it declares. Require: every bit must be set; Exclude: none may be.
	void ForEachDependency (SRecordId Record, EDepFlags Require, EDepFlags Exclude,
		TFunctionRef<void (const SDependencyView&)> Visit) const;
	// Edges that resolve to the record (an asset record: with any TargetSub; a sub-entry: with its SubId).
	// Unordered: sort when the order matters.
	void ForEachReferencer (SRecordId Record, EDepFlags Require, EDepFlags Exclude,
		TFunctionRef<void (const SDependencyView&)> Visit) const;
	/**
	 * The traversals walk files: an asset record stands for its whole file, so a file's dependencies are all
	 * of its edges (any FromSub) and it is reached by every edge that resolves to it (any TargetSub), and
	 * reaching one record of a file reaches every record of it. Hence T is in GetClosure(R) exactly when R is
	 * in GetReferencerClosure(T), and WhyReferenced(R, T) agrees with both.
	 *
	 * GetClosure: every record reachable from Roots over resolved edges with any bit of Follow (every edge
	 * when Follow is empty) - the roots first, then each reached file's asset record and sub-entries, files
	 * in breadth-first order. GetReferencerClosure: the same over referencers (transitive referencers).
	 */
	void GetClosure (std::span<const SRecordId> Roots, EDepFlags Follow, std::vector<SRecordId>& Out) const;
	void GetReferencerClosure (std::span<const SRecordId> Roots, EDepFlags Follow, std::vector<SRecordId>& Out) const;
	// A chain with the fewest edges from From's file to To's file; false when To is not reachable. Records of
	// one file give true with an empty chain. Memory grows with the files visited, not with the view.
	bool WhyReferenced (SRecordId From, SRecordId To, std::vector<SDependencyView>& OutChain) const;

	// ---- Diagnostics ----
	// The record's problems: every source's list for its path (an asset record: also its sub-entries' paths).
	void ForEachDiagnostic (SRecordId Record, TFunctionRef<void (const SDiagnostic&)> Visit) const;
	// Every source's list for one subject (a file, folder or "file#label").
	void ForEachDiagnosticAt (FStringView Subject, TFunctionRef<void (EProblemSource, const SDiagnostic&)> Visit) const;
	// Every stored diagnostic.
	void ForEachProblem (TFunctionRef<void (FStringView Subject, EProblemSource, const SDiagnostic&)> Visit) const;

	// ---- Mounts, files, folders, rule files ----
	[[nodiscard]] std::vector<SMountInfo> GetMounts () const;
	[[nodiscard]] std::optional<SMountInfo> FindMount (FStringView Name) const;
	// Byte-exact (what a scan compares against, IKnownFiles).
	[[nodiscard]] std::optional<uint32> FindFileExact (FStringView MountPath) const noexcept;
	// The file the OS means: exact, else the single row equal under the mount's file-system rule.
	[[nodiscard]] std::optional<uint32> FindFile (FStringView MountPath) const noexcept;
	bool GetFile (uint32 FileRow, SFileData& Out) const noexcept;
	[[nodiscard]] std::optional<uint32> FindFolder (FStringView MountPath) const noexcept;
	bool GetFolder (uint32 FolderRow, SFolderData& Out) const noexcept;
	// FolderMountPath empty: the mount's Config/Assets.yml.
	[[nodiscard]] std::optional<uint32> FindRuleFile (FStringView MountName, FStringView FolderMountPath) const noexcept;
	bool GetRuleFile (uint32 RuleFileRow, SRuleFileData& Out) const noexcept;
	void ForEachFile (TFunctionRef<void (uint32 FileRow)> Visit) const;
	// The files at or below a folder (by its pre-order interval).
	void ForEachFileUnder (uint32 FolderRow, TFunctionRef<void (uint32 FileRow)> Visit) const;
	void ForEachFolder (TFunctionRef<void (uint32 FolderRow)> Visit) const;
	void ForEachRuleFile (TFunctionRef<void (uint32 RuleFileRow)> Visit) const;
	void ForEachRecord (TFunctionRef<void (SRecordId)> Visit) const;   // asset records, by row

	[[nodiscard]] uint32 GetFileCount () const noexcept;
	[[nodiscard]] uint32 GetFolderCount () const noexcept;
	[[nodiscard]] uint32 GetRecordCount () const noexcept;
	[[nodiscard]] uint32 GetSubEntryCount () const noexcept;

	// ---- Text dump (§5.3) ----
	/**
	 * One section per mount (by MountId): folders, rule files and files sorted by mount path under the
	 * portable fold, then bytes. Per file: class, size and - when it has a record - importer, type, flags,
	 * cook rule, chunk, labels, AnalyzeStamp and import status, then its sub-entries (by label), tags (by key
	 * name; doubles in shortest round-trip form), edges (by target path, TargetSub, flags, field path) and
	 * companions; then the mount's diagnostics by (code, subject, message). Never mtime, identity, hashes,
	 * revisions, record ids, page layout, the volume's case rule or absolute paths: equal content dumps
	 * identically, fresh or warm, on any machine. A diagnostic's message is printed only for coded
	 * diagnostics (code 0 messages may hold absolute paths). Paths and subjects are shown with ToDisplayText
	 * (a WTF-8 surrogate as U+FFFD), so the dump is UTF-8 text; the stored spellings stay as they are.
	 */
	void DumpText (FString& Out, EDumpFlags Flags = EDumpFlags::None) const;

	// ---- Queries (§5.6-§5.8; the query engine, QueryEngine.h, says what they mean) ----
	/**
	 * Compiles Query against this view. False on the first term that does not resolve, with OutError filled (one
	 * positioned diagnostic, §5.7's SQueryDiagnostics reduced to its first error; Out left empty); true otherwise,
	 * with Out ready for Run on this view and its notes in Out.Notes.
	 */
	bool Compile (const SAssetQuery& Query, SCompiledQuery& Out, SDiagnostic* OutError = nullptr) const;
	/**
	 * Runs a compiled query: Out receives the records of the page [Offset, Offset + Limit) in result order (the whole
	 * result without a Limit). Returns how many records matched before Offset and Limit. A query compiled against
	 * another revision or store (or not compiled) returns 0 with an empty Out and OutStats->bWrongView set.
	 */
	uint32 Run (const SCompiledQuery& Query, std::vector<SRecordId>& Out, SQueryStats* OutStats = nullptr) const;
	/**
	 * Parse (§5.8), compile and run in one call - the search box, `FRT.bat assets query`. nullopt on a parse or
	 * compile error (OutError positioned in Text); else the match count as Run returns it. OutNotes receives the
	 * compile notes ("is:redirected matches nothing until its milestone"); it is cleared first.
	 */
	std::optional<uint32> RunText (FStringView Text, std::vector<SRecordId>& Out, SDiagnostic* OutError = nullptr,
		SQueryStats* OutStats = nullptr, std::vector<SDiagnostic>* OutNotes = nullptr) const;

	// ---- Images and memory ----
	// Every table of the given mounts (all mounts when empty), for snapshots (RegistryTables.h).
	[[nodiscard]] SRegistryImage ExportImage (std::span<const MountId> Mounts) const;
	[[nodiscard]] SRegistryMemoryStats GetMemoryStats () const;

private:
	friend class CRegistryStore;
	friend FRT_CORE_API const SRegistryTables& GetTables (const CRegistryView& View) noexcept;

	[[nodiscard]] SRecordId MakeRecordId (uint32 Value) const noexcept { return SRecordId { Value, static_cast<uint32>(Tables.Revision) }; }
	[[nodiscard]] SDependencyView MakeDependencyView (uint32 Source, uint32 EdgeRef) const;
	[[nodiscard]] std::vector<uint32> GetRootRows (std::span<const SRecordId> Roots) const;
	void ListFiles (std::span<const SRecordId> Roots, std::span<const uint32> Rows, std::vector<SRecordId>& Out) const;

	SRegistryTables Tables;
};
}
