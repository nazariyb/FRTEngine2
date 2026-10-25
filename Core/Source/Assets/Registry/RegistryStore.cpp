#include "Assets/Registry/RegistryStore.h"

#include <algorithm>
#include <map>
#include <unordered_map>
#include <unordered_set>

#include "Assets/AssetDiagnosticCodes.h"
#include "Assets/AssetTypes.h"
#include "Assets/Pipeline/ImporterTable.h"
#include "Containers/StringMap.h"
#include "Log/Log.h"
#include "Platform/Thread.h"
#include "Platform/Timestamp.h"


namespace frt::assets
{
/**
 * What only the writer needs: lists the readers never look at, so they stay plain std containers outside
 * the views (nothing here is copied when a view is published). AdoptImage rebuilds all of it by committing.
 */
struct CRegistryStore::SWriterState
{
	std::vector<std::vector<uint32>> FolderChildren;   // per FOLDERS row: children by (fold, bytes) of name
	std::vector<std::vector<uint32>> FolderFiles;      // per FOLDERS row: its FILES rows, unordered
	std::vector<uint32> FileSlot;                      // per FILES row: its index in FolderFiles[folder]

	// Path targets by every file path they could name (the whole text, and the text before each '#'),
	// under the portable fold: the targets to re-resolve when a record of that path comes or goes.
	std::unordered_multimap<FString, uint32, SStringHashIgnoreCase, SStringEqualIgnoreCase> PathTargetCandidates;
	// Guid targets by GUID.
	std::unordered_multimap<uint64, uint32> GuidTargets;
	// How many committed records name each companion path (portable fold).
	TStringMapIgnoreCase<uint32> CompanionRefs;
	// The PATHS id of every companion path ever named (exact bytes), so a re-upsert adds no path again.
	TStringMap<uint32> CompanionPathIds;
	// Problem subjects in byte order: the subjects at or below a path are one range ('#' and '/' follow it).
	std::map<FString, uint32, SStringLess> SubjectsByText;

	// The change ring.
	std::vector<SRecordChange> Ring;
	uint32 RingHead = 0u;                              // the oldest entry once the ring is full
	uint64 DroppedUpTo = 0u;                           // the newest revision whose changes were dropped
};


namespace
{
constexpr uint32 SourceIndex (EProblemSource Source) noexcept
{
	return static_cast<uint32>(Source);
}

SDiagnostic MakeRegistryDiagnostic (ESeverity Severity, uint32 Code, FStringView Subject, FString Message)
{
	SDiagnostic diagnostic;
	diagnostic.Severity = Severity;
	diagnostic.Code = Code;
	diagnostic.Subject = FString(Subject);
	diagnostic.Message = std::move(Message);
	return diagnostic;
}

// The stem of a file name ($Name): without the longest extension an importer knows ("Wood.frtmat.yml" ->
// "Wood"), else without its last extension; a dot-name keeps its dot.
FStringView GetStem (FStringView LeafName) noexcept
{
	uint32 bestLength = 0u;
	for (const SImporterDesc& importer : GetImporters())
	{
		for (const FStringLiteral extension : importer.Extensions)
		{
			const FStringView ext(extension);
			if (ext.Len() > bestLength && LeafName.Len() > ext.Len() && LeafName.EndsWith<ESearchCase::IgnoreCase>(ext))
			{
				bestLength = ext.Len();
			}
		}
	}
	if (bestLength > 0u)
	{
		return LeafName.LeftChop(bestLength);
	}
	const uint32 dot = LeafName.FindLast('.');
	return dot == FStringView::NotFound || dot == 0u ? LeafName : LeafName.Left(dot);
}

bool HasRelativePart (FStringView MountPath) noexcept
{
	FStringView mountName;
	FStringView relative;
	return SplitMountPath(MountPath, mountName, relative) && !relative.IsEmpty();
}

// Byte offsets of the '#' characters that may end a target's file part, rightmost first (the whole text
// is tried before them).
std::vector<uint32> GetLabelSplits (FStringView Text)
{
	std::vector<uint32> splits;
	for (uint32 i = Text.Len(); i > 1u; --i)
	{
		if (Text[i - 1u] == '#')
		{
			splits.push_back(i - 1u);
		}
	}
	return splits;
}
}


/**
 * One commit's worth of writing: the delta's items applied to the live tables in contract order, then the
 * derived state (targets, Registry problems, problem counts, folder intervals, change entries). Every write
 * goes through the store's current epoch, so pages a published view shares are cloned first.
 */
class CRegistryStore::CWriter
{
public:
	CWriter (CRegistryStore& InStore, uint64 InRevision)
		: Store(InStore), T(InStore.LiveView.Tables), W(*InStore.Writer), Epoch(InStore.Epoch), Revision(InRevision)
	{}

	void Apply (SRegistryDelta& Delta)
	{
		for (const FString& path : Delta.FolderRemoves)
		{
			RemoveFolderPath(path);
		}
		for (const FString& path : Delta.FileRemoves)
		{
			const uint32 row = FindFileRowExact(T, path);
			if (row != InvalidRow)
			{
				RemoveFile(row);
			}
		}
		for (const FString& path : Delta.RecordRemoves)
		{
			const uint32 file = FindFileRowExact(T, path);
			if (file != InvalidRow && T.Files.Record[file] != InvalidRow)
			{
				RemoveRecord(T.Files.Record[file]);
			}
		}
		for (const SRuleFileUpsert& ruleFile : Delta.RuleFileRemoves)
		{
			RemoveRuleFile(ruleFile);
		}
		for (const SProblemsClear& clear : Delta.ProblemClears)
		{
			ClearProblems(clear.MountPath, 1u << SourceIndex(clear.Source), true);
		}

		for (const SMountUpsert& mount : Delta.Mounts)
		{
			UpsertMount(mount);
		}
		for (const SFolderUpsert& folder : Delta.FolderUpserts)
		{
			UpsertFolder(folder.MountPath);
		}
		for (const SFileUpsert& file : Delta.FileUpserts)
		{
			UpsertFile(file);
		}
		for (const SRuleFileUpsert& ruleFile : Delta.RuleFileUpserts)
		{
			UpsertRuleFile(ruleFile);
		}
		for (SRecordUpsert& record : Delta.RecordUpserts)
		{
			UpsertRecord(record);
		}
		for (SProblemsSet& problems : Delta.Problems)
		{
			SetProblems(problems.Subject, problems.Source, std::move(problems.Diagnostics));
		}

		Finish();
	}

	// The asset rows changed by this commit and how (for the change ring and the stats).
	std::vector<SRecordChange> TakeChanges ()
	{
		std::vector<SRecordChange> changes;
		std::vector<uint32> rows;
		rows.reserve(Touched.size());
		for (const auto& [row, touch] : Touched)
		{
			rows.push_back(row);
		}
		std::sort(rows.begin(), rows.end());
		for (const uint32 row : rows)
		{
			const STouch& touch = Touched[row];
			const bool bLive = T.Assets.File[row] != InvalidRow;
			SRecordChange change;
			change.Revision = Revision;
			if (touch.bExistedBefore && bLive)
			{
				change.Kind = ERecordChange::Modified;
			}
			else if (!touch.bExistedBefore && bLive)
			{
				change.Kind = ERecordChange::Added;
			}
			else if (touch.bExistedBefore && !bLive)
			{
				change.Kind = ERecordChange::Removed;
			}
			else
			{
				continue;
			}
			if (bLive)
			{
				change.Id = SAssetId { T.Assets.Guid[row], 0u };
				change.MountPath = FString(T.Paths.Get(T.Files.Path[T.Assets.File[row]]));
			}
			else
			{
				change.Id = SAssetId { touch.RemovedGuid, 0u };
				change.MountPath = touch.RemovedPath;
			}
			changes.push_back(std::move(change));
		}
		return changes;
	}

private:
	struct STouch
	{
		bool bExistedBefore = false;
		SGuid RemovedGuid;
		FString RemovedPath;
	};

	// ---------------- Problems ----------------

	void Violation (FStringView Subject, FString Message)
	{
		log::Warning(log::Assets, "registry: {}: {}", Subject, Message);
		std::vector<SDiagnostic>& list = PendingViolations[FString(Subject)];
		list.push_back(MakeRegistryDiagnostic(ESeverity::Error, 0u, Subject, std::move(Message)));
	}

	uint32 GetOrAddSubject (FStringView Subject)
	{
		const uint32 existing = FindProblemSubject(T, Subject);
		if (existing != InvalidRow)
		{
			return existing;
		}
		SProblemTables& problems = T.Problems;
		const uint32 row = problems.Subject.Append(T.Paths.Add(Subject, Epoch), Epoch);
		for (CPagedColumn<FProblemList>& lists : problems.Lists)
		{
			lists.Grow(row + 1u, Epoch);
		}
		problems.BySubject.Insert(HashExact(Subject), row, Epoch);
		W.SubjectsByText.emplace(FString(Subject), row);
		return row;
	}

	void SetProblems (FStringView Subject, EProblemSource Source, std::vector<SDiagnostic>&& Diagnostics)
	{
		const uint32 source = SourceIndex(Source);
		if (source >= ProblemSourceCount)
		{
			return;
		}
		if (Diagnostics.empty())
		{
			const uint32 row = FindProblemSubject(T, Subject);
			if (row != InvalidRow && T.Problems.Lists[source][row] != nullptr)
			{
				T.Problems.Lists[source].Set(row, nullptr, Epoch);
				DirtySubjects.insert(row);
			}
			return;
		}
		const uint32 row = GetOrAddSubject(Subject);
		const FProblemList& current = T.Problems.Lists[source][row];
		if (current != nullptr && *current == Diagnostics)
		{
			return;
		}
		T.Problems.Lists[source].Set(row, std::make_shared<const std::vector<SDiagnostic>>(std::move(Diagnostics)), Epoch);
		DirtySubjects.insert(row);
	}

	// Clears the sources in SourceMask for Subject and "Subject#...", and with bSubtree "Subject/..." too.
	void ClearProblems (FStringView Subject, uint32 SourceMask, bool bSubtree)
	{
		SProblemTables& problems = T.Problems;
		for (auto it = W.SubjectsByText.lower_bound(Subject); it != W.SubjectsByText.end() && it->first.View().StartsWith(Subject); ++it)
		{
			const FStringView subject = it->first.View();
			const uint32 row = it->second;
			if (subject.Len() != Subject.Len())
			{
				const char next = subject[Subject.Len()];
				if (next != '#' && !(bSubtree && next == '/'))
				{
					continue;
				}
			}
			for (uint32 source = 0u; source < ProblemSourceCount; ++source)
			{
				if ((SourceMask & (1u << source)) != 0u && problems.Lists[source][row] != nullptr)
				{
					problems.Lists[source].Set(row, nullptr, Epoch);
					DirtySubjects.insert(row);
				}
			}
		}
	}

	// ---------------- Mounts ----------------

	const SMountRow* GetMount (FStringView MountPath, FStringView ItemPath)
	{
		const SMountRow* mount = FindMountRowForPath(T, MountPath);
		if (mount == nullptr)
		{
			Violation(ItemPath, FString::Format("'{}' is in no known mount", MountPath));
		}
		return mount;
	}

