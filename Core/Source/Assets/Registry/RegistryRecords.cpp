#include "Assets/Registry/RegistryRecords.h"

#include "Assets/Meta/MetaTypes.h"
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
	stored.Stat = SKnownFile { tables.Files.Size[row], tables.Files.MTime[row], tables.Files.Identity[row] };
	stored.MetaState = tables.Files.MetaState[row];
	stored.MetaHash = tables.Files.MetaHash[row];
	const uint32 record = tables.Files.Record[row];
	if (record == InvalidRow)
	{
		return stored;
	}
	const SAssetsTable& assets = tables.Assets;
	stored.Record = SRecordId { record, static_cast<uint32>(tables.Revision) };
	stored.Guid = assets.Guid[record];
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
	uint32 ruleIndex = 0u;
	uint32 authoredIndex = 0u;
	bool bChanged = false;
	View.ForEachLabel(Record, [&] (FStringView Label, bool bRule)
	{
		const std::vector<FString>& expected = bRule ? Settings.RuleLabels : Settings.AuthoredLabels;
		uint32& index = bRule ? ruleIndex : authoredIndex;
		bChanged = bChanged || index >= expected.size() || expected[index].View() != Label;
		++index;
	});
	return bChanged || ruleIndex != Settings.RuleLabels.size() || authoredIndex != Settings.AuthoredLabels.size();
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
	upsert.AuthoredLabels = Settings.AuthoredLabels;
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
	View.ForEachAlias(Record, [&] (const SAssetId& Alias) { upsert.Aliases.push_back(Alias); });
	View.ForEachFormerPath(Record, [&] (FStringView FormerPath) { upsert.FormerPaths.push_back(FString(FormerPath)); });
	return upsert;
}

SRecordUpsert MakeRecordUpsertFromView (const CRegistryView& View, SRecordId Record)
{
	SEffectiveFileSettings stored;
	ERecordFlags flags = ERecordFlags::None;
	if (View.IsValid(Record) && !Record.IsSubEntry())
	{
		const SRegistryTables& tables = GetTables(View);
		stored.CookRule = tables.Assets.CookRule[Record.Value];
		stored.Chunk = tables.Assets.Chunk[Record.Value];
		flags = tables.Assets.Flags[Record.Value];
		View.ForEachLabel(Record, [&] (FStringView Label, bool bRule) { (bRule ? stored.RuleLabels : stored.AuthoredLabels).push_back(FString(Label)); });
	}
	return MakeRecordUpsertFromView(View, Record, stored, flags);
}

void SetMetaRedirects (SRecordUpsert& Upsert, const SAssetMeta& Meta)
{
	Upsert.Aliases.clear();
	AppendRenamedSubAliases(Upsert.Guid, Upsert.SubEntries, Upsert.Aliases);
	for (const SGuid& alias : Meta.Aliases)
	{
		Upsert.Aliases.push_back(SAssetId { alias, 0u });
	}
	Upsert.FormerPaths = Meta.FormerPaths;
}

bool MetaRedirectsDiffer (const CRegistryView& View, SRecordId Record, const SAssetMeta& Meta)
{
	if (!View.IsValid(Record) || Record.IsSubEntry())
	{
		return true;
	}
	// The stored consolidated aliases and former paths against the meta's lists as written; a meta whose lists the
	// store normalises (a repeat, its own GUID or path) differs here every time, which costs only a re-upsert.
	uint32 alias = 0u;
	bool bDiffers = false;
	View.ForEachAlias(Record, [&] (const SAssetId& Stored)
	{
		if (Stored.SubId == 0u)
		{
			bDiffers = bDiffers || alias >= Meta.Aliases.size() || Meta.Aliases[alias] != Stored.Guid;
			++alias;
		}
	});
	uint32 former = 0u;
	View.ForEachFormerPath(Record, [&] (FStringView Stored)
	{
		bDiffers = bDiffers || former >= Meta.FormerPaths.size() || Meta.FormerPaths[former].View() != Stored;
		++former;
	});
	return bDiffers || alias != Meta.Aliases.size() || former != Meta.FormerPaths.size();
}
}
