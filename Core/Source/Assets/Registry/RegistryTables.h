#pragma once

#include <algorithm>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "Core.h"
#include "CoreTypes.h"
#include "Assets/AssetId.h"
#include "Assets/AssetTypes.h"
#include "Assets/Registry/PagedColumn.h"
#include "Assets/Registry/RegistryDelta.h"
#include "Assets/Registry/RegistryTypes.h"
#include "Containers/String.h"
#include "Containers/StringMap.h"
#include "Diagnostics/Diagnostics.h"
#include "Hash/StableHash.h"
#include "Platform/FileSystem.h"


/**
 * The registry's tables as they are stored (design §5.2), for code inside Core/Source/Assets/Registry: the
 * query engine, snapshots and journals read a view through GetTables() instead of growing the view's
 * public API, and never edit the store's files. Everything here is const access to one immutable view;
 * only CRegistryStore writes, on the owner thread, through the same structures.
 *
 * Row numbers are stable for the life of a store: a removed row is tombstoned (FILES/ASSETS: File or
 * Folder = InvalidRow; FOLDERS/RULEFILES: Mount = InvalidMountId; SUBENTRY: Owner = InvalidRow; type
 * tables: the Dead bit) and new rows append. The exception is CRegistryStore::CompactTables, which
 * renumbers SUBENTRY and type-table rows (and rebuilds the label, companion, Multi and STRINGS pools) in
 * a revision of its own. An SRecordId's Value is an ASSETS row, or SubEntryBit | a SUBENTRY row.
 *
 * Indices are CPagedHashIndex tables of row numbers keyed by in-memory hashes; the caller compares the
 * candidates it is handed against the columns (the Find* helpers below do). They are never persisted:
 * AdoptImage rebuilds them.
 */
namespace frt::assets
{
class CRegistryView;

inline constexpr uint32 InvalidRow = ~0u;
inline constexpr uint32 SubEntryBit = 1u << 31u;
inline constexpr uint16 InvalidImporter = 0xFFFFu;


// ---------------- Hashes and comparisons (in memory only) ----------------

/**
 * ASCII fast paths. The caseless hashes of Containers/ (SStringHashIgnoreCase, SPathHash) stream each code point's
 * key, byte by byte, into 256-byte chunks; up to 256 bytes that is exactly hash::HashBytes of the keys' bytes. For
 * an ASCII text the keys are its bytes with the letters lowered (the portable fold) or raised (NTFS's upcase), so
 * lowering or raising into a buffer and hashing it gives the same value at a fraction of the cost - paths are
 * hashed at every file, record and target the store adds. Anything else takes the general functors.
 */
namespace registry_detail
{
inline constexpr uint32 FastCaseBytes = 256u;

// Writes Text with its ASCII letters lowered to Out (Text.Len() bytes); returns the OR of every byte, so a result
// with bit 7 set means Text holds a non-ASCII byte (and Out is not its fold).
inline uint8 LowerAscii (FStringView Text, char* Out) noexcept
{
	const char* data = Text.GetData();
	uint8 bits = 0u;
	for (uint32 i = 0u, count = Text.Len(); i < count; ++i)
	{
		const uint8 c = static_cast<uint8>(data[i]);
		bits |= c;
		Out[i] = static_cast<char>(c + (static_cast<uint8>(c - uint8('A')) < 26u ? 32u : 0u));
	}
	return bits;
}

// The same with the letters raised.
inline uint8 UpperAscii (FStringView Text, char* Out) noexcept
{
	const char* data = Text.GetData();
	uint8 bits = 0u;
	for (uint32 i = 0u, count = Text.Len(); i < count; ++i)
	{
		const uint8 c = static_cast<uint8>(data[i]);
		bits |= c;
		Out[i] = static_cast<char>(c - (static_cast<uint8>(c - uint8('a')) < 26u ? 32u : 0u));
	}
	return bits;
}

// Equal ASCII texts under a case rule (ASCII letters equal in either case), or nullopt when either holds a
// non-ASCII byte before a difference is found.
inline std::optional<bool> EqualAsciiIgnoreCase (FStringView A, FStringView B) noexcept
{
	if (A.Len() != B.Len())
	{
		return std::nullopt;   // folding changes byte lengths (U+212A is 3 bytes, its fold 'k' one)
	}
	const char* a = A.GetData();
	const char* b = B.GetData();
	for (uint32 i = 0u, count = A.Len(); i < count; ++i)
	{
		const uint8 x = static_cast<uint8>(a[i]);
		const uint8 y = static_cast<uint8>(b[i]);
		if (((x | y) & 0x80u) != 0u)
		{
			return std::nullopt;
		}
		if (x != y && (x | 0x20u) != (y | 0x20u))
		{
			return false;
		}
		if (x != y && static_cast<uint8>((x | 0x20u) - uint8('a')) >= 26u)
		{
			return false;   // differ only in bit 5 but are not letters ('@' and '`')
		}
	}
	return true;
}
}

// The portable fold (§5.2): lookups, Path edges, companions, PATH001. Equal to SStringHashIgnoreCase.
[[nodiscard]] inline uint64 HashPortable (FStringView Text) noexcept
{
	if (Text.Len() <= registry_detail::FastCaseBytes)
	{
		char folded[registry_detail::FastCaseBytes];
		if ((registry_detail::LowerAscii(Text, folded) & 0x80u) == 0u)
		{
			return hash::HashBytes(folded, Text.Len());
		}
	}
	return SStringHashIgnoreCase {}(Text);
}
[[nodiscard]] inline bool EqualPortable (FStringView A, FStringView B) noexcept
{
	const std::optional<bool> ascii = registry_detail::EqualAsciiIgnoreCase(A, B);
	return ascii.has_value() ? *ascii : SStringEqualIgnoreCase {}(A, B);
}
[[nodiscard]] inline uint64 HashExact (FStringView Text) noexcept { return SStringHash {}(Text); }

// The mount root's file-system rule (§5.2 "File-system entries follow the volume"): NTFS's upcase table on a
// case-insensitive mount (TPathMap's rule, equal to SPathHash), byte-exact on a case-sensitive one.
[[nodiscard]] inline uint64 HashMountRule (FStringView Path, bool bCaseSensitive) noexcept
{
	if (bCaseSensitive)
	{
		return SStringHash {}(Path);
	}
	if (Path.Len() <= registry_detail::FastCaseBytes)
	{
		char upper[registry_detail::FastCaseBytes];
		if ((registry_detail::UpperAscii(Path, upper) & 0x80u) == 0u)
		{
			return hash::HashBytes(upper, Path.Len());
		}
	}
	return SPathHash {}(Path);
}
[[nodiscard]] inline bool EqualMountRule (FStringView A, FStringView B, bool bCaseSensitive) noexcept
{
	if (bCaseSensitive)
	{
		return A == B;
	}
	const std::optional<bool> ascii = registry_detail::EqualAsciiIgnoreCase(A, B);
	return ascii.has_value() ? *ascii : SPathEqual {}(A, B);
}

// Folds Text (FString::FoldCase) into Buffer when it is ASCII and fits (returns the view of it), else into Heap.
[[nodiscard]] inline FStringView FoldPortable (FStringView Text, char (&Buffer)[registry_detail::FastCaseBytes], FString& Heap)
{
	if (Text.Len() <= registry_detail::FastCaseBytes && (registry_detail::LowerAscii(Text, Buffer) & 0x80u) == 0u)
	{
		return FStringView(Buffer, Text.Len());
	}
	Heap = FString(Text).FoldCase();
	return Heap.View();
}

[[nodiscard]] inline uint64 HashGuid (const SGuid& Guid) noexcept
{
	uint64 low = 0u;
	uint64 high = 0u;
	std::memcpy(&low, Guid.Bytes, 8u);
	std::memcpy(&high, Guid.Bytes + 8u, 8u);
	return MixRegistryKey(low ^ (high * 0x9E3779B97F4A7C15ull));
}

// Records first ordered by the portable fold, then by bytes (§5.3 dump order, FOLDERS sibling order).
[[nodiscard]] inline bool LessPortableThenBytes (FStringView A, FStringView B) noexcept
{
	const int32 folded = A.Compare<ESearchCase::IgnoreCase>(B);
	return folded != 0 ? folded < 0 : A < B;
}


// ---------------- Pools ----------------

/**
 * NAMES (§5.2): interned UTF-8 names - path segments, labels, tag keys, importer and type names, Name tag
 * values - each with its folded shadow (FString::FoldCase) for caseless search. Interning is byte-exact.
 */
class CNamePool
{
public:
	[[nodiscard]] uint32 Size () const noexcept { return Pool.Size(); }
	[[nodiscard]] FStringView Get (uint32 Id) const noexcept { return Pool.Get(Id); }
	[[nodiscard]] FStringView GetFolded (uint32 Id) const noexcept { return Pool.Resolve(Folded[Id]); }