	void UpsertMount (const SMountUpsert& Upsert)
	{
		const FString root = JoinMountPath(Upsert.Name, FStringView());
		if (Upsert.Name.IsEmpty() || Upsert.Name.View().Contains('/') || Upsert.Id == InvalidMountId)
		{
			Violation(root, FString::Format("mount upsert '{}' with id {} is malformed: ignored", Upsert.Name, Upsert.Id));
			return;
		}
		const std::vector<SMountRow>& mounts = *T.Mounts;
		const SMountRow* byName = FindMountRow(T, Upsert.Name);
		if (byName != nullptr)
		{
			if (byName->Id != Upsert.Id)
			{
				Violation(root, FString::Format("mount {} has id {}; an upsert with id {} contradicts it: ignored", Upsert.Name, byName->Id, Upsert.Id));
			}
			else if (byName->bCaseSensitive != Upsert.bCaseSensitive)
			{
				SetMountCaseRule(Upsert.Id, Upsert.bCaseSensitive);
			}
			return;
		}
		if (Upsert.Id < mounts.size() && !mounts[Upsert.Id].Name.IsEmpty())
		{
			const FString other = JoinMountPath(mounts[Upsert.Id].Name, FStringView());
			Violation(other, FString::Format("mount id {} belongs to {}; an upsert of {} with that id contradicts it: ignored", Upsert.Id, other, root));
			return;
		}

		std::vector<SMountRow> updated = mounts;
		if (updated.size() <= Upsert.Id)
		{
			updated.resize(Upsert.Id + 1u);
		}
		SMountRow& row = updated[Upsert.Id];
		row.Name = Upsert.Name;
		row.Id = Upsert.Id;
		row.bCaseSensitive = Upsert.bCaseSensitive;
		T.Mounts = std::make_shared<const std::vector<SMountRow>>(std::move(updated));
		const uint32 rootFolder = CreateFolderRow(InvalidRow, root, Upsert.Id, Upsert.Name);
		UpdateMount(Upsert.Id, [&] (SMountRow& Mount) { Mount.RootFolder = rootFolder; });
	}

	// The mount root's volume changed its case rule since the mount was registered (a snapshot's mounts are
	// adopted first, then CMountTable's upserts arrive): the FILES and FOLDERS path indices of its rows are
	// rebuilt under the new rule, so FindFile and FindFolder answer for the volume as it is now.
	void SetMountCaseRule (MountId Id, bool bCaseSensitive)
	{
		const FString name = (*T.Mounts)[Id].Name;
		log::Info(log::Assets, "registry: mount {} is now case-{}: its path indices are rebuilt", name, bCaseSensitive ? "sensitive" : "insensitive");
		const uint32 fileRows = T.Files.Folder.Size();
		for (uint32 row = 0u; row < fileRows; ++row)
		{
			if (T.Files.Folder[row] != InvalidRow && T.Files.Mount[row] == Id)
			{
				const FStringView path = T.Paths.Get(T.Files.Path[row]);
				T.Indices.FilesByPath.Erase(HashMountRule(path, !bCaseSensitive), row, Epoch);
				T.Indices.FilesByPath.Insert(HashMountRule(path, bCaseSensitive), row, Epoch);
			}
		}
		const uint32 folderRows = T.Folders.Mount.Size();
		for (uint32 row = 0u; row < folderRows; ++row)
		{
			if (T.Folders.Mount[row] == Id)
			{
				const FStringView path = T.Paths.Get(T.Folders.Path[row]);
				T.Indices.FoldersByPath.Erase(HashMountRule(path, !bCaseSensitive), row, Epoch);
				T.Indices.FoldersByPath.Insert(HashMountRule(path, bCaseSensitive), row, Epoch);
			}
		}
		UpdateMount(Id, [&] (SMountRow& Mount) { Mount.bCaseSensitive = bCaseSensitive; });
	}

	template <typename TEdit>
	void UpdateMount (MountId Id, TEdit&& Edit)
	{
		std::vector<SMountRow> updated = *T.Mounts;
		Edit(updated[Id]);
		T.Mounts = std::make_shared<const std::vector<SMountRow>>(std::move(updated));
	}

	void MarkIntervalsDirty (MountId Mount)
	{
		if (DirtyIntervals.size() <= Mount)
		{
			DirtyIntervals.resize(Mount + 1u, 0u);
		}
		DirtyIntervals[Mount] = 1u;
	}

	// ---------------- Folders ----------------

	bool LessChildName (uint32 A, uint32 B) const
	{
		return LessPortableThenBytes(T.Names.Get(T.Folders.Name[A]), T.Names.Get(T.Folders.Name[B]));
	}

	uint32 CreateFolderRow (uint32 Parent, FStringView Path, MountId Mount, FStringView Name)
	{
		SFoldersTable& folders = T.Folders;
		const uint32 row = folders.Parent.Append(Parent, Epoch);
		folders.Name.Append(T.Names.Intern(Name, Epoch), Epoch);
		folders.PreorderBegin.Append(0u, Epoch);
		folders.PreorderEnd.Append(0u, Epoch);
		folders.RuleFile.Append(InvalidRow, Epoch);
		folders.Path.Append(T.Paths.Add(Path, Epoch), Epoch);
		folders.Mount.Append(Mount, Epoch);
		const bool bCaseSensitive = (*T.Mounts)[Mount].bCaseSensitive;
		T.Indices.FoldersByPath.Insert(HashMountRule(Path, bCaseSensitive), row, Epoch);
		T.Indices.FoldersByPortablePath.Insert(HashPortable(Path), row, Epoch);
		W.FolderChildren.resize(row + 1u);
		W.FolderFiles.resize(row + 1u);
		if (Parent != InvalidRow)
		{
			std::vector<uint32>& children = W.FolderChildren[Parent];
			children.insert(std::lower_bound(children.begin(), children.end(), row,
				[&] (uint32 A, uint32 B) { return LessChildName(A, B); }), row);
		}
		++T.LiveFolders;
		MarkIntervalsDirty(Mount);
		return row;
	}

	// The folder of that exact spelling, created with its missing parents. InvalidRow for a malformed path.
	// Iterative (deep trees must not exhaust the stack): up to the nearest folder that exists, then down.
	uint32 EnsureFolder (FStringView Path, const SMountRow& Mount)
	{
		std::vector<FStringView> missing;
		uint32 parent = InvalidRow;
		for (FStringView current = Path;;)
		{
			const uint32 existing = FindFolderRowExact(T, current);
			if (existing != InvalidRow)
			{
				parent = existing;
				break;
			}
			const FStringView up = GetMountPathParent(current);
			if (up.IsEmpty())
			{
				parent = Mount.RootFolder;
				break;
			}
			missing.push_back(current);
			current = up;
		}
		for (auto it = missing.rbegin(); it != missing.rend() && parent != InvalidRow; ++it)
		{
			parent = CreateFolderRow(parent, *it, Mount.Id, GetMountPathLeaf(*it));
		}
		return parent;
	}

	void UpsertFolder (FStringView Path)
	{
		if (!IsWellFormedMountPath(Path))
		{
			Violation(Path, FString::Format("folder path '{}' is not a well-formed mount path", Path));
			return;
		}
		if (const SMountRow* mount = GetMount(Path, Path))
		{
			EnsureFolder(Path, *mount);
		}
	}

	void RemoveFolderPath (FStringView Path)
	{
		const uint32 row = FindFolderRowExact(T, Path);
		if (row != InvalidRow)
		{
			RemoveFolderSubtree(row);
		}
		ClearProblems(Path, (1u << ProblemSourceCount) - 1u, true);
	}

	// Children before their parent, with an explicit stack (deep trees must not exhaust the thread's).
	void RemoveFolderSubtree (uint32 Root)
	{
		std::vector<std::pair<uint32, bool>> stack { { Root, false } };
		while (!stack.empty())
		{
			const auto [row, bExpanded] = stack.back();
			if (!bExpanded)
			{
				stack.back().second = true;
				for (const uint32 child : W.FolderChildren[row])
				{
					stack.emplace_back(child, false);
				}
				continue;
			}
			stack.pop_back();
			RemoveFolderRow(row);
		}
	}

	// One folder whose children are gone: its files, its rule file, its index entries and its parent's link.
	void RemoveFolderRow (uint32 Row)
	{
		std::vector<uint32>& files = W.FolderFiles[Row];
		while (!files.empty())
		{
			const size_t before = files.size();
			RemoveFile(files.back());
			if (files.size() == before)
			{
				files.pop_back();   // never happens: a listed file is live and removing it unlists it
			}
		}
		SFoldersTable& folders = T.Folders;
		if (folders.RuleFile[Row] != InvalidRow)
		{
			RemoveRuleFileRow(folders.RuleFile[Row]);
		}
		const MountId mount = folders.Mount[Row];
		MarkIntervalsDirty(mount);
		const uint32 parent = folders.Parent[Row];
		if (parent == InvalidRow)
		{
			return;   // a mount root stays (empty)
		}
		std::vector<uint32>& siblings = W.FolderChildren[parent];
		siblings.erase(std::find(siblings.begin(), siblings.end(), Row));
		const FStringView path = T.Paths.Get(folders.Path[Row]);
		T.Indices.FoldersByPath.Erase(HashMountRule(path, (*T.Mounts)[mount].bCaseSensitive), Row, Epoch);
		T.Indices.FoldersByPortablePath.Erase(HashPortable(path), Row, Epoch);
		folders.Mount.Set(Row, InvalidMountId, Epoch);
		--T.LiveFolders;
	}

	// Pre-order numbering of one mount's folders, siblings by (fold, bytes); writes only changed values.
	void RecomputeIntervals (MountId Mount)
	{
		if (Mount >= T.Mounts->size() || (*T.Mounts)[Mount].RootFolder == InvalidRow)
		{
			return;
		}
		struct SFrame
		{
			uint32 Folder;
			uint32 NextChild;
		};
		std::vector<SFrame> stack;
		uint32 counter = 0u;
		const uint32 root = (*T.Mounts)[Mount].RootFolder;
		T.Folders.PreorderBegin.SetIfChanged(root, counter++, Epoch);
		stack.push_back({ root, 0u });
		while (!stack.empty())
		{
			SFrame& frame = stack.back();
			const std::vector<uint32>& children = W.FolderChildren[frame.Folder];
			if (frame.NextChild < children.size())
			{
				const uint32 child = children[frame.NextChild++];
				T.Folders.PreorderBegin.SetIfChanged(child, counter++, Epoch);
				stack.push_back({ child, 0u });
			}
			else
			{
				T.Folders.PreorderEnd.SetIfChanged(frame.Folder, counter, Epoch);
				stack.pop_back();
			}
		}
	}

	// ---------------- Rule files ----------------

