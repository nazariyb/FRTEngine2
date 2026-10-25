#include "Assets/Registry/RegistryView.h"

#include <algorithm>
#include <unordered_map>

#include "Assets/AssetTypes.h"
#include "Containers/StringMap.h"


/**
 * Images (RegistryTables.h): a view's tables as plain vectors for snapshots, and the delta that turns an
 * image back into store contents. The image keeps what the source declared (target paths as written, base
 * file classes, producer flags), never what the store derives (resolution, Companion, HasProblems), so a
 * loaded image re-derives it all against whatever else is loaded.
 */
namespace frt::assets
{
namespace
{
// Image-local string tables, deduplicated.
class CImageStrings
{
public:
	explicit CImageStrings (std::vector<FString>& InTable)
		: Table(InTable)
	{}

	uint32 Add (FStringView Text)
	{
		const auto found = Index.find(Text);
		if (found != Index.end())
		{
			return found->second;
		}
		const uint32 index = static_cast<uint32>(Table.size());
		Table.push_back(FString(Text));
		Index.try_emplace(FString(Text), index);
		return index;
	}

private:
	std::vector<FString>& Table;
	TStringMap<uint32> Index;
};
}

SRegistryImage CRegistryView::ExportImage (std::span<const MountId> Mounts) const
{
	const SRegistryTables& tables = Tables;
	SRegistryImage image;
	CImageStrings names(image.Names);
	CImageStrings strings(image.Strings);
	CImageStrings typeNames(image.TypeNames);
	CImageStrings importerNames(image.ImporterNames);

	std::vector<int32> mountIndex(tables.Mounts->size(), -1);
	for (const SMountRow& mount : *tables.Mounts)
	{
		const bool bWanted = Mounts.empty() || std::find(Mounts.begin(), Mounts.end(), mount.Id) != Mounts.end();
		if (mount.Name.IsEmpty() || !bWanted)
		{
			continue;
		}
		mountIndex[mount.Id] = static_cast<int32>(image.Mounts.size());
		image.Mounts.push_back(SImageMount { mount.Name, mount.Id, mount.bCaseSensitive });
	}
	const auto isExported = [&] (MountId Mount) { return Mount < mountIndex.size() && mountIndex[Mount] >= 0; };

	// Folders in pre-order (parents first), mount by mount.
	std::vector<uint32> folderRows;
	ForEachFolder([&] (uint32 Row)
	{
		if (isExported(tables.Folders.Mount[Row]))
		{
			folderRows.push_back(Row);
		}
	});
	std::sort(folderRows.begin(), folderRows.end(), [&] (uint32 A, uint32 B)
	{
		const MountId mountA = tables.Folders.Mount[A];
		const MountId mountB = tables.Folders.Mount[B];
		return mountA != mountB ? mountA < mountB : tables.Folders.PreorderBegin[A] < tables.Folders.PreorderBegin[B];
	});
	std::unordered_map<uint32, uint32> folderIndex;
	for (const uint32 row : folderRows)
	{
		SImageFolder folder;
		folder.Mount = static_cast<uint16>(mountIndex[tables.Folders.Mount[row]]);
		const uint32 parent = tables.Folders.Parent[row];
		folder.Parent = parent != InvalidRow ? folderIndex.at(parent) : ~0u;
		folder.Name = names.Add(tables.Names.Get(tables.Folders.Name[row]));
		folderIndex.emplace(row, static_cast<uint32>(image.Folders.size()));
		image.Folders.push_back(folder);
	}

	std::unordered_map<uint32, uint32> fileIndex;
	ForEachFile([&] (uint32 Row)
	{
		if (!isExported(tables.Files.Mount[Row]))
		{
			return;
		}
		SImageFile file;
		file.Folder = folderIndex.at(tables.Files.Folder[Row]);
		file.LeafName = names.Add(tables.Names.Get(tables.Files.LeafName[Row]));
		file.Size = tables.Files.Size[Row];
		file.MTime = tables.Files.MTime[Row];
		file.Identity = tables.Files.Identity[Row];
		file.ContentHash = tables.Files.ContentHash[Row];
		file.MetaHash = tables.Files.MetaHash[Row];
		file.Class = tables.Files.BaseClass[Row];
		file.MetaState = tables.Files.MetaState[Row];
		fileIndex.emplace(Row, static_cast<uint32>(image.Files.size()));
		image.Files.push_back(file);
	});

	ForEachRuleFile([&] (uint32 Row)
	{
		const MountId mount = tables.RuleFiles.Mount[Row];
		if (!isExported(mount))
		{
			return;
		}
		const uint32 folder = tables.RuleFiles.Folder[Row];
		image.RuleFiles.push_back(SImageRuleFile { static_cast<uint16>(mountIndex[mount]), folder != InvalidRow ? folderIndex.at(folder) : ~0u,
			tables.RuleFiles.RulesHash[Row] });
	});

	const auto exportTags = [&] (SRecordId Record) -> SPoolRange
	{
		SPoolRange range { static_cast<uint32>(image.Tags.size()), 0u };
		ForEachTag(Record, [&] (FStringView Key, const STagValue& Value)
		{
			SImageTag tag;
			tag.Key = names.Add(Key);
			tag.Type = Value.Type;
			tag.Bits = Value.Bits;
			if (Value.Type == ETagType::Name)
			{
				tag.Text = names.Add(Value.Text);
				tag.Bits = 0u;
			}
			else if (Value.Type == ETagType::String)
			{
				tag.Text = strings.Add(Value.Text);
				tag.Bits = 0u;
			}
			image.Tags.push_back(tag);
		});
		range.Count = static_cast<uint32>(image.Tags.size()) - range.Begin;
		return range;
	};

	ForEachRecord([&] (SRecordId Record)
	{
		const uint32 row = Record.Value;
		const SAssetsTable& assets = tables.Assets;
		const uint32 fileRow = assets.File[row];
		if (!isExported(tables.Files.Mount[fileRow]))
		{
			return;
		}
		SImageRecord record;
		record.File = fileIndex.at(fileRow);
		record.Guid = assets.Guid[row];
		record.Type = static_cast<uint16>(typeNames.Add(CAssetTypeRegistry::Get(assets.Type[row]).Name));
		const FStringView importer = GetImporterName(Record);
		record.Importer = importer.IsEmpty() ? uint16(0xFFFFu) : static_cast<uint16>(importerNames.Add(importer));
		record.Flags = assets.Flags[row] & ~ERecordFlags::HasProblems;
		record.CookRule = assets.CookRule[row];
		record.Chunk = assets.Chunk[row];
		record.PrimaryName = assets.PrimaryName[row] != InvalidRow ? names.Add(tables.Names.Get(assets.PrimaryName[row])) : ~0u;
		record.ImportStatus = assets.ImportStatus[row];
		record.AnalyzeStamp = assets.AnalyzeStamp[row];

		record.Labels.Begin = static_cast<uint32>(image.Labels.size());
		ForEachLabel(Record, [&] (FStringView Label, bool bRule) { image.Labels.push_back(names.Add(Label) | (bRule ? RuleLabelBit : 0u)); });
		record.Labels.Count = static_cast<uint32>(image.Labels.size()) - record.Labels.Begin;

		record.Tags = exportTags(Record);

		record.SubEntries.Begin = static_cast<uint32>(image.SubEntries.size());
		ForEachSubEntry(Record, [&] (SRecordId Sub)
		{
			const uint32 sub = Sub.Value & ~SubEntryBit;
			SImageSubEntry entry;
			entry.Label = names.Add(tables.Names.Get(tables.SubEntries.Key[sub]));
			entry.SubId = tables.SubEntries.SubId[sub];
			entry.Type = static_cast<uint16>(typeNames.Add(CAssetTypeRegistry::Get(tables.SubEntries.Type[sub]).Name));
			entry.Flags = tables.SubEntries.Flags[sub];
			entry.Tags = exportTags(Sub);
			image.SubEntries.push_back(entry);
		});
		record.SubEntries.Count = static_cast<uint32>(image.SubEntries.size()) - record.SubEntries.Begin;

		record.Edges.Begin = static_cast<uint32>(image.Edges.size());
		ForEachLiveEdge(tables, row, [&] (uint32 EdgeRef)
		{
			const SEdgeRow& edgeRow = GetEdgeRow(tables, EdgeRef);
			const STargetRow& target = tables.Graph.Targets[edgeRow.Target];
			SImageEdge edge;
			edge.FromSub = edgeRow.FromSub;
			edge.Flags = edgeRow.Flags;
			edge.FieldPath = edgeRow.FieldPath != InvalidRow ? names.Add(tables.Names.Get(edgeRow.FieldPath)) : ~0u;
			if (target.Kind == ETargetKind::Path)
			{
				edge.TargetPath = strings.Add(tables.Paths.Get(target.Text));
			}
			else if (target.State == ETargetState::Resolved && !isExported(tables.Files.Mount[tables.Assets.File[target.Resolved]]))
			{
				// Leaves the image's mounts: by the target's mount path, re-resolved wherever it is loaded.
				FString path(tables.Paths.Get(tables.Files.Path[tables.Assets.File[target.Resolved]]));
				const uint32 sub = target.TargetSub != 0u ? FindSubEntryRow(tables, target.Resolved, target.TargetSub) : InvalidRow;
				if (sub != InvalidRow)
				{
					path += '#';
					path += tables.Names.Get(tables.SubEntries.Key[sub]);
				}
				if (target.TargetSub == 0u || sub != InvalidRow)
				{
					edge.TargetPath = strings.Add(path);
				}
				else
				{
					edge.Target = SAssetId { target.Guid, target.TargetSub };
				}
			}
			else
			{
				edge.Target = SAssetId { target.Guid, target.TargetSub };
				edge.TargetPath = target.Text != InvalidRow ? strings.Add(tables.Paths.Get(target.Text)) : ~0u;
			}
			image.Edges.push_back(edge);
		});
		record.Edges.Count = static_cast<uint32>(image.Edges.size()) - record.Edges.Begin;

		record.Companions.Begin = static_cast<uint32>(image.Companions.size());
		ForEachCompanion(Record, [&] (FStringView Companion) { image.Companions.push_back(strings.Add(Companion)); });
		record.Companions.Count = static_cast<uint32>(image.Companions.size()) - record.Companions.Begin;

		image.Records.push_back(record);
	});

	const uint32 subjects = tables.Problems.Subject.Size();
	for (uint32 subject = 0u; subject < subjects; ++subject)
	{
		const FStringView text = tables.Paths.Get(tables.Problems.Subject[subject]);
		FStringView mountName;
		FStringView relative;
		bool bExported = false;
		if (SplitMountPath(text, mountName, relative))
		{
			// The mount name ends at the first '/' or '#'.
			const uint32 hash = mountName.Find('#');
			const SMountRow* mount = FindMountRow(tables, hash == FStringView::NotFound ? mountName : mountName.Left(hash));
			bExported = mount != nullptr && isExported(mount->Id);
		}
		if (!bExported)
		{
			continue;
		}
		for (uint32 source = 0u; source < ProblemSourceCount; ++source)
		{
			if (source == static_cast<uint32>(EProblemSource::Registry))
			{
				continue;   // the store derives these again
			}
			const FProblemList& list = tables.Problems.Lists[source][subject];
			if (list != nullptr && !list->empty())
			{
				image.Problems.push_back(SImageProblems { strings.Add(text), static_cast<EProblemSource>(source), *list });
			}
		}
	}
	return image;
}

SRegistryDelta MakeImageDelta (SRegistryImage&& Image)
{
	SRegistryDelta delta;
	const auto name = [&] (uint32 Index) -> FStringView { return Index < Image.Names.size() ? Image.Names[Index].View() : FStringView(); };
	const auto text = [&] (uint32 Index) -> FStringView { return Index < Image.Strings.size() ? Image.Strings[Index].View() : FStringView(); };

	for (const SImageMount& mount : Image.Mounts)
	{
		delta.Mounts.push_back(SMountUpsert { mount.Name, mount.bCaseSensitive, mount.Id });
	}

	// Folder paths (a parent may come after its child in a hand-made image: resolve on demand).
	std::vector<FString> folderPaths(Image.Folders.size());
	std::vector<uint8> state(Image.Folders.size(), 0u);   // 0 unknown, 1 in progress, 2 done
	const auto folderPath = [&] (auto& Self, uint32 Index) -> FStringView
	{
		if (Index >= Image.Folders.size() || state[Index] == 1u)
		{
			return FStringView();
		}
		if (state[Index] == 2u)
		{
			return folderPaths[Index];
		}
		state[Index] = 1u;
		const SImageFolder& folder = Image.Folders[Index];
		if (folder.Parent == ~0u)
		{
			folderPaths[Index] = JoinMountPath(name(folder.Name), FStringView());
		}
		else
		{
			const FStringView parent = Self(Self, folder.Parent);
			folderPaths[Index] = parent.IsEmpty() ? FString() : MakeChildMountPath(parent, name(folder.Name));
		}
		state[Index] = 2u;
		return folderPaths[Index];
	};
	for (uint32 i = 0u; i < Image.Folders.size(); ++i)
	{
		const FStringView path = folderPath(folderPath, i);
		if (!path.IsEmpty() && Image.Folders[i].Parent != ~0u)
		{
			delta.FolderUpserts.push_back(SFolderUpsert { FString(path) });
		}
	}

	std::vector<FString> filePaths(Image.Files.size());
	for (uint32 i = 0u; i < Image.Files.size(); ++i)
	{
		const SImageFile& file = Image.Files[i];
		const FStringView folder = folderPath(folderPath, file.Folder);
		if (folder.IsEmpty())
		{
			continue;
		}
		filePaths[i] = MakeChildMountPath(folder, name(file.LeafName));
		SFileUpsert upsert;
		upsert.MountPath = filePaths[i];
		upsert.Size = file.Size;
		upsert.MTime = file.MTime;
		upsert.Identity = file.Identity;
		upsert.ContentHash = file.ContentHash;
		upsert.Class = file.Class;
		upsert.MetaState = file.MetaState;
		upsert.MetaHash = file.MetaHash;
		delta.FileUpserts.push_back(std::move(upsert));
	}

	for (const SImageRuleFile& ruleFile : Image.RuleFiles)
	{
		if (ruleFile.Mount >= Image.Mounts.size())
		{
			continue;
		}
		SRuleFileUpsert upsert;
		upsert.MountName = Image.Mounts[ruleFile.Mount].Name;
		upsert.FolderMountPath = ruleFile.Folder != ~0u ? FString(folderPath(folderPath, ruleFile.Folder)) : FString();
		upsert.RulesHash = ruleFile.RulesHash;
		if (ruleFile.Folder == ~0u || !upsert.FolderMountPath.IsEmpty())
		{
			delta.RuleFileUpserts.push_back(std::move(upsert));
		}
	}

	const auto findType = [&] (uint16 Index) -> std::optional<reflect::TypeId>
	{
		if (Index >= Image.TypeNames.size())
		{
			return std::nullopt;
		}
		const std::optional<reflect::TypeId> type = CAssetTypeRegistry::Find(Image.TypeNames[Index]);
		// Only the exact dotted name: a short name that happens to match is not the same type.
		if (type && FStringView(CAssetTypeRegistry::Get(*type).Name) == Image.TypeNames[Index].View())
		{
			return type;
		}
		return std::nullopt;
	};
	const auto makeTags = [&] (SPoolRange Range, std::vector<STagDecl>& Out)
	{
		for (uint32 i = Range.Begin; i < Range.Begin + Range.Count && i < Image.Tags.size(); ++i)
		{
			const SImageTag& tag = Image.Tags[i];
			STagDecl decl;
			decl.Key = FString(name(tag.Key));
			if (tag.Type == ETagType::Name)
			{
				decl.Value = STagValue::FromName(name(tag.Text));
			}
			else if (tag.Type == ETagType::String)
			{
				decl.Value = STagValue::FromString(text(tag.Text));
			}
			else
			{
				decl.Value.Type = tag.Type;
				decl.Value.Bits = tag.Bits;
			}
			Out.push_back(std::move(decl));
		}
	};

	for (const SImageRecord& image : Image.Records)
	{
		if (image.File >= filePaths.size() || filePaths[image.File].IsEmpty())
		{
			continue;
		}
		SRecordUpsert record;
		record.FileMountPath = filePaths[image.File];
		record.Guid = image.Guid;
		record.Flags = image.Flags & ~ERecordFlags::HasProblems;
		const std::optional<reflect::TypeId> type = findType(image.Type);
		record.Type = type.value_or(asset_types::Unknown);
		if (!type)
		{
			record.Flags |= ERecordFlags::Schemaless | ERecordFlags::StaleTags;
		}
		record.Importer = image.Importer < Image.ImporterNames.size() ? Image.ImporterNames[image.Importer] : FString();
		record.CookRule = image.CookRule;
		record.Chunk = image.Chunk;
		record.PrimaryName = image.PrimaryName != ~0u ? FString(name(image.PrimaryName)) : FString();
		record.ImportStatus = image.ImportStatus;
		record.AnalyzeStamp = image.AnalyzeStamp;
		for (uint32 i = image.Labels.Begin; i < image.Labels.Begin + image.Labels.Count && i < Image.Labels.size(); ++i)
		{
			if ((Image.Labels[i] & RuleLabelBit) != 0u)
			{
				record.RuleLabels.push_back(FString(name(Image.Labels[i] & ~RuleLabelBit)));
			}
		}
		if (type)
		{
			makeTags(image.Tags, record.Tags);
		}
		for (uint32 i = image.SubEntries.Begin; i < image.SubEntries.Begin + image.SubEntries.Count && i < Image.SubEntries.size(); ++i)
		{
			const SImageSubEntry& sub = Image.SubEntries[i];
			SSubEntryDecl decl;
			decl.Label = FString(name(sub.Label));
			decl.SubId = sub.SubId;
			decl.Flags = sub.Flags;
			const std::optional<reflect::TypeId> subType = findType(sub.Type);
			decl.Type = subType.value_or(asset_types::Unknown);
			if (subType)
			{
				makeTags(sub.Tags, decl.Tags);
			}
			else
			{
				decl.Flags |= ERecordFlags::Schemaless | ERecordFlags::StaleTags;
				record.Flags |= ERecordFlags::StaleTags;
			}
			record.SubEntries.push_back(std::move(decl));
		}
		for (uint32 i = image.Edges.Begin; i < image.Edges.Begin + image.Edges.Count && i < Image.Edges.size(); ++i)
		{
			const SImageEdge& edge = Image.Edges[i];
			SEdgeDecl decl;
			decl.FromSub = edge.FromSub;
			decl.Target = edge.Target;
			decl.TargetPath = edge.TargetPath != ~0u ? FString(text(edge.TargetPath)) : FString();
			decl.Flags = edge.Flags;
			decl.FieldPath = edge.FieldPath != ~0u ? FString(name(edge.FieldPath)) : FString();
			record.Edges.push_back(std::move(decl));
		}
		for (uint32 i = image.Companions.Begin; i < image.Companions.Begin + image.Companions.Count && i < Image.Companions.size(); ++i)
		{
			record.Companions.push_back(FString(text(Image.Companions[i])));
		}
		delta.RecordUpserts.push_back(std::move(record));
	}

	for (SImageProblems& problems : Image.Problems)
	{
		if (problems.Subject < Image.Strings.size())
		{
			delta.Problems.push_back(SProblemsSet { Image.Strings[problems.Subject], problems.Source, std::move(problems.Diagnostics) });
		}
	}
	return delta;
}
}
