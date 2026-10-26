#include "Assets/Discovery/MountScanner.h"

#include <algorithm>
#include <atomic>
#include <utility>

#include "Asserts.h"
#include "Assets/AssetDiagnosticCodes.h"
#include "Assets/Discovery/BuiltinExcludes.h"
#include "Containers/StringMap.h"
#include "Jobs/WorkerPool.h"
#include "Platform/FileSystem.h"
#include "Platform/Timestamp.h"
#include "Profiler/CpuProfiler.h"


namespace frt::assets
{
namespace
{
// The scan is what start-up waits for (BuildWorld blocks on the initial scan, §5.4).
constexpr jobs::EWorkPriority ScanPriority = jobs::EWorkPriority::High;

// One entry of a listing, kept past the callback.
struct SListedEntry
{
	FString Name;
	platform::SFileStat Stat;
	bool bNameSurrogate = false;
};

// The batch order (ScanTypes.h): SStringLessIgnoreCase, ties broken by byte order.
bool IsNameBefore (FStringView A, FStringView B) noexcept
{
	const int32 folded = A.Compare<ESearchCase::IgnoreCase>(B);
	return folded != 0 ? folded < 0 : A < B;
}

SDiagnostic MakeDiagnostic (ESeverity Severity, uint32 Code, FStringView Subject, FString Message)
{
	SDiagnostic diagnostic;
	diagnostic.Severity = Severity;
	diagnostic.Code = Code;
	// The Subject is a key (the registry's problem lists, FILES rows): the on-disk spelling, as every producer
	// spells it. Text for people is the Message's, and the outputs show the Subject with ToDisplayText.
	diagnostic.Subject = FString(Subject);
	diagnostic.Message = std::move(Message);
	return diagnostic;
}

// The mount-relative part's length in bytes ("/Project/A/b.png": 7; a mount root: 0).
uint32 GetRelativeLength (FStringView MountPath, uint32 MountNameLength) noexcept
{
	const uint32 rootLength = 1u + MountNameLength;
	return MountPath.Len() > rootLength ? MountPath.Len() - rootLength - 1u : 0u;
}

EScanFileState CompareWithKnown (const IKnownFiles* Known, FStringView FileMountPath, const platform::SFileStat& Stat)
{
	if (Known == nullptr)
	{
		return EScanFileState::New;
	}
	const std::optional<SKnownFile> known = Known->Find(FileMountPath);
	if (!known.has_value())
	{
		return EScanFileState::New;
	}
	// Inequality, never "newer than": tunnelling and CopyFile restore older times (§5.4).
	if (known->Size != Stat.Size || known->MTime != Stat.LastWriteTime)
	{
		return EScanFileState::Changed;
	}
	return known->Identity == Stat.Identity ? EScanFileState::Unchanged : EScanFileState::IdentityChanged;
}

// The PATH002 and PATH003 checks of one recorded entry (PATH001 needs its siblings).
void CheckEntryName (FStringView MountPath, FStringView Name, uint32 MountNameLength, std::vector<SDiagnostic>& Out)
{
	const FStringView reason = GetNonPortableNameReason(Name);
	if (!reason.IsEmpty())
	{
		Out.push_back(MakeDiagnostic(ESeverity::Warning, Path002, MountPath,
			FString::Format("'{}' is not a portable name: it {}", ToDisplayText(Name), reason)));
	}
	const uint32 relativeLength = GetRelativeLength(MountPath, MountNameLength);
	if (relativeLength > PortablePathMaxBytes)
	{
		Out.push_back(MakeDiagnostic(ESeverity::Warning, Path003, MountPath,
			FString::Format("the mount-relative path is {} UTF-8 bytes long, over the portable limit of {}", relativeLength, PortablePathMaxBytes)));
	}
}

// CON PRN AUX NUL COM1-COM9 LPT1-LPT9, in any case and with any extension ("con.txt", "Aux.tar.gz").
bool IsReservedDeviceName (FStringView Name) noexcept
{
	const FStringView base = Name.SplitFirst('.').Left;
	static constexpr FStringLiteral ReservedNames[] = { "CON", "PRN", "AUX", "NUL" };
	for (const FStringLiteral reserved : ReservedNames)
	{
		if (base.Equals<ESearchCase::IgnoreCaseAscii>(reserved))
		{
			return true;
		}
	}
	return base.Len() == 4u && base[3] >= '1' && base[3] <= '9'
		&& (base.Left(3u).Equals<ESearchCase::IgnoreCaseAscii>("COM") || base.Left(3u).Equals<ESearchCase::IgnoreCaseAscii>("LPT"));
}

/**
 * A folder name that Win32's path normalization (GetFullPathNameW, which the file layer applies to every
 * path shorter than MAX_PATH) rewrites: a trailing dot or space is dropped, and a device name may become
 * the device. Listing "<dir>/Textures." would list "<dir>/Textures", so such a folder is never listed:
 * it is a failed folder (consumers keep what they knew) besides its PATH002. The same on every platform,
 * so a batch does not depend on where it was made.
 */
bool IsFolderNameRewrittenByWin32 (FStringView Name) noexcept
{
	return Name.EndsWith('.') || Name.EndsWith(' ') || IsReservedDeviceName(Name);
}

SDiagnostic MakeCaseCollision (FStringView MountPath, FStringView Name, std::span<const FStringView> Others)
{
	FString others;
	for (const FStringView other : Others)
	{
		others.AppendFormat("{}'{}'", others.IsEmpty() ? "" : ", ", ToDisplayText(other));
	}
	return MakeDiagnostic(ESeverity::Warning, Path001, MountPath, FString::Format("'{}' and {} differ only in case: a case-sensitive file "
		"system keeps them apart, and lookups under the portable fold cannot tell them apart", ToDisplayText(Name), others));
}


// One folder of the scan: what its parent decided (path, scope) and what its own task found. Each node is
// written only by its own task, before the outstanding count says it is done.
struct SFolderNode
{
	FString MountPath;
	std::filesystem::path Directory;
	std::shared_ptr<const SScanScope> ParentScope;