	void UpsertRuleFile (const SRuleFileUpsert& Upsert)
	{
		const SMountRow* mount = FindMountRow(T, Upsert.MountName);
		const FString subject = Upsert.FolderMountPath.IsEmpty() ? JoinMountPath(Upsert.MountName, FStringView()) : Upsert.FolderMountPath;
		if (mount == nullptr)
		{
			Violation(subject, FString::Format("rule file of unknown mount '{}'", Upsert.MountName));
			return;
		}
		SRuleFilesTable& ruleFiles = T.RuleFiles;
		if (Upsert.FolderMountPath.IsEmpty())
		{
			if (mount->ConfigRuleFile != InvalidRow)
			{
				ruleFiles.RulesHash.SetIfChanged(mount->ConfigRuleFile, Upsert.RulesHash, Epoch);
				return;
			}
			const uint32 row = AppendRuleFileRow(mount->Id, InvalidRow, Upsert.RulesHash);
			UpdateMount(mount->Id, [&] (SMountRow& Mount) { Mount.ConfigRuleFile = row; });
			return;
		}
		const SMountRow* pathMount = FindMountRowForPath(T, Upsert.FolderMountPath);
		if (!IsWellFormedMountPath(Upsert.FolderMountPath) || pathMount != mount)
		{
			Violation(subject, FString::Format("rule file folder '{}' is not a folder of mount {}", Upsert.FolderMountPath, Upsert.MountName));
			return;
		}
		const uint32 folder = EnsureFolder(Upsert.FolderMountPath, *mount);
		if (folder == InvalidRow)
		{
			return;
		}
		const uint32 existing = T.Folders.RuleFile[folder];
		if (existing != InvalidRow)
		{
			ruleFiles.RulesHash.SetIfChanged(existing, Upsert.RulesHash, Epoch);
			return;
		}
		T.Folders.RuleFile.Set(folder, AppendRuleFileRow(mount->Id, folder, Upsert.RulesHash), Epoch);
	}

	uint32 AppendRuleFileRow (MountId Mount, uint32 Folder, uint64 RulesHash)
	{
		SRuleFilesTable& ruleFiles = T.RuleFiles;
		const uint32 row = ruleFiles.Mount.Append(Mount, Epoch);
		ruleFiles.Folder.Append(Folder, Epoch);
		ruleFiles.RulesHash.Append(RulesHash, Epoch);
		++T.LiveRuleFiles;
		return row;
	}

	void RemoveRuleFile (const SRuleFileUpsert& Remove)
	{
		const SMountRow* mount = FindMountRow(T, Remove.MountName);
		if (mount == nullptr)
		{
			return;
		}
		if (Remove.FolderMountPath.IsEmpty())
		{
			if (mount->ConfigRuleFile != InvalidRow)
			{
				RemoveRuleFileRow(mount->ConfigRuleFile);
			}
			return;
		}
		const uint32 folder = FindFolderRowExact(T, Remove.FolderMountPath);
		if (folder != InvalidRow && T.Folders.RuleFile[folder] != InvalidRow)
		{
			RemoveRuleFileRow(T.Folders.RuleFile[folder]);
		}
	}

	void RemoveRuleFileRow (uint32 Row)
	{
		SRuleFilesTable& ruleFiles = T.RuleFiles;
		const MountId mount = ruleFiles.Mount[Row];
		if (mount == InvalidMountId)
		{
			return;
		}
		const uint32 folder = ruleFiles.Folder[Row];
		if (folder == InvalidRow)
		{
			UpdateMount(mount, [] (SMountRow& Mount) { Mount.ConfigRuleFile = InvalidRow; });
		}
		else
		{
			T.Folders.RuleFile.Set(folder, InvalidRow, Epoch);
		}
		ruleFiles.Mount.Set(Row, InvalidMountId, Epoch);
		--T.LiveRuleFiles;
	}

	// ---------------- Files ----------------

	EFileClass EffectiveClass (FStringView Path, EFileClass BaseClass) const
	{
		const auto found = W.CompanionRefs.find(Path);
		return found != W.CompanionRefs.end() && found->second > 0u ? EFileClass::Companion : BaseClass;
	}

	// The FILES row of that exact spelling, created (with its folders) when missing. InvalidRow on a violation.
	uint32 EnsureFile (FStringView Path, FStringView ItemPath, bool& bOutCreated)
	{
		bOutCreated = false;
		if (!IsWellFormedMountPath(Path) || !HasRelativePart(Path))
		{
			Violation(ItemPath, FString::Format("file path '{}' is not a well-formed mount path of a file", Path));
			return InvalidRow;
		}
		const uint32 existing = FindFileRowExact(T, Path);
		if (existing != InvalidRow)
		{
			return existing;
		}
		const SMountRow* mount = GetMount(Path, ItemPath);
		if (mount == nullptr)
		{
			return InvalidRow;
		}
		const uint32 folder = EnsureFolder(GetMountPathParent(Path), *mount);
		if (folder == InvalidRow)
		{
			return InvalidRow;
		}
		SFilesTable& files = T.Files;
		const uint32 row = files.Folder.Append(folder, Epoch);
		files.LeafName.Append(T.Names.Intern(GetMountPathLeaf(Path), Epoch), Epoch);
		const uint32 pathId = T.Paths.Add(Path, Epoch);
		files.Path.Append(pathId, Epoch);
		const FString folded = FString(Path).FoldCase();
		files.FoldedPath.Append(folded.View() == Path ? T.Paths.GetRef(pathId) : T.Paths.Store(folded.View()), Epoch);
		files.Mount.Append(mount->Id, Epoch);
		files.Size.Append(0u, Epoch);
		files.MTime.Append(0, Epoch);
		files.Identity.Append(platform::SFileIdentity {}, Epoch);
		files.ContentHash.Append(hash::SContentHash {}, Epoch);
		files.MetaHash.Append(0u, Epoch);
		files.Class.Append(EffectiveClass(Path, EFileClass::Asset), Epoch);
		files.BaseClass.Append(EFileClass::Asset, Epoch);
		files.MetaState.Append(EMetaState::PendingMeta, Epoch);
		files.Record.Append(InvalidRow, Epoch);
		T.Indices.FilesByPath.Insert(HashMountRule(Path, mount->bCaseSensitive), row, Epoch);
		T.Indices.FilesByPortablePath.Insert(HashPortable(Path), row, Epoch);
		W.FileSlot.resize(row + 1u);
		W.FileSlot[row] = static_cast<uint32>(W.FolderFiles[folder].size());
		W.FolderFiles[folder].push_back(row);
		++T.LiveFiles;
		bOutCreated = true;
		return row;
	}

	void UpsertFile (const SFileUpsert& Upsert)
	{
		bool bCreated = false;
		const uint32 row = EnsureFile(Upsert.MountPath, Upsert.MountPath, bCreated);
		if (row == InvalidRow)
		{
			return;
		}
		SFilesTable& files = T.Files;
		files.Size.SetIfChanged(row, Upsert.Size, Epoch);
		files.MTime.SetIfChanged(row, Upsert.MTime, Epoch);
		files.Identity.SetIfChanged(row, Upsert.Identity, Epoch);
		if (Upsert.ContentHash.has_value())
		{
			files.ContentHash.SetIfChanged(row, *Upsert.ContentHash, Epoch);
		}
		files.MetaHash.SetIfChanged(row, Upsert.MetaHash, Epoch);
		files.BaseClass.SetIfChanged(row, Upsert.Class, Epoch);
		files.Class.SetIfChanged(row, EffectiveClass(Upsert.MountPath, Upsert.Class), Epoch);
		files.MetaState.SetIfChanged(row, Upsert.MetaState, Epoch);
	}

	void RemoveFile (uint32 Row)
	{
		SFilesTable& files = T.Files;
		if (files.Folder[Row] == InvalidRow)
		{
			return;
		}
		if (files.Record[Row] != InvalidRow)
		{
			RemoveRecord(files.Record[Row]);
		}
		const FStringView path = T.Paths.Get(files.Path[Row]);
		const MountId mount = files.Mount[Row];
		T.Indices.FilesByPath.Erase(HashMountRule(path, (*T.Mounts)[mount].bCaseSensitive), Row, Epoch);
		T.Indices.FilesByPortablePath.Erase(HashPortable(path), Row, Epoch);
		std::vector<uint32>& siblings = W.FolderFiles[files.Folder[Row]];
		const uint32 slot = W.FileSlot[Row];
		siblings[slot] = siblings.back();
		W.FileSlot[siblings[slot]] = slot;
		siblings.pop_back();
		files.Folder.Set(Row, InvalidRow, Epoch);
		--T.LiveFiles;
		ClearProblems(path, (1u << SourceIndex(EProblemSource::Analyze)) | (1u << SourceIndex(EProblemSource::Registry)), false);
	}

	void UpdateCompanionClass (FStringView Path)
	{
		const auto found = W.CompanionRefs.find(Path);
		const bool bCompanion = found != W.CompanionRefs.end() && found->second > 0u;
		T.Indices.FilesByPortablePath.ForEach(HashPortable(Path), [&] (uint32 Row)
		{
			if (T.Files.Folder[Row] != InvalidRow && EqualPortable(T.Paths.Get(T.Files.Path[Row]), Path))
			{
				T.Files.Class.SetIfChanged(Row, bCompanion ? EFileClass::Companion : T.Files.BaseClass[Row], Epoch);
			}
			return true;
		});
	}

	void AddCompanionRef (FStringView Path)
	{
		const auto [it, bInserted] = W.CompanionRefs.try_emplace(FString(Path), 0u);
		if (++it->second == 1u)
		{
			UpdateCompanionClass(Path);
		}
	}

	void ReleaseCompanionRef (FStringView Path)
	{
		const auto found = W.CompanionRefs.find(Path);
		if (found == W.CompanionRefs.end())
		{
			return;
		}
		if (--found->second == 0u)
		{
			W.CompanionRefs.erase(found);
			UpdateCompanionClass(Path);
		}
	}

	// ---------------- Type tables ----------------

	uint32 AddTypeRow (reflect::TypeId Type, uint32 RecordValue)
	{
		CTypeTable& table = T.Types[Type];
		const uint32 row = table.Records.Append(RecordValue, Epoch);
		table.Dead.Grow(row + 1u, Epoch);
		for (STagColumn& column : table.Columns)
		{
			column.Values.Grow(row + 1u, Epoch);
			column.PresentBits.Grow(row + 1u, Epoch);
			column.MultiRanges.Grow(row + 1u, Epoch);
		}
		++table.LiveRows;
		return row;
	}

	// Tombstones a type row; its MultiValue lists and STRINGS entries become garbage (CompactRecordTables).
	void KillTypeRow (reflect::TypeId Type, uint32 Row)
	{
		CTypeTable& table = T.Types[Type];
		if (table.Dead.Test(Row))
		{
			return;
		}
		table.Dead.Set(Row, true, Epoch);
		--table.LiveRows;
		for (uint32 key = 0u; key < table.Keys.size(); ++key)
		{
			if (table.Columns[key].PresentBits.Test(Row))
			{
				T.GarbagePoolItems += CountPoolItems(table.Keys[key], table.Columns[key], Row);
			}
		}
	}

	// The pool items a present value of a row holds: its MultiValue list and the STRINGS entries it names.
	static uint32 CountPoolItems (const STagKeyDesc& Desc, const STagColumn& Column, uint32 Row) noexcept
	{
		const bool bString = Desc.Type == ETagType::String;
		if (HasAnyFlags(Desc.Flags, ETagFlags::MultiValue))
		{
			const uint32 count = Column.MultiRanges[Row].Count;
			return bString ? count * 2u : count;
		}
		return bString ? 1u : 0u;
	}

	// The bits a value is stored as: a Name interned, a String in STRINGS, else Value.Bits. Old: the bits the
	// row holds for the key; a String with the same text keeps its STRINGS entry.
	uint64 EncodeTag (ETagType Type, const STagValue& Value, const uint64* Old)
	{
		if (Type == ETagType::Name)
		{
			return T.Names.Intern(Value.Text, Epoch);
		}
		if (Type == ETagType::String)
		{
			if (Old != nullptr && T.Strings.Get(static_cast<uint32>(*Old)) == Value.Text.View())
			{
				return *Old;
			}
			return T.Strings.Add(Value.Text, Epoch);
		}
		return Value.Bits;
	}

