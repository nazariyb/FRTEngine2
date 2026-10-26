#include "Assets/Registry/RegistryRecords.h"

#include "Assets/Registry/RegistryTables.h"


namespace frt::assets
{
std::optional<SStoredFile> FindStoredFile (const CRegistryView& View, FStringView MountPath)
{
	const SRegistryTables& tables = GetTables(View);
	const uint32 row = FindFileRowExact(tables, MountPath);
	if (row == InvalidRow)
	{
		return std::nullopt;
	}
	SStoredFile stored;
	stored.FileRow = row;
	stored.Class = tables.Files.Class[row];
	stored.BaseClass = tables.Files.BaseClass[row];
	stored.Size = tables.Files.Size[row];
	stored.MTime = tables.Files.MTime[row];
	stored.Identity = tables.Files.Identity[row];
	const uint32 record = tables.Files.Record[row];
	if (record == InvalidRow)
	{
		return stored;
	}
	const SAssetsTable& assets = tables.Assets;
	stored.Record = SRecordId { record, static_cast<uint32>(tables.Revision) };
	stored.Type = assets.Type[record];
	stored.Flags = assets.Flags[record] & ~ERecordFlags::HasProblems;
	const uint16 importer = assets.Importer[record];
	stored.Importer = importer < tables.Importers->size() ? (*tables.Importers)[importer].View() : FStringView();
	stored.ImportStatus = assets.ImportStatus[record];
	stored.AnalyzeStamp = assets.AnalyzeStamp[record];
	return stored;
}

bool RuleOutputsDiffer (const CRegistryView& View, SRecordId Record, const SEffectiveFileSettings& Settings, ERecordFlags Flags)
{
	if (!View.IsValid(Record) || Record.IsSubEntry())
	{
		return true;
	}
	const SRegistryTables& tables = GetTables(View);
	const uint32 row = Record.Value;
	if (tables.Assets.CookRule[row] != Settings.CookRule || tables.Assets.Chunk[row] != Settings.Chunk
		|| HasAnyFlags(tables.Assets.Flags[row], ERecordFlags::TypeExcluded) != HasAnyFlags(Flags, ERecordFlags::TypeExcluded))
	{
		return true;
	}
	uint32 index = 0u;
	bool bChanged = false;
	View.ForEachLabel(Record, [&] (FStringView Label, bool bRule)
	{
		if (!bRule)
		{
			return;   // authored labels (.meta, M4) are not rule outputs
		}
		bChanged = bChanged || index >= Settings.RuleLabels.size() || Settings.RuleLabels[index].View() != Label;
		++index;
	});
	return bChanged || index != Settings.RuleLabels.size();
}

SRecordUpsert MakeRecordUpsertFromView (const CRegistryView& View, SRecordId Record, const SEffectiveFileSettings& Settings, ERecordFlags Flags)
{
	SRecordUpsert upsert;
	SAssetData data;
	if (!View.GetRecord(Record, data) || Record.IsSubEntry())
	{
		return upsert;
	}
	const SRegistryTables& tables = GetTables(View);
	const uint32 row = Record.Value;
	upsert.FileMountPath = data.MountPath;
	upsert.Guid = data.Id.Guid;
	upsert.Type = data.Type;
	upsert.Importer = FString(View.GetImporterName(Record));
	upsert.Flags = Flags & ~ERecordFlags::HasProblems;
	upsert.CookRule = Settings.CookRule;
	upsert.Chunk = Settings.Chunk;
	upsert.RuleLabels = Settings.RuleLabels;
	if (const uint32 primary = tables.Assets.PrimaryName[row]; primary != InvalidRow)
	{
		upsert.PrimaryName = FString(tables.Names.Get(primary));
	}
	upsert.ImportStatus = tables.Assets.ImportStatus[row];
	upsert.AnalyzeStamp = tables.Assets.AnalyzeStamp[row];
	View.ForEachTag(Record, [&] (FStringView Key, const STagValue& Value) { upsert.Tags.push_back(STagDecl { FString(Key), Value }); });
	View.ForEachSubEntry(Record, [&] (SRecordId Sub)
	{
		SAssetData subData;
		if (!View.GetRecord(Sub, subData))
		{
			return;
		}
		SSubEntryDecl& decl = upsert.SubEntries.emplace_back();
		decl.Label = subData.Name;
		decl.SubId = subData.Id.SubId;
		decl.Type = subData.Type;
		decl.Flags = subData.Flags & ~ERecordFlags::HasProblems;
		View.ForEachTag(Sub, [&] (FStringView Key, const STagValue& Value) { decl.Tags.push_back(STagDecl { FString(Key), Value }); });
	});
	View.ForEachDependency(Record, EDepFlags {}, EDepFlags {}, [&] (const SDependencyView& Edge)
	{
		SEdgeDecl& decl = upsert.Edges.emplace_back();
		decl.FromSub = Edge.FromSub;
		if (Edge.bGuidTarget)
		{
			decl.Target = Edge.TargetId;
		}
		decl.TargetPath = FString(Edge.TargetPath);
		decl.Flags = Edge.Flags & ~EDepFlags::Unresolved;
		decl.FieldPath = FString(Edge.FieldPath);
	});
	View.ForEachCompanion(Record, [&] (FStringView Companion) { upsert.Companions.push_back(FString(Companion)); });
	return upsert;
}
}
