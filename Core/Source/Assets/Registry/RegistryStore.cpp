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
// One change-ring entry: SRecordChange without its FString (the path is a PATHS id, which never changes and
// is never reused), so 64k entries take 2 MB; ForEachChangeSince spells the path out.
struct CRegistryStore::SRingEntry
{
	uint64 Revision = 0u;
	SGuid Guid;
	uint32 Path = InvalidRow;
	ERecordChange Kind = ERecordChange::Modified;
};

namespace
{
// The epoch of the writer's own hash indices: they are never published, so their pages are never frozen.
constexpr uint64 WriterEpoch = 0u;

// A companion path (portable fold) and how many committed records name it.
struct SCompanionRef
{
	uint32 Path = InvalidRow;                          // PATHS: the spelling that first named it
	uint32 Count = 0u;
};
}

/**
 * What only the writer needs: lists the readers never look at, so they stay outside the views (nothing here is
 * copied when a view is published). Its hash indices are CPagedHashIndex tables at WriterEpoch, keyed like the
 * view's (in-memory hashes, rows compared by the caller), never std node containers: they cost a slot of 8 bytes
 * per entry instead of a node, a bucket and, for strings, a copy of the key. AdoptImage rebuilds all of it.
 */
struct CRegistryStore::SWriterState
{
	std::vector<std::vector<uint32>> FolderChildren;   // per FOLDERS row: children by (fold, bytes) of name
	std::vector<std::vector<uint32>> FolderFiles;      // per FOLDERS row: its FILES rows, unordered
	std::vector<uint32> FileSlot;                      // per FILES row: its index in FolderFiles[folder]
	std::vector<uint32> FileOrder;                     // the live FILES rows by PathRank (CompareFileOrder)

	// Path targets by the portable hash (HashPortable) of every file path they could name (the whole text,
	// and the text before each '#'): the targets to re-resolve when a record of that path comes or goes. A
	// hash collision only re-resolves a target that stays as it was; no path is kept twice.
	CPagedHashIndex PathTargetCandidates;
	// Guid targets by GUID (HashGuid).
	CPagedHashIndex GuidTargets;
	// How many committed records name each companion path (portable fold): CompanionRefs, found by HashPortable
	// of the path in CompanionRefsByFold.
	std::vector<SCompanionRef> CompanionRefs;
	CPagedHashIndex CompanionRefsByFold;
	// The PATHS id of every companion path ever named (HashExact of its bytes), so a re-upsert adds no path again.
	CPagedHashIndex CompanionPathIds;
	// Problem subjects in byte order: the subjects at or below a path are one range ('#' and '/' follow it).
	std::map<FString, uint32, SStringLess> SubjectsByText;

	// The change ring.
	std::vector<SRingEntry> Ring;
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

bool HasRelativePart (FStringView MountPath) noexcept
{
	FStringView mountName;
	FStringView relative;
	return SplitMountPath(MountPath, mountName, relative) && !relative.IsEmpty();
}

// Visit(uint32 Split) for the byte offsets of the '#' characters that may end a target's file part, rightmost
// first (the whole text is tried before them); Visit returns false to stop.
template <typename TVisit>
void ForEachLabelSplit (FStringView Text, TVisit&& Visit)
{
	const char* data = Text.GetData();
	for (uint32 i = Text.Len(); i > 1u; --i)
	{
		if (data[i - 1u] == '#' && !Visit(i - 1u))
		{
			return;
		}
	}
}

/**
 * A record keeps one edge per distinct (FromSub, Target, Flags): Keep(i) is false for an edge that repeats an
 * earlier one. Short lists compare pairwise; long ones (a table's rows, one edge each) go through a hash set, so
 * a 50k-row table is not quadratic.
 */
class CEdgeDeduplicator
{
public:
	// Whether Edge is new among the edges seen since the last Reset (and remembers it).
	bool Add (const SEdgeRow& Edge, std::span<const SEdgeRow> Kept)
	{
		if (Kept.size() < 32u)
		{
			for (const SEdgeRow& other : Kept)
			{
				if (other.FromSub == Edge.FromSub && other.Target == Edge.Target && other.Flags == Edge.Flags)
				{
					return false;
				}
			}
			return true;
		}
		if (Kept.size() == 32u && Seen.GetCount() == 0u)
		{
			for (uint32 i = 0u; i < Kept.size(); ++i)
			{
				Seen.Insert(Hash(Kept[i]), i, WriterEpoch);
			}
		}
		bool bRepeated = false;
		const uint64 hash = Hash(Edge);
		Seen.ForEach(hash, [&] (uint32 Index)
		{
			const SEdgeRow& other = Kept[Index];
			bRepeated = other.FromSub == Edge.FromSub && other.Target == Edge.Target && other.Flags == Edge.Flags;
			return !bRepeated;
		});
		if (!bRepeated)
		{
			Seen.Insert(hash, static_cast<uint32>(Kept.size()), WriterEpoch);
		}
		return !bRepeated;
	}

	void Reset () noexcept
	{
		if (Seen.GetCount() != 0u)
		{
			Seen.Clear();
		}
	}

private:
	static uint64 Hash (const SEdgeRow& Edge) noexcept
	{
		return MixRegistryKey(Edge.FromSub ^ (static_cast<uint64>(Edge.Target) << 16u) ^ (static_cast<uint64>(Edge.Flags) * 0x9E3779B97F4A7C15ull));
	}

	CPagedHashIndex Seen;
};
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
	std::vector<SRingEntry> TakeChanges ()
	{
		std::vector<SRingEntry> changes;
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
			SRingEntry change;
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
				change.Guid = T.Assets.Guid[row];
				change.Path = T.Files.Path[T.Assets.File[row]];
			}
			else
			{
				change.Guid = touch.RemovedGuid;
				change.Path = touch.RemovedPath;
			}
			changes.push_back(change);
		}
		return changes;
	}