	/**
	 * Writes every key of a type row from Tags; a key Tags does not name becomes absent. Row may be a reused
	 * row that holds the record's previous tags: values, MultiValue lists and STRINGS entries that did not
	 * change stay as they are, so re-upserting the same tags writes nothing and leaves no garbage.
	 */
	void WriteTags (reflect::TypeId Type, uint32 Row, const std::vector<STagDecl>& Tags, FStringView Subject)
	{
		CTypeTable& table = T.Types[Type];
		const FStringLiteral typeName = CAssetTypeRegistry::Get(Type).Name;
		const uint32 keyCount = static_cast<uint32>(table.Keys.size());
		std::vector<std::vector<const STagValue*>> values(keyCount);
		for (const STagDecl& tag : Tags)
		{
			const std::optional<uint32> key = CAssetTypeRegistry::FindKey(Type, tag.Key);
			if (!key)
			{
				Violation(Subject, FString::Format("tag '{}' is not a key of {}: dropped", tag.Key, typeName));
				continue;
			}
			const STagKeyDesc& desc = table.Keys[*key];
			if (tag.Value.Type != desc.Type)
			{
				Violation(Subject, FString::Format("tag '{}' of {} is {}, not {}: dropped", tag.Key, typeName,
					enum_::ToString(tag.Value.Type), enum_::ToString(desc.Type)));
				continue;
			}
			if (!HasAnyFlags(desc.Flags, ETagFlags::MultiValue) && !values[*key].empty())
			{
				Violation(Subject, FString::Format("tag '{}' of {} is not MultiValue and repeats: the first value is kept", tag.Key, typeName));
				continue;
			}
			values[*key].push_back(&tag.Value);
		}
		for (uint32 key = 0u; key < keyCount; ++key)
		{
			const STagKeyDesc& desc = table.Keys[key];
			STagColumn& column = table.Columns[key];
			const bool bWasPresent = column.PresentBits.Test(Row);
			const std::vector<const STagValue*>& list = values[key];
			if (list.empty())
			{
				if (bWasPresent)
				{
					T.GarbagePoolItems += CountPoolItems(desc, column, Row);
					column.PresentBits.Set(Row, false, Epoch);
					column.MultiRanges.SetIfChanged(Row, SPoolRange {}, Epoch);
				}
				continue;
			}
			if (!HasAnyFlags(desc.Flags, ETagFlags::MultiValue))
			{
				const uint64 old = column.Values[Row];
				const uint64 bits = EncodeTag(desc.Type, *list[0], bWasPresent ? &old : nullptr);
				if (bWasPresent && desc.Type == ETagType::String && bits != old)
				{
					++T.GarbagePoolItems;
				}
				column.Values.SetIfChanged(Row, bits, Epoch);
				column.PresentBits.Set(Row, true, Epoch);
				continue;
			}
			// A MultiValue list: kept when every item is the same, else appended anew.
			const SPoolRange oldRange = bWasPresent ? column.MultiRanges[Row] : SPoolRange {};
			std::vector<uint64> items;
			items.reserve(list.size());
			bool bSame = oldRange.Count == list.size();
			for (uint32 i = 0u; i < list.size(); ++i)
			{
				const uint64 oldItem = bSame ? column.Multi[oldRange.Begin + i] : 0u;
				items.push_back(EncodeTag(desc.Type, *list[i], bSame ? &oldItem : nullptr));
				bSame = bSame && items.back() == oldItem;
			}
			if (!bSame)
			{
				if (bWasPresent)
				{
					T.GarbagePoolItems += CountPoolItems(desc, column, Row);
				}
				const uint32 begin = column.Multi.Append(items, Epoch);
				column.MultiRanges.Set(Row, SPoolRange { begin, static_cast<uint32>(items.size()) }, Epoch);
			}
			column.Values.SetIfChanged(Row, items[0], Epoch);
			column.PresentBits.Set(Row, true, Epoch);
		}
	}

	// ---------------- Records ----------------

	void Touch (uint32 AssetRow, bool bExistedBefore)
	{
		const auto [it, bInserted] = Touched.try_emplace(AssetRow);
		if (bInserted)
		{
			it->second.bExistedBefore = bExistedBefore;
		}
	}

	void MarkDirty (uint32 AssetRow)
	{
		DirtyRecords.insert(AssetRow);
	}

	uint16 InternImporter (FStringView Name)
	{
		if (Name.IsEmpty())
		{
			return InvalidImporter;
		}
		const std::vector<FString>& importers = *T.Importers;
		for (uint32 i = 0u; i < importers.size(); ++i)
		{
			if (importers[i] == Name)
			{
				return static_cast<uint16>(i);
			}
		}
		const uint16 index = static_cast<uint16>(importers.size());
		std::vector<FString> updated = importers;
		updated.push_back(FString(Name));
		T.Importers = std::make_shared<const std::vector<FString>>(std::move(updated));   // frees `importers`
		return index;
	}

	uint32 AppendAssetRow ()
	{
		SAssetsTable& assets = T.Assets;
		const uint32 row = assets.Guid.Append(SGuid {}, Epoch);
		assets.AssetId64.Append(0u, Epoch);
		assets.File.Append(InvalidRow, Epoch);
		assets.Type.Append(asset_types::Unknown, Epoch);
		assets.AssetType.Append(0u, Epoch);
		assets.Importer.Append(InvalidImporter, Epoch);
		assets.Flags.Append(ERecordFlags::None, Epoch);
		assets.CookRule.Append(ECookRule::Default, Epoch);
		assets.Chunk.Append(0u, Epoch);
		assets.PrimaryName.Append(InvalidRow, Epoch);
		assets.Labels.Append(SPoolRange {}, Epoch);
		assets.FirstSub.Append(0u, Epoch);
		assets.NumSubs.Append(0u, Epoch);
		assets.TypeRow.Append(InvalidRow, Epoch);
		assets.ImportStatus.Append(EImportStatus::NotAnalyzed, Epoch);
		assets.Problems.Append(0u, Epoch);
		assets.AnalyzeStamp.Append(0u, Epoch);
		assets.Name.Append(InvalidRow, Epoch);
		assets.ChangedRevision.Append(0u, Epoch);
		assets.Companions.Append(SPoolRange {}, Epoch);
		assets.BaseEdges.Append(SPoolRange {}, Epoch);
		assets.BaseEdgesMasked.Grow(row + 1u, Epoch);
		assets.OverflowEdges.Append(SPoolRange {}, Epoch);
		return row;
	}

	// Drops what an upsert replaces: edges and index entries; the type row and the sub-entries unless the caller
	// reuses them (labels and companions are compared and replaced by SetLabels and SetCompanions).
	void ReleaseRecordContents (uint32 Row, bool bKeepTypeRow, bool bKeepSubEntries)
	{
		SAssetsTable& assets = T.Assets;
		if (!bKeepTypeRow && assets.TypeRow[Row] != InvalidRow)
		{
			KillTypeRow(assets.Type[Row], assets.TypeRow[Row]);
		}
		if (!bKeepSubEntries)
		{
			SSubEntryTable& subs = T.SubEntries;
			const uint32 firstSub = assets.FirstSub[Row];
			for (uint32 i = 0u; i < assets.NumSubs[Row]; ++i)
			{
				const uint32 sub = firstSub + i;
				if (subs.Owner[sub] == InvalidRow)
				{
					continue;
				}
				KillTypeRow(subs.Type[sub], subs.TypeRow[sub]);
				T.Indices.RecordsById64.Erase(MixRegistryKey(subs.AssetId64[sub]), SubEntryBit | sub, Epoch);
				subs.Owner.Set(sub, InvalidRow, Epoch);
				--T.LiveSubEntries;
			}
			assets.NumSubs.SetIfChanged(Row, 0u, Epoch);
		}

		SGraphTables& graph = T.Graph;
		if (!assets.BaseEdgesMasked.Test(Row))
		{
			const uint32 count = assets.BaseEdges[Row].Count;
			graph.GarbageEdges += count;
			graph.LiveEdges -= count;
			assets.BaseEdgesMasked.Set(Row, true, Epoch);
		}
		const uint32 overflow = assets.OverflowEdges[Row].Count;
		graph.GarbageEdges += overflow;
		graph.LiveEdges -= overflow;
		assets.OverflowEdges.SetIfChanged(Row, SPoolRange {}, Epoch);

		T.Indices.RecordsById64.Erase(MixRegistryKey(assets.AssetId64[Row]), Row, Epoch);
		T.Indices.RecordsByGuid.Erase(HashGuid(assets.Guid[Row]), Row, Epoch);
		TouchedGuids.insert(HashGuid(assets.Guid[Row]));
	}

	void RemoveRecord (uint32 Row)
	{
		SAssetsTable& assets = T.Assets;
		const uint32 file = assets.File[Row];
		if (file == InvalidRow)
		{
			return;
		}
		Touch(Row, true);
		const FStringView path = T.Paths.Get(T.Files.Path[file]);
		STouch& touch = Touched[Row];
		touch.RemovedGuid = assets.Guid[Row];
		touch.RemovedPath = FString(path);
		ReleaseRecordContents(Row, false, false);
		SetLabels(Row, {});
		SetCompanions(Row, {}, path);
		T.Indices.RecordsByPortablePath.Erase(HashPortable(path), Row, Epoch);
		TouchedRecordPaths.insert(FString(path));
		T.Files.Record.Set(file, InvalidRow, Epoch);
		assets.File.Set(Row, InvalidRow, Epoch);
		assets.TypeRow.Set(Row, InvalidRow, Epoch);
		--T.LiveRecords;
		ClearProblems(path, (1u << SourceIndex(EProblemSource::Analyze)) | (1u << SourceIndex(EProblemSource::Registry)), false);
	}