	[[nodiscard]] std::optional<uint32> Find (FStringView Name) const
	{
		return FindHashed(Name, HashExact(Name));
	}

	uint32 Intern (FStringView Name, uint64 Epoch)
	{
		const uint64 hash = HashExact(Name);
		if (const std::optional<uint32> existing = FindHashed(Name, hash))
		{
			return *existing;
		}
		return Add(Name, hash, Epoch);
	}

	// InternBulk over a snapshot's NAMES table.
	void InternAll (std::span<const FString> Names, std::vector<uint32>& OutIds, uint64 Epoch)
	{
		std::vector<FStringView> views(Names.size());
		for (size_t i = 0u; i < Names.size(); ++i)
		{
			views[i] = Names[i].View();
		}
		InternBulk(views, OutIds, Epoch);
	}

	/**
	 * Interns many names at once (a load): OutIds[i] is Names[i]'s id; names may repeat. The index is sized once
	 * and the lookups run in the order of the names' home slots, so the probes walk the index front to back
	 * instead of missing the cache each; the new names are added in the order given - a load lays them out as
	 * the records list them, which keeps later scans over them sequential - and listed in the index at the end,
	 * again in slot order. The views must stay valid until it returns.
	 */
	void InternBulk (std::span<const FStringView> Names, std::vector<uint32>& OutIds, uint64 Epoch)
	{
		const uint32 count = static_cast<uint32>(Names.size());
		OutIds.assign(count, InvalidId);
		if (count == 0u)
		{
			return;
		}
		Index.Reserve(Size() + count, Epoch);
		std::vector<uint64> hashes(count);
		std::vector<std::pair<uint64, uint32>> order(count);   // (hash, index) in home-slot order
		for (uint32 i = 0u; i < count; ++i)
		{
			hashes[i] = HashExact(Names[i]);
			order[i] = { hashes[i], i };
		}
		{
			std::vector<std::pair<uint64, uint32>> scratch;
			RadixSortByKey(order, scratch, [this] (const std::pair<uint64, uint32>& Entry) { return static_cast<uint64>(Index.GetHomeSlot(Entry.first)); });
		}
		// A name the index lacks takes, as its representative, the first entry of its spelling within its home
		// slot's group (a repeat always shares the slot).
		std::vector<uint32> representative(count, InvalidId);
		uint32 group = 0u;
		for (uint32 k = 0u; k < count; ++k)
		{
			const auto [hash, index] = order[k];
			if (k > 0u && Index.GetHomeSlot(hash) != Index.GetHomeSlot(order[k - 1u].first))
			{
				group = k;
			}
			if (const std::optional<uint32> existing = FindHashed(Names[index], hash))
			{
				OutIds[index] = *existing;
				continue;
			}
			for (uint32 j = group; j < k; ++j)
			{
				const uint32 other = order[j].second;
				if (order[j].first == hash && OutIds[other] == InvalidId && representative[other] == InvalidId && Names[other] == Names[index])
				{
					representative[index] = other;
					break;
				}
			}
		}
		std::vector<std::pair<uint64, uint32>> added;
		for (uint32 i = 0u; i < count; ++i)
		{
			if (OutIds[i] == InvalidId && representative[i] == InvalidId)
			{
				OutIds[i] = AddUnlisted(Names[i], Epoch);
				added.emplace_back(hashes[i], OutIds[i]);
			}
		}
		for (uint32 i = 0u; i < count; ++i)
		{
			if (representative[i] != InvalidId)
			{
				OutIds[i] = OutIds[representative[i]];
			}
		}
		Index.InsertBulk(added, Epoch);
	}