	bool bFailed = false;
	bool bHasRuleFile = false;
	uint64 RulesHash = 0u;
	std::shared_ptr<const SScanScope> Scope;
	std::vector<SScanFile> Files;                           // sorted; Folder is set at assembly
	std::vector<std::unique_ptr<SFolderNode>> Children;     // sorted
	std::vector<FString> SkippedLinks;
	std::vector<SDiagnostic> Diagnostics;
	std::vector<SDiagnostic> RuleDiagnostics;   // the oracle's, for SScanBatch::RuleDiagnostics
};

struct SScanState
{
	jobs::CWorkerPool* Pool = nullptr;
	SMount Mount;
	const IScanMembership* Membership = nullptr;
	const IKnownFiles* Known = nullptr;
	TUniqueFunction<void (SScanBatch&&)> OnDone;
	uint64 StartTicks = 0u;

	SScanBatch Batch;                      // RootMountPath and what the root task found above the tree
	std::unique_ptr<SFolderNode> Root;     // null: nothing to scan
	std::atomic<uint32> Outstanding = 0u;  // folders submitted and not yet finished
	std::atomic<uint32> FoldersListed = 0u;
};

using FScanStatePtr = std::shared_ptr<SScanState>;

void RunFolder (const FScanStatePtr& State, SFolderNode& Node);

// Lists Node's directory and fills the node: its scope, files, subfolders to scan, links and diagnostics.
void ScanFolder (SScanState& State, SFolderNode& Node)
{
	FRT_CPU_SCOPE("Assets.Scan.Folder");
	const bool bMountRoot = Node.MountPath.Len() == 1u + State.Mount.Name.Len();
	if (!bMountRoot && IsFolderNameRewrittenByWin32(GetMountPathLeaf(Node.MountPath)))
	{
		Node.bFailed = true;
		Node.Scope = Node.ParentScope;
		Node.Diagnostics.push_back(MakeDiagnostic(ESeverity::Warning, 0u, Node.MountPath, FString::Format("cannot scan the folder {}: Windows "
			"drops a trailing dot or space from a name and may read a device name as the device, so its listing could be another "
			"folder's; rename it", FString::FromPath(Node.Directory))));
		return;
	}
	std::vector<SListedEntry> entries;
	FString error;
	const bool bListed = platform::EnumerateDirectory(Node.Directory, [&entries] (const platform::SDirectoryEntry& Entry)
	{
		entries.push_back({ FString(Entry.Name), Entry.Stat, Entry.bNameSurrogate });
	}, &error);
	if (!bListed)
	{
		// What was delivered before a midway failure is dropped: the folder is unknown, not empty.
		Node.bFailed = true;
		Node.Scope = Node.ParentScope;
		Node.Diagnostics.push_back(MakeDiagnostic(ESeverity::Warning, 0u, Node.MountPath, FString::Format("cannot scan the folder: {}", error)));
		return;
	}
	State.FoldersListed.fetch_add(1u, std::memory_order_relaxed);
	std::sort(entries.begin(), entries.end(), [] (const SListedEntry& A, const SListedEntry& B) { return IsNameBefore(A.Name, B.Name); });

	// The folder's rule file goes to the oracle before anything is classified. Under the portable fold;
	// a case-sensitive folder can hold two spellings, and the reserved one wins.
	const SListedEntry* ruleFile = nullptr;
	for (const SListedEntry& entry : entries)
	{
		if (!entry.Stat.bDirectory && !entry.bNameSurrogate && IsRuleFileName(entry.Name) && (ruleFile == nullptr || entry.Name == RuleFileName))
		{
			ruleFile = &entry;
		}
	}
	const std::filesystem::path ruleFilePath = ruleFile != nullptr ? Node.Directory / ruleFile->Name.ToPath() : std::filesystem::path();
	uint64 rulesHash = 0u;
	std::shared_ptr<const SScanScope> scope = State.Membership->EnterFolder(Node.ParentScope, Node.MountPath,
		ruleFile != nullptr ? &ruleFilePath : nullptr, rulesHash, Node.RuleDiagnostics);
	Node.Scope = scope != nullptr ? std::move(scope) : Node.ParentScope;
	Node.bHasRuleFile = ruleFile != nullptr;
	Node.RulesHash = rulesHash;

	// The entries the batch records, in order: the portable-name checks look at these only.
	std::vector<uint32> recorded;
	recorded.reserve(entries.size());
	FString childPath(Node.MountPath);
	childPath += '/';
	const uint32 prefixLength = childPath.Len();
	for (uint32 i = 0u; i < entries.size(); ++i)
	{
		const SListedEntry& entry = entries[i];
		childPath.LeftInline(prefixLength);
		childPath += entry.Name;
		if (entry.bNameSurrogate)
		{
			Node.SkippedLinks.push_back(childPath);
			Node.Diagnostics.push_back(MakeDiagnostic(ESeverity::Info, 0u, childPath, FString::Format("{} is a symbolic link or junction: "
				"not followed, and nothing through it is registered", FString::FromPath(Node.Directory / entry.Name.ToPath()))));
			continue;
		}
		if (entry.Stat.bDirectory)
		{
			if (!State.Membership->ShouldDescend(*Node.Scope, childPath))
			{
				continue;
			}
			auto child = std::make_unique<SFolderNode>();
			child->MountPath = childPath;
			child->Directory = Node.Directory / entry.Name.ToPath();
			child->ParentScope = Node.Scope;
			Node.Children.push_back(std::move(child));
			recorded.push_back(i);
			continue;
		}
		if (IsRuleFileName(entry.Name))
		{
			// The folder's rule file (or a second spelling of it): never a file of the batch, but a file of
			// the mount all the same, so the portable-name checks see it (Assets.yml beside assets.yml).
			recorded.push_back(i);
			continue;
		}
		const std::optional<SFileVerdict> verdict = State.Membership->Classify(*Node.Scope, childPath);
		if (!verdict.has_value())
		{
			continue;
		}
		SScanFile file;
		file.Name = entry.Name;
		file.Size = entry.Stat.Size;
		file.MTime = entry.Stat.LastWriteTime;
		file.Identity = entry.Stat.Identity;
		file.Verdict = *verdict;
		file.State = CompareWithKnown(State.Known, childPath, entry.Stat);
		Node.Files.push_back(std::move(file));
		recorded.push_back(i);
	}

	// Portable names. Sorted by the fold, so siblings equal under it are adjacent.
	const uint32 mountNameLength = State.Mount.Name.Len();
	std::vector<FStringView> others;
	for (uint32 start = 0u; start < recorded.size();)
	{
		uint32 end = start + 1u;
		while (end < recorded.size() && entries[recorded[end]].Name.View().Equals<ESearchCase::IgnoreCase>(entries[recorded[start]].Name))
		{
			++end;
		}
		for (uint32 k = start; k < end; ++k)
		{
			const FString& name = entries[recorded[k]].Name;
			childPath.LeftInline(prefixLength);
			childPath += name;
			if (end - start > 1u)
			{
				others.clear();
				for (uint32 o = start; o < end; ++o)
				{
					if (o != k)
					{
						others.push_back(entries[recorded[o]].Name.View());
					}
				}
				Node.Diagnostics.push_back(MakeCaseCollision(childPath, name, others));
			}
			CheckEntryName(childPath, name, mountNameLength, Node.Diagnostics);
		}
		start = end;
	}
}

// Assembles the batch in the order ScanTypes.h states and hands it to the owner thread. Runs on the
// worker that finished the last folder; the nodes are complete by then (the count's acquire).
void Deliver (SScanState& State)
{
	SScanBatch& batch = State.Batch;
	if (State.Root != nullptr)
	{
		struct SFrame
		{
			SFolderNode* Node = nullptr;
			uint32 Parent = ~0u;
		};
		std::vector<SFrame> stack;
		stack.push_back({ State.Root.get(), ~0u });
		while (!stack.empty())
		{
			const SFrame frame = stack.back();
			stack.pop_back();
			SFolderNode& node = *frame.Node;
			const uint32 index = static_cast<uint32>(batch.Folders.size());

			SScanFolder folder;
			folder.MountPath = std::move(node.MountPath);
			folder.Parent = frame.Parent;
			folder.bHasRuleFile = node.bHasRuleFile;
			folder.RulesHash = node.RulesHash;
			folder.Scope = std::move(node.Scope);
			batch.Folders.push_back(std::move(folder));
			if (node.bFailed)
			{
				batch.FailedFolders.push_back(batch.Folders.back().MountPath);
			}
			for (SScanFile& file : node.Files)
			{
				file.Folder = index;
				batch.Files.push_back(std::move(file));
			}
			for (FString& link : node.SkippedLinks)
			{
				batch.SkippedLinks.push_back(std::move(link));
			}
			for (SDiagnostic& diagnostic : node.RuleDiagnostics)
			{
				batch.RuleDiagnostics.push_back(std::move(diagnostic));
			}
			for (SDiagnostic& diagnostic : node.Diagnostics)
			{
				batch.Diagnostics.push_back(std::move(diagnostic));
			}
			// Pre-order: the first child is popped next.
			for (auto child = node.Children.rbegin(); child != node.Children.rend(); ++child)
			{
				stack.push_back({ child->get(), index });
			}
		}
		State.Root.reset();
	}
	batch.FoldersListed = State.FoldersListed.load(std::memory_order_relaxed);
	batch.Milliseconds = platform::TimestampToMilliseconds(platform::ReadTimestamp() - State.StartTicks);

	State.Pool->Submit(ScanPriority, jobs::NoCancelGroup, [] {},
		[OnDone = std::move(State.OnDone), Batch = std::move(State.Batch)] () mutable
		{
			OnDone(std::move(Batch));
		});
}

void FinishFolder (const FScanStatePtr& State)
{
	if (State->Outstanding.fetch_sub(1u, std::memory_order_acq_rel) == 1u)
	{
		Deliver(*State);
	}
}

// Scans Node, then submits its subfolders (counted before this one is finished, so the count never
// reaches zero early).
void RunFolder (const FScanStatePtr& State, SFolderNode& Node)
{
	ScanFolder(*State, Node);
	if (!Node.Children.empty())
	{
		State->Outstanding.fetch_add(static_cast<uint32>(Node.Children.size()), std::memory_order_relaxed);
		for (const std::unique_ptr<SFolderNode>& child : Node.Children)
		{
			SFolderNode* childNode = child.get();
			State->Pool->Submit(ScanPriority, jobs::NoCancelGroup, [State, childNode] { RunFolder(State, *childNode); });
		}
	}
	FinishFolder(State);
}

void FailRoot (SScanState& State, FString Message)
{
	State.Batch.FailedFolders.push_back(State.Batch.RootMountPath);
	State.Batch.Diagnostics.push_back(MakeDiagnostic(ESeverity::Error, 0u, State.Batch.RootMountPath, std::move(Message)));
}

// The first task: checks the root, opens its scope, looks at it in its parent's listing (a subtree root),
// and scans it like every other folder.
void RunRoot (const FScanStatePtr& State)
{
	FRT_CPU_SCOPE("Assets.Scan.Root");
	SScanState& state = *State;
	const FStringView rootPath = state.Batch.RootMountPath;

	FStringView mountName;
	FStringView relative;
	std::optional<std::filesystem::path> below;
	if (IsWellFormedMountPath(rootPath) && SplitMountPath(rootPath, mountName, relative) && mountName == state.Mount.Name
		&& !relative.Contains('\\') && !relative.Contains(':'))
	{
		below = relative.IsEmpty() ? std::filesystem::path() : relative.TryToPath();
	}
	if (!below.has_value())
	{
		FailRoot(state, FString::Format("'{}' is not a folder of mount /{}", ToDisplayText(rootPath), state.Mount.Name));
		Deliver(state);
		return;
	}
	std::filesystem::path directory = state.Mount.ContentDir;
	if (!below->empty())
	{
		directory /= *below;
		directory.make_preferred();
	}

	// A folder above the root whose name Win32 rewrites would be read as another folder (ScanFolder
	// handles the root's own name like every other folder's).
	for (FStringView rest = relative; rest.Contains('/');)
	{
		const SStringSplit split = rest.SplitFirst('/');
		if (IsFolderNameRewrittenByWin32(split.Left))
		{
			state.Batch.FailedFolders.push_back(state.Batch.RootMountPath);
			state.Batch.Diagnostics.push_back(MakeDiagnostic(ESeverity::Warning, 0u, rootPath, FString::Format("cannot scan the folder {}: "
				"Windows drops a trailing dot or space from the name '{}' above it and may read a device name as the device, so its "
				"listing could be another folder's; rename it", FString::FromPath(directory), ToDisplayText(split.Left))));
			Deliver(state);
			return;
		}
		rest = split.Right;
	}

	const std::shared_ptr<const SScanScope> scopeAbove = state.Membership->OpenScope(rootPath, state.Batch.RuleDiagnostics);
	if (scopeAbove == nullptr)
	{
		FailRoot(state, FString::Format("the membership oracle opened no scope for '{}' (IScanMembership::OpenScope returned null)",
			ToDisplayText(rootPath)));
		Deliver(state);
		return;
	}

	// The root as its parent lists it. "Not there" is decided only from a listing that holds no entry the
	// root's directory could open as - never from a failed stat or a byte-exact miss alone - because a
	// consumer removes everything it knew at or below an absent root. Without the parent's listing, the
	// root's own listing decides (and a failure puts the root in FailedFolders).
	const FString rootName = relative.IsEmpty() ? FString::FromPath(directory.filename()) : FString(GetMountPathLeaf(rootPath));
	if (relative.IsEmpty())
	{
		// A mount root: its name is looked up by its parent's rule, which nothing queried, so NTFS's (a
		// match the content directory then cannot be listed under only fails the scan). A mount whose
		// content directory is not there is empty (Resolve warned about it).
		if (!rootName.IsEmpty())
		{
			const FString rootKey = MakePathKey(rootName);
			bool bPresent = false;
			const bool bParentListed = platform::EnumerateDirectory(directory.parent_path(), [&] (const platform::SDirectoryEntry& Entry)
			{
				bPresent = bPresent || Entry.Name == rootName.View() || MakePathKey(Entry.Name) == rootKey;
			});
			if (bParentListed && !bPresent)
			{
				Deliver(state);
				return;
			}
		}
	}
	else
	{
		// A subtree root: a link is never followed, a root that is not there holds nothing, and its
		// siblings decide PATH001 for it. On a case-insensitive mount, a folder the root's spelling opens
		// under NTFS's rule but spelled otherwise on disk ("/Project/textures" for Textures, as a path typed
		// or taken from an event can be) is there: the batch would key its files by a spelling the disk does
		// not have, so the root fails instead, naming the on-disk spelling.
		const bool bByRule = !state.Mount.Volume.bCaseSensitive;
		const FString rootKey = bByRule ? MakePathKey(rootName) : FString();
		std::vector<FString> collisions;
		std::vector<FString> otherSpellings;
		bool bFound = false;
		bool bLink = false;
		const bool bParentListed = platform::EnumerateDirectory(directory.parent_path(), [&] (const platform::SDirectoryEntry& Entry)
		{
			if (Entry.Name == rootName.View())
			{
				bFound = Entry.Stat.bDirectory || Entry.bNameSurrogate;
				bLink = Entry.bNameSurrogate;
				return;
			}
			if (bByRule && (Entry.Stat.bDirectory || Entry.bNameSurrogate) && MakePathKey(Entry.Name) == rootKey)
			{
				otherSpellings.emplace_back(Entry.Name);
			}
			if (!Entry.bNameSurrogate && Entry.Name.Equals<ESearchCase::IgnoreCase>(rootName))
			{
				collisions.emplace_back(Entry.Name);
			}
		});
		if (bParentListed && !bFound && !otherSpellings.empty())
		{
			std::sort(otherSpellings.begin(), otherSpellings.end(), [] (const FString& A, const FString& B) { return IsNameBefore(A, B); });
			state.Batch.FailedFolders.push_back(state.Batch.RootMountPath);
			state.Batch.Diagnostics.push_back(MakeDiagnostic(ESeverity::Warning, 0u, rootPath, FString::Format("not scanned: the folder is "
				"spelled '{}' on disk, and a scan keys files by their on-disk spelling; scan it under that spelling",
				ToDisplayText(otherSpellings.front()))));
			Deliver(state);
			return;
		}
		if (bParentListed && !bFound)
		{
			Deliver(state);
			return;
		}
		if (bLink)
		{
			state.Batch.SkippedLinks.push_back(state.Batch.RootMountPath);
			state.Batch.Diagnostics.push_back(MakeDiagnostic(ESeverity::Info, 0u, rootPath, FString::Format("{} is a symbolic link or junction: "
				"not followed, and nothing through it is registered", FString::FromPath(directory))));
			Deliver(state);
			return;
		}
		if (!state.Membership->ShouldDescend(*scopeAbove, rootPath))
		{
			Deliver(state);
			return;
		}
		if (!collisions.empty())
		{
			std::sort(collisions.begin(), collisions.end(), [] (const FString& A, const FString& B) { return IsNameBefore(A, B); });
			std::vector<FStringView> others(collisions.begin(), collisions.end());
			state.Batch.Diagnostics.push_back(MakeCaseCollision(rootPath, rootName, others));
		}
		CheckEntryName(rootPath, rootName, state.Mount.Name.Len(), state.Batch.Diagnostics);
	}
	if (relative.IsEmpty() && !state.Membership->ShouldDescend(*scopeAbove, rootPath))
	{
		Deliver(state);
		return;
	}

	state.Root = std::make_unique<SFolderNode>();
	state.Root->MountPath = FString(rootPath);
	state.Root->Directory = std::move(directory);
	state.Root->ParentScope = scopeAbove;
	state.Outstanding.store(1u, std::memory_order_relaxed);
	RunFolder(State, *state.Root);
}
}


FStringView GetNonPortableNameReason (FStringView Name) noexcept
{
	if (!Name.IsValidUtf8())
	{
		return "is not valid UTF-8 (it holds a lone UTF-16 surrogate)";
	}
	for (const char ch : Name)
	{
		switch (ch)
		{
		case '<': return "contains '<'";
		case '>': return "contains '>'";
		case ':': return "contains ':'";
		case '"': return "contains '\"'";
		case '/': return "contains '/'";
		case '\\': return "contains '\\'";
		case '|': return "contains '|'";
		case '?': return "contains '?'";
		case '*': return "contains '*'";
		default: break;
		}
		if (static_cast<unsigned char>(ch) < 0x20u)
		{
			return "contains a control character";
		}
	}
	if (Name.EndsWith('.'))
	{
		return "ends with a dot";
	}
	if (Name.EndsWith(' '))
	{
		return "ends with a space";
	}

	if (IsReservedDeviceName(Name))
	{
		return "is a reserved device name on Windows";
	}
	return FStringView();
}


CDefaultScanMembership::CDefaultScanMembership ()
	: Scope(std::make_shared<const SScanScope>())
{
}

std::shared_ptr<const SScanScope> CDefaultScanMembership::OpenScope (FStringView /*FolderMountPath*/, std::vector<SDiagnostic>& /*OutDiagnostics*/) const
{
	return Scope;
}

std::shared_ptr<const SScanScope> CDefaultScanMembership::EnterFolder (const std::shared_ptr<const SScanScope>& Parent,
	FStringView /*FolderMountPath*/, const std::filesystem::path* /*RuleFile*/, uint64& OutRulesHash, std::vector<SDiagnostic>& /*OutDiagnostics*/) const
{
	OutRulesHash = 0u;
	return Parent;
}

bool CDefaultScanMembership::ShouldDescend (const SScanScope& /*Scope*/, FStringView FolderMountPath) const
{
	return !IsBuiltinExcludedName(GetMountPathLeaf(FolderMountPath), true);
}

std::optional<SFileVerdict> CDefaultScanMembership::Classify (const SScanScope& /*Scope*/, FStringView FileMountPath) const
{
	if (IsBuiltinExcludedName(GetMountPathLeaf(FileMountPath), false))
	{
		return std::nullopt;
	}
	return SFileVerdict { EFileClass::Asset, false };
}


void CMountScanner::ScanAsync (jobs::CWorkerPool& Pool, const SMount& Mount, FStringView RootMountPath, const IScanMembership& Membership,
	const IKnownFiles* Known, TUniqueFunction<void (SScanBatch&&)> OnDone)
{
	frt_assert(OnDone.IsSet());
	auto state = std::make_shared<SScanState>();
	state->Pool = &Pool;
	state->Mount = Mount;
	state->Membership = &Membership;
	state->Known = Known;
	state->OnDone = std::move(OnDone);
	state->StartTicks = platform::ReadTimestamp();
	state->Batch.RootMountPath = FString(RootMountPath);
	Pool.Submit(ScanPriority, jobs::NoCancelGroup, [state] { RunRoot(state); });
}

SScanBatch CMountScanner::ScanBlocking (jobs::CWorkerPool& Pool, const SMount& Mount, FStringView RootMountPath, const IScanMembership& Membership,
	const IKnownFiles* Known)
{
	frt_assert(Pool.IsOwnerThread());
	std::optional<SScanBatch> result;
	ScanAsync(Pool, Mount, RootMountPath, Membership, Known, [&result] (SScanBatch&& Batch) { result = std::move(Batch); });
	Pool.PumpUntil([&result] { return result.has_value(); });
	if (!result.has_value())
	{
		// Only a drain of the pool from inside another completion gets here.
		SScanBatch failed;
		failed.RootMountPath = FString(RootMountPath);
		failed.FailedFolders.push_back(failed.RootMountPath);
		return failed;
	}
	return std::move(*result);
}
}
