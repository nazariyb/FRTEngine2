#include "Assets/Registry/RegistryView.h"

#include <algorithm>

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
/**
 * An image table of texts filled in first-use order, deduplicated by text, from texts the store addresses by id:
 * a dense table per id space answers every use after the first without hashing, and only the first use of an id
 * hashes its text (two ids of one space may spell the same text - PATHS is not interned - and two spaces share
 * the table). The image is the one a text-keyed map would build, so snapshots keep their bytes.
 */
class CImageTexts
{
public:
	explicit CImageTexts (std::vector<FString>& InTable)
		: Table(InTable)
	{}

	// Space: a dense table over one id space, grown on demand; Id: the text's id there.
	uint32 Add (std::vector<uint32>& Space, uint32 Id, FStringView Text)
	{
		if (Id >= Space.size())
		{
			Space.resize((std::max)(static_cast<size_t>(Id) + 1u, Space.size() * 2u), ~0u);
		}
		uint32& slot = Space[Id];
		if (slot == ~0u)
		{
			slot = AddText(Text);
		}
		return slot;
	}

	// Like Add, for an id space whose ids never share a text (NAMES is interned): no text hashing at all. The table
	// must take texts from this space only.
	uint32 AddDistinct (std::vector<uint32>& Space, uint32 Id, FStringView Text)
	{
		if (Id >= Space.size())
		{
			Space.resize((std::max)(static_cast<size_t>(Id) + 1u, Space.size() * 2u), ~0u);
		}
		uint32& slot = Space[Id];
		if (slot == ~0u)
		{
			slot = static_cast<uint32>(Table.size());
			Table.push_back(FString(Text));
		}
		return slot;
	}

	// A text with no id (a type or importer name).
	uint32 AddText (FStringView Text)
	{
		const uint64 hash = HashExact(Text);
		uint32 found = ~0u;
		Index.ForEach(hash, [&] (uint32 Entry)
		{
			if (Table[Entry].View() == Text)
			{
				found = Entry;
				return false;
			}
			return true;
		});
		if (found == ~0u)
		{
			found = static_cast<uint32>(Table.size());
			Table.push_back(FString(Text));
			Index.Insert(hash, found, 0u);
		}
		return found;
	}

private:
	std::vector<FString>& Table;
	CPagedHashIndex Index;
};
}

/**
 * The image of the given mounts, read straight from the tables (row by row, column by column) rather than through
 * the record accessors, with every text added to the image's tables through CImageTexts. The order of first uses -
 * folders in pre-order, files, rule files, then each record's primary name, labels, tags, sub-entries, edges and
 * companions, then problems - fixes the image's indices, as before.
 */