	// Room in the index for Count more names (a bulk load).
	void Reserve (uint32 Count, uint64 Epoch)
	{
		Index.Reserve(Size() + Count, Epoch);
	}

	[[nodiscard]] uint64 GetResidentBytes () const noexcept
	{
		return Pool.GetResidentBytes() + Folded.GetResidentBytes() + Index.GetResidentBytes();
	}

private:
	[[nodiscard]] std::optional<uint32> FindHashed (FStringView Name, uint64 Hash) const
	{
		std::optional<uint32> found;
		Index.ForEach(Hash, [&] (uint32 Id)
		{
			if (Pool.Get(Id) == Name)
			{
				found = Id;
				return false;
			}
			return true;
		});
		return found;
	}

	uint32 Add (FStringView Name, uint64 Hash, uint64 Epoch)
	{
		const uint32 id = AddUnlisted(Name, Epoch);
		Index.Insert(Hash, id, Epoch);
		return id;
	}

	// The name and its folded shadow, not yet in the index.
	uint32 AddUnlisted (FStringView Name, uint64 Epoch)
	{
		const uint32 id = Pool.Add(Name, Epoch);
		char buffer[registry_detail::FastCaseBytes];
		FString heap;
		const FStringView folded = FoldPortable(Name, buffer, heap);
		Folded.Append(folded == Name ? Pool.GetRef(id) : Pool.Store(folded), Epoch);
		return id;
	}

	static constexpr uint32 InvalidId = ~0u;

public:

private:
	CStringPool Pool;
	CPagedColumn<CStringPool::SRef> Folded;
	CPagedHashIndex Index;
};

/**
 * STRINGS (§5.2): free-text tag values (DisplayName), each with its folded shadow; not interned.
 */
class CTextPool
{
public:
	[[nodiscard]] uint32 Size () const noexcept { return Pool.Size(); }
	[[nodiscard]] FStringView Get (uint32 Id) const noexcept { return Pool.Get(Id); }
	[[nodiscard]] FStringView GetFolded (uint32 Id) const noexcept { return Pool.Resolve(Folded[Id]); }

	uint32 Add (FStringView Text, uint64 Epoch)
	{
		const uint32 id = Pool.Add(Text, Epoch);
		char buffer[registry_detail::FastCaseBytes];
		FString heap;
		Folded.Append(Pool.Store(FoldPortable(Text, buffer, heap)), Epoch);
		return id;
	}