	void UpsertRecord (SRecordUpsert& Upsert)
	{
		const FStringView path = Upsert.FileMountPath;
		bool bCreatedFile = false;
		const uint32 file = EnsureFile(path, path, bCreatedFile);
		if (file == InvalidRow)
		{
			return;
		}
		SAssetsTable& assets = T.Assets;
		reflect::TypeId type = Upsert.Type;
		if (type >= asset_types::Count)
		{
			Violation(path, FString::Format("record type {} is not in the asset type table: frt.Unknown", type));
			type = asset_types::Unknown;
		}

		uint32 row = T.Files.Record[file];
		const bool bExisted = row != InvalidRow;
		const std::vector<SSubPlan> subPlan = PlanSubEntries(Upsert.SubEntries, path);
		bool bKeepTypeRow = false;
		bool bKeepSubEntries = false;
		if (bExisted)
		{
			// The same type and the same sub-entries (SubIds and types, in order) reuse their rows in place:
			// a re-upsert of an unchanged structure (a rules edit, a re-analyzed file) appends nothing.
			bKeepTypeRow = assets.Type[row] == type && assets.TypeRow[row] != InvalidRow;
			bKeepSubEntries = CanReuseSubEntries(row, subPlan);
			ReleaseRecordContents(row, bKeepTypeRow, bKeepSubEntries);
		}
		else
		{
			row = AppendAssetRow();
			++T.LiveRecords;
			T.Indices.RecordsByPortablePath.Insert(HashPortable(path), row, Epoch);
			TouchedRecordPaths.insert(FString(path));
		}
		Touch(row, bExisted);
		MarkDirty(row);
		RecordsWithViolations.insert(row);

		const SGuid guid = Upsert.Guid.IsValid() ? Upsert.Guid : MakeProvisionalGuid(path);
		const uint64 id64 = MakeAssetId64(SAssetId { guid, 0u }).Value;
		assets.Guid.SetIfChanged(row, guid, Epoch);
		assets.AssetId64.SetIfChanged(row, id64, Epoch);
		assets.File.SetIfChanged(row, file, Epoch);
		T.Files.Record.SetIfChanged(file, row, Epoch);
		T.Indices.RecordsByGuid.Insert(HashGuid(guid), row, Epoch);
		T.Indices.RecordsById64.Insert(MixRegistryKey(id64), row, Epoch);
		TouchedGuids.insert(HashGuid(guid));

		if (!bKeepTypeRow)
		{
			assets.Type.Set(row, type, Epoch);
			assets.TypeRow.Set(row, AddTypeRow(type, row), Epoch);
		}
		assets.AssetType.SetIfChanged(row, type, Epoch);
		assets.Importer.SetIfChanged(row, InternImporter(Upsert.Importer), Epoch);
		const ERecordFlags producerFlags = Upsert.Flags & ~ERecordFlags::HasProblems;
		assets.Flags.SetIfChanged(row, producerFlags | (assets.Flags[row] & ERecordFlags::HasProblems), Epoch);
		assets.CookRule.SetIfChanged(row, Upsert.CookRule, Epoch);
		assets.Chunk.SetIfChanged(row, Upsert.Chunk, Epoch);
		assets.PrimaryName.SetIfChanged(row, Upsert.PrimaryName.IsEmpty() ? InvalidRow : T.Names.Intern(Upsert.PrimaryName, Epoch), Epoch);
		assets.ImportStatus.SetIfChanged(row, Upsert.ImportStatus, Epoch);
		assets.AnalyzeStamp.SetIfChanged(row, Upsert.AnalyzeStamp, Epoch);
		assets.Name.SetIfChanged(row, T.Names.Intern(GetStem(GetMountPathLeaf(path)), Epoch), Epoch);
		assets.ChangedRevision.SetIfChanged(row, Revision, Epoch);

		std::vector<uint32> labels;
		labels.reserve(Upsert.RuleLabels.size());
		for (const FString& label : Upsert.RuleLabels)
		{
			labels.push_back(T.Names.Intern(label, Epoch) | RuleLabelBit);
		}
		SetLabels(row, labels);

		WriteTags(type, assets.TypeRow[row], Upsert.Tags, path);
		WriteSubEntries(row, guid, subPlan, path, bKeepSubEntries);
		WriteEdges(row, Upsert.Edges, path);
		SetCompanions(row, Upsert.Companions, path);
	}

	// A record's label list: kept when it is the same, else appended anew (the old one is garbage).
	void SetLabels (uint32 Row, const std::vector<uint32>& Labels)
	{
		SAssetsTable& assets = T.Assets;
		const SPoolRange old = assets.Labels[Row];
		bool bSame = old.Count == Labels.size();
		for (uint32 i = 0u; bSame && i < Labels.size(); ++i)
		{
			bSame = T.Labels[old.Begin + i] == Labels[i];
		}
		if (bSame)
		{
			return;
		}
		T.GarbagePoolItems += old.Count;
		assets.Labels.SetIfChanged(Row, SPoolRange { Labels.empty() ? 0u : T.Labels.Append(Labels, Epoch), static_cast<uint32>(Labels.size()) }, Epoch);
	}

	// A record's companions: each path interned once (CompanionPathIds), the list kept when it is the same.
	// New references are counted before old ones are released, so a path in both never changes class.
	void SetCompanions (uint32 Row, const std::vector<FString>& Companions, FStringView Path)
	{
		std::vector<uint32> ids;
		ids.reserve(Companions.size());
		for (const FString& companion : Companions)
		{
			if (!IsWellFormedMountPath(companion) || !HasRelativePart(companion))
			{
				Violation(Path, FString::Format("companion '{}' is not a well-formed mount path of a file: dropped", companion));
				continue;
			}
			const auto [it, bInserted] = W.CompanionPathIds.try_emplace(companion, 0u);
			if (bInserted)
			{
				it->second = T.Paths.Add(companion, Epoch);
			}
			ids.push_back(it->second);
		}
		SAssetsTable& assets = T.Assets;
		const SPoolRange old = assets.Companions[Row];
		bool bSame = old.Count == ids.size();
		for (uint32 i = 0u; bSame && i < ids.size(); ++i)
		{
			bSame = T.Companions[old.Begin + i] == ids[i];
		}
		if (bSame)
		{
			return;
		}
		for (const uint32 id : ids)
		{
			AddCompanionRef(T.Paths.Get(id));
		}
		for (uint32 i = 0u; i < old.Count; ++i)
		{
			ReleaseCompanionRef(T.Paths.Get(T.Companions[old.Begin + i]));
		}
		T.GarbagePoolItems += old.Count;
		assets.Companions.SetIfChanged(Row, SPoolRange { ids.empty() ? 0u : T.Companions.Append(ids, Epoch), static_cast<uint32>(ids.size()) }, Epoch);
	}

	// The sub-entries an upsert declares, checked (a repeated SubId is dropped, an unknown type becomes
	// frt.Unknown; both reported) before the store decides whether the record's rows can be reused.
	struct SSubPlan
	{
		const SSubEntryDecl* Decl = nullptr;
		uint64 SubId = 0u;
		reflect::TypeId Type = 0u;
	};

	std::vector<SSubPlan> PlanSubEntries (const std::vector<SSubEntryDecl>& Decls, FStringView Path)
	{
		std::vector<SSubPlan> plan;
		plan.reserve(Decls.size());
		std::unordered_set<uint64> subIds;
		for (const SSubEntryDecl& decl : Decls)
		{
			const uint64 subId = decl.SubId != 0u ? decl.SubId : MakeSubId(decl.Label);
			if (!subIds.insert(subId).second)
			{
				Violation(Path, FString::Format("sub-entry '{}' repeats SubId {:016x} of another sub-entry: dropped", decl.Label, subId));
				continue;
			}
			reflect::TypeId type = decl.Type;
			if (type >= asset_types::Count)
			{
				Violation(Path, FString::Format("sub-entry '{}' has type {}, not in the asset type table: frt.Unknown", decl.Label, type));
				type = asset_types::Unknown;
			}
			plan.push_back(SSubPlan { &decl, subId, type });
		}
		return plan;
	}

	// The record's sub-entry rows can take the plan in place: the same SubIds with the same types, in order.
	bool CanReuseSubEntries (uint32 Row, const std::vector<SSubPlan>& Plan) const
	{
		const SSubEntryTable& subs = T.SubEntries;
		if (T.Assets.NumSubs[Row] != Plan.size())
		{
			return false;
		}
		const uint32 first = T.Assets.FirstSub[Row];
		for (uint32 i = 0u; i < Plan.size(); ++i)
		{
			const uint32 sub = first + i;
			if (subs.Owner[sub] != Row || subs.SubId[sub] != Plan[i].SubId || subs.Type[sub] != Plan[i].Type)
			{
				return false;
			}
		}
		return true;
	}

	void WriteSubEntries (uint32 Row, const SGuid& Guid, const std::vector<SSubPlan>& Plan, FStringView Path, bool bReuse)
	{
		SAssetsTable& assets = T.Assets;
		SSubEntryTable& subs = T.SubEntries;
		if (bReuse)
		{
			const uint32 first = assets.FirstSub[Row];
			for (uint32 i = 0u; i < Plan.size(); ++i)
			{
				const uint32 sub = first + i;
				const SSubEntryDecl& decl = *Plan[i].Decl;
				subs.Key.SetIfChanged(sub, T.Names.Intern(decl.Label, Epoch), Epoch);
				subs.Flags.SetIfChanged(sub, decl.Flags & ~ERecordFlags::HasProblems, Epoch);
				const uint64 id64 = MakeAssetId64(SAssetId { Guid, Plan[i].SubId }).Value;
				if (subs.AssetId64[sub] != id64)
				{
					T.Indices.RecordsById64.Erase(MixRegistryKey(subs.AssetId64[sub]), SubEntryBit | sub, Epoch);
					T.Indices.RecordsById64.Insert(MixRegistryKey(id64), SubEntryBit | sub, Epoch);
					subs.AssetId64.Set(sub, id64, Epoch);
				}
				WriteTags(Plan[i].Type, subs.TypeRow[sub], decl.Tags, Path);
			}
			return;
		}
		const uint32 first = subs.Owner.Size();
		for (const SSubPlan& plan : Plan)
		{
			const SSubEntryDecl& decl = *plan.Decl;
			const uint32 sub = subs.Owner.Append(Row, Epoch);
			subs.SubId.Append(plan.SubId, Epoch);
			subs.Key.Append(T.Names.Intern(decl.Label, Epoch), Epoch);
			subs.Type.Append(plan.Type, Epoch);
			subs.Flags.Append(decl.Flags & ~ERecordFlags::HasProblems, Epoch);
			subs.TypeRow.Append(AddTypeRow(plan.Type, SubEntryBit | sub), Epoch);
			const uint64 id64 = MakeAssetId64(SAssetId { Guid, plan.SubId }).Value;
			subs.AssetId64.Append(id64, Epoch);
			T.Indices.RecordsById64.Insert(MixRegistryKey(id64), SubEntryBit | sub, Epoch);
			++T.LiveSubEntries;
			WriteTags(plan.Type, subs.TypeRow[sub], decl.Tags, Path);
		}
		assets.FirstSub.SetIfChanged(Row, Plan.empty() ? 0u : first, Epoch);
		assets.NumSubs.SetIfChanged(Row, static_cast<uint32>(Plan.size()), Epoch);
	}

	// ---------------- Edges and targets ----------------

	uint32 FindTarget (const SEdgeDecl& Edge) const
	{
		uint32 found = InvalidRow;
		if (Edge.Target.IsValid())
		{
			const uint64 hash = HashGuid(Edge.Target.Guid) ^ MixRegistryKey(Edge.Target.SubId);
			T.Graph.TargetsByKey.ForEach(hash, [&] (uint32 Row)
			{
				const STargetRow& target = T.Graph.Targets[Row];
				if (target.Kind == ETargetKind::Guid && target.Guid == Edge.Target.Guid && target.TargetSub == Edge.Target.SubId)
				{
					found = Row;
					return false;
				}
				return true;
			});
			return found;
		}
		T.Graph.TargetsByKey.ForEach(HashExact(Edge.TargetPath), [&] (uint32 Row)
		{
			const STargetRow& target = T.Graph.Targets[Row];
			if (target.Kind == ETargetKind::Path && T.Paths.Get(target.Text) == Edge.TargetPath.View())
			{
				found = Row;
				return false;
			}
			return true;
		});
		return found;
	}