private:
	struct STouch
	{
		bool bExistedBefore = false;
		SGuid RemovedGuid;
		uint32 RemovedPath = InvalidRow;   // PATHS
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

	// The CompanionRefs entry of Path (portable fold), or InvalidRow.
	uint32 FindCompanionRef (FStringView Path, uint64 PortableHash) const
	{
		uint32 found = InvalidRow;
		W.CompanionRefsByFold.ForEach(PortableHash, [&] (uint32 Index)
		{
			if (EqualPortable(T.Paths.Get(W.CompanionRefs[Index].Path), Path))
			{
				found = Index;
				return false;
			}
			return true;
		});
		return found;
	}

	[[nodiscard]] bool IsNamedCompanion (FStringView Path, uint64 PortableHash) const
	{
		if (W.CompanionRefs.empty())
		{
			return false;
		}
		const uint32 ref = FindCompanionRef(Path, PortableHash);
		return ref != InvalidRow && W.CompanionRefs[ref].Count > 0u;
	}

	EFileClass EffectiveClass (FStringView Path, EFileClass BaseClass, uint64 PortableHash) const
	{
		return IsNamedCompanion(Path, PortableHash) ? EFileClass::Companion : BaseClass;
	}

	EFileClass EffectiveClass (FStringView Path, EFileClass BaseClass) const
	{
		return W.CompanionRefs.empty() ? BaseClass : EffectiveClass(Path, BaseClass, HashPortable(Path));
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
		bOutCreated = true;
		SNewFile file;
		file.LeafName = T.Names.Intern(GetMountPathLeaf(Path), Epoch);
		return CreateFileRow(folder, Path, *mount, file);
	}

	// What CreateFileRow writes into a new FILES row (an upsert's file starts as class Asset with no stats until
	// the upsert says otherwise), and the hashes it shares with its caller.
	struct SNewFile
	{
		uint32 PathId = InvalidRow;                 // PATHS; InvalidRow: CreateFileRow adds the path
		uint32 LeafName = InvalidRow;               // NAMES
		uint64 MountRuleHash = 0u;                  // HashMountRule of the path; 0: CreateFileRow computes it
		uint64 PortableHash = 0u;                   // out: HashPortable of the path
		uint64 Size = 0u;
		int64 MTime = 0;
		platform::SFileIdentity Identity;
		hash::SContentHash ContentHash;
		uint64 MetaHash = 0u;
		EFileClass Class = EFileClass::Asset;
		EMetaState MetaState = EMetaState::PendingMeta;
	};

	// A new FILES row in Folder, listed in the path indices.
	uint32 CreateFileRow (uint32 Folder, FStringView Path, const SMountRow& Mount, SNewFile& File)
	{
		SFilesTable& files = T.Files;
		const uint32 row = files.Folder.Append(Folder, Epoch);
		files.LeafName.Append(File.LeafName, Epoch);
		const uint32 pathId = File.PathId != InvalidRow ? File.PathId : T.Paths.Add(Path, Epoch);
		files.Path.Append(pathId, Epoch);
		char buffer[registry_detail::FastCaseBytes];
		FString heap;
		const FStringView folded = FoldPortable(Path, buffer, heap);
		File.PortableHash = folded.GetData() == buffer ? hash::HashBytes(buffer, folded.Len()) : HashPortable(Path);
		files.FoldedPath.Append(folded == Path ? T.Paths.GetRef(pathId) : T.Paths.Store(folded), Epoch);
		files.Mount.Append(Mount.Id, Epoch);
		files.Size.Append(File.Size, Epoch);
		files.MTime.Append(File.MTime, Epoch);
		files.Identity.Append(File.Identity, Epoch);
		files.ContentHash.Append(File.ContentHash, Epoch);
		files.MetaHash.Append(File.MetaHash, Epoch);
		const EFileClass fileClass = EffectiveClass(Path, File.Class, File.PortableHash);
		files.Class.Append(fileClass, Epoch);
		if (fileClass == EFileClass::Companion && !bBulk)
		{
			TouchedRecordPaths.insert(FString(Path));   // edges to the path now find a companion
		}
		files.BaseClass.Append(File.Class, Epoch);
		files.MetaState.Append(File.MetaState, Epoch);
		files.Record.Append(InvalidRow, Epoch);
		files.PathRank.Append(0u, Epoch);   // UpdateFileOrder ranks it at the end of the commit
		NewFiles.push_back(row);
		T.Indices.FilesByPath.Insert(File.MountRuleHash != 0u ? File.MountRuleHash : HashMountRule(Path, Mount.bCaseSensitive), row, Epoch);
		T.Indices.FilesByPortablePath.Insert(File.PortableHash, row, Epoch);
		W.FileSlot.resize(row + 1u);
		W.FileSlot[row] = static_cast<uint32>(W.FolderFiles[Folder].size());
		W.FolderFiles[Folder].push_back(row);
		++T.LiveFiles;
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
		if (files.Class[Row] == EFileClass::Companion)
		{
			TouchedRecordPaths.insert(FString(path));   // edges to the companion become Unresolved
		}
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
		bFilesRemoved = true;
		ClearProblems(path, (1u << SourceIndex(EProblemSource::Analyze)) | (1u << SourceIndex(EProblemSource::Registry)), false);
	}

	// Called when a path starts or stops being named as a companion: its FILES rows change class, and the
	// targets naming it are re-resolved (ETargetState::Companion), whichever comes first, file or record.
	void UpdateCompanionClass (FStringView Path, uint64 PortableHash, bool bCompanion)
	{
		if (!bBulk)
		{
			TouchedRecordPaths.insert(FString(Path));   // bulk adoption resolves every target at the end
		}
		T.Indices.FilesByPortablePath.ForEach(PortableHash, [&] (uint32 Row)
		{
			if (T.Files.Folder[Row] != InvalidRow && EqualPortable(T.Paths.Get(T.Files.Path[Row]), Path))
			{
				T.Files.Class.SetIfChanged(Row, bCompanion ? EFileClass::Companion : T.Files.BaseClass[Row], Epoch);
			}
			return true;
		});
	}

	// PathId: the PATHS id of Path.
	void AddCompanionRef (FStringView Path, uint32 PathId)
	{
		const uint64 hash = HashPortable(Path);
		uint32 ref = FindCompanionRef(Path, hash);
		if (ref == InvalidRow)
		{
			ref = static_cast<uint32>(W.CompanionRefs.size());
			W.CompanionRefs.push_back(SCompanionRef { PathId, 0u });
			W.CompanionRefsByFold.Insert(hash, ref, WriterEpoch);
		}
		if (++W.CompanionRefs[ref].Count == 1u)
		{
			UpdateCompanionClass(Path, hash, true);
		}
	}

	// An entry whose count drops to zero stays (with count 0) for the next record that names the path.
	void ReleaseCompanionRef (FStringView Path)
	{
		const uint64 hash = HashPortable(Path);
		const uint32 ref = FindCompanionRef(Path, hash);
		if (ref != InvalidRow && W.CompanionRefs[ref].Count > 0u && --W.CompanionRefs[ref].Count == 0u)
		{
			UpdateCompanionClass(Path, hash, false);
		}
	}

	// The PATHS id of a companion spelling, added the first time it is named.
	uint32 GetCompanionPathId (FStringView Path)
	{
		const uint64 hash = HashExact(Path);
		uint32 found = InvalidRow;
		W.CompanionPathIds.ForEach(hash, [&] (uint32 Id)
		{
			if (T.Paths.Get(Id) == Path)
			{
				found = Id;
				return false;
			}
			return true;
		});
		if (found == InvalidRow)
		{
			found = T.Paths.Add(Path, Epoch);
			W.CompanionPathIds.Insert(hash, found, WriterEpoch);
		}
		return found;
	}

	// ---------------- Type tables ----------------

	uint32 AddTypeRow (reflect::TypeId Type, uint32 RecordValue)
	{
		CTypeTable& table = T.Types[Type];
		const uint32 row = table.Records.Append(RecordValue, Epoch);
		table.Dead.Grow(row + 1u, Epoch);
		for (uint32 key = 0u; key < table.Columns.size(); ++key)
		{
			STagColumn& column = table.Columns[key];
			column.Values.Grow(row + 1u, Epoch);
			column.PresentBits.Grow(row + 1u, Epoch);
			if (HasAnyFlags(table.Keys[key].Flags, ETagFlags::MultiValue))
			{
				column.MultiRanges.Grow(row + 1u, Epoch);
			}
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
					if (HasAnyFlags(desc.Flags, ETagFlags::MultiValue))
					{
						column.MultiRanges.SetIfChanged(Row, SPoolRange {}, Epoch);
					}
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
		assets.CookRule.Append(ECookRule::Auto, Epoch);
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
		touch.RemovedPath = T.Files.Path[file];
		ReleaseRecordContents(Row, false, false);
		DropEdges(Row);
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
		assets.Name.SetIfChanged(row, T.Names.Intern(GetAssetStem(GetMountPathLeaf(path)), Epoch), Epoch);
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
			ids.push_back(GetCompanionPathId(companion));
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
			AddCompanionRef(T.Paths.Get(id), id);
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
			bool bLabelsChanged = false;
			const uint32 first = assets.FirstSub[Row];
			for (uint32 i = 0u; i < Plan.size(); ++i)
			{
				const uint32 sub = first + i;
				const SSubEntryDecl& decl = *Plan[i].Decl;
				const uint32 key = T.Names.Intern(decl.Label, Epoch);
				bLabelsChanged = bLabelsChanged || subs.Key[sub] != key;
				subs.Key.SetIfChanged(sub, key, Epoch);
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
			if (bLabelsChanged)
			{
				RankSubEntries(Row);
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
			subs.LabelRank.Append(0u, Epoch);
			T.Indices.RecordsById64.Insert(MixRegistryKey(id64), SubEntryBit | sub, Epoch);
			++T.LiveSubEntries;
			WriteTags(plan.Type, subs.TypeRow[sub], decl.Tags, Path);
		}
		assets.FirstSub.SetIfChanged(Row, Plan.empty() ? 0u : first, Epoch);
		assets.NumSubs.SetIfChanged(Row, static_cast<uint32>(Plan.size()), Epoch);
		RankSubEntries(Row);
	}

	/**
	 * LabelRank of a record's sub-entries: their order by LessSubEntryLabel. Long lists sort by the first eight bytes
	 * of the folded label as one integer (a table's rows: "row_12" and "row_120" differ there), comparing labels only
	 * where those bytes tie.
	 */
	void RankSubEntries (uint32 Row)
	{
		const uint32 count = T.Assets.NumSubs[Row];
		if (count == 0u)
		{
			return;
		}
		const uint32 first = T.Assets.FirstSub[Row];
		SubOrder.clear();
		for (uint32 i = 0u; i < count; ++i)
		{
			const FStringView folded = T.Names.GetFolded(T.SubEntries.Key[first + i]);
			uint64 prefix = 0u;
			for (uint32 b = 0u; b < 8u; ++b)
			{
				prefix = (prefix << 8u) | (b < folded.Len() ? static_cast<uint8>(folded.GetData()[b]) : 0u);
			}
			SubOrder.emplace_back(prefix, first + i);
		}
		std::sort(SubOrder.begin(), SubOrder.end(), [&] (const std::pair<uint64, uint32>& A, const std::pair<uint64, uint32>& B)
		{
			return A.first != B.first ? A.first < B.first : LessSubEntryLabel(T, A.second, B.second);
		});
		for (uint32 i = 0u; i < count; ++i)
		{
			T.SubEntries.LabelRank.SetIfChanged(SubOrder[i].second, i, Epoch);
		}
	}

	// ---------------- File order (SFilesTable::PathRank) ----------------

	static constexpr uint64 RankLimit = 0xFFFFFFFFull;   // ranks lie strictly between 0 and RankLimit

	// Ranks FileOrder afresh, evenly spaced over the first two thirds of the range (later files sort last often).
	void RenumberFiles ()
	{
		const std::vector<uint32>& order = W.FileOrder;
		const uint64 spacing = (std::max)(uint64(1u), RankLimit / (order.size() + order.size() / 2u + 2u));
		for (size_t i = 0u; i < order.size(); ++i)
		{
			T.Files.PathRank.SetIfChanged(order[i], static_cast<uint32>((i + 1u) * spacing), Epoch);
		}
	}

	/**
	 * Keeps FileOrder and PathRank in step with the commit's files: removed ones leave the order (their ranks stay
	 * with their dead rows), added ones take a rank between their neighbours'. Many added at once - a scan's batch, a
	 * load - are sorted and merged, and everything is renumbered once.
	 */
	void UpdateFileOrder ()
	{
		std::vector<uint32>& order = W.FileOrder;
		const auto isDead = [&] (uint32 Row) { return T.Files.Folder[Row] == InvalidRow; };
		if (bFilesRemoved)
		{
			std::erase_if(order, isDead);
		}
		std::erase_if(NewFiles, isDead);
		if (NewFiles.empty())
		{
			return;
		}
		const auto less = [&] (uint32 A, uint32 B) { return CompareFileOrder(T, A, B) < 0; };
		std::sort(NewFiles.begin(), NewFiles.end(), less);
		if (NewFiles.size() * 8u > order.size())
		{
			std::vector<uint32> merged(order.size() + NewFiles.size());
			std::merge(order.begin(), order.end(), NewFiles.begin(), NewFiles.end(), merged.begin(), less);
			order.swap(merged);
			RenumberFiles();
			NewFiles.clear();
			return;
		}
		for (const uint32 row : NewFiles)
		{
			const auto at = std::lower_bound(order.begin(), order.end(), row, less);
			const size_t position = static_cast<size_t>(at - order.begin());
			const uint64 low = position > 0u ? T.Files.PathRank[order[position - 1u]] : 0u;
			const uint64 high = position < order.size() ? T.Files.PathRank[order[position]] : RankLimit;
			order.insert(at, row);
			if (high - low >= 2u)
			{
				T.Files.PathRank.Set(row, static_cast<uint32>(low + (high - low) / 2u), Epoch);
			}
			else
			{
				RenumberFiles();
			}
		}
		NewFiles.clear();
	}

	// ---------------- Edges and targets ----------------

	// SEdgeRow::FromSub for a declared FromSub SubId (Positions: the record's (SubId, position) pairs, sorted): 0 for
	// the record, InvalidRow for a SubId the record does not declare.
	static uint32 FindSubPosition (const std::vector<std::pair<uint64, uint32>>& Positions, uint64 SubId) noexcept
	{
		if (SubId == 0u)
		{
			return 0u;
		}
		const auto found = std::lower_bound(Positions.begin(), Positions.end(), std::pair<uint64, uint32>(SubId, 0u));
		return found != Positions.end() && found->first == SubId ? found->second : InvalidRow;
	}

	// A target by id when Target is valid, else by the path TargetPath (a Guid target keeps TargetPath as
	// its hint).
	uint32 FindTarget (const SAssetId& Target, FStringView TargetPath) const
	{
		uint32 found = InvalidRow;
		if (Target.IsValid())
		{
			const uint64 hash = HashGuid(Target.Guid) ^ MixRegistryKey(Target.SubId);
			T.Graph.TargetsByKey.ForEach(hash, [&] (uint32 Row)
			{
				const STargetRow& target = T.Graph.Targets[Row];
				if (target.Kind == ETargetKind::Guid && target.Guid == Target.Guid && target.TargetSub == Target.SubId)
				{
					found = Row;
					return false;
				}
				return true;
			});
			return found;
		}
		T.Graph.TargetsByKey.ForEach(HashExact(TargetPath), [&] (uint32 Row)
		{
			const STargetRow& target = T.Graph.Targets[Row];
			if (target.Kind == ETargetKind::Path && T.Paths.Get(target.Text) == TargetPath)
			{
				found = Row;
				return false;
			}
			return true;
		});
		return found;
	}

	// Whether a path target names something a target can resolve to: a mount path below a mount's root.
	static bool IsTargetPath (FStringView TargetPath) noexcept
	{
		FStringView mountName;
		FStringView relative;
		return SplitMountPath(TargetPath, mountName, relative) && !relative.IsEmpty();
	}

	uint32 GetOrCreateTarget (const SAssetId& Id, FStringView TargetPath, FStringView Path)
	{
		if (!Id.IsValid() && !IsTargetPath(TargetPath))
		{
			Violation(Path, FString::Format("edge target '{}' is neither an id nor a mount path: dropped", TargetPath));
			return InvalidRow;
		}
		const uint32 existing = FindTarget(Id, TargetPath);
		if (existing != InvalidRow)
		{
			return existing;
		}
		SGraphTables& graph = T.Graph;
		STargetRow target;
		if (Id.IsValid())
		{
			target.Kind = ETargetKind::Guid;
			target.Guid = Id.Guid;
			target.TargetSub = Id.SubId;
			target.Text = TargetPath.IsEmpty() ? InvalidRow : T.Paths.Add(TargetPath, Epoch);
		}
		else
		{
			target.Kind = ETargetKind::Path;
			// Most references spell a file's path exactly: share its PATHS entry.
			const uint32 file = FindFileRowExact(T, TargetPath);
			target.Text = file != InvalidRow ? T.Files.Path[file] : T.Paths.Add(TargetPath, Epoch);
		}
		const uint32 row = graph.Targets.Append(target, Epoch);
		graph.OverflowReverseHead.Append(InvalidRow, Epoch);
		if (target.Kind == ETargetKind::Guid)
		{
			graph.TargetsByKey.Insert(HashGuid(target.Guid) ^ MixRegistryKey(target.TargetSub), row, Epoch);
			W.GuidTargets.Insert(HashGuid(target.Guid), row, WriterEpoch);
		}
		else
		{
			const FStringView text = TargetPath;
			graph.TargetsByKey.Insert(HashExact(text), row, Epoch);
			W.PathTargetCandidates.Insert(HashPortable(text), row, WriterEpoch);
			ForEachLabelSplit(text, [&] (uint32 Split)
			{
				W.PathTargetCandidates.Insert(HashPortable(text.Left(Split)), row, WriterEpoch);
				return true;
			});
		}
		TargetsToResolve.insert(row);
		return row;
	}

	// Masks a record's base slice and drops its overflow list: every edge it had becomes garbage.
	void DropEdges (uint32 Row)
	{
		SAssetsTable& assets = T.Assets;
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
	}

	// A record's edges: kept where they are when the list is the same as its live edges (a re-upsert of an
	// unchanged file leaves no garbage and no reverse entry), else the old ones are dropped and the new ones
	// appended to an overflow list.
	void WriteEdges (uint32 Row, const std::vector<SEdgeDecl>& Decls, FStringView Path)
	{
		SAssetsTable& assets = T.Assets;
		SGraphTables& graph = T.Graph;
		// The record's sub-entries by SubId: an edge names its sub-entry by position (SEdgeRow::FromSub).
		std::vector<std::pair<uint64, uint32>> subPositions;
		subPositions.reserve(assets.NumSubs[Row]);
		for (uint32 i = 0u; i < assets.NumSubs[Row]; ++i)
		{
			subPositions.emplace_back(T.SubEntries.SubId[assets.FirstSub[Row] + i], i + 1u);
		}
		std::sort(subPositions.begin(), subPositions.end());
		std::vector<SEdgeRow> edges;
		edges.reserve(Decls.size());
		CEdgeDeduplicator unique;
		for (const SEdgeDecl& decl : Decls)
		{
			const uint32 fromSub = FindSubPosition(subPositions, decl.FromSub);
			if (fromSub == InvalidRow)
			{
				Violation(Path, FString::Format("edge to '{}' comes from SubId {:016x}, which the record does not declare: dropped",
					decl.TargetPath, decl.FromSub));
				continue;
			}
			const uint32 target = GetOrCreateTarget(decl.Target, decl.TargetPath, Path);
			if (target == InvalidRow)
			{
				continue;
			}
			SEdgeRow edge;
			edge.FromSub = fromSub;
			edge.Target = target;
			edge.FieldPath = decl.FieldPath.IsEmpty() ? InvalidRow : T.Names.Intern(decl.FieldPath, Epoch);
			edge.Flags = decl.Flags & ~EDepFlags::Unresolved;
			if (unique.Add(edge, edges))
			{
				edges.push_back(edge);
			}
		}
		bool bSame = true;
		uint32 liveCount = 0u;
		ForEachLiveEdge(T, Row, [&] (uint32 EdgeRef)
		{
			bSame = bSame && liveCount < edges.size() && GetEdgeRow(T, EdgeRef) == edges[liveCount];
			++liveCount;
		});
		if (bSame && liveCount == edges.size())
		{
			return;
		}
		DropEdges(Row);
		const uint32 begin = graph.OverflowEdges.Size();
		for (const SEdgeRow& edge : edges)
		{
			const uint32 index = graph.OverflowEdges.Append(edge, Epoch);
			SOverflowReverseEdge reverse;
			reverse.Source = Row;
			reverse.Edge = OverflowEdgeBit | index;
			reverse.Next = graph.OverflowReverseHead[edge.Target];
			graph.OverflowReverseHead.Set(edge.Target, graph.OverflowReverseEdges.Append(reverse, Epoch), Epoch);
		}
		assets.OverflowEdges.SetIfChanged(Row, SPoolRange { edges.empty() ? 0u : begin, static_cast<uint32>(edges.size()) }, Epoch);
		graph.LiveEdges += static_cast<uint32>(edges.size());
	}

	// WholeHash: HashPortable of a Path target's whole text when the caller has it, else 0.
	STargetRow ResolveTarget (const STargetRow& Current, uint64 WholeHash = 0u) const
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
		bool bDone = false;
		const auto trySplit = [&] (uint32 Split)
		{
			const FStringView filePart = text.Left(Split);
			uint32 first = InvalidRow;
			const uint64 hash = Split == text.Len() && WholeHash != 0u ? WholeHash : HashPortable(filePart);
			const uint32 matches = FindRecordRowsPortableHashed(T, filePart, hash, first);
			if (matches == 0u)
			{
				return true;
			}
			result.FilePartLength = Split;
			bDone = true;
			if (matches > 1u)
			{
				result.State = ETargetState::Ambiguous;
				return false;
			}
			result.State = ETargetState::Resolved;
			result.Resolved = first;
			result.TargetSub = Split < text.Len() ? MakeSubId(text.RightChop(Split + 1u)) : 0u;
			result.bSpellingDiffers = T.Paths.Get(T.Files.Path[T.Assets.File[first]]) != filePart;
			return false;
		};
		if (trySplit(text.Len()))
		{
			ForEachLabelSplit(text, trySplit);
		}
		if (bDone)
		{
			return result;
		}
		result.TargetSub = 0u;
		if (IsCompanionFile(text))
		{
			result.State = ETargetState::Companion;
		}
		return result;
	}

	// Whether a live FILES row of that path (portable fold) is a companion: an edge to it needs the file, which
	// has no record of its own (§5.2).
	bool IsCompanionFile (FStringView Path) const
	{
		bool bCompanion = false;
		T.Indices.FilesByPortablePath.ForEach(HashPortable(Path), [&] (uint32 Row)
		{
			bCompanion = T.Files.Folder[Row] != InvalidRow && T.Files.Class[Row] == EFileClass::Companion
				&& EqualPortable(T.Paths.Get(T.Files.Path[Row]), Path);
			return !bCompanion;
		});
		return bCompanion;
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

	// ---------------- Bulk adoption of an image by an empty store ----------------

public:
	/**
	 * AdoptImage's fast path: an image adopted by an empty store fills the tables straight from its vectors instead
	 * of a delta built from the image and replayed item by item. Everything the image names by index is resolved
	 * through dense tables over its indices, never by string: names interned once (InternAll), file paths written
	 * into PATHS from their folder's path and their leaf, each distinct target path made a target once (the image
	 * deduplicates its strings), companions found by string index; edges go straight into the base CSR (built once,
	 * as a compaction does) and every target is resolved once at the end, with Registry problems computed only for
	 * the records whose edges carry REF006 or PATH001. The result is the replay's (the tests compare dumps and
	 * answers). The checks the replay makes as it applies items are made here as each item is written; an item the
	 * replay would not apply as it is - a bad index, a malformed path, a tag key the type lacks or of another type,
	 * a repeated SubId, an edge from an undeclared sub-entry, two records or two rows for one file - makes it return
	 * false with only this empty store written (the image unchanged): the caller empties the store and replays the
	 * image, which reports what it skips.
	 */
	bool AdoptIntoFreshStore (SRegistryImage& Image)
	{
		bBulk = true;
		const auto nameAt = [&] (uint32 Index) { return Index < Image.Names.size() ? Image.Names[Index].View() : FStringView(); };
		const auto textAt = [&] (uint32 Index) { return Index < Image.Strings.size() ? Image.Strings[Index].View() : FStringView(); };

		// Mounts: distinct names and ids, each name a path segment.
		for (uint32 i = 0u; i < Image.Mounts.size(); ++i)
		{
			const SImageMount& mount = Image.Mounts[i];
			if (!IsPathSegment(mount.Name) || mount.Id == InvalidMountId)
			{
				return false;
			}
			for (uint32 j = 0u; j < i; ++j)
			{
				if (Image.Mounts[j].Name == mount.Name || Image.Mounts[j].Id == mount.Id)
				{
					return false;
				}
			}
		}
		for (const SImageMount& mount : Image.Mounts)
		{
			UpsertMount(SMountUpsert { mount.Name, mount.bCaseSensitive, mount.Id });
		}

		std::vector<uint32> nameIds;
		T.Names.InternAll(Image.Names, nameIds, Epoch);
		const uint32 emptyName = T.Names.Intern(FStringView(), Epoch);
		const auto nameIdOf = [&] (uint32 Index) { return Index < nameIds.size() ? nameIds[Index] : emptyName; };

		// Folders, parents first: a root is its mount's root row.
		std::vector<FString> folderPaths;
		std::vector<uint32> folderOrder;
		std::vector<uint16> folderMounts;   // image mount index of each folder's path
		if (!PlanFolders(Image, folderPaths, folderOrder, folderMounts))
		{
			return false;
		}
		const uint32 folderCount = static_cast<uint32>(Image.Folders.size());
		std::vector<uint32> folderRows(folderCount, InvalidRow);
		for (const uint32 folder : folderOrder)
		{
			const SImageFolder& image = Image.Folders[folder];
			const SMountRow& mount = (*T.Mounts)[Image.Mounts[folderMounts[folder]].Id];
			if (image.Parent == ~0u)
			{
				folderRows[folder] = mount.RootFolder;
				continue;
			}
			if (FindFolderRowExact(T, folderPaths[folder]) != InvalidRow)
			{
				return false;   // two image folders of one path: the replay makes them one
			}
			folderRows[folder] = CreateFolderRow(folderRows[image.Parent], folderPaths[folder], mount.Id, nameAt(image.Name));
		}

		// Files, each path written into PATHS once, from its folder's path and its leaf.
		const uint32 fileCount = static_cast<uint32>(Image.Files.size());
		const uint32 recordCount = static_cast<uint32>(Image.Records.size());
		T.Indices.FilesByPath.Reserve(fileCount, Epoch);
		T.Indices.FilesByPortablePath.Reserve(fileCount, Epoch);
		T.Indices.RecordsByPortablePath.Reserve(recordCount, Epoch);
		T.Indices.RecordsByGuid.Reserve(recordCount, Epoch);
		T.Indices.RecordsById64.Reserve(recordCount + static_cast<uint32>(Image.SubEntries.size()), Epoch);
		T.Names.Reserve(recordCount, Epoch);   // the records' stems
		std::vector<uint32> fileRows(fileCount, InvalidRow);
		std::vector<uint64> portableHashes(fileCount, 0u);
		for (uint32 i = 0u; i < fileCount; ++i)
		{
			const SImageFile& image = Image.Files[i];
			const FStringView leaf = nameAt(image.LeafName);
			if (image.Folder >= folderCount || !IsPathSegment(leaf))
			{
				return false;
			}
			const SMountRow& mount = (*T.Mounts)[Image.Mounts[folderMounts[image.Folder]].Id];
			SNewFile file;
			file.PathId = T.Paths.AddJoined(folderPaths[image.Folder], leaf, Epoch);
			const FStringView path = T.Paths.Get(file.PathId);
			file.MountRuleHash = HashMountRule(path, mount.bCaseSensitive);
			bool bRepeated = false;
			T.Indices.FilesByPath.ForEach(file.MountRuleHash, [&] (uint32 Row)
			{
				bRepeated = T.Paths.Get(T.Files.Path[Row]) == path;
				return !bRepeated;
			});
			if (bRepeated)
			{
				return false;   // two rows of one path: the replay makes them one
			}
			file.LeafName = nameIdOf(image.LeafName);
			file.Size = image.Size;
			file.MTime = image.MTime;
			file.Identity = image.Identity;
			file.ContentHash = image.ContentHash;
			file.MetaHash = image.MetaHash;
			file.Class = image.Class;
			file.MetaState = image.MetaState;
			fileRows[i] = CreateFileRow(folderRows[image.Folder], path, mount, file);
			portableHashes[i] = file.PortableHash;
		}
		// The file order: an image written from a store lists its files in it (ExportImage), so checking each
		// neighbour is all it takes; any other image is sorted.
		{
			W.FileOrder = fileRows;
			const auto less = [&] (uint32 A, uint32 B) { return CompareFileOrder(T, A, B) < 0; };
			if (!std::is_sorted(W.FileOrder.begin(), W.FileOrder.end(), less))
			{
				std::sort(W.FileOrder.begin(), W.FileOrder.end(), less);
			}
			RenumberFiles();
			NewFiles.clear();
		}

		// Types by exact dotted name, as MakeImageDelta resolves them; each type table sized for its rows at once.
		std::vector<int32> typeOf(Image.TypeNames.size(), -1);
		for (uint32 i = 0u; i < Image.TypeNames.size(); ++i)
		{
			if (const std::optional<reflect::TypeId> type = CAssetTypeRegistry::FindExact(Image.TypeNames[i]))
			{
				typeOf[i] = *type;
			}
		}
		const auto typeIdOf = [&] (uint16 Index) -> reflect::TypeId
		{
			return Index < typeOf.size() && typeOf[Index] >= 0 ? static_cast<reflect::TypeId>(typeOf[Index]) : asset_types::Unknown;
		};
		{
			std::vector<uint32> rowsPerType(T.Types.size(), 0u);
			for (const SImageRecord& record : Image.Records)
			{
				++rowsPerType[typeIdOf(record.Type)];
			}
			for (const SImageSubEntry& sub : Image.SubEntries)
			{
				++rowsPerType[typeIdOf(sub.Type)];
			}
			GrowTypeTables(rowsPerType);
		}

		// Records, their sub-entries, tags, edges and companions.
		SAssetsTable& assets = T.Assets;
		SSubEntryTable& subs = T.SubEntries;
		GrowAssetRows(recordCount);
		GrowSubEntryRows(static_cast<uint32>(Image.SubEntries.size()));
		std::vector<uint8> hasRecord(fileCount, 0u);
		std::vector<uint16> importerIds(Image.ImporterNames.size(), InvalidImporter);
		std::vector<uint32> pathTargets(Image.Strings.size(), InvalidRow);       // image string -> its path target
		std::vector<uint32> companionPaths(Image.Strings.size(), InvalidRow);    // image string -> its PATHS id
		std::vector<SEdgeRow> edges;
		edges.reserve(Image.Edges.size());
		std::vector<SPoolRange> edgeRanges;
		edgeRanges.reserve(recordCount);
		std::vector<uint32> labels;
		std::vector<uint32> companions;
		std::vector<uint64> subIds;
		std::vector<std::pair<uint64, uint32>> subPositions;
		CEdgeDeduplicator unique;
		SImageKeys keys(T.Types.size());
		for (uint32 r = 0u; r < recordCount; ++r)
		{
			const SImageRecord& image = Image.Records[r];
			if (image.File >= fileCount || hasRecord[image.File] != 0u || image.Type >= Image.TypeNames.size())
			{
				return false;
			}
			hasRecord[image.File] = 1u;
			const uint32 file = fileRows[image.File];
			const FStringView path = T.Paths.Get(T.Files.Path[file]);
			const uint32 row = r;
			++T.LiveRecords;
			T.Indices.RecordsByPortablePath.Insert(portableHashes[image.File], row, Epoch);
			const SGuid guid = image.Guid.IsValid() ? image.Guid : MakeProvisionalGuid(path);
			const uint64 id64 = MakeAssetId64(SAssetId { guid, 0u }).Value;
			assets.Guid.Set(row, guid, Epoch);
			assets.AssetId64.Set(row, id64, Epoch);
			assets.File.Set(row, file, Epoch);
			T.Files.Record.Set(file, row, Epoch);
			T.Indices.RecordsByGuid.Insert(HashGuid(guid), row, Epoch);
			T.Indices.RecordsById64.Insert(MixRegistryKey(id64), row, Epoch);

			const reflect::TypeId type = typeIdOf(image.Type);
			const bool bTyped = typeOf[image.Type] >= 0;
			ERecordFlags flags = image.Flags & ~ERecordFlags::HasProblems;
			if (!bTyped)
			{
				flags |= ERecordFlags::Schemaless | ERecordFlags::StaleTags;
			}
			const SPoolRange subRange = ClampRange(image.SubEntries, Image.SubEntries.size());
			for (uint32 i = subRange.Begin; i < subRange.Begin + subRange.Count; ++i)
			{
				if (Image.SubEntries[i].Type >= Image.TypeNames.size())
				{
					return false;
				}
				if (typeOf[Image.SubEntries[i].Type] < 0)
				{
					flags |= ERecordFlags::StaleTags;
				}
			}
			uint16 importer = InvalidImporter;
			if (image.Importer < importerIds.size())
			{
				if (importerIds[image.Importer] == InvalidImporter)
				{
					importerIds[image.Importer] = InternImporter(Image.ImporterNames[image.Importer]);
				}
				importer = importerIds[image.Importer];
			}
			assets.Type.Set(row, type, Epoch);
			assets.TypeRow.Set(row, TakeTypeRow(type, row), Epoch);
			assets.AssetType.Set(row, type, Epoch);
			assets.Importer.Set(row, importer, Epoch);
			assets.Flags.Set(row, flags, Epoch);
			assets.CookRule.Set(row, image.CookRule, Epoch);
			assets.Chunk.Set(row, image.Chunk, Epoch);
			const FStringView primaryName = nameAt(image.PrimaryName);
			assets.PrimaryName.Set(row, primaryName.IsEmpty() ? InvalidRow : nameIds[image.PrimaryName], Epoch);
			assets.ImportStatus.Set(row, image.ImportStatus, Epoch);
			assets.AnalyzeStamp.Set(row, image.AnalyzeStamp, Epoch);
			assets.Name.Set(row, T.Names.Intern(GetAssetStem(GetMountPathLeaf(path)), Epoch), Epoch);
			assets.ChangedRevision.Set(row, Revision, Epoch);

			labels.clear();
			const SPoolRange labelRange = ClampRange(image.Labels, Image.Labels.size());
			for (uint32 i = labelRange.Begin; i < labelRange.Begin + labelRange.Count; ++i)
			{
				if ((Image.Labels[i] & RuleLabelBit) != 0u)
				{
					labels.push_back(nameIdOf(Image.Labels[i] & ~RuleLabelBit) | RuleLabelBit);
				}
			}
			if (!labels.empty())
			{
				assets.Labels.Set(row, SPoolRange { T.Labels.Append(labels, Epoch), static_cast<uint32>(labels.size()) }, Epoch);
			}
			if (bTyped && !WriteImageTags(type, assets.TypeRow[row], Image, image.Tags, nameIdOf, keys))
			{
				return false;
			}

			// Sub-entries, each SubId once.
			subIds.clear();
			const uint32 firstSub = T.LiveSubEntries;
			for (uint32 i = subRange.Begin; i < subRange.Begin + subRange.Count; ++i)
			{
				const SImageSubEntry& entry = Image.SubEntries[i];
				const reflect::TypeId subType = typeIdOf(entry.Type);
				const bool bSubTyped = typeOf[entry.Type] >= 0;
				ERecordFlags subFlags = entry.Flags & ~ERecordFlags::HasProblems;
				if (!bSubTyped)
				{
					subFlags |= ERecordFlags::Schemaless | ERecordFlags::StaleTags;
				}
				const uint64 subId = entry.SubId != 0u ? entry.SubId : MakeSubId(nameAt(entry.Label));
				subIds.push_back(subId);
				const uint32 sub = T.LiveSubEntries++;
				subs.Owner.Set(sub, row, Epoch);
				subs.SubId.Set(sub, subId, Epoch);
				subs.Key.Set(sub, nameIdOf(entry.Label), Epoch);
				subs.Type.Set(sub, subType, Epoch);
				subs.Flags.Set(sub, subFlags, Epoch);
				subs.TypeRow.Set(sub, TakeTypeRow(subType, SubEntryBit | sub), Epoch);
				const uint64 subId64 = MakeAssetId64(SAssetId { guid, subId }).Value;
				subs.AssetId64.Set(sub, subId64, Epoch);
				T.Indices.RecordsById64.Insert(MixRegistryKey(subId64), SubEntryBit | sub, Epoch);
				if (bSubTyped && !WriteImageTags(subType, subs.TypeRow[sub], Image, entry.Tags, nameIdOf, keys))
				{
					return false;
				}
			}
			subPositions.clear();
			for (uint32 i = 0u; i < subIds.size(); ++i)
			{
				subPositions.emplace_back(subIds[i], i + 1u);
			}
			std::sort(subPositions.begin(), subPositions.end());
			if (std::adjacent_find(subPositions.begin(), subPositions.end(), [] (const auto& A, const auto& B) { return A.first == B.first; })
				!= subPositions.end())
			{
				return false;   // a repeated SubId: the replay drops the second one
			}
			assets.FirstSub.Set(row, subRange.Count > 0u ? firstSub : 0u, Epoch);
			assets.NumSubs.Set(row, subRange.Count, Epoch);
			RankSubEntries(row);

			// Edges straight into the base CSR, one per distinct (FromSub, target, flags).
			const uint32 edgeBegin = static_cast<uint32>(edges.size());
			unique.Reset();
			const SPoolRange edgeRange = ClampRange(image.Edges, Image.Edges.size());
			for (uint32 i = edgeRange.Begin; i < edgeRange.Begin + edgeRange.Count; ++i)
			{
				const SImageEdge& edge = Image.Edges[i];
				SEdgeRow edgeRow;
				edgeRow.FromSub = FindSubPosition(subPositions, edge.FromSub);
				if (edgeRow.FromSub == InvalidRow)
				{
					return false;
				}
				if (edge.Target.IsValid())
				{
					edgeRow.Target = GetOrCreateTarget(edge.Target, textAt(edge.TargetPath), path);
				}
				else
				{
					const FStringView targetPath = textAt(edge.TargetPath);
					if (edge.TargetPath == ~0u || !IsTargetPath(targetPath))
					{
						return false;
					}
					uint32& target = pathTargets[edge.TargetPath];
					if (target == InvalidRow)
					{
						target = CreatePathTarget(targetPath);
					}
					edgeRow.Target = target;
				}
				const FStringView fieldPath = nameAt(edge.FieldPath);
				edgeRow.FieldPath = fieldPath.IsEmpty() ? InvalidRow : nameIds[edge.FieldPath];
				edgeRow.Flags = edge.Flags & ~EDepFlags::Unresolved;
				const std::span<const SEdgeRow> kept(edges.data() + edgeBegin, edges.size() - edgeBegin);
				if (unique.Add(edgeRow, kept))
				{
					edges.push_back(edgeRow);
				}
			}
			edgeRanges.push_back(SPoolRange { edgeBegin, static_cast<uint32>(edges.size()) - edgeBegin });

			// Companions, each path interned once.
			companions.clear();
			const SPoolRange companionRange = ClampRange(image.Companions, Image.Companions.size());
			for (uint32 i = companionRange.Begin; i < companionRange.Begin + companionRange.Count; ++i)
			{
				const uint32 text = Image.Companions[i];
				const FStringView companion = textAt(text);
				if (!IsWellFormedMountPath(companion) || !HasRelativePart(companion))
				{
					return false;
				}
				uint32& pathId = companionPaths[text];
				if (pathId == InvalidRow)
				{
					pathId = GetCompanionPathId(companion);
				}
				companions.push_back(pathId);
				AddCompanionRef(companion, pathId);
			}
			if (!companions.empty())
			{
				assets.Companions.Set(row, SPoolRange { T.Companions.Append(companions, Epoch), static_cast<uint32>(companions.size()) }, Epoch);
			}
		}
		Store.InstallGraphTables(edges, edgeRanges);

		// Rule files and problems as the replay applies them (they are few, and may report violations).
		for (const SImageRuleFile& ruleFile : Image.RuleFiles)
		{
			if (ruleFile.Mount >= Image.Mounts.size())
			{
				continue;
			}
			SRuleFileUpsert upsert;
			upsert.MountName = Image.Mounts[ruleFile.Mount].Name;
			upsert.RulesHash = ruleFile.RulesHash;
			if (ruleFile.Folder != ~0u)
			{
				if (ruleFile.Folder >= folderPaths.size() || folderPaths[ruleFile.Folder].IsEmpty())
				{
					continue;
				}
				upsert.FolderMountPath = folderPaths[ruleFile.Folder];
			}
			UpsertRuleFile(upsert);
		}
		for (SImageProblems& problems : Image.Problems)
		{
			if (problems.Subject < Image.Strings.size())
			{
				SetProblems(Image.Strings[problems.Subject], problems.Source, std::move(problems.Diagnostics));
			}
		}
		FinishAdoption();
		return true;
	}

private:
	// The image's tag keys per type: (image name index, key index) pairs, searched linearly (a type has a few keys;
	// a snapshot's key names are the same few indices throughout).
	struct SImageKeys
	{
		explicit SImageKeys (size_t TypeCount)
			: ByType(TypeCount)
		{}

		std::vector<std::vector<std::pair<uint32, int32>>> ByType;
	};

	static SPoolRange ClampRange (SPoolRange Range, size_t Size) noexcept
	{
		const uint64 begin = (std::min)(static_cast<uint64>(Range.Begin), static_cast<uint64>(Size));
		const uint64 end = (std::min)(static_cast<uint64>(Range.Begin) + Range.Count, static_cast<uint64>(Size));
		return SPoolRange { static_cast<uint32>(begin), static_cast<uint32>(end - begin) };
	}

	static bool IsPathSegment (FStringView Name) noexcept
	{
		return !Name.IsEmpty() && !Name.Contains('/') && Name != "." && Name != "..";
	}

	// Folder paths, iteratively (up a parent chain to a resolved folder or a root, then down), each folder after its
	// parent in OutOrder; OutMounts: the image mount of each folder's root. False for a parent out of range, a cycle,
	// a root that does not name its mount or a name that is no path segment.
	static bool PlanFolders (const SRegistryImage& Image, std::vector<FString>& OutPaths, std::vector<uint32>& OutOrder, std::vector<uint16>& OutMounts)
	{
		const auto nameAt = [&] (uint32 Index) { return Index < Image.Names.size() ? Image.Names[Index].View() : FStringView(); };
		const uint32 folderCount = static_cast<uint32>(Image.Folders.size());
		OutPaths.assign(folderCount, FString());
		OutMounts.assign(folderCount, uint16(0u));
		OutOrder.reserve(folderCount);
		std::vector<uint8> state(folderCount, 0u);   // 0 unknown, 1 on the current chain, 2 resolved
		std::vector<uint32> chain;
		for (uint32 i = 0u; i < folderCount; ++i)
		{
			chain.clear();
			uint32 current = i;
			while (current < folderCount && state[current] == 0u)
			{
				state[current] = 1u;
				chain.push_back(current);
				if (Image.Folders[current].Parent == ~0u)
				{
					break;
				}
				current = Image.Folders[current].Parent;
			}
			if (chain.empty())
			{
				continue;   // resolved before
			}
			const bool bRootOnTop = Image.Folders[chain.back()].Parent == ~0u;
			if (!bRootOnTop && (current >= folderCount || state[current] != 2u))
			{
				return false;   // a parent out of range, or a cycle
			}
			FStringView base = bRootOnTop ? FStringView() : OutPaths[current].View();
			uint16 mount = bRootOnTop ? uint16(0u) : OutMounts[current];
			for (auto it = chain.rbegin(); it != chain.rend(); ++it)
			{
				const SImageFolder& folder = Image.Folders[*it];
				const FStringView name = nameAt(folder.Name);
				if (folder.Parent == ~0u)
				{
					if (folder.Mount >= Image.Mounts.size() || Image.Mounts[folder.Mount].Name.View() != name)
					{
						return false;   // a root that does not name its mount
					}
					OutPaths[*it] = JoinMountPath(name, FStringView());
					mount = folder.Mount;
				}
				else
				{
					if (!IsPathSegment(name))
					{
						return false;
					}
					OutPaths[*it] = MakeChildMountPath(base, name);
				}
				OutMounts[*it] = mount;
				state[*it] = 2u;
				base = OutPaths[*it];
				OutOrder.push_back(*it);
			}
		}
		return true;
	}

	// Sizes each type table for RowsPerType more rows at once; TakeTypeRow hands them out in order.
	void GrowTypeTables (const std::vector<uint32>& RowsPerType)
	{
		TypeCursor.assign(T.Types.size(), 0u);
		for (uint32 type = 0u; type < T.Types.size(); ++type)
		{
			CTypeTable& table = T.Types[type];
			TypeCursor[type] = table.Records.Size();
			if (RowsPerType[type] == 0u)
			{
				continue;
			}
			const uint32 size = table.Records.Size() + RowsPerType[type];
			table.Records.Grow(size, Epoch);
			table.Dead.Grow(size, Epoch);
			for (uint32 key = 0u; key < table.Columns.size(); ++key)
			{
				STagColumn& column = table.Columns[key];
				column.Values.Grow(size, Epoch);
				column.PresentBits.Grow(size, Epoch);
				if (HasAnyFlags(table.Keys[key].Flags, ETagFlags::MultiValue))
				{
					column.MultiRanges.Grow(size, Epoch);
				}
			}
		}
	}

	uint32 TakeTypeRow (reflect::TypeId Type, uint32 RecordValue)
	{
		CTypeTable& table = T.Types[Type];
		const uint32 row = TypeCursor[Type]++;
		table.Records.Set(row, RecordValue, Epoch);
		++table.LiveRows;
		return row;
	}

	// Count more ASSETS rows with AppendAssetRow's values, written in place by the bulk path.
	void GrowAssetRows (uint32 Count)
	{
		SAssetsTable& assets = T.Assets;
		const uint32 size = assets.Guid.Size() + Count;
		assets.Guid.Grow(size, Epoch);
		assets.AssetId64.Grow(size, Epoch);
		assets.File.Grow(size, Epoch);
		assets.Type.Grow(size, Epoch);
		assets.AssetType.Grow(size, Epoch);
		assets.Importer.Grow(size, Epoch);
		assets.Flags.Grow(size, Epoch);
		assets.CookRule.Grow(size, Epoch);
		assets.Chunk.Grow(size, Epoch);
		assets.PrimaryName.Grow(size, Epoch);
		assets.Labels.Grow(size, Epoch);
		assets.FirstSub.Grow(size, Epoch);
		assets.NumSubs.Grow(size, Epoch);
		assets.TypeRow.Grow(size, Epoch);
		assets.ImportStatus.Grow(size, Epoch);
		assets.Problems.Grow(size, Epoch);
		assets.AnalyzeStamp.Grow(size, Epoch);
		assets.Name.Grow(size, Epoch);
		assets.ChangedRevision.Grow(size, Epoch);
		assets.Companions.Grow(size, Epoch);
		assets.BaseEdges.Grow(size, Epoch);
		assets.BaseEdgesMasked.Grow(size, Epoch);
		assets.OverflowEdges.Grow(size, Epoch);
	}

	void GrowSubEntryRows (uint32 Count)
	{
		SSubEntryTable& subs = T.SubEntries;
		const uint32 size = subs.Owner.Size() + Count;
		subs.Owner.Grow(size, Epoch);
		subs.SubId.Grow(size, Epoch);
		subs.Key.Grow(size, Epoch);
		subs.Type.Grow(size, Epoch);
		subs.Flags.Grow(size, Epoch);
		subs.TypeRow.Grow(size, Epoch);
		subs.AssetId64.Grow(size, Epoch);
		subs.LabelRank.Grow(size, Epoch);
	}

	// The path target of an image string met for the first time: new, unless another string of the image spells the
	// same path (an image need not deduplicate its strings).
	uint32 CreatePathTarget (FStringView TargetPath)
	{
		SGraphTables& graph = T.Graph;
		const uint64 exact = HashExact(TargetPath);
		uint32 existing = InvalidRow;
		graph.TargetsByKey.ForEach(exact, [&] (uint32 Row)
		{
			const STargetRow& other = graph.Targets[Row];
			if (other.Kind == ETargetKind::Path && T.Paths.Get(other.Text) == TargetPath)
			{
				existing = Row;
				return false;
			}
			return true;
		});
		if (existing != InvalidRow)
		{
			return existing;
		}
		STargetRow target;
		target.Kind = ETargetKind::Path;
		const uint32 file = FindFileRowExact(T, TargetPath);   // most references spell a file's path exactly
		target.Text = file != InvalidRow ? T.Files.Path[file] : T.Paths.Add(TargetPath, Epoch);
		const uint32 row = graph.Targets.Append(target, Epoch);
		graph.OverflowReverseHead.Append(InvalidRow, Epoch);
		graph.TargetsByKey.Insert(exact, row, Epoch);
		const uint64 portable = HashPortable(TargetPath);
		W.PathTargetCandidates.Insert(portable, row, WriterEpoch);
		ForEachLabelSplit(TargetPath, [&] (uint32 Split)
		{
			W.PathTargetCandidates.Insert(HashPortable(TargetPath.Left(Split)), row, WriterEpoch);
			return true;
		});
		TargetPortableHashes.resize(row + 1u, 0u);
		TargetPortableHashes[row] = portable;
		return row;
	}

	// The key index of an image tag key for Type, or -1.
	int32 FindImageKey (SImageKeys& Keys, const SRegistryImage& Image, reflect::TypeId Type, uint32 Name) const
	{
		std::vector<std::pair<uint32, int32>>& known = Keys.ByType[Type];
		for (const auto& [name, key] : known)
		{
			if (name == Name)
			{
				return key;
			}
		}
		int32 key = -1;
		if (Name < Image.Names.size())
		{
			const std::optional<uint32> found = CAssetTypeRegistry::FindKey(Type, Image.Names[Name]);
			key = found ? static_cast<int32>(*found) : -1;
		}
		known.emplace_back(Name, key);
		return key;
	}

	/**
	 * A fresh type row's tags from the image. False for a tag the replay would drop (a key the type lacks, a value
	 * of another type, a single-valued key repeated).
	 */
	template <typename TNameId>
	bool WriteImageTags (reflect::TypeId Type, uint32 Row, const SRegistryImage& Image, SPoolRange Range, TNameId&& NameIdOf, SImageKeys& Keys)
	{
		CTypeTable& table = T.Types[Type];
		const SPoolRange range = ClampRange(Range, Image.Tags.size());
		uint64 seen = 0u;   // single-valued keys met so far (a type has fewer than 64 keys)
		bool bMulti = false;
		for (uint32 i = range.Begin; i < range.Begin + range.Count; ++i)
		{
			const SImageTag& tag = Image.Tags[i];
			const int32 key = FindImageKey(Keys, Image, Type, tag.Key);
			if (key < 0 || key >= 64 || tag.Type != table.Keys[key].Type)
			{
				return false;
			}
			const STagKeyDesc& desc = table.Keys[key];
			if (HasAnyFlags(desc.Flags, ETagFlags::MultiValue))
			{
				bMulti = true;
				continue;
			}
			if ((seen & (uint64(1) << key)) != 0u)
			{
				return false;
			}
			seen |= uint64(1) << key;
			STagColumn& column = table.Columns[key];
			column.Values.Set(Row, EncodeImageTag(desc.Type, tag, Image, NameIdOf), Epoch);
			column.PresentBits.Set(Row, true, Epoch);
		}
		if (!bMulti)
		{
			return true;
		}
		// MultiValue keys: each key's values in tag order, as one list.
		std::vector<uint64> items;
		for (uint32 key = 0u; key < table.Keys.size(); ++key)
		{
			const STagKeyDesc& desc = table.Keys[key];
			if (!HasAnyFlags(desc.Flags, ETagFlags::MultiValue))
			{
				continue;
			}
			items.clear();
			for (uint32 i = range.Begin; i < range.Begin + range.Count; ++i)
			{
				const SImageTag& tag = Image.Tags[i];
				if (FindImageKey(Keys, Image, Type, tag.Key) == static_cast<int32>(key))
				{
					items.push_back(EncodeImageTag(desc.Type, tag, Image, NameIdOf));
				}
			}
			if (!items.empty())
			{
				STagColumn& column = table.Columns[key];
				const uint32 begin = column.Multi.Append(items, Epoch);
				column.MultiRanges.Set(Row, SPoolRange { begin, static_cast<uint32>(items.size()) }, Epoch);
				column.Values.Set(Row, items[0], Epoch);
				column.PresentBits.Set(Row, true, Epoch);
			}
		}
		return true;
	}

	template <typename TNameId>
	uint64 EncodeImageTag (ETagType Type, const SImageTag& Tag, const SRegistryImage& Image, TNameId&& NameIdOf)
	{
		if (Type == ETagType::Name)
		{
			return NameIdOf(Tag.Text);
		}
		if (Type == ETagType::String)
		{
			return T.Strings.Add(Tag.Text < Image.Strings.size() ? Image.Strings[Tag.Text].View() : FStringView(), Epoch);
		}
		return Tag.Bits;
	}

	// Finish for AdoptIntoFreshStore: every target resolved once (the CSR is built: the reverse entries name their
	// sources), then Registry problems for the records whose edges carry REF006 or PATH001, problem counts,
	// intervals.
	void FinishAdoption ()
	{
		const uint32 targetRows = T.Graph.Targets.Size();
		std::vector<uint32> sources;
		for (uint32 target = 0u; target < targetRows; ++target)
		{
			const STargetRow current = T.Graph.Targets[target];
			const uint64 portable = target < TargetPortableHashes.size() ? TargetPortableHashes[target] : 0u;
			const STargetRow resolved = current.Kind == ETargetKind::Path && portable != 0u ? ResolveTarget(current, portable) : ResolveTarget(current);
			if (resolved.Resolved != InvalidRow)
			{
				T.Graph.TargetsByResolved.Insert(MixRegistryKey(resolved.Resolved), target, Epoch);
			}
			T.Graph.Targets.Set(target, resolved, Epoch);
			if (resolved.State == ETargetState::Ambiguous || (resolved.State == ETargetState::Resolved && resolved.bSpellingDiffers))
			{
				ForEachLiveReverseEdge(T, target, [&] (const SReverseEdge& Entry) { sources.push_back(Entry.Source); });
			}
		}
		std::sort(sources.begin(), sources.end());
		sources.erase(std::unique(sources.begin(), sources.end()), sources.end());
		for (const uint32 source : sources)
		{
			RecomputeRegistryProblems(source);
		}
		for (auto& [subject, diagnostics] : PendingViolations)
		{
			SetProblems(subject, EProblemSource::Registry, std::move(diagnostics));
		}
		std::unordered_set<uint32> counted;
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
	}

	std::vector<uint32> NewFiles;                    // FILES rows this commit added (UpdateFileOrder ranks them)
	bool bFilesRemoved = false;                      // this commit removed a FILES row
	std::vector<std::pair<uint64, uint32>> SubOrder; // RankSubEntries' scratch
	bool bBulk = false;                              // AdoptIntoFreshStore: targets resolve at the end, all at once
	std::vector<uint32> TypeCursor;                  // per type: the next row GrowTypeTables made
	std::vector<uint64> TargetPortableHashes;        // per target row: HashPortable of its whole text (bulk)

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
			ForEachLabelSplit(Subject, [&] (uint32 Split)
			{
				file = FindFileRowExact(T, Subject.Left(Split));
				return file == InvalidRow;
			});
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
			W.PathTargetCandidates.ForEach(HashPortable(path), [&] (uint32 Target)
			{
				TargetsToResolve.insert(Target);
				return true;
			});
		}
		for (const uint64 guidHash : TouchedGuids)
		{
			W.GuidTargets.ForEach(guidHash, [&] (uint32 Target)
			{
				TargetsToResolve.insert(Target);
				return true;
			});
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
		UpdateFileOrder();

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
	InitTables();
	Publish();
}

CRegistryStore::~CRegistryStore () = default;

// The tables of an empty store: no mounts or importers, a type table per CAssetTypeRegistry type with its keys.
void CRegistryStore::InitTables ()
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
}

// Back to an empty store before anything was published from it (a bulk adoption that stopped part way).
void CRegistryStore::ResetToEmpty ()
{
	LiveView.Tables = SRegistryTables {};
	InitTables();
	*Writer = SWriterState {};
}

void CRegistryStore::CheckOwnerThread ()
{
	const uint64 thread = platform::GetCurrentThreadId();
	if (OwnerThread == 0u)
	{
		OwnerThread = thread;
	}
	frt_assert(OwnerThread == thread);
}

void CRegistryStore::ReleaseOwner () noexcept
{
	frt_assert(OwnerThread == 0u || OwnerThread == platform::GetCurrentThreadId());
	OwnerThread = 0u;
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
	std::vector<SRingEntry> changes = writer.TakeChanges();

	LastStats = SRegistryCommitStats {};
	LastStats.RecordsChanged = static_cast<uint32>(changes.size());
	SWriterState& state = *Writer;
	for (const SRingEntry& change : changes)
	{
		if (state.Ring.size() < ChangeRingCapacity)
		{
			if (state.Ring.size() == state.Ring.capacity())
			{
				// Doubling from 1024 ends at exactly ChangeRingCapacity (a power of two), never past it.
				state.Ring.reserve((std::min)((std::max)(state.Ring.capacity() * 2u, size_t(1024u)), size_t(ChangeRingCapacity)));
			}
			state.Ring.push_back(change);
		}
		else
		{
			state.DroppedUpTo = (std::max)(state.DroppedUpTo, state.Ring[state.RingHead].Revision);
			state.Ring[state.RingHead] = change;
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
	SRecordChange change;
	for (uint32 i = 0u; i < count; ++i)
	{
		const SRingEntry& entry = state.Ring[(state.RingHead + i) % count];
		if (entry.Revision > Revision)
		{
			change.Revision = entry.Revision;
			change.Kind = entry.Kind;
			change.Id = SAssetId { entry.Guid, 0u };
			change.MountPath = FString(LiveView.Tables.Paths.Get(entry.Path));
			Visit(change);
		}
	}
	return true;
}

namespace
{
// Approximate heap bytes of the writer's std containers (MSVC's layouts: a hash map's nodes carry two
// links and its bucket table two pointers per bucket; a tree node three links and two flags; every block
// some 16 bytes of allocator overhead), FString keys' own blocks included.
constexpr uint64 BlockOverhead = 16u;

uint64 GetHeapBytes (const FString& Text) noexcept
{
	return Text.IsHeapAllocated() ? Text.GetCapacity() + 1u + BlockOverhead : 0u;
}

template <typename TMap>
uint64 GetTreeMapBytes (const TMap& Map)
{
	uint64 bytes = 0u;
	for (const auto& entry : Map)
	{
		bytes += 3u * sizeof(void*) + 2u + sizeof(entry) + BlockOverhead + GetHeapBytes(entry.first);
	}
	return bytes;
}

uint64 GetListsBytes (const std::vector<std::vector<uint32>>& Lists)
{
	uint64 bytes = Lists.capacity() * sizeof(std::vector<uint32>);
	for (const std::vector<uint32>& list : Lists)
	{
		bytes += list.capacity() == 0u ? 0u : list.capacity() * sizeof(uint32) + BlockOverhead;
	}
	return bytes;
}
}

SRegistryMemoryStats CRegistryStore::GetMemoryStats () const
{
	SRegistryMemoryStats stats = ComputeMemoryStats(LiveView.Tables);
	const SWriterState& state = *Writer;
	stats.Writer = GetListsBytes(state.FolderChildren) + GetListsBytes(state.FolderFiles) + state.FileSlot.capacity() * sizeof(uint32)
		+ state.PathTargetCandidates.GetResidentBytes() + state.GuidTargets.GetResidentBytes() + state.CompanionRefs.capacity() * sizeof(SCompanionRef)
		+ state.CompanionRefsByFold.GetResidentBytes() + state.CompanionPathIds.GetResidentBytes() + GetTreeMapBytes(state.SubjectsByText);
	stats.ChangeRing = state.Ring.capacity() * sizeof(SRingEntry);
	stats.Total += stats.Writer + stats.ChangeRing;
	return stats;
}

void CRegistryStore::AdoptImage (SRegistryImage&& Image, uint64 Revision)
{
	CheckOwnerThread();
	const uint64 start = platform::ReadTimestamp();
	if (IsEmpty())
	{
		bool bAdopted = false;
		{
			CWriter writer(*this, Revision);
			bAdopted = writer.AdoptIntoFreshStore(Image);
		}
		SRegistryTables& tables = LiveView.Tables;
		if (bAdopted)
		{
			tables.Revision = Revision;
			LastStats = SRegistryCommitStats {};
			LastStats.RecordsChanged = tables.LiveRecords;
			LastStats.bCompactedGraph = true;
			Writer->DroppedUpTo = (std::max)(Writer->DroppedUpTo, Revision);   // readers refresh everything
			Publish();
			LastStats.Milliseconds = platform::TimestampToMilliseconds(platform::ReadTimestamp() - start);
			return;
		}
		ResetToEmpty();   // the image holds an item the bulk path does not take: the replay applies and reports it
	}
	SRegistryTables& tables = LiveView.Tables;
	SRegistryDelta delta = MakeImageDelta(std::move(Image));
	tables.Revision = Revision;
	const bool bReplayed = true;
	tables.Indices.FilesByPath.Reserve(tables.LiveFiles + static_cast<uint32>(delta.FileUpserts.size()), Epoch);
	tables.Indices.FilesByPortablePath.Reserve(tables.LiveFiles + static_cast<uint32>(delta.FileUpserts.size()), Epoch);
	tables.Indices.RecordsByPortablePath.Reserve(tables.LiveRecords + static_cast<uint32>(delta.RecordUpserts.size()), Epoch);
	tables.Indices.RecordsByGuid.Reserve(tables.LiveRecords + static_cast<uint32>(delta.RecordUpserts.size()), Epoch);

	CWriter writer(*this, Revision);
	writer.Apply(delta);
	LastStats = SRegistryCommitStats {};
	LastStats.bReplayedImage = bReplayed;
	LastStats.RecordsChanged = static_cast<uint32>(writer.TakeChanges().size());
	Writer->DroppedUpTo = (std::max)(Writer->DroppedUpTo, Revision);   // readers refresh everything

	CompactGraphTables();
	LastStats.bCompactedGraph = true;
	Publish();
	LastStats.Milliseconds = platform::TimestampToMilliseconds(platform::ReadTimestamp() - start);
}

// Nothing committed or adopted yet (only the type tables' key names in NAMES).
bool CRegistryStore::IsEmpty () const noexcept
{
	const SRegistryTables& tables = LiveView.Tables;
	return tables.Mounts->empty() && tables.Folders.Mount.Size() == 0u && tables.Files.Folder.Size() == 0u && tables.Assets.File.Size() == 0u
		&& tables.Graph.Targets.Size() == 0u && tables.Problems.Subject.Size() == 0u && tables.RuleFiles.Mount.Size() == 0u
		&& tables.SubEntries.Owner.Size() == 0u && tables.Revision == 0u;
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
			subs.LabelRank.Append(oldSubs.LabelRank[old], epoch);
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
				const STagKeyDesc& desc = old.Keys[key];
				to.Values.Grow(row + 1u, epoch);
				to.PresentBits.Grow(row + 1u, epoch);
				if (HasAnyFlags(desc.Flags, ETagFlags::MultiValue))
				{
					to.MultiRanges.Grow(row + 1u, epoch);
				}
				if (!from.PresentBits.Test(oldRow))
				{
					continue;
				}
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
	const SRegistryTables& tables = LiveView.Tables;
	const SAssetsTable& assets = tables.Assets;
	const uint32 assetRows = assets.File.Size();
	std::vector<SEdgeRow> edges;
	edges.reserve(tables.Graph.LiveEdges);
	std::vector<SPoolRange> ranges(assetRows);
	for (uint32 row = 0u; row < assetRows; ++row)
	{
		ranges[row].Begin = static_cast<uint32>(edges.size());
		if (assets.File[row] != InvalidRow)
		{
			ForEachLiveEdge(tables, row, [&] (uint32 EdgeRef) { edges.push_back(GetEdgeRow(tables, EdgeRef)); });
		}
		ranges[row].Count = static_cast<uint32>(edges.size()) - ranges[row].Begin;
	}
	InstallGraphTables(edges, ranges);
}

/**
 * Makes Edges the base CSR (Ranges: each ASSETS row's slice, in row order, empty for removed rows) and builds the
 * reverse CSR by a counting sort over targets; the overflow lists and masks start empty. New columns, so views
 * holding the old ones keep them. Shared by compaction and bulk adoption.
 */
void CRegistryStore::InstallGraphTables (std::span<const SEdgeRow> Edges, std::span<const SPoolRange> Ranges)
{
	SRegistryTables& tables = LiveView.Tables;
	SAssetsTable& assets = tables.Assets;
	SGraphTables& graph = tables.Graph;
	const uint32 assetRows = assets.File.Size();
	const uint32 targetRows = graph.Targets.Size();
	frt_assert(Ranges.size() == assetRows);

	std::vector<uint32> counts(targetRows + 1u, 0u);
	for (const SEdgeRow& edge : Edges)
	{
		++counts[edge.Target + 1u];
	}
	for (uint32 i = 1u; i <= targetRows; ++i)
	{
		counts[i] += counts[i - 1u];
	}
	std::vector<SReverseEdge> reverse(Edges.size());
	std::vector<uint32> cursor(counts.begin(), counts.end() - 1);
	for (uint32 row = 0u; row < assetRows; ++row)
	{
		const SPoolRange range = Ranges[row];
		for (uint32 i = range.Begin; i < range.Begin + range.Count; ++i)
		{
			SReverseEdge& entry = reverse[cursor[Edges[i].Target]++];
			entry.Source = row;
			entry.Edge = i;
		}
	}

	const uint64 epoch = Epoch;
	CPagedBitmap masked;
	masked.Grow(assetRows, epoch);
	for (uint32 row = 0u; row < assetRows; ++row)
	{
		if (assets.File[row] == InvalidRow)
		{
			masked.Set(row, true, epoch);
		}
	}
	std::vector<SPoolRange> baseReverse(targetRows);
	for (uint32 target = 0u; target < targetRows; ++target)
	{
		baseReverse[target] = SPoolRange { counts[target], counts[target + 1u] - counts[target] };
	}
	const std::vector<uint32> reverseHeads(targetRows, InvalidRow);
	const std::vector<SPoolRange> noOverflow(assetRows);

	assets.BaseEdges.Assign(Ranges, epoch);
	assets.BaseEdgesMasked = std::move(masked);
	assets.OverflowEdges.Assign(noOverflow, epoch);
	graph.BaseEdges.Assign(Edges, epoch);
	graph.OverflowEdges.Clear();
	graph.BaseReverse.Assign(baseReverse, epoch);
	graph.BaseReverseEdges.Assign(reverse, epoch);
	graph.OverflowReverseHead.Assign(reverseHeads, epoch);
	graph.OverflowReverseEdges.Clear();
	graph.LiveEdges = static_cast<uint32>(Edges.size());
	graph.GarbageEdges = 0u;
}
}
