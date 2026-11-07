#include "Assets/Registry/RegistryView.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>

#include "Assets/AssetDiagnosticCodes.h"
#include "Assets/AssetTypes.h"
#include "Assets/Pipeline/ImporterTable.h"


namespace frt::assets
{
// ---------------- Table lookups (RegistryTables.h) ----------------

const SRegistryTables& GetTables (const CRegistryView& View) noexcept
{
	return View.Tables;
}

namespace
{
// Every extension an importer of the table claims (available or not), lowered, longest first; empty when one is
// not ASCII (then every name goes to GetAssetStem).
struct SStemExtensions
{
	static constexpr uint32 MaxBytes = 64u;

	SStemExtensions () noexcept
	{
		for (const SImporterDesc& importer : GetImporters())
		{
			for (const FStringLiteral extension : importer.Extensions)
			{
				const FStringView text = extension;
				char lowered[MaxBytes];
				if (text.Len() == 0u || text.Len() > MaxBytes || (registry_detail::LowerAscii(text, lowered) & 0x80u) != 0u)
				{
					bUsable = false;
					return;
				}
				FString value(FStringView(lowered, text.Len()));
				if (std::find(Lowered.begin(), Lowered.end(), value) == Lowered.end())
				{
					Lowered.push_back(std::move(value));
				}
				Longest = (std::max)(Longest, text.Len());
			}
		}
		std::stable_sort(Lowered.begin(), Lowered.end(), [] (const FString& A, const FString& B) { return A.Len() > B.Len(); });
	}