	[[nodiscard]] uint64 GetResidentBytes () const noexcept { return Pool.GetResidentBytes() + Folded.GetResidentBytes(); }

private:
	CStringPool Pool;
	CPagedColumn<CStringPool::SRef> Folded;
};


// ---------------- Tables (§5.2) ----------------

// A mount, indexed by its MountId (CMountTable's). Name empty: no mount has that id.
struct SMountRow
{
	FString Name;                       // "Engine", "Project", "Plugin.X"
	MountId Id = InvalidMountId;
	bool bCaseSensitive = false;        // the mount root's volume rule for the FILES and FOLDERS path indices
	uint32 RootFolder = InvalidRow;     // the FOLDERS row of "/<Name>"
	uint32 ConfigRuleFile = InvalidRow; // the RULEFILES row of the mount's Config/Assets.yml
};

// FILES. Folder = InvalidRow: a removed row. Path is the full mount path in on-disk spelling (PATHS).
struct SFilesTable
{
	CPagedColumn<uint32> Folder;
	CPagedColumn<uint32> LeafName;                        // NAMES
	CPagedColumn<uint32> Path;                            // PATHS
	CPagedColumn<CStringPool::SRef> FoldedPath;           // Path's folded shadow (FString::FoldCase) in PATHS' bytes
	CPagedColumn<MountId> Mount;
	CPagedColumn<uint64> Size;
	CPagedColumn<int64> MTime;
	CPagedColumn<platform::SFileIdentity> Identity;       // 24 B, as platform::Stat reports it
	CPagedColumn<hash::SContentHash> ContentHash;         // zero = not hashed
	CPagedColumn<uint64> MetaHash;
	CPagedColumn<EFileClass> Class;                       // effective: Companion while a record names the file
	CPagedColumn<EFileClass> BaseClass;                   // as the last SFileUpsert gave it
	CPagedColumn<EMetaState> MetaState;
	CPagedColumn<uint32> Record;                          // ASSETS row or InvalidRow
	// The order of the live files by folded path, then path bytes (the queries' result order, §5.6 step 5), as
	// increasing numbers with gaps (CompareFileOrder): a file added between two others takes a number between
	// theirs, so a commit renumbers nothing unless a gap runs out or it adds many files at once.
	CPagedColumn<uint32> PathRank;
};

// FOLDERS, pre-order numbered per mount: a folder's subtree is the folders whose PreorderBegin lies in
// [PreorderBegin, PreorderEnd) of the same mount. Mount = InvalidMountId: a removed row.
struct SFoldersTable
{
	CPagedColumn<uint32> Parent;                          // InvalidRow for a mount root
	CPagedColumn<uint32> Name;                            // NAMES (a mount root: the mount name)
	CPagedColumn<uint32> PreorderBegin;
	CPagedColumn<uint32> PreorderEnd;
	CPagedColumn<uint32> RuleFile;                        // RULEFILES row of its Assets.yml, InvalidRow = none
	CPagedColumn<uint32> Path;                            // PATHS
	CPagedColumn<MountId> Mount;
};

// RULEFILES. Folder = InvalidRow: the mount's Config/Assets.yml. Mount = InvalidMountId: a removed row.
struct SRuleFilesTable
{
	CPagedColumn<MountId> Mount;
	CPagedColumn<uint32> Folder;
	CPagedColumn<uint64> RulesHash;
};

// ASSETS. File = InvalidRow: a removed record. Flags hold the producer's flags plus HasProblems, which the
// store derives from the problem lists.
struct SAssetsTable
{
	CPagedColumn<SGuid> Guid;
	CPagedColumn<uint64> AssetId64;
	CPagedColumn<uint32> File;
	CPagedColumn<reflect::TypeId> Type;
	CPagedColumn<uint16> AssetType;                       // M3: equal to Type
	CPagedColumn<uint16> Importer;                        // index into SRegistryTables::Importers
	CPagedColumn<ERecordFlags> Flags;
	CPagedColumn<ECookRule> CookRule;
	CPagedColumn<uint16> Chunk;
	CPagedColumn<uint32> PrimaryName;                     // NAMES, InvalidRow = none
	CPagedColumn<SPoolRange> Labels;                      // in SRegistryTables::Labels
	CPagedColumn<uint32> FirstSub;
	CPagedColumn<uint32> NumSubs;
	CPagedColumn<uint32> TypeRow;                         // row in Types[Type]
	CPagedColumn<EImportStatus> ImportStatus;
	CPagedColumn<uint16> Problems;                        // diagnostics of the record's subjects (clamped)
	CPagedColumn<uint64> AnalyzeStamp;
	CPagedColumn<uint32> Name;                            // NAMES: the stem ($Name)
	CPagedColumn<uint64> ChangedRevision;                 // the revision that last changed the record
	CPagedColumn<SPoolRange> Companions;                  // in SRegistryTables::Companions (PATHS ids)
	CPagedColumn<SPoolRange> BaseEdges;                   // the record's slice of SGraphTables::BaseEdges
	CPagedBitmap BaseEdgesMasked;                         // the slice is superseded (or the record removed)
	CPagedColumn<SPoolRange> OverflowEdges;               // the record's edges since the last compaction
};

// SUBENTRY. Owner = InvalidRow: a removed row. A record's sub-entries are FirstSub .. FirstSub + NumSubs.
struct SSubEntryTable
{
	CPagedColumn<uint32> Owner;                           // ASSETS row
	CPagedColumn<uint64> SubId;
	CPagedColumn<uint32> Key;                             // NAMES: the label or row key
	CPagedColumn<reflect::TypeId> Type;
	CPagedColumn<ERecordFlags> Flags;
	CPagedColumn<uint32> TypeRow;
	CPagedColumn<uint64> AssetId64;
	// The sub-entry's place among its record's sub-entries by folded label, then label bytes (0 first): with
	// SFilesTable::PathRank, the result order of a query needs no string compare.
	CPagedColumn<uint32> LabelRank;
};


// ---------------- Dependency graph (§5.2 DEPS) ----------------

/**
 * Edges point at a TARGETS row, never at a record: a target is what the source named - a mount path
 * (optionally with "#label") or an SAssetId - and is resolved to a record by the store, at every commit
 * that changes the records its key can match. So an unresolved edge is kept and rebound in place, and the
 * graph never depends on Analyze or commit order. The edge's target record and TargetSub are the target
 * row's (Resolved, TargetSub); its effective flags carry Unresolved unless the target is Resolved or Companion.
 * Companion: a Path target that names no record but a live FILES row of class Companion (a glTF's .bin under
 * §5.2's companion rule): the file the source needs is there, so the edge is not broken, yet there is no
 * record to point at (Resolved stays InvalidRow). It is re-resolved whenever that path's companion state
 * changes, in either commit order, like every other target.
 */
enum class ETargetKind : uint8 { Path, Guid };
enum class ETargetState : uint8 { Unresolved, Resolved, Ambiguous, Companion };   // Ambiguous: PATH001, two records match

struct STargetRow
{
	SGuid Guid;                         // Guid targets
	uint64 TargetSub = 0u;              // Guid targets: the id's SubId; Path targets: MakeSubId(label) once resolved
	uint32 Text = InvalidRow;           // PATHS: the path as the source wrote it ("#label" included); a Guid target's hint
	uint32 Resolved = InvalidRow;       // ASSETS row
	uint32 FilePartLength = 0u;         // Path targets, resolved: bytes of Text naming the file (the label follows a '#')
	ETargetKind Kind = ETargetKind::Path;
	ETargetState State = ETargetState::Unresolved;
	bool bSpellingDiffers = false;      // resolved under the fold with a spelling other than the file's (REF006)
};

// A record keeps one edge per distinct (FromSub, Target, Flags) (§5.2 "one edge per distinct (target, sub,
// flags)"): a repeat with another FieldPath (two material slots naming one texture) is dropped and the
// first FieldPath stays, so each source counts once among a target's referencers.
// FromSub names the declaring sub-entry by its position among the record's sub-entries (+1; 0 = the record): a
// record's edges are rewritten whenever its sub-entries are, and compaction keeps their order, so the position
// stays right, in 4 bytes instead of an 8-byte SubId (16 bytes an edge instead of 24). GetEdgeFromSubId gives the
// SubId back.
struct SEdgeRow
{
	uint32 FromSub = 0u;                // 0 = the record; else 1 + the sub-entry's index from the record's FirstSub
	uint32 Target = InvalidRow;         // TARGETS row
	uint32 FieldPath = InvalidRow;      // NAMES
	EDepFlags Flags {};                 // as declared, without Unresolved