	uint32 GetOrCreateTarget (const SEdgeDecl& Edge, FStringView Path)
	{
		if (!Edge.Target.IsValid())
		{
			FStringView mountName;
			FStringView relative;
			if (!SplitMountPath(Edge.TargetPath, mountName, relative) || relative.IsEmpty())
			{
				Violation(Path, FString::Format("edge target '{}' is neither an id nor a mount path: dropped", Edge.TargetPath));
				return InvalidRow;
			}
		}
		const uint32 existing = FindTarget(Edge);
		if (existing != InvalidRow)
		{
			return existing;
		}
		SGraphTables& graph = T.Graph;
		STargetRow target;
		if (Edge.Target.IsValid())
		{
			target.Kind = ETargetKind::Guid;
			target.Guid = Edge.Target.Guid;
			target.TargetSub = Edge.Target.SubId;
			target.Text = Edge.TargetPath.IsEmpty() ? InvalidRow : T.Paths.Add(Edge.TargetPath, Epoch);
		}
		else
		{
			target.Kind = ETargetKind::Path;
			target.Text = T.Paths.Add(Edge.TargetPath, Epoch);
		}
		const uint32 row = graph.Targets.Append(target, Epoch);
		graph.OverflowReverseHead.Append(InvalidRow, Epoch);
		if (target.Kind == ETargetKind::Guid)
		{
			graph.TargetsByKey.Insert(HashGuid(target.Guid) ^ MixRegistryKey(target.TargetSub), row, Epoch);
			W.GuidTargets.emplace(HashGuid(target.Guid), row);
		}
		else
		{
			const FStringView text = Edge.TargetPath;
			graph.TargetsByKey.Insert(HashExact(text), row, Epoch);
			W.PathTargetCandidates.emplace(FString(text), row);
			for (const uint32 split : GetLabelSplits(text))
			{
				W.PathTargetCandidates.emplace(FString(text.Left(split)), row);
			}
		}
		TargetsToResolve.insert(row);
		return row;
	}

	void WriteEdges (uint32 Row, const std::vector<SEdgeDecl>& Decls, FStringView Path)
	{
		SAssetsTable& assets = T.Assets;
		SGraphTables& graph = T.Graph;
		std::unordered_set<uint64> subIds;
		for (uint32 i = 0u; i < assets.NumSubs[Row]; ++i)
		{
			subIds.insert(T.SubEntries.SubId[assets.FirstSub[Row] + i]);
		}
		std::vector<SEdgeRow> edges;
		edges.reserve(Decls.size());
		for (const SEdgeDecl& decl : Decls)
		{
			if (decl.FromSub != 0u && !subIds.contains(decl.FromSub))
			{
				Violation(Path, FString::Format("edge to '{}' comes from SubId {:016x}, which the record does not declare: dropped",
					decl.TargetPath, decl.FromSub));
				continue;
			}
			const uint32 target = GetOrCreateTarget(decl, Path);
			if (target == InvalidRow)
			{
				continue;
			}
			SEdgeRow edge;
			edge.FromSub = decl.FromSub;
			edge.Target = target;
			edge.FieldPath = decl.FieldPath.IsEmpty() ? InvalidRow : T.Names.Intern(decl.FieldPath, Epoch);
			edge.Flags = decl.Flags & ~EDepFlags::Unresolved;
			const bool bDuplicate = std::any_of(edges.begin(), edges.end(), [&] (const SEdgeRow& Other)
			{
				return Other.FromSub == edge.FromSub && Other.Target == edge.Target && Other.Flags == edge.Flags;
			});
			if (!bDuplicate)
			{
				edges.push_back(edge);
			}
		}
		assets.BaseEdgesMasked.Set(Row, true, Epoch);
		const uint32 begin = graph.OverflowEdges.Size();
		for (const SEdgeRow& edge : edges)
		{
			const uint32 index = graph.OverflowEdges.Append(edge, Epoch);
			SReverseEdge reverse;
			reverse.Source = Row;
			reverse.Edge = OverflowEdgeBit | index;
			reverse.Next = graph.OverflowReverseHead[edge.Target];
			graph.OverflowReverseHead.Set(edge.Target, graph.OverflowReverseEdges.Append(reverse, Epoch), Epoch);
		}
		assets.OverflowEdges.SetIfChanged(Row, SPoolRange { edges.empty() ? 0u : begin, static_cast<uint32>(edges.size()) }, Epoch);
		graph.LiveEdges += static_cast<uint32>(edges.size());
	}

	STargetRow ResolveTarget (const STargetRow& Current) const
	{
		STargetRow result = Current;
		result.State = ETargetState::Unresolved;
		result.Resolved = InvalidRow;
		result.bSpellingDiffers = false;
		result.FilePartLength = 0u;
		if (Current.Kind == ETargetKind::Guid)
		{
			uint32 matches = 0u;
			uint32 first = InvalidRow;
			T.Indices.RecordsByGuid.ForEach(HashGuid(Current.Guid), [&] (uint32 Row)
			{
				if (T.Assets.File[Row] != InvalidRow && T.Assets.Guid[Row] == Current.Guid)
				{
					++matches;
					first = first == InvalidRow || Row < first ? Row : first;
				}
				return true;
			});
			if (matches == 1u)
			{
				result.State = ETargetState::Resolved;
				result.Resolved = first;
			}
			else if (matches > 1u)
			{
				result.State = ETargetState::Ambiguous;
			}
			return result;
		}

		const FStringView text = T.Paths.Get(Current.Text);
		std::vector<uint32> splits = GetLabelSplits(text);
		splits.insert(splits.begin(), text.Len());
		for (const uint32 split : splits)
		{
			const FStringView filePart = text.Left(split);
			uint32 first = InvalidRow;
			const uint32 matches = FindRecordRowsPortable(T, filePart, first);
			if (matches == 0u)
			{
				continue;
			}
			result.FilePartLength = split;
			if (matches > 1u)
			{
				result.State = ETargetState::Ambiguous;
				return result;
			}
			result.State = ETargetState::Resolved;
			result.Resolved = first;
			result.TargetSub = split < text.Len() ? MakeSubId(text.RightChop(split + 1u)) : 0u;
			result.bSpellingDiffers = T.Paths.Get(T.Files.Path[T.Assets.File[first]]) != filePart;
			return result;
		}
		result.TargetSub = 0u;
		return result;
	}

	void ApplyTargetResolution (uint32 Row)
	{
		SGraphTables& graph = T.Graph;
		const STargetRow current = graph.Targets[Row];
		const STargetRow resolved = ResolveTarget(current);
		if (resolved.State == current.State && resolved.Resolved == current.Resolved && resolved.TargetSub == current.TargetSub
			&& resolved.bSpellingDiffers == current.bSpellingDiffers && resolved.FilePartLength == current.FilePartLength)
		{
			return;
		}
		if (current.Resolved != InvalidRow)
		{
			graph.TargetsByResolved.Erase(MixRegistryKey(current.Resolved), Row, Epoch);
		}
		if (resolved.Resolved != InvalidRow)
		{
			graph.TargetsByResolved.Insert(MixRegistryKey(resolved.Resolved), Row, Epoch);
		}
		graph.Targets.Set(Row, resolved, Epoch);
		ForEachLiveReverseEdge(T, Row, [&] (const SReverseEdge& Entry)
		{
			if (!Touched.contains(Entry.Source))
			{
				Touch(Entry.Source, true);
			}
			MarkDirty(Entry.Source);
		});
	}

	// ---------------- Derived state ----------------

	// A record's Registry list: the contract problems of its last upsert (fresh ones when it was upserted in
	// this commit, else the stored ones) plus REF006 / PATH001 of its edges.
	void RecomputeRegistryProblems (uint32 Row)
	{
		if (T.Assets.File[Row] == InvalidRow)
		{
			return;
		}
		const FStringView path = T.Paths.Get(T.Files.Path[T.Assets.File[Row]]);
		std::vector<SDiagnostic> diagnostics;
		if (RecordsWithViolations.contains(Row))
		{
			const auto found = PendingViolations.find(path);
			if (found != PendingViolations.end())
			{
				diagnostics = std::move(found->second);
				PendingViolations.erase(found);
			}
		}
		else
		{
			const uint32 subject = FindProblemSubject(T, path);
			const uint32 source = SourceIndex(EProblemSource::Registry);
			if (subject != InvalidRow && T.Problems.Lists[source][subject] != nullptr)
			{
				for (const SDiagnostic& diagnostic : *T.Problems.Lists[source][subject])
				{
					if (diagnostic.Code != Path001 && diagnostic.Code != Ref006)
					{
						diagnostics.push_back(diagnostic);
					}
				}
			}
		}
		std::unordered_set<uint32> reported;
		ForEachLiveEdge(T, Row, [&] (uint32 EdgeRef)
		{
			const SEdgeRow& edge = GetEdgeRow(T, EdgeRef);
			const STargetRow& target = T.Graph.Targets[edge.Target];
			if (!reported.insert(edge.Target).second)
			{
				return;
			}
			const FStringView text = target.Text != InvalidRow ? T.Paths.Get(target.Text) : FStringView();
			if (target.State == ETargetState::Ambiguous)
			{
				SDiagnostic diagnostic = MakeRegistryDiagnostic(ESeverity::Warning, Path001, path, target.Kind == ETargetKind::Path
					? FString::Format("reference '{}' matches several files that differ only in case: it stays unresolved", text)
					: FString::Format("reference to {} matches several records with that id: it stays unresolved", target.Guid.ToString()));
				diagnostic.FieldPath = edge.FieldPath != InvalidRow ? FString(T.Names.Get(edge.FieldPath)) : FString();
				diagnostics.push_back(std::move(diagnostic));
			}
			else if (target.State == ETargetState::Resolved && target.bSpellingDiffers)
			{
				const FStringView onDisk = T.Paths.Get(T.Files.Path[T.Assets.File[target.Resolved]]);
				SDiagnostic diagnostic = MakeRegistryDiagnostic(ESeverity::Info, Ref006, path,
					FString::Format("reference '{}' resolves to '{}', which is spelled differently on disk", text.Left(target.FilePartLength), onDisk));
				diagnostic.FieldPath = edge.FieldPath != InvalidRow ? FString(T.Names.Get(edge.FieldPath)) : FString();
				diagnostics.push_back(std::move(diagnostic));
			}
		});
		SetProblems(path, EProblemSource::Registry, std::move(diagnostics));
	}

	// The asset record a problem subject belongs to (its file, or "file#label"), InvalidRow for none.
	uint32 FindRecordOfSubject (FStringView Subject) const
	{
		uint32 file = FindFileRowExact(T, Subject);
		if (file == InvalidRow)
		{
			for (const uint32 split : GetLabelSplits(Subject))
			{
				file = FindFileRowExact(T, Subject.Left(split));
				if (file != InvalidRow)
				{
					break;
				}
			}
		}
		return file != InvalidRow ? T.Files.Record[file] : InvalidRow;
	}

	uint32 CountSubjectProblems (FStringView Subject) const
	{
		const uint32 subject = FindProblemSubject(T, Subject);
		if (subject == InvalidRow)
		{
			return 0u;
		}
		uint32 count = 0u;
		for (const CPagedColumn<FProblemList>& lists : T.Problems.Lists)
		{
			if (lists[subject] != nullptr)
			{
				count += static_cast<uint32>(lists[subject]->size());
			}
		}
		return count;
	}