SRegistryImage CRegistryView::ExportImage (std::span<const MountId> Mounts) const
{
	const SRegistryTables& tables = Tables;
	SRegistryImage image;
	CImageTexts names(image.Names);
	CImageTexts strings(image.Strings);
	CImageTexts typeNames(image.TypeNames);
	CImageTexts importerNames(image.ImporterNames);
	std::vector<uint32> nameIds;        // NAMES id -> image name
	std::vector<uint32> pathIds;        // PATHS id -> image string
	std::vector<uint32> textIds;        // STRINGS id -> image string
	std::vector<uint32> typeIds;        // TypeId -> image type name
	std::vector<uint32> importerIds;    // importer index -> image importer name
	const auto nameOf = [&] (uint32 NameId) { return names.AddDistinct(nameIds, NameId, tables.Names.Get(NameId)); };
	const auto pathOf = [&] (uint32 PathId) { return strings.Add(pathIds, PathId, tables.Paths.Get(PathId)); };
	const auto typeOf = [&] (reflect::TypeId Type) { return static_cast<uint16>(typeNames.Add(typeIds, Type, CAssetTypeRegistry::Get(Type).Name)); };

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
	const uint32 folderRows = tables.Folders.Mount.Size();
	std::vector<uint32> folders;
	for (uint32 row = 0u; row < folderRows; ++row)
	{
		if (tables.Folders.Mount[row] != InvalidMountId && isExported(tables.Folders.Mount[row]))
		{
			folders.push_back(row);
		}
	}
	std::sort(folders.begin(), folders.end(), [&] (uint32 A, uint32 B)
	{
		const MountId mountA = tables.Folders.Mount[A];
		const MountId mountB = tables.Folders.Mount[B];
		return mountA != mountB ? mountA < mountB : tables.Folders.PreorderBegin[A] < tables.Folders.PreorderBegin[B];
	});
	std::vector<uint32> folderIndex(folderRows, ~0u);
	image.Folders.reserve(folders.size());
	for (const uint32 row : folders)
	{
		SImageFolder folder;
		folder.Mount = static_cast<uint16>(mountIndex[tables.Folders.Mount[row]]);
		const uint32 parent = tables.Folders.Parent[row];
		folder.Parent = parent != InvalidRow ? folderIndex[parent] : ~0u;
		folder.Name = nameOf(tables.Folders.Name[row]);
		folderIndex[row] = static_cast<uint32>(image.Folders.size());
		image.Folders.push_back(folder);
	}

	// Files in their path order (PathRank), so a loading store finds them ranked already.
	const SFilesTable& files = tables.Files;
	const uint32 fileRows = files.Folder.Size();
	std::vector<uint32> fileIndex(fileRows, ~0u);
	std::vector<std::pair<uint32, uint32>> rankedFiles;   // (PathRank, row)
	rankedFiles.reserve(tables.LiveFiles);
	for (uint32 row = 0u; row < fileRows; ++row)
	{
		if (files.Folder[row] != InvalidRow && isExported(files.Mount[row]))
		{
			rankedFiles.emplace_back(files.PathRank[row], row);
		}
	}
	{
		std::vector<std::pair<uint32, uint32>> scratch;
		RadixSortByKey(rankedFiles, scratch, [] (const std::pair<uint32, uint32>& File) { return static_cast<uint64>(File.first); });
	}
	image.Files.reserve(rankedFiles.size());
	for (const auto& [rank, row] : rankedFiles)
	{
		SImageFile file;
		file.Folder = folderIndex[files.Folder[row]];
		file.LeafName = nameOf(files.LeafName[row]);
		file.Size = files.Size[row];
		file.MTime = files.MTime[row];
		file.Identity = files.Identity[row];
		file.ContentHash = files.ContentHash[row];
		file.MetaHash = files.MetaHash[row];
		file.Class = files.BaseClass[row];
		file.MetaState = files.MetaState[row];
		fileIndex[row] = static_cast<uint32>(image.Files.size());
		image.Files.push_back(file);
	}

	const uint32 ruleFileRows = tables.RuleFiles.Mount.Size();
	for (uint32 row = 0u; row < ruleFileRows; ++row)
	{
		const MountId mount = tables.RuleFiles.Mount[row];
		if (mount == InvalidMountId || !isExported(mount))
		{
			continue;
		}
		const uint32 folder = tables.RuleFiles.Folder[row];
		image.RuleFiles.push_back(SImageRuleFile { static_cast<uint16>(mountIndex[mount]), folder != InvalidRow ? folderIndex[folder] : ~0u,
			tables.RuleFiles.RulesHash[row] });
	}

	// A type row's present values, key by key in the type's key order (a MultiValue key's list in order).
	const auto exportTags = [&] (reflect::TypeId Type, uint32 TypeRow) -> SPoolRange
	{
		SPoolRange range { static_cast<uint32>(image.Tags.size()), 0u };
		if (Type >= tables.Types.size() || TypeRow == InvalidRow)
		{
			return range;
		}
		const CTypeTable& table = tables.Types[Type];
		for (uint32 key = 0u; key < table.Keys.size(); ++key)
		{
			const STagColumn& column = table.Columns[key];
			if (!column.PresentBits.Test(TypeRow))
			{
				continue;
			}
			const STagKeyDesc& desc = table.Keys[key];
			const uint32 keyName = nameOf(desc.NameId);
			const auto add = [&] (uint64 Bits)
			{
				SImageTag tag;
				tag.Key = keyName;
				tag.Type = desc.Type;
				tag.Bits = Bits;
				if (desc.Type == ETagType::Name)
				{
					tag.Text = nameOf(static_cast<uint32>(Bits));
					tag.Bits = 0u;
				}
				else if (desc.Type == ETagType::String)
				{
					tag.Text = strings.Add(textIds, static_cast<uint32>(Bits), tables.Strings.Get(static_cast<uint32>(Bits)));
					tag.Bits = 0u;
				}
				image.Tags.push_back(tag);
			};
			if (HasAnyFlags(desc.Flags, ETagFlags::MultiValue))
			{
				const SPoolRange values = column.MultiRanges[TypeRow];
				for (uint32 i = 0u; i < values.Count; ++i)
				{
					add(column.Multi[values.Begin + i]);
				}
			}
			else
			{
				add(column.Values[TypeRow]);
			}
		}
		range.Count = static_cast<uint32>(image.Tags.size()) - range.Begin;
		return range;
	};

	const SAssetsTable& assets = tables.Assets;
	const SSubEntryTable& subs = tables.SubEntries;
	const uint32 assetRows = assets.File.Size();
	image.Records.reserve(tables.LiveRecords);
	image.SubEntries.reserve(tables.LiveSubEntries);
	image.Edges.reserve(tables.Graph.LiveEdges);
	image.Labels.reserve(tables.Labels.Size());
	image.Companions.reserve(tables.Companions.Size());
	image.Names.reserve(tables.Names.Size());
	size_t tagRows = 0u;
	for (const CTypeTable& table : tables.Types)
	{
		tagRows += static_cast<size_t>(table.LiveRows) * table.Keys.size();
	}
	image.Tags.reserve(tagRows);
	for (uint32 row = 0u; row < assetRows; ++row)
	{
		const uint32 fileRow = assets.File[row];
		if (fileRow == InvalidRow || !isExported(files.Mount[fileRow]))
		{
			continue;
		}
		SImageRecord record;
		record.File = fileIndex[fileRow];
		record.Guid = assets.Guid[row];
		record.Type = typeOf(assets.Type[row]);
		const uint16 importer = assets.Importer[row];
		record.Importer = importer < tables.Importers->size() && !(*tables.Importers)[importer].IsEmpty()
			? static_cast<uint16>(importerNames.Add(importerIds, importer, (*tables.Importers)[importer]))
			: uint16(0xFFFFu);
		record.Flags = assets.Flags[row] & ~ERecordFlags::HasProblems;
		record.CookRule = assets.CookRule[row];
		record.Chunk = assets.Chunk[row];
		record.PrimaryName = assets.PrimaryName[row] != InvalidRow ? nameOf(assets.PrimaryName[row]) : ~0u;
		record.ImportStatus = assets.ImportStatus[row];
		record.AnalyzeStamp = assets.AnalyzeStamp[row];

		record.Labels.Begin = static_cast<uint32>(image.Labels.size());
		const SPoolRange labels = assets.Labels[row];
		for (uint32 i = 0u; i < labels.Count; ++i)
		{
			const uint32 label = tables.Labels[labels.Begin + i];
			image.Labels.push_back(nameOf(label & ~RuleLabelBit) | (label & RuleLabelBit));
		}
		record.Labels.Count = labels.Count;

		record.Tags = exportTags(assets.Type[row], assets.TypeRow[row]);

		record.SubEntries.Begin = static_cast<uint32>(image.SubEntries.size());
		const uint32 firstSub = assets.FirstSub[row];
		for (uint32 i = 0u; i < assets.NumSubs[row]; ++i)
		{
			const uint32 sub = firstSub + i;
			SImageSubEntry entry;
			entry.Label = nameOf(subs.Key[sub]);
			entry.SubId = subs.SubId[sub];
			entry.Type = typeOf(subs.Type[sub]);
			entry.Flags = subs.Flags[sub];
			entry.Tags = exportTags(subs.Type[sub], subs.TypeRow[sub]);
			image.SubEntries.push_back(entry);
		}
		record.SubEntries.Count = assets.NumSubs[row];

		record.Edges.Begin = static_cast<uint32>(image.Edges.size());
		ForEachLiveEdge(tables, row, [&] (uint32 EdgeRef)
		{
			const SEdgeRow& edgeRow = GetEdgeRow(tables, EdgeRef);
			const STargetRow& target = tables.Graph.Targets[edgeRow.Target];
			SImageEdge edge;
			edge.FromSub = GetEdgeFromSubId(tables, row, edgeRow);
			edge.Flags = edgeRow.Flags;
			edge.FieldPath = edgeRow.FieldPath != InvalidRow ? nameOf(edgeRow.FieldPath) : ~0u;
			if (target.Kind == ETargetKind::Path)
			{
				edge.TargetPath = pathOf(target.Text);
			}
			else if (target.State == ETargetState::Resolved && !isExported(files.Mount[assets.File[target.Resolved]]))
			{
				// Leaves the image's mounts: by the target's mount path, re-resolved wherever it is loaded.
				FString path(tables.Paths.Get(files.Path[assets.File[target.Resolved]]));
				const uint32 sub = target.TargetSub != 0u ? FindSubEntryRow(tables, target.Resolved, target.TargetSub) : InvalidRow;
				if (sub != InvalidRow)
				{
					path += '#';
					path += tables.Names.Get(subs.Key[sub]);
				}
				if (target.TargetSub == 0u || sub != InvalidRow)
				{
					edge.TargetPath = strings.AddText(path);
				}
				else
				{
					edge.Target = SAssetId { target.Guid, target.TargetSub };
				}
			}
			else
			{
				edge.Target = SAssetId { target.Guid, target.TargetSub };
				edge.TargetPath = target.Text != InvalidRow ? pathOf(target.Text) : ~0u;
			}
			image.Edges.push_back(edge);
		});
		record.Edges.Count = static_cast<uint32>(image.Edges.size()) - record.Edges.Begin;

		record.Companions.Begin = static_cast<uint32>(image.Companions.size());
		const SPoolRange companions = assets.Companions[row];
		for (uint32 i = 0u; i < companions.Count; ++i)
		{
			image.Companions.push_back(pathOf(tables.Companions[companions.Begin + i]));
		}
		record.Companions.Count = companions.Count;

		image.Records.push_back(record);
	}

	const uint32 subjects = tables.Problems.Subject.Size();
	for (uint32 subject = 0u; subject < subjects; ++subject)
	{
		const FStringView text = tables.Paths.Get(tables.Problems.Subject[subject]);
		bool bExported = false;
		if (const FStringView mountName = GetMountNameOf(text); !mountName.IsEmpty())
		{
			const SMountRow* mount = FindMountRow(tables, mountName);
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
				image.Problems.push_back(SImageProblems { pathOf(tables.Problems.Subject[subject]), static_cast<EProblemSource>(source), *list });
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

	// Folder paths (a parent may come after its child in a hand-made image: resolve on demand). Iterative, so a
	// deep or hostile image cannot exhaust the stack: up the parent chain to a folder already resolved (or a
	// mount root), then down. A cycle or an out-of-range parent leaves the folders below it without a path.
	std::vector<FString> folderPaths(Image.Folders.size());
	std::vector<uint8> state(Image.Folders.size(), 0u);   // 0 unknown, 1 in progress, 2 done
	std::vector<uint32> chain;
	const auto folderPath = [&] (uint32 Index) -> FStringView
	{
		chain.clear();
		FStringView base;
		for (uint32 current = Index;;)
		{
			if (current >= Image.Folders.size() || state[current] == 1u)
			{
				break;
			}
			if (state[current] == 2u)
			{
				base = folderPaths[current];
				break;
			}
			state[current] = 1u;
			chain.push_back(current);
			if (Image.Folders[current].Parent == ~0u)
			{
				break;
			}
			current = Image.Folders[current].Parent;
		}
		for (auto it = chain.rbegin(); it != chain.rend(); ++it)
		{
			const SImageFolder& folder = Image.Folders[*it];
			if (folder.Parent == ~0u)
			{
				folderPaths[*it] = JoinMountPath(name(folder.Name), FStringView());
			}
			else
			{
				folderPaths[*it] = base.IsEmpty() ? FString() : MakeChildMountPath(base, name(folder.Name));
			}
			state[*it] = 2u;
			base = folderPaths[*it];
		}
		return Index < Image.Folders.size() && state[Index] == 2u ? folderPaths[Index].View() : FStringView();
	};
	for (uint32 i = 0u; i < Image.Folders.size(); ++i)
	{
		const FStringView path = folderPath(i);
		if (!path.IsEmpty() && Image.Folders[i].Parent != ~0u)
		{
			delta.FolderUpserts.push_back(SFolderUpsert { FString(path) });
		}
	}

	std::vector<FString> filePaths(Image.Files.size());
	for (uint32 i = 0u; i < Image.Files.size(); ++i)
	{
		const SImageFile& file = Image.Files[i];
		const FStringView folder = folderPath(file.Folder);
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
		upsert.FolderMountPath = ruleFile.Folder != ~0u ? FString(folderPath(ruleFile.Folder)) : FString();
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
		// Only the exact dotted name: a short name that happens to match is not the same type.
		return CAssetTypeRegistry::FindExact(Image.TypeNames[Index]);
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
