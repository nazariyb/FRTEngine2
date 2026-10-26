#include "Assets/Registry/RegistryView.h"

#include <algorithm>
#include <unordered_map>

#include "Assets/AssetDiagnosticCodes.h"
#include "Assets/AssetTypes.h"


namespace frt::assets
{
// ---------------- Table lookups (RegistryTables.h) ----------------

const SRegistryTables& GetTables (const CRegistryView& View) noexcept
{
	return View.Tables;
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

uint32 FindRecordValueById64 (const SRegistryTables& Tables, uint64 AssetId64) noexcept
{
	uint32 found = InvalidRow;
	Tables.Indices.RecordsById64.ForEach(MixRegistryKey(AssetId64), [&] (uint32 Value)
	{
		if ((Value & SubEntryBit) != 0u)
		{
			const uint32 sub = Value & ~SubEntryBit;
			if (Tables.SubEntries.Owner[sub] != InvalidRow && Tables.SubEntries.AssetId64[sub] == AssetId64)
			{
				found = Value;
				return false;
			}
		}
		else if (Tables.Assets.File[Value] != InvalidRow && Tables.Assets.AssetId64[Value] == AssetId64)
		{
			found = Value;
			return false;
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
		+ files.Record.GetResidentBytes();
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
		+ assets.BaseEdgesMasked.GetResidentBytes() + assets.OverflowEdges.GetResidentBytes();
	const SSubEntryTable& subs = Tables.SubEntries;
	stats.SubEntries = subs.Owner.GetResidentBytes() + subs.SubId.GetResidentBytes() + subs.Key.GetResidentBytes()
		+ subs.Type.GetResidentBytes() + subs.Flags.GetResidentBytes() + subs.TypeRow.GetResidentBytes()
		+ subs.AssetId64.GetResidentBytes();
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
		+ indices.RecordsById64.GetResidentBytes() + indices.Aliases.GetResidentBytes();
	stats.Total = stats.Files + stats.Folders + stats.RuleFiles + stats.Assets + stats.SubEntries + stats.TypeTables + stats.Graph
		+ stats.Names + stats.Strings + stats.Labels + stats.Problems + stats.Indices;
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

std::optional<SRecordId> CRegistryView::Find (const SAssetId& Id) const
{
	if (!Id.IsValid())
	{
		return std::nullopt;
	}
	if (Id.SubId == 0u)
	{
		uint32 found = InvalidRow;
		Tables.Indices.RecordsByGuid.ForEach(HashGuid(Id.Guid), [&] (uint32 Row)
		{
			if (Tables.Assets.File[Row] != InvalidRow && Tables.Assets.Guid[Row] == Id.Guid)
			{
				found = found == InvalidRow || Row < found ? Row : found;
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
	// The alias index (consolidated GUIDs, renamed labels, §5.2) holds nothing until M4.
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

std::optional<SRecordId> CRegistryView::FindByPath (FStringView MountPath, SDiagnostic* OutError) const
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
	for (const uint32 split : splits)
	{
		const FStringView filePart = MountPath.Left(split);
		uint32 row = InvalidRow;
		const uint32 matches = FindRecordRowsPortable(Tables, filePart, row);
		if (matches == 0u)
		{
			continue;
		}
		if (matches > 1u)
		{
			if (OutError != nullptr)
			{
				*OutError = SDiagnostic {};
				OutError->Severity = ESeverity::Warning;
				OutError->Code = Path001;
				OutError->Subject = FString(filePart);
				OutError->Message = FString::Format("'{}' matches {} files that differ only in case", filePart, matches);
			}
			return std::nullopt;
		}
		if (split == MountPath.Len())
		{
			return MakeRecordId(row);
		}
		const FStringView label = MountPath.RightChop(split + 1u);
		const uint32 exact = FindSubEntryRow(Tables, row, MakeSubId(label));
		if (exact != InvalidRow)
		{
			return MakeRecordId(SubEntryBit | exact);
		}
		uint32 folded = InvalidRow;
		uint32 foldedMatches = 0u;
		for (uint32 i = 0u; i < Tables.Assets.NumSubs[row]; ++i)
		{
			const uint32 sub = Tables.Assets.FirstSub[row] + i;
			if (EqualPortable(Tables.Names.Get(Tables.SubEntries.Key[sub]), label))
			{
				folded = sub;
				++foldedMatches;
			}
		}
		if (foldedMatches == 1u)
		{
			return MakeRecordId(SubEntryBit | folded);
		}
		return std::nullopt;
	}
	return std::nullopt;
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
	}
	if (target.State == ETargetState::Resolved)
	{
		const uint32 toSub = target.TargetSub != 0u ? FindSubEntryRow(Tables, target.Resolved, target.TargetSub) : InvalidRow;
		view.To = MakeRecordId(toSub != InvalidRow ? (SubEntryBit | toSub) : target.Resolved);
		view.TargetId = SAssetId { Tables.Assets.Guid[target.Resolved], target.TargetSub };
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
		if (target.Resolved != row || (bSub && target.TargetSub != subId))
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
	return Tables.LiveFiles;
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