	void RecomputeProblemCount (uint32 Row)
	{
		SAssetsTable& assets = T.Assets;
		if (assets.File[Row] == InvalidRow)
		{
			return;
		}
		const FStringView path = T.Paths.Get(T.Files.Path[assets.File[Row]]);
		uint32 count = CountSubjectProblems(path);
		for (uint32 i = 0u; i < assets.NumSubs[Row]; ++i)
		{
			const uint32 sub = assets.FirstSub[Row] + i;
			FString subject(path);
			subject += '#';
			subject += T.Names.Get(T.SubEntries.Key[sub]);
			count += CountSubjectProblems(subject);
		}
		const uint16 problems = static_cast<uint16>(count < 0xFFFFu ? count : 0xFFFFu);
		const ERecordFlags flags = assets.Flags[Row];
		const ERecordFlags updated = count > 0u ? (flags | ERecordFlags::HasProblems) : (flags & ~ERecordFlags::HasProblems);
		if (assets.Problems[Row] != problems || flags != updated)
		{
			// $Problems and is:problem changed, so the record changed (a change-ring entry, ChangedRevision),
			// also when the delta only set or cleared a problem list.
			assets.Problems.Set(Row, problems, Epoch);
			assets.Flags.Set(Row, updated, Epoch);
			Touch(Row, true);
		}
	}

	void Finish ()
	{
		// Targets whose candidate records changed, and the new ones.
		for (const FString& path : TouchedRecordPaths)
		{
			const auto [begin, end] = W.PathTargetCandidates.equal_range(path);
			for (auto it = begin; it != end; ++it)
			{
				TargetsToResolve.insert(it->second);
			}
		}
		for (const uint64 guidHash : TouchedGuids)
		{
			const auto [begin, end] = W.GuidTargets.equal_range(guidHash);
			for (auto it = begin; it != end; ++it)
			{
				TargetsToResolve.insert(it->second);
			}
		}
		std::vector<uint32> targets(TargetsToResolve.begin(), TargetsToResolve.end());
		std::sort(targets.begin(), targets.end());
		for (const uint32 target : targets)
		{
			ApplyTargetResolution(target);
		}

		// Registry problems of every record whose edges or targets changed.
		std::vector<uint32> dirty(DirtyRecords.begin(), DirtyRecords.end());
		std::sort(dirty.begin(), dirty.end());
		for (const uint32 row : dirty)
		{
			RecomputeRegistryProblems(row);
		}
		// Contract problems of items that have no record (a malformed file upsert, a rule file, a mount): they
		// replace that subject's Registry list.
		for (auto& [subject, diagnostics] : PendingViolations)
		{
			SetProblems(subject, EProblemSource::Registry, std::move(diagnostics));
		}

		// Problem counts and HasProblems of the records whose subjects changed.
		std::unordered_set<uint32> counted(dirty.begin(), dirty.end());
		for (const auto& [row, touch] : Touched)
		{
			counted.insert(row);
		}
		for (const uint32 subject : DirtySubjects)
		{
			const uint32 record = FindRecordOfSubject(T.Paths.Get(T.Problems.Subject[subject]));
			if (record != InvalidRow)
			{
				counted.insert(record);
			}
		}
		for (const uint32 row : counted)
		{
			RecomputeProblemCount(row);
		}

		for (MountId mount = 0u; mount < DirtyIntervals.size(); ++mount)
		{
			if (DirtyIntervals[mount] != 0u)
			{
				RecomputeIntervals(mount);
			}
		}

		for (const auto& [row, touch] : Touched)
		{
			if (T.Assets.File[row] != InvalidRow)
			{
				T.Assets.ChangedRevision.SetIfChanged(row, Revision, Epoch);
			}
		}
	}

	CRegistryStore& Store;
	SRegistryTables& T;
	SWriterState& W;
	const uint64 Epoch;
	const uint64 Revision;