	bool operator== (const SEdgeRow&) const noexcept = default;
};

// A reverse entry: Source is the ASSETS row whose edge Edge (OverflowEdgeBit | an OverflowEdges index, else a
// BaseEdges index) names the target. It is live while that edge is still one of Source's live edges.
inline constexpr uint32 OverflowEdgeBit = 1u << 31u;
struct SReverseEdge
{
	uint32 Source = InvalidRow;
	uint32 Edge = InvalidRow;
};

// An entry of a target's overflow list: the reverse entry and the next older entry of the same target. (The base
// CSR's entries need no link: 8 bytes an edge instead of 12.)
struct SOverflowReverseEdge
{
	uint32 Source = InvalidRow;
	uint32 Edge = InvalidRow;
	uint32 Next = InvalidRow;
};

/**
 * The persisted CSR (BaseEdges, sliced per record by SAssetsTable::BaseEdges; BaseReverseEdges, sliced per
 * target by BaseReverse) plus per-record overflow lists for edges added since the last compaction. A record
 * upsert masks its base slice and appends its new edges; a removed edge is never edited, only masked. The
 * reverse side grows one overflow entry per new edge, linked per target; entries of masked edges stay until
 * a compaction rebuilds both CSRs (never per edit).
 */
struct SGraphTables
{
	CPagedColumn<SEdgeRow> BaseEdges;
	CPagedColumn<SEdgeRow> OverflowEdges;
	CPagedColumn<STargetRow> Targets;
	CPagedColumn<SPoolRange> BaseReverse;                 // per target row
	CPagedColumn<SReverseEdge> BaseReverseEdges;
	CPagedColumn<uint32> OverflowReverseHead;             // per target row: newest entry, InvalidRow = none
	CPagedColumn<SOverflowReverseEdge> OverflowReverseEdges;
	CPagedHashIndex TargetsByKey;                         // Path: HashExact(Text); Guid: HashGuid ^ TargetSub
	CPagedHashIndex TargetsByResolved;                    // MixRegistryKey(ASSETS row) -> target rows resolved to it
	uint32 LiveEdges = 0u;
	uint32 GarbageEdges = 0u;                             // masked base and superseded overflow edges
};


// ---------------- Type tables (§5.2) ----------------

// A tag key as the registry stores it: every name interned (NAMES).
struct STagKeyDesc
{
	uint32 NameId = InvalidRow;
	ETagType Type = ETagType::None;
	ETagFlags Flags = ETagFlags::None;
	uint32 EnumTable = InvalidRow;
	uint32 UnitNameId = InvalidRow;
};

// One key's column: Values (int64 | double bits | bool | NameId | STRINGS id | AssetId64 | row key), Present
// (absent = no value, never zero), MultiValue keys' per-row lists in Multi. MultiRanges and Multi hold rows
// only for a MultiValue key (another key's are empty: Size() 0); Values holds such a row's first item.
struct STagColumn
{
	CPagedColumn<uint64> Values;
	CPagedBitmap PresentBits;
	CPagedColumn<SPoolRange> MultiRanges;
	CPagedPool<uint64> Multi;
};

/**
 * The rows of every record of one type (an asset or a sub-entry), one column per tag key. Records[r] is the
 * row's SRecordId value (a view stamps its revision); Dead marks tombstoned rows, which queries skip.
 */
class CTypeTable
{
public:
	reflect::TypeId Type = 0u;
	uint64 TagSchemaHash = 0u;
	CPagedColumn<uint32> Records;
	CPagedBitmap Dead;
	std::vector<STagKeyDesc> Keys;
	std::vector<STagColumn> Columns;      // Columns[k] belongs to Keys[k]
	uint32 LiveRows = 0u;
};

// XXH3 of the type's hand-written key list (name, type, flags per key): M3's TagSchemaHash (§5.3).
[[nodiscard]] FRT_CORE_API uint64 ComputeTagSchemaHash (const SAssetTypeInfo& Type) noexcept;


// ---------------- Problems ----------------

using FProblemList = std::shared_ptr<const std::vector<SDiagnostic>>;
inline constexpr uint32 ProblemSourceCount = 5u;   // EProblemSource
static_assert(ProblemSourceCount == static_cast<uint32>(EProblemSource::Unclaimed) + 1u, "a list per EProblemSource");

// Per (subject, source) lists (RegistryDelta.h). A subject row is never removed; its lists empty instead.
struct SProblemTables
{
	CPagedColumn<uint32> Subject;                          // PATHS
	CPagedColumn<FProblemList> Lists[ProblemSourceCount];  // null = empty
	CPagedHashIndex BySubject;                             // HashExact(subject)
};


// ---------------- Indices ----------------

struct SRegistryIndices
{
	CPagedHashIndex FilesByPath;              // FILES, the mount's rule (HashMountRule); equal rows coexist
	CPagedHashIndex FilesByPortablePath;      // FILES, HashPortable (companions, PATH001 checks)
	CPagedHashIndex FoldersByPath;            // FOLDERS, the mount's rule
	CPagedHashIndex FoldersByPortablePath;    // FOLDERS, HashPortable (queries' dir:)
	CPagedHashIndex RecordsByPortablePath;    // ASSETS, HashPortable of the file path; several records per key
	CPagedHashIndex RecordsByGuid;            // ASSETS, HashGuid
	CPagedHashIndex RecordsById64;            // ASSETS and SUBENTRY: AssetId64 -> SRecordId value
	CPagedHashIndex Aliases;                  // non-canonical SAssetId -> ASSETS row (§5.2; empty in M3)
};


// ---------------- The view's tables ----------------

struct SRegistryTables
{
	uint64 Revision = 0u;
	std::shared_ptr<const std::vector<SMountRow>> Mounts;        // indexed by MountId
	std::shared_ptr<const std::vector<FString>> Importers;       // the importer-name table (ASSETS::Importer)