	std::vector<FString> Lowered;
	uint32 Longest = 0u;
	bool bUsable = true;
};
}

FStringView GetRecordStem (FStringView LeafName) noexcept
{
	static const SStemExtensions extensions;
	if (!extensions.bUsable)
	{
		return GetAssetStem(LeafName);
	}
	// The suffix GetAssetStem's folded compare matches lies in the last bytes: when they are ASCII, a match is a
	// byte compare of the lowered tail (each ASCII byte is one key of the fold), and it removes exactly the
	// extension's bytes, with at least one byte before it.
	const uint32 tail = (std::min)(LeafName.Len(), extensions.Longest);
	char lowered[SStemExtensions::MaxBytes];
	// The last bytes as bytes (they may begin inside a code point, which only sends the name to GetAssetStem).
	const FStringView last(LeafName.GetData() + (LeafName.Len() - tail), tail);
	if ((registry_detail::LowerAscii(last, lowered) & 0x80u) != 0u || LeafName.Contains('/'))
	{
		return GetAssetStem(LeafName);
	}
	for (const FString& extension : extensions.Lowered)
	{
		const uint32 length = extension.Len();
		if (length < LeafName.Len() && std::memcmp(lowered + (tail - length), extension.GetData(), length) == 0)
		{
			return LeafName.LeftChop(length);
		}
	}
	const uint32 dot = LeafName.FindLast('.');
	return dot == FStringView::NotFound || dot == 0u ? LeafName : LeafName.Left(dot);
}

uint64 ComputeTagSchemaHash (const SAssetTypeInfo& Type) noexcept
{
	FString text;
	text += Type.Name;
	text += '\n';
	for (const SStaticTagKey& key : Type.Keys)
	{
		text += key.Name;
		text += ':';
		text += enum_::ToString(key.Type);
		text += ':';
		text.AppendNumber(static_cast<uint32>(key.Flags));
		text += '\n';
	}
	return hash::Xxh3_64(text.View());
}

const SMountRow* FindMountRow (const SRegistryTables& Tables, FStringView Name) noexcept
{
	if (Tables.Mounts == nullptr)
	{
		return nullptr;
	}
	for (const SMountRow& mount : *Tables.Mounts)
	{
		if (!mount.Name.IsEmpty() && mount.Name == Name)
		{
			return &mount;
		}
	}
	return nullptr;
}

const SMountRow* FindMountRowForPath (const SRegistryTables& Tables, FStringView MountPath) noexcept
{
	FStringView mountName;
	FStringView relative;
	if (!SplitMountPath(MountPath, mountName, relative))
	{
		return nullptr;
	}
	return FindMountRow(Tables, mountName);
}

namespace
{
template <typename TIsLive>
uint32 FindRowExact (const SRegistryTables& Tables, const CPagedHashIndex& Index, const CPagedColumn<uint32>& Paths,
	FStringView MountPath, TIsLive&& IsLive) noexcept
{
	const SMountRow* mount = FindMountRowForPath(Tables, MountPath);
	if (mount == nullptr)
	{
		return InvalidRow;
	}
	uint32 found = InvalidRow;
	Index.ForEach(HashMountRule(MountPath, mount->bCaseSensitive), [&] (uint32 Row)
	{
		if (IsLive(Row) && Tables.Paths.Get(Paths[Row]) == MountPath)
		{
			found = Row;
			return false;
		}
		return true;
	});
	return found;
}

template <typename TIsLive>
uint32 FindRowByMountRule (const SRegistryTables& Tables, const CPagedHashIndex& Index, const CPagedColumn<uint32>& Paths,
	FStringView MountPath, TIsLive&& IsLive) noexcept
{
	const uint32 exact = FindRowExact(Tables, Index, Paths, MountPath, IsLive);
	if (exact != InvalidRow)
	{
		return exact;
	}
	const SMountRow* mount = FindMountRowForPath(Tables, MountPath);
	if (mount == nullptr)
	{
		return InvalidRow;
	}
	uint32 found = InvalidRow;
	uint32 matches = 0u;
	Index.ForEach(HashMountRule(MountPath, mount->bCaseSensitive), [&] (uint32 Row)
	{
		if (IsLive(Row) && EqualMountRule(Tables.Paths.Get(Paths[Row]), MountPath, mount->bCaseSensitive))
		{
			found = Row;
			++matches;
		}
		return true;
	});
	return matches == 1u ? found : InvalidRow;
}
}

uint32 FindFileRowExact (const SRegistryTables& Tables, FStringView MountPath) noexcept
{
	return FindRowExact(Tables, Tables.Indices.FilesByPath, Tables.Files.Path, MountPath,
		[&] (uint32 Row) { return Tables.Files.Folder[Row] != InvalidRow; });
}

uint32 FindFileRow (const SRegistryTables& Tables, FStringView MountPath) noexcept
{
	return FindRowByMountRule(Tables, Tables.Indices.FilesByPath, Tables.Files.Path, MountPath,
		[&] (uint32 Row) { return Tables.Files.Folder[Row] != InvalidRow; });
}

uint32 FindFolderRowExact (const SRegistryTables& Tables, FStringView MountPath) noexcept
{
	return FindRowExact(Tables, Tables.Indices.FoldersByPath, Tables.Folders.Path, MountPath,
		[&] (uint32 Row) { return Tables.Folders.Mount[Row] != InvalidMountId; });
}

uint32 FindFolderRow (const SRegistryTables& Tables, FStringView MountPath) noexcept
{
	return FindRowByMountRule(Tables, Tables.Indices.FoldersByPath, Tables.Folders.Path, MountPath,
		[&] (uint32 Row) { return Tables.Folders.Mount[Row] != InvalidMountId; });
}

uint32 FindFolderRowsPortable (const SRegistryTables& Tables, FStringView MountPath, uint32& OutRow) noexcept
{
	OutRow = InvalidRow;
	uint32 matches = 0u;
	Tables.Indices.FoldersByPortablePath.ForEach(HashPortable(MountPath), [&] (uint32 Row)
	{
		if (Tables.Folders.Mount[Row] != InvalidMountId && EqualPortable(Tables.Paths.Get(Tables.Folders.Path[Row]), MountPath))
		{
			++matches;
			OutRow = OutRow == InvalidRow || Row < OutRow ? Row : OutRow;
		}
		return true;
	});
	return matches;
}

uint32 FindRecordRowsPortable (const SRegistryTables& Tables, FStringView FileMountPath, uint32& OutRow) noexcept
{
	return FindRecordRowsPortableHashed(Tables, FileMountPath, HashPortable(FileMountPath), OutRow);
}

uint32 FindRecordRowsPortableHashed (const SRegistryTables& Tables, FStringView FileMountPath, uint64 PortableHash, uint32& OutRow) noexcept
{
	OutRow = InvalidRow;
	uint32 matches = 0u;
	Tables.Indices.RecordsByPortablePath.ForEach(PortableHash, [&] (uint32 Row)
	{
		const uint32 file = Tables.Assets.File[Row];
		if (file != InvalidRow && EqualPortable(Tables.Paths.Get(Tables.Files.Path[file]), FileMountPath))
		{
			++matches;
			OutRow = OutRow == InvalidRow || Row < OutRow ? Row : OutRow;
		}
		return true;
	});
	return matches;
}

// Two records with one GUID share their ids: the one of the preferred row (IsPreferredGuidRow) answers.
uint32 FindRecordValueById64 (const SRegistryTables& Tables, uint64 AssetId64) noexcept
{
	uint32 found = InvalidRow;
	uint32 foundOwner = InvalidRow;
	Tables.Indices.RecordsById64.ForEach(MixRegistryKey(AssetId64), [&] (uint32 Value)
	{
		uint32 owner = InvalidRow;
		if ((Value & SubEntryBit) != 0u)
		{
			const uint32 sub = Value & ~SubEntryBit;
			if (Tables.SubEntries.Owner[sub] != InvalidRow && Tables.SubEntries.AssetId64[sub] == AssetId64)
			{
				owner = Tables.SubEntries.Owner[sub];
			}
		}
		else if (Tables.Assets.File[Value] != InvalidRow && Tables.Assets.AssetId64[Value] == AssetId64)
		{
			owner = Value;
		}
		if (owner != InvalidRow && IsPreferredGuidRow(Tables, owner, foundOwner))
		{
			found = Value;
			foundOwner = owner;
		}
		return true;
	});
	return found;
}

uint32 FindSubEntryRow (const SRegistryTables& Tables, uint32 AssetRow, uint64 SubId) noexcept
{
	if (AssetRow >= Tables.Assets.File.Size() || Tables.Assets.File[AssetRow] == InvalidRow || SubId == 0u)
	{
		return InvalidRow;
	}
	const uint64 id64 = MakeAssetId64(SAssetId { Tables.Assets.Guid[AssetRow], SubId }).Value;
	const uint32 value = FindRecordValueById64(Tables, id64);
	if (value == InvalidRow || (value & SubEntryBit) == 0u)
	{
		return InvalidRow;
	}
	const uint32 sub = value & ~SubEntryBit;
	return Tables.SubEntries.Owner[sub] == AssetRow && Tables.SubEntries.SubId[sub] == SubId ? sub : InvalidRow;
}

uint32 FindProblemSubject (const SRegistryTables& Tables, FStringView Subject) noexcept
{
	uint32 found = InvalidRow;
	Tables.Problems.BySubject.ForEach(HashExact(Subject), [&] (uint32 Row)
	{
		if (Tables.Paths.Get(Tables.Problems.Subject[Row]) == Subject)
		{
			found = Row;
			return false;
		}
		return true;
	});
	return found;
}

uint32 FindRecordRowsByFormerPath (const SRegistryTables& Tables, FStringView FileMountPath, uint64 PortableHash, uint32& OutRow) noexcept
{
	OutRow = InvalidRow;
	uint32 matches = 0u;
	Tables.Indices.RecordsByFormerPath.ForEach(PortableHash, [&] (uint32 Row)
	{
		const uint32 redirect = Tables.Assets.File[Row] != InvalidRow ? Tables.Assets.Redirect[Row] : InvalidRow;
		if (redirect == InvalidRow)
		{
			return true;
		}
		const SPoolRange range = Tables.Redirects.FormerPaths[redirect];
		for (uint32 i = 0u; i < range.Count; ++i)
		{
			if (EqualPortable(Tables.Paths.Get(Tables.FormerPaths[range.Begin + i]), FileMountPath))
			{
				++matches;
				OutRow = IsPreferredGuidRow(Tables, Row, OutRow) ? Row : OutRow;
				break;
			}
		}
		return true;
	});
	return matches;
}

bool ResolveAlias (const SRegistryTables& Tables, const SAssetId& Id, uint32& OutRow, uint64& OutSub) noexcept
{
	OutRow = InvalidRow;
	OutSub = 0u;
	if (!Id.IsValid())
	{
		return false;
	}
	uint32 exact = InvalidRow;
	uint64 exactSub = 0u;
	uint32 whole = InvalidRow;
	Tables.Indices.Aliases.ForEach(HashGuid(Id.Guid), [&] (uint32 Index)
	{
		const SAliasEntry& entry = Tables.AliasEntries[Index];
		if (entry.Record == InvalidRow || Tables.Assets.File[entry.Record] == InvalidRow || entry.Id.Guid != Id.Guid)
		{
			return true;
		}
		if (entry.Id.SubId == Id.SubId && IsPreferredGuidRow(Tables, entry.Record, exact))
		{
			exact = entry.Record;
			exactSub = entry.Id.SubId != 0u ? entry.TargetSub : 0u;
		}
		else if (entry.Id.SubId == 0u && IsPreferredGuidRow(Tables, entry.Record, whole))
		{
			whole = entry.Record;
		}
		return true;
	});
	if (exact != InvalidRow)
	{
		OutRow = exact;
		OutSub = exactSub;
		return true;
	}
	if (whole != InvalidRow)
	{
		OutRow = whole;
		OutSub = Id.SubId;
		return true;
	}
	return false;
}

uint64 GetResolvedSub (const SRegistryTables& Tables, const STargetRow& Target) noexcept
{
	if (Target.Kind != ETargetKind::Guid || !Target.bRedirected || Target.State != ETargetState::Resolved)
	{
		return Target.TargetSub;
	}
	uint32 row = InvalidRow;
	uint64 sub = 0u;
	if (Target.TargetSub != 0u && Tables.Assets.Guid[Target.Resolved] == Target.Guid)
	{
		// The record's own GUID with an id no sub-entry has: a renamed sub-asset's alias.
		return ResolveAlias(Tables, SAssetId { Target.Guid, Target.TargetSub }, row, sub) && row == Target.Resolved ? sub : Target.TargetSub;
	}
	return Target.TargetSub;   // a consolidated GUID: the same sub-entry of the record it resolves to
}

uint32 FindMetaCopyRow (const SRegistryTables& Tables, const SGuid& Guid) noexcept
{
	uint32 found = InvalidRow;
	Tables.Indices.MetaCopiesByGuid.ForEach(HashGuid(Guid), [&] (uint32 Row)
	{
		if (Tables.MetaCopies.Guid[Row] == Guid)
		{
			found = Row;
			return false;
		}
		return true;
	});
	return found;
}

uint32 FindTombstoneRow (const SRegistryTables& Tables, const SGuid& Guid) noexcept
{
	uint32 found = InvalidRow;
	Tables.Indices.TombstonesByGuid.ForEach(HashGuid(Guid), [&] (uint32 Row)
	{
		if (Tables.Tombstones.Guid[Row] == Guid)
		{
			found = Row;
			return false;
		}
		return true;
	});
	return found;
}

bool IsEdgeLive (const SRegistryTables& Tables, uint32 Source, uint32 Edge) noexcept
{
	const SAssetsTable& assets = Tables.Assets;
	if (Source >= assets.File.Size() || assets.File[Source] == InvalidRow)
	{
		return false;
	}
	if ((Edge & OverflowEdgeBit) != 0u)
	{
		const uint32 index = Edge & ~OverflowEdgeBit;
		const SPoolRange range = assets.OverflowEdges[Source];
		return index >= range.Begin && index < range.Begin + range.Count;
	}
	const SPoolRange range = assets.BaseEdges[Source];
	return !assets.BaseEdgesMasked.Test(Source) && Edge >= range.Begin && Edge < range.Begin + range.Count;
}

const SEdgeRow& GetEdgeRow (const SRegistryTables& Tables, uint32 Edge) noexcept
{
	return (Edge & OverflowEdgeBit) != 0u ? Tables.Graph.OverflowEdges[Edge & ~OverflowEdgeBit] : Tables.Graph.BaseEdges[Edge];
}

SRegistryMemoryStats ComputeMemoryStats (const SRegistryTables& Tables)
{
	SRegistryMemoryStats stats;
	const SFilesTable& files = Tables.Files;
	stats.Files = files.Folder.GetResidentBytes() + files.LeafName.GetResidentBytes() + files.Path.GetResidentBytes()
		+ files.FoldedPath.GetResidentBytes() + files.Mount.GetResidentBytes() + files.Size.GetResidentBytes() + files.MTime.GetResidentBytes()
		+ files.Identity.GetResidentBytes() + files.ContentHash.GetResidentBytes() + files.MetaHash.GetResidentBytes()
		+ files.Class.GetResidentBytes() + files.BaseClass.GetResidentBytes() + files.MetaState.GetResidentBytes()
		+ files.Record.GetResidentBytes() + files.PathRank.GetResidentBytes();
	const SFoldersTable& folders = Tables.Folders;
	stats.Folders = folders.Parent.GetResidentBytes() + folders.Name.GetResidentBytes() + folders.PreorderBegin.GetResidentBytes()
		+ folders.PreorderEnd.GetResidentBytes() + folders.RuleFile.GetResidentBytes() + folders.Path.GetResidentBytes()
		+ folders.Mount.GetResidentBytes();
	stats.RuleFiles = Tables.RuleFiles.Mount.GetResidentBytes() + Tables.RuleFiles.Folder.GetResidentBytes()
		+ Tables.RuleFiles.RulesHash.GetResidentBytes();
	const SAssetsTable& assets = Tables.Assets;
	stats.Assets = assets.Guid.GetResidentBytes() + assets.AssetId64.GetResidentBytes() + assets.File.GetResidentBytes()
		+ assets.Type.GetResidentBytes() + assets.AssetType.GetResidentBytes() + assets.Importer.GetResidentBytes()
		+ assets.Flags.GetResidentBytes() + assets.CookRule.GetResidentBytes() + assets.Chunk.GetResidentBytes()
		+ assets.PrimaryName.GetResidentBytes() + assets.Labels.GetResidentBytes() + assets.FirstSub.GetResidentBytes()
		+ assets.NumSubs.GetResidentBytes() + assets.TypeRow.GetResidentBytes() + assets.ImportStatus.GetResidentBytes()
		+ assets.Problems.GetResidentBytes() + assets.AnalyzeStamp.GetResidentBytes() + assets.Name.GetResidentBytes()
		+ assets.ChangedRevision.GetResidentBytes() + assets.Companions.GetResidentBytes() + assets.BaseEdges.GetResidentBytes()
		+ assets.BaseEdgesMasked.GetResidentBytes() + assets.OverflowEdges.GetResidentBytes() + assets.Redirect.GetResidentBytes();
	stats.Identity = Tables.AliasEntries.GetResidentBytes() + Tables.FormerPaths.GetResidentBytes()
		+ Tables.Redirects.Aliases.GetResidentBytes() + Tables.Redirects.FormerPaths.GetResidentBytes()
		+ Tables.MetaCopies.Guid.GetResidentBytes() + Tables.MetaCopies.Hash.GetResidentBytes() + Tables.MetaCopies.Bytes.GetResidentBytes()
		+ Tables.MetaCopies.SourcePath.GetResidentBytes() + Tables.MetaBytes.GetResidentBytes()
		+ Tables.Tombstones.Guid.GetResidentBytes() + Tables.Tombstones.LastPath.GetResidentBytes() + Tables.Tombstones.Hash.GetResidentBytes()
		+ Tables.Tombstones.Identity.GetResidentBytes() + Tables.Tombstones.RemovedFileTime.GetResidentBytes();
	const SSubEntryTable& subs = Tables.SubEntries;
	stats.SubEntries = subs.Owner.GetResidentBytes() + subs.SubId.GetResidentBytes() + subs.Key.GetResidentBytes()
		+ subs.Type.GetResidentBytes() + subs.Flags.GetResidentBytes() + subs.TypeRow.GetResidentBytes()
		+ subs.AssetId64.GetResidentBytes() + subs.LabelRank.GetResidentBytes();
	for (const CTypeTable& table : Tables.Types)
	{
		stats.TypeTables += table.Records.GetResidentBytes() + table.Dead.GetResidentBytes();
		for (const STagColumn& column : table.Columns)
		{
			stats.TypeTables += column.Values.GetResidentBytes() + column.PresentBits.GetResidentBytes()
				+ column.MultiRanges.GetResidentBytes() + column.Multi.GetResidentBytes();
		}
	}
	const SGraphTables& graph = Tables.Graph;
	stats.Graph = graph.BaseEdges.GetResidentBytes() + graph.OverflowEdges.GetResidentBytes() + graph.Targets.GetResidentBytes()
		+ graph.BaseReverse.GetResidentBytes() + graph.BaseReverseEdges.GetResidentBytes()
		+ graph.OverflowReverseHead.GetResidentBytes() + graph.OverflowReverseEdges.GetResidentBytes()
		+ graph.TargetsByKey.GetResidentBytes() + graph.TargetsByResolved.GetResidentBytes();
	stats.Names = Tables.Names.GetResidentBytes();
	stats.Strings = Tables.Strings.GetResidentBytes() + Tables.Paths.GetResidentBytes();
	stats.Labels = Tables.Labels.GetResidentBytes() + Tables.Companions.GetResidentBytes();
	stats.Problems = Tables.Problems.Subject.GetResidentBytes() + Tables.Problems.BySubject.GetResidentBytes();
	for (const CPagedColumn<FProblemList>& lists : Tables.Problems.Lists)
	{
		stats.Problems += lists.GetResidentBytes();
		for (uint32 page = 0u; page < lists.GetPageCount(); ++page)
		{
			for (const FProblemList& list : lists.GetPage(page))
			{
				if (list != nullptr)
				{
					stats.Problems += sizeof(std::vector<SDiagnostic>) + list->capacity() * sizeof(SDiagnostic);
				}
			}
		}
	}
	const SRegistryIndices& indices = Tables.Indices;
	stats.Indices = indices.FilesByPath.GetResidentBytes() + indices.FilesByPortablePath.GetResidentBytes()
		+ indices.FoldersByPath.GetResidentBytes() + indices.FoldersByPortablePath.GetResidentBytes()
		+ indices.RecordsByPortablePath.GetResidentBytes() + indices.RecordsByGuid.GetResidentBytes()
		+ indices.RecordsById64.GetResidentBytes() + indices.Aliases.GetResidentBytes() + indices.RecordsByFormerPath.GetResidentBytes()
		+ indices.MetaCopiesByGuid.GetResidentBytes() + indices.TombstonesByGuid.GetResidentBytes();
	stats.Total = stats.Files + stats.Folders + stats.RuleFiles + stats.Assets + stats.SubEntries + stats.TypeTables + stats.Graph
		+ stats.Names + stats.Strings + stats.Labels + stats.Problems + stats.Identity + stats.Indices;
	return stats;
}


// ---------------- CRegistryView ----------------

namespace
{
STagValue DecodeTag (const SRegistryTables& Tables, ETagType Type, uint64 Bits)
{
	switch (Type)
	{
	case ETagType::Name:
		return STagValue::FromName(Tables.Names.Get(static_cast<uint32>(Bits)));
	case ETagType::String:
		return STagValue::FromString(Tables.Strings.Get(static_cast<uint32>(Bits)));
	default:
	{
		STagValue value;
		value.Type = Type;
		value.Bits = Bits;
		return value;
	}
	}
}

bool MatchesFlags (EDepFlags Flags, EDepFlags Require, EDepFlags Exclude) noexcept
{
	return HasAllFlags(Flags, Require) && !HasAnyFlags(Flags, Exclude);
}
}

CRegistryView::CRegistryView ()
{
	Tables.Mounts = std::make_shared<const std::vector<SMountRow>>();
	Tables.Importers = std::make_shared<const std::vector<FString>>();
}

CRegistryView::CRegistryView (const CRegistryView&) = default;
CRegistryView& CRegistryView::operator= (const CRegistryView&) = default;
CRegistryView::~CRegistryView () = default;

uint64 CRegistryView::GetRevision () const noexcept
{
	return Tables.Revision;
}

bool CRegistryView::IsValid (SRecordId Record) const noexcept
{
	if (!Record.IsValid() || Record.Revision != static_cast<uint32>(Tables.Revision))
	{
		return false;
	}
	if (Record.IsSubEntry())
	{
		const uint32 sub = Record.Value & ~SubEntryBit;
		return sub < Tables.SubEntries.Owner.Size() && Tables.SubEntries.Owner[sub] != InvalidRow;
	}
	return Record.Value < Tables.Assets.File.Size() && Tables.Assets.File[Record.Value] != InvalidRow;
}

EExists CRegistryView::Exists (const SAssetId& Id) const
{
	return Find(Id).has_value() ? EExists::Yes : EExists::No;
}

std::optional<SRecordId> CRegistryView::Find (const SAssetId& Id, bool* bOutRedirected) const
{
	if (bOutRedirected != nullptr)
	{
		*bOutRedirected = false;
	}
	if (!Id.IsValid())
	{
		return std::nullopt;
	}
	if (std::optional<SRecordId> canonical = FindCanonical(Id))
	{
		return canonical;
	}
	// The alias index (§5.2): a consolidated GUID, a renamed sub-asset's new label.
	uint32 row = InvalidRow;
	uint64 sub = 0u;
	if (!ResolveAlias(Tables, Id, row, sub))
	{
		return std::nullopt;
	}
	if (bOutRedirected != nullptr)
	{
		*bOutRedirected = true;
	}
	if (sub == 0u)
	{
		return MakeRecordId(row);
	}
	const uint32 subRow = FindSubEntryRow(Tables, row, sub);
	return subRow != InvalidRow ? std::optional<SRecordId>(MakeRecordId(SubEntryBit | subRow)) : std::nullopt;
}

std::optional<SRecordId> CRegistryView::FindCanonical (const SAssetId& Id) const
{
	if (Id.SubId == 0u)
	{
		uint32 found = InvalidRow;
		Tables.Indices.RecordsByGuid.ForEach(HashGuid(Id.Guid), [&] (uint32 Row)
		{
			if (Tables.Assets.File[Row] != InvalidRow && Tables.Assets.Guid[Row] == Id.Guid && IsPreferredGuidRow(Tables, Row, found))
			{
				found = Row;
			}
			return true;
		});
		if (found != InvalidRow)
		{
			return MakeRecordId(found);
		}
	}
	else
	{
		const uint32 value = FindRecordValueById64(Tables, MakeAssetId64(Id).Value);
		if (value != InvalidRow && (value & SubEntryBit) != 0u)
		{
			const uint32 sub = value & ~SubEntryBit;
			const uint32 owner = Tables.SubEntries.Owner[sub];
			if (Tables.SubEntries.SubId[sub] == Id.SubId && Tables.Assets.Guid[owner] == Id.Guid)
			{
				return MakeRecordId(value);
			}
		}
	}
	return std::nullopt;
}

std::optional<SRecordId> CRegistryView::Find (SAssetId64 Id) const
{
	if (!Id.IsValid())
	{
		return std::nullopt;
	}
	const uint32 value = FindRecordValueById64(Tables, Id.Value);
	return value != InvalidRow ? std::optional<SRecordId>(MakeRecordId(value)) : std::nullopt;
}

std::optional<SRecordId> CRegistryView::FindByPath (FStringView MountPath, SDiagnostic* OutError, SDiagnostic* OutNotice) const
{
	std::vector<uint32> splits;
	splits.push_back(MountPath.Len());
	for (uint32 i = MountPath.Len(); i > 1u; --i)
	{
		if (MountPath[i - 1u] == '#')
		{
			splits.push_back(i - 1u);
		}
	}
	const auto notice = [&] (uint32 Row, FStringView FilePart, bool bFormer)
	{
		const FStringView onDisk = Tables.Paths.Get(Tables.Files.Path[Tables.Assets.File[Row]]);
		if (OutNotice == nullptr || (!bFormer && onDisk == FilePart))
		{
			return;
		}
		*OutNotice = SDiagnostic {};
		OutNotice->Severity = ESeverity::Info;
		OutNotice->Code = Ref006;
		OutNotice->Subject = FString(onDisk);
		OutNotice->Message = bFormer ? FString::Format("'{}' is a former path of '{}' (FormerPaths)", FilePart, onDisk)
			: FString::Format("'{}' is spelled '{}' on disk", FilePart, onDisk);
	};
	if (OutNotice != nullptr)
	{
		*OutNotice = SDiagnostic {};
	}
	// Every spelling of a current path first, then the records' FormerPaths (a file now at an old path takes it over).
	for (const bool bFormer : { false, true })
	{
		if (bFormer && Tables.Indices.RecordsByFormerPath.GetCount() == 0u)
		{
			break;
		}
		for (const uint32 split : splits)
		{
			const FStringView filePart = MountPath.Left(split);
			uint32 row = InvalidRow;
			const uint32 matches = bFormer ? FindRecordRowsByFormerPath(Tables, filePart, HashPortable(filePart), row)
				: FindRecordRowsPortable(Tables, filePart, row);
			if (matches == 0u)
			{
				continue;
			}
			if (matches > 1u && !bFormer)
			{
				if (OutError != nullptr)
				{
					*OutError = SDiagnostic {};
					OutError->Severity = ESeverity::Error;
					OutError->Code = Path001;
					OutError->Subject = FString(filePart);
					OutError->Message = FString::Format("'{}' matches {} files that differ only in case", filePart, matches);
				}
				return std::nullopt;
			}
			notice(row, filePart, bFormer);
			return FindInRecord(row, split == MountPath.Len() ? std::optional<FStringView>() : std::optional<FStringView>(MountPath.RightChop(split + 1u)));
		}
	}
	return std::nullopt;
}

// The record of an ASSETS row (no label), or its sub-entry of that label: exact, else the one equal under the fold.
std::optional<SRecordId> CRegistryView::FindInRecord (uint32 Row, std::optional<FStringView> Label) const
{
	if (!Label.has_value())
	{
		return MakeRecordId(Row);
	}
	const FStringView label = *Label;
	const uint32 exact = FindSubEntryRow(Tables, Row, MakeSubId(label));
	if (exact != InvalidRow)
	{
		return MakeRecordId(SubEntryBit | exact);
	}
	uint32 folded = InvalidRow;
	uint32 foldedMatches = 0u;
	for (uint32 i = 0u; i < Tables.Assets.NumSubs[Row]; ++i)
	{
		const uint32 sub = Tables.Assets.FirstSub[Row] + i;
		const FStringView key = Tables.Names.Get(Tables.SubEntries.Key[sub]);
		if (key == label)
		{
			return MakeRecordId(SubEntryBit | sub);   // a renamed sub-asset: its label, its old label's id
		}
		if (EqualPortable(key, label))
		{
			folded = sub;
			++foldedMatches;
		}
	}
	return foldedMatches == 1u ? std::optional<SRecordId>(MakeRecordId(SubEntryBit | folded)) : std::nullopt;
}

bool CRegistryView::GetRecord (SRecordId Record, SAssetData& Out) const
{
	if (!IsValid(Record))
	{
		return false;
	}
	const SAssetsTable& assets = Tables.Assets;
	Out = SAssetData {};
	Out.Record = Record;
	if (Record.IsSubEntry())
	{
		const uint32 sub = Record.Value & ~SubEntryBit;
		const SSubEntryTable& subs = Tables.SubEntries;
		const uint32 owner = subs.Owner[sub];
		const FStringView label = Tables.Names.Get(subs.Key[sub]);
		Out.Id = SAssetId { assets.Guid[owner], subs.SubId[sub] };
		Out.Id64 = SAssetId64 { subs.AssetId64[sub] };
		Out.Type = subs.Type[sub];
		Out.AssetType = subs.Type[sub];
		Out.Flags = subs.Flags[sub];
		Out.MountPath = FString(Tables.Paths.Get(Tables.Files.Path[assets.File[owner]]));
		Out.MountPath += '#';
		Out.MountPath += label;
		Out.Name = FString(label);
		Out.Revision = assets.ChangedRevision[owner];
		return true;
	}
	const uint32 row = Record.Value;
	Out.Id = SAssetId { assets.Guid[row], 0u };
	Out.Id64 = SAssetId64 { assets.AssetId64[row] };
	Out.Type = assets.Type[row];
	Out.AssetType = assets.AssetType[row];
	Out.Flags = assets.Flags[row];
	Out.MountPath = FString(Tables.Paths.Get(Tables.Files.Path[assets.File[row]]));
	Out.Name = FString(Tables.Names.Get(assets.Name[row]));
	Out.Revision = assets.ChangedRevision[row];
	return true;
}

void CRegistryView::ForEachSubEntry (SRecordId Record, TFunctionRef<void (SRecordId)> Visit) const
{
	if (!IsValid(Record) || Record.IsSubEntry())
	{
		return;
	}
	const uint32 first = Tables.Assets.FirstSub[Record.Value];
	const uint32 count = Tables.Assets.NumSubs[Record.Value];
	for (uint32 i = 0u; i < count; ++i)
	{
		Visit(MakeRecordId(SubEntryBit | (first + i)));
	}
}

SRecordId CRegistryView::GetOwner (SRecordId Record) const noexcept
{
	if (!IsValid(Record))
	{
		return SRecordId {};
	}
	return Record.IsSubEntry() ? MakeRecordId(Tables.SubEntries.Owner[Record.Value & ~SubEntryBit]) : Record;
}

namespace
{
// The type table row of a record: false for an invalid record.
bool GetTypeRow (const SRegistryTables& Tables, SRecordId Record, reflect::TypeId& OutType, uint32& OutRow) noexcept
{
	if (Record.IsSubEntry())
	{
		const uint32 sub = Record.Value & ~SubEntryBit;
		OutType = Tables.SubEntries.Type[sub];
		OutRow = Tables.SubEntries.TypeRow[sub];
	}
	else
	{
		OutType = Tables.Assets.Type[Record.Value];
		OutRow = Tables.Assets.TypeRow[Record.Value];
	}
	return OutType < Tables.Types.size() && OutRow != InvalidRow;
}

template <typename TVisit>
void ForEachValueOfKey (const SRegistryTables& Tables, const CTypeTable& Table, uint32 Key, uint32 Row, TVisit&& Visit)
{
	const STagColumn& column = Table.Columns[Key];
	if (!column.PresentBits.Test(Row))
	{
		return;
	}
	const STagKeyDesc& desc = Table.Keys[Key];
	if (HasAnyFlags(desc.Flags, ETagFlags::MultiValue))
	{
		const SPoolRange range = column.MultiRanges[Row];
		for (uint32 i = 0u; i < range.Count; ++i)
		{
			Visit(DecodeTag(Tables, desc.Type, column.Multi[range.Begin + i]));
		}
		return;
	}
	Visit(DecodeTag(Tables, desc.Type, column.Values[Row]));
}
}

bool CRegistryView::GetTag (SRecordId Record, FStringView TagName, STagValue& Out) const
{
	bool bFound = false;
	ForEachTagValue(Record, TagName, [&] (const STagValue& Value)
	{
		if (!bFound)
		{
			Out = Value;
			bFound = true;
		}
	});
	return bFound;
}

void CRegistryView::ForEachTagValue (SRecordId Record, FStringView TagName, TFunctionRef<void (const STagValue&)> Visit) const
{
	reflect::TypeId type = 0u;
	uint32 row = InvalidRow;
	if (!IsValid(Record) || !GetTypeRow(Tables, Record, type, row))
	{
		return;
	}
	const std::optional<uint32> key = CAssetTypeRegistry::FindKey(type, TagName);
	if (!key || *key >= Tables.Types[type].Columns.size())
	{
		return;
	}
	ForEachValueOfKey(Tables, Tables.Types[type], *key, row, Visit);
}

void CRegistryView::ForEachTag (SRecordId Record, TFunctionRef<void (FStringView Key, const STagValue&)> Visit) const
{
	reflect::TypeId type = 0u;
	uint32 row = InvalidRow;
	if (!IsValid(Record) || !GetTypeRow(Tables, Record, type, row))
	{
		return;
	}
	const CTypeTable& table = Tables.Types[type];
	for (uint32 key = 0u; key < table.Keys.size(); ++key)
	{
		const FStringView name = Tables.Names.Get(table.Keys[key].NameId);
		ForEachValueOfKey(Tables, table, key, row, [&] (const STagValue& Value) { Visit(name, Value); });
	}
}

void CRegistryView::ForEachLabel (SRecordId Record, TFunctionRef<void (FStringView Label, bool bRule)> Visit) const
{
	if (!IsValid(Record) || Record.IsSubEntry())
	{
		return;
	}
	const SPoolRange range = Tables.Assets.Labels[Record.Value];
	for (uint32 i = 0u; i < range.Count; ++i)
	{
		const uint32 label = Tables.Labels[range.Begin + i];
		Visit(Tables.Names.Get(label & ~RuleLabelBit), (label & RuleLabelBit) != 0u);
	}
}

void CRegistryView::ForEachCompanion (SRecordId Record, TFunctionRef<void (FStringView MountPath)> Visit) const
{
	if (!IsValid(Record) || Record.IsSubEntry())
	{
		return;
	}
	const SPoolRange range = Tables.Assets.Companions[Record.Value];
	for (uint32 i = 0u; i < range.Count; ++i)
	{
		Visit(Tables.Paths.Get(Tables.Companions[range.Begin + i]));
	}
}

void CRegistryView::ForEachAlias (SRecordId Record, TFunctionRef<void (const SAssetId& Alias)> Visit) const
{
	if (!IsValid(Record) || Record.IsSubEntry() || Tables.Assets.Redirect[Record.Value] == InvalidRow)
	{
		return;
	}
	const SPoolRange range = Tables.Redirects.Aliases[Tables.Assets.Redirect[Record.Value]];
	for (uint32 i = 0u; i < range.Count; ++i)
	{
		Visit(Tables.AliasEntries[range.Begin + i].Id);
	}
}

void CRegistryView::ForEachFormerPath (SRecordId Record, TFunctionRef<void (FStringView MountPath)> Visit) const
{
	if (!IsValid(Record) || Record.IsSubEntry() || Tables.Assets.Redirect[Record.Value] == InvalidRow)
	{
		return;
	}
	const SPoolRange range = Tables.Redirects.FormerPaths[Tables.Assets.Redirect[Record.Value]];
	for (uint32 i = 0u; i < range.Count; ++i)
	{
		Visit(Tables.Paths.Get(Tables.FormerPaths[range.Begin + i]));
	}
}

namespace
{
SMetaCopyView MakeMetaCopyView (const SRegistryTables& Tables, uint32 Row) noexcept
{
	const FStringView bytes = Tables.MetaBytes.Get(Tables.MetaCopies.Bytes[Row]);
	return SMetaCopyView { Tables.MetaCopies.Guid[Row], Tables.MetaCopies.Hash[Row],
		std::span<const uint8>(reinterpret_cast<const uint8*>(bytes.GetData()), bytes.Len()), Tables.Paths.Get(Tables.MetaCopies.SourcePath[Row]) };
}

STombstoneView MakeTombstoneView (const SRegistryTables& Tables, uint32 Row) noexcept
{
	return STombstoneView { Tables.Tombstones.Guid[Row], Tables.Paths.Get(Tables.Tombstones.LastPath[Row]), Tables.Tombstones.Hash[Row],
		Tables.Tombstones.Identity[Row], Tables.Tombstones.RemovedFileTime[Row] };
}
}

std::optional<SMetaCopyView> CRegistryView::FindMetaCopy (const SGuid& Guid) const noexcept
{
	const uint32 row = Tables.LiveMetaCopies != 0u ? FindMetaCopyRow(Tables, Guid) : InvalidRow;
	return row != InvalidRow ? std::optional<SMetaCopyView>(MakeMetaCopyView(Tables, row)) : std::nullopt;
}

void CRegistryView::ForEachMetaCopy (TFunctionRef<void (const SMetaCopyView&)> Visit) const
{
	for (uint32 row = 0u; row < Tables.MetaCopies.Guid.Size(); ++row)
	{
		if (Tables.MetaCopies.Guid[row].IsValid())
		{
			Visit(MakeMetaCopyView(Tables, row));
		}
	}
}

std::optional<STombstoneView> CRegistryView::FindTombstone (const SGuid& Guid) const noexcept
{
	const uint32 row = Tables.LiveTombstones != 0u ? FindTombstoneRow(Tables, Guid) : InvalidRow;
	return row != InvalidRow ? std::optional<STombstoneView>(MakeTombstoneView(Tables, row)) : std::nullopt;
}

void CRegistryView::ForEachTombstone (TFunctionRef<void (const STombstoneView&)> Visit) const
{
	for (uint32 row = 0u; row < Tables.Tombstones.Guid.Size(); ++row)
	{
		if (Tables.Tombstones.Guid[row].IsValid())
		{
			Visit(MakeTombstoneView(Tables, row));
		}
	}
}

uint32 CRegistryView::GetMetaCopyCount () const noexcept
{
	return Tables.LiveMetaCopies;
}

uint32 CRegistryView::GetTombstoneCount () const noexcept
{
	return Tables.LiveTombstones;
}

FStringView CRegistryView::GetImporterName (SRecordId Record) const noexcept
{
	const SRecordId owner = GetOwner(Record);
	if (!owner.IsValid())
	{
		return FStringView();
	}
	const uint16 importer = Tables.Assets.Importer[owner.Value];
	return importer < Tables.Importers->size() ? (*Tables.Importers)[importer].View() : FStringView();
}

SDependencyView CRegistryView::MakeDependencyView (uint32 Source, uint32 EdgeRef) const
{
	const SEdgeRow& edge = GetEdgeRow(Tables, EdgeRef);
	const STargetRow& target = Tables.Graph.Targets[edge.Target];
	SDependencyView view;
	const uint32 fromSub = GetEdgeSourceSub(Tables, Source, edge);
	view.FromSub = fromSub != InvalidRow ? Tables.SubEntries.SubId[fromSub] : 0u;
	view.From = MakeRecordId(fromSub != InvalidRow ? (SubEntryBit | fromSub) : Source);
	const bool bSatisfied = target.State == ETargetState::Resolved || target.State == ETargetState::Companion;
	view.Flags = bSatisfied ? edge.Flags : (edge.Flags | EDepFlags::Unresolved);
	view.TargetSub = target.TargetSub;
	view.TargetPath = target.Text != InvalidRow ? Tables.Paths.Get(target.Text) : FStringView();
	view.FieldPath = edge.FieldPath != InvalidRow ? Tables.Names.Get(edge.FieldPath) : FStringView();
	view.bAmbiguous = target.State == ETargetState::Ambiguous;
	view.bCompanion = target.State == ETargetState::Companion;
	if (target.Kind == ETargetKind::Guid)
	{
		view.TargetId = SAssetId { target.Guid, target.TargetSub };
		view.bGuidTarget = true;
	}
	if (target.State == ETargetState::Resolved)
	{
		const uint64 resolvedSub = GetResolvedSub(Tables, target);
		const uint32 toSub = resolvedSub != 0u ? FindSubEntryRow(Tables, target.Resolved, resolvedSub) : InvalidRow;
		view.To = MakeRecordId(toSub != InvalidRow ? (SubEntryBit | toSub) : target.Resolved);
		if (target.Kind == ETargetKind::Path)
		{
			view.TargetId = SAssetId { Tables.Assets.Guid[target.Resolved], resolvedSub };   // a Guid target keeps its id as declared
		}
		view.TargetSub = resolvedSub;
		view.bRedirected = target.bRedirected;
	}
	return view;
}

void CRegistryView::ForEachDependency (SRecordId Record, EDepFlags Require, EDepFlags Exclude,
	TFunctionRef<void (const SDependencyView&)> Visit) const
{
	if (!IsValid(Record))
	{
		return;
	}
	const bool bSub = Record.IsSubEntry();
	const uint32 row = bSub ? Tables.SubEntries.Owner[Record.Value & ~SubEntryBit] : Record.Value;
	ForEachLiveEdge(Tables, row, [&] (uint32 EdgeRef)
	{
		if (bSub && GetEdgeSourceSub(Tables, row, GetEdgeRow(Tables, EdgeRef)) != (Record.Value & ~SubEntryBit))
		{
			return;
		}
		const SDependencyView view = MakeDependencyView(row, EdgeRef);
		if (MatchesFlags(view.Flags, Require, Exclude))
		{
			Visit(view);
		}
	});
}

void CRegistryView::ForEachReferencer (SRecordId Record, EDepFlags Require, EDepFlags Exclude,
	TFunctionRef<void (const SDependencyView&)> Visit) const
{
	if (!IsValid(Record))
	{
		return;
	}
	const bool bSub = Record.IsSubEntry();
	const uint32 row = bSub ? Tables.SubEntries.Owner[Record.Value & ~SubEntryBit] : Record.Value;
	const uint64 subId = bSub ? Tables.SubEntries.SubId[Record.Value & ~SubEntryBit] : 0u;
	Tables.Graph.TargetsByResolved.ForEach(MixRegistryKey(row), [&] (uint32 TargetRow)
	{
		const STargetRow& target = Tables.Graph.Targets[TargetRow];
		if (target.Resolved != row || (bSub && GetResolvedSub(Tables, target) != subId))
		{
			return true;
		}
		ForEachLiveReverseEdge(Tables, TargetRow, [&] (const SReverseEdge& Entry)
		{
			const SDependencyView view = MakeDependencyView(Entry.Source, Entry.Edge);
			if (MatchesFlags(view.Flags, Require, Exclude))
			{
				Visit(view);
			}
		});
		return true;
	});
}

namespace
{
/**
 * The traversals' unit is a file (an asset record stands for its whole file): its dependencies are every
 * edge of the file, whichever sub-entry declared it, and it is reached by every edge that resolves to it,
 * whichever sub-entry the edge names. Reaching any record of a file reaches all of them. So the closure
 * of R holds T exactly when the referencer closure of T holds R, for asset records and sub-entries alike,
 * and WhyReferenced agrees with both.
 */
bool FollowsEdge (EDepFlags Flags, EDepFlags Follow) noexcept
{
	return Follow == EDepFlags {} || HasAnyFlags(Flags, Follow);
}

// The asset row of a valid record.
uint32 GetOwnerRow (const SRegistryTables& Tables, SRecordId Record) noexcept
{
	return Record.IsSubEntry() ? Tables.SubEntries.Owner[Record.Value & ~SubEntryBit] : Record.Value;
}

// Visit(uint32 SourceRow, uint32 EdgeRef) for every live edge of a file that resolves to AssetRow.
template <typename TVisit>
void ForEachEdgeInto (const SRegistryTables& Tables, uint32 AssetRow, TVisit&& Visit)
{
	Tables.Graph.TargetsByResolved.ForEach(MixRegistryKey(AssetRow), [&] (uint32 TargetRow)
	{
		if (Tables.Graph.Targets[TargetRow].Resolved == AssetRow)
		{
			ForEachLiveReverseEdge(Tables, TargetRow, [&] (const SReverseEdge& Entry) { Visit(Entry.Source, Entry.Edge); });
		}
		return true;
	});
}

// Visit(uint32 TargetRow, uint32 EdgeRef) for every live, resolved edge of a file.
template <typename TVisit>
void ForEachEdgeOutOf (const SRegistryTables& Tables, uint32 AssetRow, TVisit&& Visit)
{
	ForEachLiveEdge(Tables, AssetRow, [&] (uint32 EdgeRef)
	{
		const STargetRow& target = Tables.Graph.Targets[GetEdgeRow(Tables, EdgeRef).Target];
		if (target.State == ETargetState::Resolved)
		{
			Visit(target.Resolved, EdgeRef);
		}
	});
}

// Breadth-first over files from the roots' files: Expand(Row, Enter) calls Enter(AssetRow) for every file
// one step from Row. Returns the files in the order they were reached.
template <typename TExpand>
std::vector<uint32> WalkFiles (const SRegistryTables& Tables, std::span<const uint32> RootRows, TExpand&& Expand)
{
	std::vector<uint8> seen(Tables.Assets.File.Size(), 0u);
	std::vector<uint32> rows;
	const auto enter = [&] (uint32 Row)
	{
		if (seen[Row] == 0u)
		{
			seen[Row] = 1u;
			rows.push_back(Row);
		}
	};
	for (const uint32 root : RootRows)
	{
		enter(root);
	}
	for (size_t i = 0u; i < rows.size(); ++i)
	{
		Expand(rows[i], enter);
	}
	return rows;
}
}

std::vector<uint32> CRegistryView::GetRootRows (std::span<const SRecordId> Roots) const
{
	std::vector<uint32> rows;
	for (const SRecordId root : Roots)
	{
		if (IsValid(root))
		{
			rows.push_back(GetOwnerRow(Tables, root));
		}
	}
	return rows;
}

// The roots first (valid ones, once each), then every record of each file in walk order: the asset record,
// then its sub-entries in declared order.
void CRegistryView::ListFiles (std::span<const SRecordId> Roots, std::span<const uint32> Rows, std::vector<SRecordId>& Out) const
{
	Out.clear();
	std::vector<uint32> rootValues;
	for (const SRecordId root : Roots)
	{
		if (IsValid(root) && std::find(rootValues.begin(), rootValues.end(), root.Value) == rootValues.end())
		{
			rootValues.push_back(root.Value);
			Out.push_back(root);
		}
	}
	std::sort(rootValues.begin(), rootValues.end());
	const auto list = [&] (uint32 Value)
	{
		if (!std::binary_search(rootValues.begin(), rootValues.end(), Value))
		{
			Out.push_back(MakeRecordId(Value));
		}
	};
	for (const uint32 row : Rows)
	{
		list(row);
		const uint32 first = Tables.Assets.FirstSub[row];
		for (uint32 i = 0u; i < Tables.Assets.NumSubs[row]; ++i)
		{
			list(SubEntryBit | (first + i));
		}
	}
}

void CRegistryView::GetClosure (std::span<const SRecordId> Roots, EDepFlags Follow, std::vector<SRecordId>& Out) const
{
	const std::vector<uint32> roots = GetRootRows(Roots);
	const std::vector<uint32> rows = WalkFiles(Tables, roots, [&] (uint32 Row, auto&& Enter)
	{
		ForEachEdgeOutOf(Tables, Row, [&] (uint32 Target, uint32 EdgeRef)
		{
			if (FollowsEdge(GetEdgeRow(Tables, EdgeRef).Flags, Follow))
			{
				Enter(Target);
			}
		});
	});
	ListFiles(Roots, rows, Out);
}

void CRegistryView::GetReferencerClosure (std::span<const SRecordId> Roots, EDepFlags Follow, std::vector<SRecordId>& Out) const
{
	const std::vector<uint32> roots = GetRootRows(Roots);
	const std::vector<uint32> rows = WalkFiles(Tables, roots, [&] (uint32 Row, auto&& Enter)
	{
		ForEachEdgeInto(Tables, Row, [&] (uint32 Source, uint32 EdgeRef)
		{
			if (FollowsEdge(GetEdgeRow(Tables, EdgeRef).Flags, Follow))
			{
				Enter(Source);
			}
		});
	});
	ListFiles(Roots, rows, Out);
}

bool CRegistryView::WhyReferenced (SRecordId From, SRecordId To, std::vector<SDependencyView>& OutChain) const
{
	OutChain.clear();
	if (!IsValid(From) || !IsValid(To))
	{
		return false;
	}
	const uint32 start = GetOwnerRow(Tables, From);
	const uint32 goal = GetOwnerRow(Tables, To);
	if (start == goal)
	{
		return true;   // one file: its records reach one another with no edge
	}
	// Only the files reached so far: the file each was reached from and the edge that did it.
	struct SStep
	{
		uint32 Parent = InvalidRow;
		uint32 EdgeRef = InvalidRow;
	};
	std::unordered_map<uint32, SStep> steps;
	steps.emplace(start, SStep {});
	std::vector<uint32> queue { start };
	bool bFound = false;
	for (size_t i = 0u; i < queue.size() && !bFound; ++i)
	{
		const uint32 current = queue[i];
		ForEachEdgeOutOf(Tables, current, [&] (uint32 Target, uint32 EdgeRef)
		{
			if (!bFound && steps.try_emplace(Target, SStep { current, EdgeRef }).second)
			{
				queue.push_back(Target);
				bFound = Target == goal;
			}
		});
	}
	if (!bFound)
	{
		return false;
	}
	for (uint32 row = goal; row != start;)
	{
		const SStep& step = steps.at(row);
		OutChain.push_back(MakeDependencyView(step.Parent, step.EdgeRef));
		row = step.Parent;
	}
	std::reverse(OutChain.begin(), OutChain.end());
	return true;
}

void CRegistryView::ForEachDiagnosticAt (FStringView Subject, TFunctionRef<void (EProblemSource, const SDiagnostic&)> Visit) const
{
	const uint32 subject = FindProblemSubject(Tables, Subject);
	if (subject == InvalidRow)
	{
		return;
	}
	for (uint32 source = 0u; source < ProblemSourceCount; ++source)
	{
		const FProblemList& list = Tables.Problems.Lists[source][subject];
		if (list == nullptr)
		{
			continue;
		}
		for (const SDiagnostic& diagnostic : *list)
		{
			Visit(static_cast<EProblemSource>(source), diagnostic);
		}
	}
}

void CRegistryView::ForEachDiagnostic (SRecordId Record, TFunctionRef<void (const SDiagnostic&)> Visit) const
{
	if (!IsValid(Record))
	{
		return;
	}
	const auto visitSubject = [&] (FStringView Subject)
	{
		ForEachDiagnosticAt(Subject, [&] (EProblemSource, const SDiagnostic& Diagnostic) { Visit(Diagnostic); });
	};
	const SRecordId owner = GetOwner(Record);
	const FStringView path = Tables.Paths.Get(Tables.Files.Path[Tables.Assets.File[owner.Value]]);
	const auto visitSub = [&] (uint32 Sub)
	{
		FString subject(path);
		subject += '#';
		subject += Tables.Names.Get(Tables.SubEntries.Key[Sub]);
		visitSubject(subject);
	};
	if (Record.IsSubEntry())
	{
		visitSub(Record.Value & ~SubEntryBit);
		return;
	}
	visitSubject(path);
	for (uint32 i = 0u; i < Tables.Assets.NumSubs[owner.Value]; ++i)
	{
		visitSub(Tables.Assets.FirstSub[owner.Value] + i);
	}
}

void CRegistryView::ForEachProblem (TFunctionRef<void (FStringView Subject, EProblemSource, const SDiagnostic&)> Visit) const
{
	const uint32 subjects = Tables.Problems.Subject.Size();
	for (uint32 subject = 0u; subject < subjects; ++subject)
	{
		const FStringView text = Tables.Paths.Get(Tables.Problems.Subject[subject]);
		for (uint32 source = 0u; source < ProblemSourceCount; ++source)
		{
			const FProblemList& list = Tables.Problems.Lists[source][subject];
			if (list == nullptr)
			{
				continue;
			}
			for (const SDiagnostic& diagnostic : *list)
			{
				Visit(text, static_cast<EProblemSource>(source), diagnostic);
			}
		}
	}
}

std::vector<SMountInfo> CRegistryView::GetMounts () const
{
	std::vector<SMountInfo> mounts;
	for (const SMountRow& mount : *Tables.Mounts)
	{
		if (!mount.Name.IsEmpty())
		{
			mounts.push_back(SMountInfo { mount.Name.View(), mount.Id, mount.bCaseSensitive, mount.RootFolder });
		}
	}
	return mounts;
}

std::optional<SMountInfo> CRegistryView::FindMount (FStringView Name) const
{
	const SMountRow* mount = FindMountRow(Tables, Name);
	if (mount == nullptr)
	{
		return std::nullopt;
	}
	return SMountInfo { mount->Name.View(), mount->Id, mount->bCaseSensitive, mount->RootFolder };
}

std::optional<uint32> CRegistryView::FindFileExact (FStringView MountPath) const noexcept
{
	const uint32 row = FindFileRowExact(Tables, MountPath);
	return row != InvalidRow ? std::optional<uint32>(row) : std::nullopt;
}

std::optional<uint32> CRegistryView::FindFile (FStringView MountPath) const noexcept
{
	const uint32 row = FindFileRow(Tables, MountPath);
	return row != InvalidRow ? std::optional<uint32>(row) : std::nullopt;
}

bool CRegistryView::GetFile (uint32 FileRow, SFileData& Out) const noexcept
{
	const SFilesTable& files = Tables.Files;
	if (FileRow >= files.Folder.Size() || files.Folder[FileRow] == InvalidRow)
	{
		return false;
	}
	Out.MountPath = Tables.Paths.Get(files.Path[FileRow]);
	Out.Mount = files.Mount[FileRow];
	Out.Size = files.Size[FileRow];
	Out.MTime = files.MTime[FileRow];
	Out.Identity = files.Identity[FileRow];
	Out.ContentHash = files.ContentHash[FileRow];
	Out.MetaHash = files.MetaHash[FileRow];
	Out.Class = files.Class[FileRow];
	Out.MetaState = files.MetaState[FileRow];
	Out.Record = files.Record[FileRow] != InvalidRow ? MakeRecordId(files.Record[FileRow]) : SRecordId {};
	Out.Folder = files.Folder[FileRow];
	return true;
}

std::optional<uint32> CRegistryView::FindFolder (FStringView MountPath) const noexcept
{
	const uint32 row = FindFolderRow(Tables, MountPath);
	return row != InvalidRow ? std::optional<uint32>(row) : std::nullopt;
}

bool CRegistryView::GetFolder (uint32 FolderRow, SFolderData& Out) const noexcept
{
	const SFoldersTable& folders = Tables.Folders;
	if (FolderRow >= folders.Mount.Size() || folders.Mount[FolderRow] == InvalidMountId)
	{
		return false;
	}
	Out.MountPath = Tables.Paths.Get(folders.Path[FolderRow]);
	Out.Mount = folders.Mount[FolderRow];
	Out.Parent = folders.Parent[FolderRow];
	Out.PreorderBegin = folders.PreorderBegin[FolderRow];
	Out.PreorderEnd = folders.PreorderEnd[FolderRow];
	Out.RuleFile = folders.RuleFile[FolderRow];
	return true;
}

std::optional<uint32> CRegistryView::FindRuleFile (FStringView MountName, FStringView FolderMountPath) const noexcept
{
	const SMountRow* mount = FindMountRow(Tables, MountName);
	if (mount == nullptr)
	{
		return std::nullopt;
	}
	uint32 row = InvalidRow;
	if (FolderMountPath.IsEmpty())
	{
		row = mount->ConfigRuleFile;
	}
	else
	{
		const uint32 folder = FindFolderRowExact(Tables, FolderMountPath);
		row = folder != InvalidRow && Tables.Folders.Mount[folder] == mount->Id ? Tables.Folders.RuleFile[folder] : InvalidRow;
	}
	return row != InvalidRow ? std::optional<uint32>(row) : std::nullopt;
}

bool CRegistryView::GetRuleFile (uint32 RuleFileRow, SRuleFileData& Out) const noexcept
{
	const SRuleFilesTable& ruleFiles = Tables.RuleFiles;
	if (RuleFileRow >= ruleFiles.Mount.Size() || ruleFiles.Mount[RuleFileRow] == InvalidMountId)
	{
		return false;
	}
	Out.Mount = ruleFiles.Mount[RuleFileRow];
	Out.Folder = ruleFiles.Folder[RuleFileRow];
	Out.FolderMountPath = Out.Folder != InvalidRow ? Tables.Paths.Get(Tables.Folders.Path[Out.Folder]) : FStringView();
	Out.RulesHash = ruleFiles.RulesHash[RuleFileRow];
	return true;
}

void CRegistryView::ForEachFile (TFunctionRef<void (uint32 FileRow)> Visit) const
{
	const uint32 rows = Tables.Files.Folder.Size();
	for (uint32 row = 0u; row < rows; ++row)
	{
		if (Tables.Files.Folder[row] != InvalidRow)
		{
			Visit(row);
		}
	}
}

void CRegistryView::ForEachFileUnder (uint32 FolderRow, TFunctionRef<void (uint32 FileRow)> Visit) const
{
	SFolderData folder;
	if (!GetFolder(FolderRow, folder))
	{
		return;
	}
	const uint32 rows = Tables.Files.Folder.Size();
	for (uint32 row = 0u; row < rows; ++row)
	{
		const uint32 parent = Tables.Files.Folder[row];
		if (parent == InvalidRow || Tables.Files.Mount[row] != folder.Mount)
		{
			continue;
		}
		const uint32 preorder = Tables.Folders.PreorderBegin[parent];
		if (preorder >= folder.PreorderBegin && preorder < folder.PreorderEnd)
		{
			Visit(row);
		}
	}
}

void CRegistryView::ForEachFolder (TFunctionRef<void (uint32 FolderRow)> Visit) const
{
	const uint32 rows = Tables.Folders.Mount.Size();
	for (uint32 row = 0u; row < rows; ++row)
	{
		if (Tables.Folders.Mount[row] != InvalidMountId)
		{
			Visit(row);
		}
	}
}

void CRegistryView::ForEachRuleFile (TFunctionRef<void (uint32 RuleFileRow)> Visit) const
{
	const uint32 rows = Tables.RuleFiles.Mount.Size();
	for (uint32 row = 0u; row < rows; ++row)
	{
		if (Tables.RuleFiles.Mount[row] != InvalidMountId)
		{
			Visit(row);
		}
	}
}

void CRegistryView::ForEachRecord (TFunctionRef<void (SRecordId)> Visit) const
{
	const uint32 rows = Tables.Assets.File.Size();
	for (uint32 row = 0u; row < rows; ++row)
	{
		if (Tables.Assets.File[row] != InvalidRow)
		{
			Visit(MakeRecordId(row));
		}
	}
}

uint32 CRegistryView::GetFileCount () const noexcept
{
	return Tables.LiveFiles - Tables.LiveMetaFiles;
}

uint32 CRegistryView::GetMetaFileCount () const noexcept
{
	return Tables.LiveMetaFiles;
}

uint32 CRegistryView::GetFolderCount () const noexcept
{
	return Tables.LiveFolders;
}

uint32 CRegistryView::GetRecordCount () const noexcept
{
	return Tables.LiveRecords;
}

uint32 CRegistryView::GetSubEntryCount () const noexcept
{
	return Tables.LiveSubEntries;
}

SRegistryMemoryStats CRegistryView::GetMemoryStats () const
{
	return ComputeMemoryStats(Tables);
}
}