	std::unordered_map<uint32, STouch> Touched;
	std::unordered_set<uint32> DirtyRecords;
	std::unordered_set<uint32> RecordsWithViolations;
	std::unordered_set<uint32> DirtySubjects;
	std::unordered_set<uint32> TargetsToResolve;
	std::unordered_set<uint64> TouchedGuids;
	FStringSetIgnoreCase TouchedRecordPaths;
	TStringMap<std::vector<SDiagnostic>> PendingViolations;
	std::vector<uint8> DirtyIntervals;
};


// ---------------- CRegistryStore ----------------

CRegistryStore::CRegistryStore ()
	: Writer(std::make_unique<SWriterState>())
{
	SRegistryTables& tables = LiveView.Tables;
	tables.Mounts = std::make_shared<const std::vector<SMountRow>>();
	tables.Importers = std::make_shared<const std::vector<FString>>();
	const std::span<const SAssetTypeInfo> types = CAssetTypeRegistry::GetTypes();
	tables.Types.resize(types.size());
	for (uint32 i = 0u; i < types.size(); ++i)
	{
		CTypeTable& table = tables.Types[i];
		table.Type = static_cast<reflect::TypeId>(i);
		table.TagSchemaHash = ComputeTagSchemaHash(types[i]);
		for (const SStaticTagKey& key : types[i].Keys)
		{
			STagKeyDesc desc;
			desc.NameId = tables.Names.Intern(key.Name, Epoch);
			desc.Type = key.Type;
			desc.Flags = key.Flags;
			table.Keys.push_back(desc);
		}
		table.Columns.resize(table.Keys.size());
	}
	Publish();
}

CRegistryStore::~CRegistryStore () = default;

void CRegistryStore::CheckOwnerThread ()
{
	const uint64 thread = platform::GetCurrentThreadId();
	if (OwnerThread == 0u)
	{
		OwnerThread = thread;
	}
	frt_assert(OwnerThread == thread);
}

void CRegistryStore::Publish ()
{
	Published.store(std::make_shared<const CRegistryView>(LiveView), std::memory_order_release);
	++Epoch;   // every page the published view holds is frozen now
}

const CRegistryView& CRegistryStore::Live () const noexcept
{
	return LiveView;
}

std::shared_ptr<const CRegistryView> CRegistryStore::Acquire () const noexcept
{
	return Published.load(std::memory_order_acquire);
}

uint64 CRegistryStore::GetRevision () const noexcept
{
	return LiveView.Tables.Revision;
}

void CRegistryStore::Commit (SRegistryDelta&& Delta)
{
	CheckOwnerThread();
	const uint64 start = platform::ReadTimestamp();
	SRegistryTables& tables = LiveView.Tables;
	tables.Revision += 1u;

	CWriter writer(*this, tables.Revision);
	writer.Apply(Delta);
	std::vector<SRecordChange> changes = writer.TakeChanges();

	LastStats = SRegistryCommitStats {};
	LastStats.RecordsChanged = static_cast<uint32>(changes.size());
	SWriterState& state = *Writer;
	for (SRecordChange& change : changes)
	{
		if (state.Ring.size() < ChangeRingCapacity)
		{
			state.Ring.push_back(std::move(change));
		}
		else
		{
			state.DroppedUpTo = (std::max)(state.DroppedUpTo, state.Ring[state.RingHead].Revision);
			state.Ring[state.RingHead] = std::move(change);
			state.RingHead = (state.RingHead + 1u) % ChangeRingCapacity;
		}
	}

	Publish();   // never a compaction here: NeedsCompaction / Compact, at the owner's idle time
	LastStats.Milliseconds = platform::TimestampToMilliseconds(platform::ReadTimestamp() - start);
}

bool CRegistryStore::ForEachChangeSince (uint64 Revision, TFunctionRef<void (const SRecordChange&)> Visit) const
{
	const SWriterState& state = *Writer;
	if (Revision < state.DroppedUpTo)
	{
		return false;
	}
	const uint32 count = static_cast<uint32>(state.Ring.size());
	for (uint32 i = 0u; i < count; ++i)
	{
		const SRecordChange& change = state.Ring[(state.RingHead + i) % count];
		if (change.Revision > Revision)
		{
			Visit(change);
		}
	}
	return true;
}

void CRegistryStore::AdoptImage (SRegistryImage&& Image, uint64 Revision)
{
	CheckOwnerThread();
	const uint64 start = platform::ReadTimestamp();
	SRegistryDelta delta = MakeImageDelta(std::move(Image));
	SRegistryTables& tables = LiveView.Tables;
	tables.Revision = Revision;
	tables.Indices.FilesByPath.Reserve(tables.LiveFiles + static_cast<uint32>(delta.FileUpserts.size()), Epoch);
	tables.Indices.FilesByPortablePath.Reserve(tables.LiveFiles + static_cast<uint32>(delta.FileUpserts.size()), Epoch);
	tables.Indices.RecordsByPortablePath.Reserve(tables.LiveRecords + static_cast<uint32>(delta.RecordUpserts.size()), Epoch);
	tables.Indices.RecordsByGuid.Reserve(tables.LiveRecords + static_cast<uint32>(delta.RecordUpserts.size()), Epoch);

	CWriter writer(*this, Revision);
	writer.Apply(delta);
	LastStats = SRegistryCommitStats {};
	LastStats.RecordsChanged = static_cast<uint32>(writer.TakeChanges().size());
	Writer->DroppedUpTo = (std::max)(Writer->DroppedUpTo, Revision);   // readers refresh everything

	CompactGraphTables();
	LastStats.bCompactedGraph = true;
	Publish();
	LastStats.Milliseconds = platform::TimestampToMilliseconds(platform::ReadTimestamp() - start);
}

bool CRegistryStore::GraphNeedsCompaction () const noexcept
{
	const SGraphTables& graph = LiveView.Tables.Graph;
	return graph.GarbageEdges >= 65536u && graph.GarbageEdges >= graph.LiveEdges / 4u;
}

bool CRegistryStore::RecordTablesNeedCompaction () const noexcept
{
	const SRegistryTables& tables = LiveView.Tables;
	uint64 rows = tables.SubEntries.Owner.Size();
	uint64 deadRows = rows - tables.LiveSubEntries;
	uint64 poolItems = static_cast<uint64>(tables.Labels.Size()) + tables.Companions.Size() + tables.Strings.Size();
	for (const CTypeTable& table : tables.Types)
	{
		rows += table.Records.Size();
		deadRows += table.Records.Size() - table.LiveRows;
		for (const STagColumn& column : table.Columns)
		{
			poolItems += column.Multi.Size();
		}
	}
	const uint64 garbage = tables.GarbagePoolItems;
	return (deadRows >= 4096u && deadRows * 4u >= rows) || (garbage >= 16384u && garbage * 4u >= poolItems);
}

bool CRegistryStore::NeedsCompaction () const noexcept
{
	return GraphNeedsCompaction() || RecordTablesNeedCompaction();
}

void CRegistryStore::Compact ()
{
	CheckOwnerThread();
	const bool bGraph = GraphNeedsCompaction();
	const bool bTables = RecordTablesNeedCompaction();
	if (bGraph || bTables)
	{
		RunCompaction(bGraph, bTables);
	}
}

void CRegistryStore::CompactGraph ()
{
	CheckOwnerThread();
	RunCompaction(true, false);
}

void CRegistryStore::CompactTables ()
{
	CheckOwnerThread();
	RunCompaction(false, true);
}

void CRegistryStore::RunCompaction (bool bGraph, bool bTables)
{
	const uint64 start = platform::ReadTimestamp();
	LastStats = SRegistryCommitStats {};
	if (bTables)
	{
		LiveView.Tables.Revision += 1u;   // sub-entry and type-table rows are renumbered
		CompactRecordTables();
		LastStats.bCompactedTables = true;
	}
	if (bGraph)
	{
		CompactGraphTables();
		LastStats.bCompactedGraph = true;
	}
	Publish();
	LastStats.Milliseconds = platform::TimestampToMilliseconds(platform::ReadTimestamp() - start);
}

/**
 * Rebuilds SUBENTRY, the type tables, the label, companion, Multi and STRINGS pools and the AssetId64 index
 * from the live rows, then remaps FirstSub and TypeRow. New columns are built (as CompactGraphTables does),
 * so views that hold the old ones keep them; Names and Paths stay (NAMES is interned, PATHS ids are stable).
 */
void CRegistryStore::CompactRecordTables ()
{
	SRegistryTables& tables = LiveView.Tables;
	SAssetsTable& assets = tables.Assets;
	const uint64 epoch = Epoch;
	const uint32 assetRows = assets.File.Size();

	// SUBENTRY: each live record's sub-entries in declared order, records by row.
	const SSubEntryTable& oldSubs = tables.SubEntries;
	SSubEntryTable subs;
	std::vector<uint32> subMap(oldSubs.Owner.Size(), InvalidRow);
	for (uint32 row = 0u; row < assetRows; ++row)
	{
		const uint32 count = assets.File[row] != InvalidRow ? assets.NumSubs[row] : 0u;
		const uint32 first = subs.Owner.Size();
		for (uint32 i = 0u; i < count; ++i)
		{
			const uint32 old = assets.FirstSub[row] + i;
			subMap[old] = subs.Owner.Append(row, epoch);
			subs.SubId.Append(oldSubs.SubId[old], epoch);
			subs.Key.Append(oldSubs.Key[old], epoch);
			subs.Type.Append(oldSubs.Type[old], epoch);
			subs.Flags.Append(oldSubs.Flags[old], epoch);
			subs.TypeRow.Append(oldSubs.TypeRow[old], epoch);   // remapped below
			subs.AssetId64.Append(oldSubs.AssetId64[old], epoch);
		}
		assets.FirstSub.SetIfChanged(row, count > 0u ? first : 0u, epoch);
	}

	// Type tables without dead rows; STRINGS rebuilt from the values that remain.
	CTextPool strings;
	std::vector<CTypeTable> types;
	types.reserve(tables.Types.size());
	std::vector<std::vector<uint32>> rowMaps(tables.Types.size());
	for (uint32 type = 0u; type < tables.Types.size(); ++type)
	{
		const CTypeTable& old = tables.Types[type];
		CTypeTable table;
		table.Type = old.Type;
		table.TagSchemaHash = old.TagSchemaHash;
		table.Keys = old.Keys;
		table.Columns.resize(old.Columns.size());
		std::vector<uint32>& rowMap = rowMaps[type];
		rowMap.assign(old.Records.Size(), InvalidRow);
		for (uint32 oldRow = 0u; oldRow < old.Records.Size(); ++oldRow)
		{
			const uint32 record = old.Records[oldRow];
			const bool bSub = (record & SubEntryBit) != 0u;
			if (old.Dead.Test(oldRow) || (bSub && subMap[record & ~SubEntryBit] == InvalidRow))
			{
				continue;
			}
			const uint32 row = table.Records.Append(bSub ? (SubEntryBit | subMap[record & ~SubEntryBit]) : record, epoch);
			rowMap[oldRow] = row;
			table.Dead.Grow(row + 1u, epoch);
			for (uint32 key = 0u; key < old.Columns.size(); ++key)
			{
				const STagColumn& from = old.Columns[key];
				STagColumn& to = table.Columns[key];
				to.Values.Grow(row + 1u, epoch);
				to.PresentBits.Grow(row + 1u, epoch);
				to.MultiRanges.Grow(row + 1u, epoch);
				if (!from.PresentBits.Test(oldRow))
				{
					continue;
				}
				const STagKeyDesc& desc = old.Keys[key];
				const auto remap = [&] (uint64 Bits) -> uint64
				{
					return desc.Type == ETagType::String ? strings.Add(tables.Strings.Get(static_cast<uint32>(Bits)), epoch) : Bits;
				};
				to.PresentBits.Set(row, true, epoch);
				if (HasAnyFlags(desc.Flags, ETagFlags::MultiValue))
				{
					const SPoolRange range = from.MultiRanges[oldRow];
					std::vector<uint64> items(range.Count);
					for (uint32 i = 0u; i < range.Count; ++i)
					{
						items[i] = remap(from.Multi[range.Begin + i]);
					}
					to.MultiRanges.Set(row, SPoolRange { items.empty() ? 0u : to.Multi.Append(items, epoch), range.Count }, epoch);
					to.Values.Set(row, items.empty() ? 0u : items[0], epoch);
				}
				else
				{
					to.Values.Set(row, remap(from.Values[oldRow]), epoch);
				}
			}
		}
		table.LiveRows = table.Records.Size();
		types.push_back(std::move(table));
	}
	for (uint32 row = 0u; row < assetRows; ++row)
	{
		if (assets.File[row] != InvalidRow && assets.TypeRow[row] != InvalidRow)
		{
			assets.TypeRow.SetIfChanged(row, rowMaps[assets.Type[row]][assets.TypeRow[row]], epoch);
		}
	}
	for (uint32 sub = 0u; sub < subs.Owner.Size(); ++sub)
	{
		subs.TypeRow.Set(sub, rowMaps[subs.Type[sub]][subs.TypeRow[sub]], epoch);
	}

	// Labels and companions: the live records' lists, in row order.
	CPagedPool<uint32> labels;
	CPagedPool<uint32> companions;
	std::vector<uint32> items;
	const auto repack = [&] (CPagedColumn<SPoolRange>& Ranges, const CPagedPool<uint32>& From, CPagedPool<uint32>& To, uint32 Row)
	{
		const SPoolRange range = assets.File[Row] != InvalidRow ? Ranges[Row] : SPoolRange {};
		items.resize(range.Count);
		for (uint32 i = 0u; i < range.Count; ++i)
		{
			items[i] = From[range.Begin + i];
		}
		Ranges.SetIfChanged(Row, SPoolRange { items.empty() ? 0u : To.Append(items, epoch), range.Count }, epoch);
	};
	for (uint32 row = 0u; row < assetRows; ++row)
	{
		repack(assets.Labels, tables.Labels, labels, row);
		repack(assets.Companions, tables.Companions, companions, row);
	}

	// The AssetId64 index names SUBENTRY rows: rebuilt.
	CPagedHashIndex byId64;
	byId64.Reserve(tables.LiveRecords + subs.Owner.Size(), epoch);
	for (uint32 row = 0u; row < assetRows; ++row)
	{
		if (assets.File[row] != InvalidRow)
		{
			byId64.Insert(MixRegistryKey(assets.AssetId64[row]), row, epoch);
		}
	}
	for (uint32 sub = 0u; sub < subs.Owner.Size(); ++sub)
	{
		byId64.Insert(MixRegistryKey(subs.AssetId64[sub]), SubEntryBit | sub, epoch);
	}

	tables.SubEntries = std::move(subs);
	tables.LiveSubEntries = tables.SubEntries.Owner.Size();
	tables.Types = std::move(types);
	tables.Strings = std::move(strings);
	tables.Labels = std::move(labels);
	tables.Companions = std::move(companions);
	tables.Indices.RecordsById64 = std::move(byId64);
	tables.GarbagePoolItems = 0u;
}

/**
 * Rebuilds both CSRs from the live edges: each live record's edges in row order become its BaseEdges slice;
 * the reverse CSR is a counting sort of them by target. Overflow lists and masks start empty again. The
 * columns are built anew, so views that hold the old ones keep them.
 */
void CRegistryStore::CompactGraphTables ()
{
	SRegistryTables& tables = LiveView.Tables;
	SAssetsTable& assets = tables.Assets;
	SGraphTables& graph = tables.Graph;
	const uint32 assetRows = assets.File.Size();
	const uint32 targetRows = graph.Targets.Size();

	std::vector<SEdgeRow> edges;
	edges.reserve(graph.LiveEdges);
	std::vector<SPoolRange> ranges(assetRows);
	std::vector<uint32> sources;
	sources.reserve(graph.LiveEdges);
	for (uint32 row = 0u; row < assetRows; ++row)
	{
		ranges[row].Begin = static_cast<uint32>(edges.size());
		if (assets.File[row] != InvalidRow)
		{
			ForEachLiveEdge(tables, row, [&] (uint32 EdgeRef)
			{
				edges.push_back(GetEdgeRow(tables, EdgeRef));
				sources.push_back(row);
			});
		}
		ranges[row].Count = static_cast<uint32>(edges.size()) - ranges[row].Begin;
	}

	std::vector<uint32> counts(targetRows + 1u, 0u);
	for (const SEdgeRow& edge : edges)
	{
		++counts[edge.Target + 1u];
	}
	for (uint32 i = 1u; i <= targetRows; ++i)
	{
		counts[i] += counts[i - 1u];
	}
	std::vector<SReverseEdge> reverse(edges.size());
	std::vector<uint32> cursor(counts.begin(), counts.end() - 1);
	for (uint32 i = 0u; i < edges.size(); ++i)
	{
		SReverseEdge& entry = reverse[cursor[edges[i].Target]++];
		entry.Source = sources[i];
		entry.Edge = i;
	}

	const uint64 epoch = Epoch;
	CPagedColumn<SEdgeRow> baseEdges;
	baseEdges.Grow(static_cast<uint32>(edges.size()), epoch);
	for (uint32 i = 0u; i < edges.size(); ++i)
	{
		baseEdges.Set(i, edges[i], epoch);
	}
	CPagedColumn<SPoolRange> baseRanges;
	CPagedBitmap masked;
	CPagedColumn<SPoolRange> overflowRanges;
	baseRanges.Grow(assetRows, epoch);
	masked.Grow(assetRows, epoch);
	overflowRanges.Grow(assetRows, epoch);
	for (uint32 row = 0u; row < assetRows; ++row)
	{
		baseRanges.Set(row, ranges[row], epoch);
		if (assets.File[row] == InvalidRow)
		{
			masked.Set(row, true, epoch);
		}
	}
	CPagedColumn<SReverseEdge> baseReverseEdges;
	baseReverseEdges.Grow(static_cast<uint32>(reverse.size()), epoch);
	for (uint32 i = 0u; i < reverse.size(); ++i)
	{
		baseReverseEdges.Set(i, reverse[i], epoch);
	}
	CPagedColumn<SPoolRange> baseReverse;
	CPagedColumn<uint32> reverseHeads;
	baseReverse.Grow(targetRows, epoch);
	reverseHeads.Grow(targetRows, epoch);
	for (uint32 target = 0u; target < targetRows; ++target)
	{
		baseReverse.Set(target, SPoolRange { counts[target], counts[target + 1u] - counts[target] }, epoch);
		reverseHeads.Set(target, InvalidRow, epoch);
	}

	assets.BaseEdges = std::move(baseRanges);
	assets.BaseEdgesMasked = std::move(masked);
	assets.OverflowEdges = std::move(overflowRanges);
	graph.BaseEdges = std::move(baseEdges);
	graph.OverflowEdges.Clear();
	graph.BaseReverse = std::move(baseReverse);
	graph.BaseReverseEdges = std::move(baseReverseEdges);
	graph.OverflowReverseHead = std::move(reverseHeads);
	graph.OverflowReverseEdges.Clear();
	graph.LiveEdges = static_cast<uint32>(edges.size());
	graph.GarbageEdges = 0u;
}
}