	CNamePool Names;
	CTextPool Strings;
	CStringPool Paths;                                           // file, folder, target, subject and companion paths

	SFilesTable Files;
	SFoldersTable Folders;
	SRuleFilesTable RuleFiles;
	SAssetsTable Assets;
	SSubEntryTable SubEntries;
	CPagedPool<uint32> Labels;                                   // NameId | RuleLabelBit
	CPagedPool<uint32> Companions;                               // PATHS ids
	SGraphTables Graph;
	std::vector<CTypeTable> Types;                               // indexed by TypeId (CAssetTypeRegistry)
	SProblemTables Problems;
	SRegistryIndices Indices;

	uint32 LiveFiles = 0u;
	uint32 LiveFolders = 0u;
	uint32 LiveRuleFiles = 0u;
	uint32 LiveRecords = 0u;
	uint32 LiveSubEntries = 0u;
	// Items of Labels, Companions, the type tables' Multi pools and STRINGS that no live row uses any more
	// (replaced lists, dead rows' values): what CRegistryStore::CompactTables reclaims.
	uint32 GarbagePoolItems = 0u;
};

inline constexpr uint32 RuleLabelBit = 1u << 31u;   // SRegistryTables::Labels: a rule label (Assets.yml), else authored

// The tables of a view (a free function so the view's public API stays §5.7's).
[[nodiscard]] FRT_CORE_API const SRegistryTables& GetTables (const CRegistryView& View) noexcept;

// GetAssetStem (ImporterTable.h: the record's $Name) of a file's leaf name, the same answer for every name: an ASCII
// name compares its lowered tail with the importers' extensions, lowered once, instead of folding a compare per
// extension (about 200 ns a name, most of what interning the stems cost a load); any other name asks GetAssetStem.
[[nodiscard]] FRT_CORE_API FStringView GetRecordStem (FStringView LeafName) noexcept;

// The order SFilesTable::PathRank keeps: folded path bytes, then path bytes, then the row (two live files never
// share a path; the row only makes the order total). < 0, 0, > 0.
[[nodiscard]] inline int32 CompareFileOrder (const SRegistryTables& Tables, uint32 A, uint32 B) noexcept;

// A live FILES row's mount path folded (FString::FoldCase), for caseless substring search over whole paths
// ("Textures/T_Rock" crosses a '/') without folding per query. Paths never change, so neither does this.
[[nodiscard]] inline FStringView GetFoldedFilePath (const SRegistryTables& Tables, uint32 FileRow) noexcept
{
	return Tables.Paths.Resolve(Tables.Files.FoldedPath[FileRow]);
}

inline int32 CompareFileOrder (const SRegistryTables& Tables, uint32 A, uint32 B) noexcept
{
	int32 compared = GetFoldedFilePath(Tables, A).Compare(GetFoldedFilePath(Tables, B));
	if (compared == 0)
	{
		compared = Tables.Paths.Get(Tables.Files.Path[A]).Compare(Tables.Paths.Get(Tables.Files.Path[B]));
	}
	return compared != 0 ? compared : (A < B ? -1 : (A > B ? 1 : 0));
}

// The order SSubEntryTable::LabelRank keeps among one record's sub-entries: folded label bytes, label bytes, row.
[[nodiscard]] inline bool LessSubEntryLabel (const SRegistryTables& Tables, uint32 A, uint32 B) noexcept
{
	const uint32 keyA = Tables.SubEntries.Key[A];
	const uint32 keyB = Tables.SubEntries.Key[B];
	if (keyA != keyB)
	{
		const int32 folded = Tables.Names.GetFolded(keyA).Compare(Tables.Names.GetFolded(keyB));
		if (folded != 0)
		{
			return folded < 0;
		}
		const int32 raw = Tables.Names.Get(keyA).Compare(Tables.Names.Get(keyB));
		if (raw != 0)
		{
			return raw < 0;
		}
	}
	return A < B;
}




// ---------------- Lookups over the tables (no allocation) ----------------

// The mount with that name (exact bytes), or null.
[[nodiscard]] FRT_CORE_API const SMountRow* FindMountRow (const SRegistryTables& Tables, FStringView Name) noexcept;
// The mount a mount path names (its first segment), or null.
[[nodiscard]] FRT_CORE_API const SMountRow* FindMountRowForPath (const SRegistryTables& Tables, FStringView MountPath) noexcept;

// The FILES row spelled exactly so (byte-exact), or InvalidRow.
[[nodiscard]] FRT_CORE_API uint32 FindFileRowExact (const SRegistryTables& Tables, FStringView MountPath) noexcept;
// The FILES row the OS means: byte-exact first, else the single row equal under the mount's rule; InvalidRow
// when none or several (a case-sensitive directory inside a case-insensitive mount holds both spellings).
[[nodiscard]] FRT_CORE_API uint32 FindFileRow (const SRegistryTables& Tables, FStringView MountPath) noexcept;
// The same for FOLDERS (a mount root included).
[[nodiscard]] FRT_CORE_API uint32 FindFolderRowExact (const SRegistryTables& Tables, FStringView MountPath) noexcept;
[[nodiscard]] FRT_CORE_API uint32 FindFolderRow (const SRegistryTables& Tables, FStringView MountPath) noexcept;
// Folders equal to MountPath under the portable fold (queries' dir:); returns how many, the first in OutRow.
FRT_CORE_API uint32 FindFolderRowsPortable (const SRegistryTables& Tables, FStringView MountPath, uint32& OutRow) noexcept;

// Live records whose file path equals FileMountPath under the portable fold: how many there are, and the
// one with the lowest row in OutRow (InvalidRow when none).
FRT_CORE_API uint32 FindRecordRowsPortable (const SRegistryTables& Tables, FStringView FileMountPath, uint32& OutRow) noexcept;
// The same with HashPortable(FileMountPath) given.
FRT_CORE_API uint32 FindRecordRowsPortableHashed (const SRegistryTables& Tables, FStringView FileMountPath, uint64 PortableHash, uint32& OutRow) noexcept;
// The SRecordId value with that AssetId64 (asset or sub-entry), or InvalidRow.
[[nodiscard]] FRT_CORE_API uint32 FindRecordValueById64 (const SRegistryTables& Tables, uint64 AssetId64) noexcept;
// The sub-entry row of an ASSETS row with that SubId, or InvalidRow.
[[nodiscard]] FRT_CORE_API uint32 FindSubEntryRow (const SRegistryTables& Tables, uint32 AssetRow, uint64 SubId) noexcept;

// The subject row of a problem subject, or InvalidRow.
[[nodiscard]] FRT_CORE_API uint32 FindProblemSubject (const SRegistryTables& Tables, FStringView Subject) noexcept;

// The SUBENTRY row an edge of the ASSETS row AssetRow comes from, or InvalidRow for the record itself.
[[nodiscard]] inline uint32 GetEdgeSourceSub (const SRegistryTables& Tables, uint32 AssetRow, const SEdgeRow& Edge) noexcept
{
	return Edge.FromSub != 0u ? Tables.Assets.FirstSub[AssetRow] + Edge.FromSub - 1u : InvalidRow;
}

// The SubId of that sub-entry (SEdgeDecl::FromSub), 0 for the record.
[[nodiscard]] inline uint64 GetEdgeFromSubId (const SRegistryTables& Tables, uint32 AssetRow, const SEdgeRow& Edge) noexcept
{
	const uint32 sub = GetEdgeSourceSub(Tables, AssetRow, Edge);
	return sub != InvalidRow ? Tables.SubEntries.SubId[sub] : 0u;
}

// True when the edge Edge (OverflowEdgeBit | index, or a base index) is still one of Source's live edges.
[[nodiscard]] FRT_CORE_API bool IsEdgeLive (const SRegistryTables& Tables, uint32 Source, uint32 Edge) noexcept;
[[nodiscard]] FRT_CORE_API const SEdgeRow& GetEdgeRow (const SRegistryTables& Tables, uint32 Edge) noexcept;

// Visit(uint32 EdgeRef) for every live edge of an ASSETS row, in declaration order (EdgeRef as in IsEdgeLive).
template <typename TVisit>
void ForEachLiveEdge (const SRegistryTables& Tables, uint32 AssetRow, TVisit&& Visit)
{
	const SAssetsTable& assets = Tables.Assets;
	if (!assets.BaseEdgesMasked.Test(AssetRow))
	{
		const SPoolRange base = assets.BaseEdges[AssetRow];
		for (uint32 i = 0u; i < base.Count; ++i)
		{
			Visit(base.Begin + i);
		}
	}
	const SPoolRange overflow = assets.OverflowEdges[AssetRow];
	for (uint32 i = 0u; i < overflow.Count; ++i)
	{
		Visit(OverflowEdgeBit | (overflow.Begin + i));
	}
}

// Visit(const SReverseEdge&) for every live reverse entry of a TARGETS row.
template <typename TVisit>
void ForEachLiveReverseEdge (const SRegistryTables& Tables, uint32 TargetRow, TVisit&& Visit)
{
	const SGraphTables& graph = Tables.Graph;
	if (TargetRow < graph.BaseReverse.Size())
	{
		const SPoolRange range = graph.BaseReverse[TargetRow];
		for (uint32 i = 0u; i < range.Count; ++i)
		{
			const SReverseEdge& entry = graph.BaseReverseEdges[range.Begin + i];
			if (IsEdgeLive(Tables, entry.Source, entry.Edge))
			{
				Visit(entry);
			}
		}
	}
	for (uint32 node = graph.OverflowReverseHead[TargetRow]; node != InvalidRow; node = graph.OverflowReverseEdges[node].Next)
	{
		const SOverflowReverseEdge& entry = graph.OverflowReverseEdges[node];
		if (IsEdgeLive(Tables, entry.Source, entry.Edge))
		{
			Visit(SReverseEdge { entry.Source, entry.Edge });
		}
	}
}


// ---------------- Memory ----------------

// Resident bytes of a view (every page, pool and index it keeps alive), per table (§5.11).
struct SRegistryMemoryStats
{
	uint64 Files = 0u;
	uint64 Folders = 0u;
	uint64 RuleFiles = 0u;
	uint64 Assets = 0u;
	uint64 SubEntries = 0u;
	uint64 TypeTables = 0u;
	uint64 Graph = 0u;
	uint64 Names = 0u;
	uint64 Strings = 0u;          // STRINGS and the path pool
	uint64 Labels = 0u;           // labels and companions
	uint64 Problems = 0u;
	uint64 Indices = 0u;
	uint64 Writer = 0u;           // CRegistryStore::GetMemoryStats only: the writer's lists and maps
	uint64 ChangeRing = 0u;       // CRegistryStore::GetMemoryStats only
	uint64 Total = 0u;
};

[[nodiscard]] FRT_CORE_API SRegistryMemoryStats ComputeMemoryStats (const SRegistryTables& Tables);


// ---------------- Images (snapshots, AdoptImage / ExportImage) ----------------

/**
 * Every table of a set of mounts as plain vectors, with its own string pools and name tables, so a
 * snapshot writer serializes it and a loader rebuilds a store from it (CRegistryStore::AdoptImage) without
 * knowing the paged layout. Indices into Names / Strings are image-local; ~0u is none. Mount, type and
 * importer references go through the image's own tables by name, so ids of another process never leak in.
 *
 *  - Folders: every folder of the image's mounts, each mount's root included (Parent ~0u, Name = the mount
 *    name). A parent precedes its children.
 *  - Files: Class is the base class (what the scan said); the store derives Companion again.
 *  - Records: Flags without HasProblems (derived again). Type indexes TypeNames; Importer indexes
 *    ImporterNames (~0 none). Ranges index Labels, SubEntries, Tags, Edges and Companions.
 *  - Edges: a Path target keeps the path the source wrote; a Guid target keeps its id - unless it resolved
 *    to a record outside the image's mounts, which is exported by that record's mount path (§5.3).
 */
struct SImageMount
{
	FString Name;
	MountId Id = InvalidMountId;      // the id AdoptImage gives the mount (the loader remaps it by name first)
	bool bCaseSensitive = false;
};

struct SImageFolder
{
	uint16 Mount = 0u;                // index into SRegistryImage::Mounts
	uint32 Parent = ~0u;              // index into Folders; ~0u for a mount root
	uint32 Name = ~0u;                // Names
};

struct SImageFile
{
	uint32 Folder = ~0u;
	uint32 LeafName = ~0u;            // Names
	uint64 Size = 0u;
	int64 MTime = 0;
	platform::SFileIdentity Identity;
	hash::SContentHash ContentHash;
	uint64 MetaHash = 0u;
	EFileClass Class = EFileClass::Unclaimed;
	EMetaState MetaState = EMetaState::PendingMeta;
};

struct SImageRuleFile
{
	uint16 Mount = 0u;
	uint32 Folder = ~0u;              // ~0u: the mount's Config/Assets.yml
	uint64 RulesHash = 0u;
};

struct SImageTag
{
	uint32 Key = ~0u;                 // Names
	ETagType Type = ETagType::None;
	uint64 Bits = 0u;
	uint32 Text = ~0u;                // Name: Names; String: Strings
};

struct SImageSubEntry
{
	uint32 Label = ~0u;               // Names
	uint64 SubId = 0u;
	uint16 Type = 0u;                 // TypeNames
	ERecordFlags Flags {};
	SPoolRange Tags;
};

struct SImageEdge
{
	uint64 FromSub = 0u;
	SAssetId Target;                  // valid: a Guid target
	uint32 TargetPath = ~0u;          // Strings: a Path target's text, or a Guid target's hint
	EDepFlags Flags {};
	uint32 FieldPath = ~0u;           // Names
};

struct SImageRecord
{
	uint32 File = ~0u;
	SGuid Guid;
	uint16 Type = 0u;                 // TypeNames
	uint16 Importer = 0xFFFFu;        // ImporterNames
	ERecordFlags Flags {};
	ECookRule CookRule = ECookRule::Auto;
	uint16 Chunk = 0u;
	uint32 PrimaryName = ~0u;         // Names
	EImportStatus ImportStatus = EImportStatus::NotAnalyzed;
	uint64 AnalyzeStamp = 0u;
	SPoolRange Labels;                // into Labels: Names ids | RuleLabelBit
	SPoolRange SubEntries;
	SPoolRange Tags;
	SPoolRange Edges;
	SPoolRange Companions;            // into Companions: Strings ids
};

struct SImageProblems
{
	uint32 Subject = ~0u;             // Strings
	EProblemSource Source = EProblemSource::Scan;
	std::vector<SDiagnostic> Diagnostics;
};

struct SRegistryImage
{
	std::vector<SImageMount> Mounts;
	std::vector<FString> Names;
	std::vector<FString> Strings;
	std::vector<FString> TypeNames;       // stable type names ("frt.Texture")
	std::vector<FString> ImporterNames;
	std::vector<SImageFolder> Folders;
	std::vector<SImageFile> Files;
	std::vector<SImageRuleFile> RuleFiles;
	std::vector<SImageRecord> Records;
	std::vector<uint32> Labels;
	std::vector<SImageSubEntry> SubEntries;
	std::vector<SImageTag> Tags;
	std::vector<SImageEdge> Edges;
	std::vector<uint32> Companions;
	std::vector<SImageProblems> Problems;
};

/**
 * The delta that recreates an image's contents (what CRegistryStore::AdoptImage commits). A record whose type
 * name CAssetTypeRegistry does not know becomes frt.Unknown with Schemaless | StaleTags and no tags (a
 * sub-entry likewise, flagging its record). Out-of-range indices drop the item they appear in.
 */
[[nodiscard]] FRT_CORE_API SRegistryDelta MakeImageDelta (SRegistryImage&& Image);
}


FRT_DECLARE_ENUM_REFLECTION(
	frt::assets::ETargetState,
	FRT_ENUM_ENTRY(frt::assets::ETargetState, Unresolved),
	FRT_ENUM_ENTRY(frt::assets::ETargetState, Resolved),
	FRT_ENUM_ENTRY(frt::assets::ETargetState, Ambiguous),
	FRT_ENUM_ENTRY(frt::assets::ETargetState, Companion));
