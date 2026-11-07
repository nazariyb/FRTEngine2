#include "Assets/AssetRegistry.h"

#include <algorithm>
#include <atomic>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <system_error>
#include <utility>

#include "Assets/AssetDiagnosticCodes.h"
#include "Assets/Discovery/MountScanner.h"
#include "Assets/FileWatcherService.h"
#include "Assets/Meta/AssetRepairs.h"
#include "Assets/Meta/MetaFile.h"
#include "Assets/Meta/MetaPaths.h"
#include "Assets/Meta/MetaTypes.h"
#include "Assets/MountPaths.h"
#include "Assets/Pipeline/ImporterTable.h"
#include "Assets/Pipeline/Importers/AssetImporter.h"
#include "Assets/Registry/Journal.h"
#include "Assets/Registry/RegistryParallel.h"
#include "Assets/Registry/RegistryRecords.h"
#include "Assets/Registry/Snapshot.h"
#include "Containers/StringMap.h"
#include "Jobs/WorkerPool.h"
#include "Log/Log.h"
#include "Platform/Thread.h"
#include "Platform/Timestamp.h"


namespace frt::assets
{
namespace
{
constexpr FStringLiteral LogCategory = "assets";

double MillisecondsSince (uint64 Start) noexcept
{
	return platform::TimestampToMilliseconds(platform::ReadTimestamp() - Start);
}

bool IsAtOrBelowAny (FStringView Path, std::span<const FString> Ancestors) noexcept
{
	return std::any_of(Ancestors.begin(), Ancestors.end(), [Path] (const FString& Ancestor) { return IsMountPathAtOrBelow(Path, Ancestor.View()); });
}

// The folder a scan of a changed path covers: its parent, or the mount root for the root itself.
FString ScanRootOf (FStringView MountPath)
{
	const FStringView parent = GetMountPathParent(MountPath);
	return FString(parent.IsEmpty() ? MountPath : parent);
}

/**
 * Runs OnLost when destroyed before Dismiss: held by a pool completion, so work the pool drops (a cancelled group)
 * still tells the registry, which counts what is outstanding. The pool destroys completions on its owner thread,
 * the registry's.
 */
class CLostGuard
{
public:
	explicit CLostGuard (TUniqueFunction<void ()> InOnLost) noexcept
		: OnLost(std::move(InOnLost))
	{}
	CLostGuard (CLostGuard&& Other) noexcept
		: OnLost(std::move(Other.OnLost))
	{
		Other.OnLost = nullptr;
	}
	CLostGuard& operator= (CLostGuard&&) = delete;
	~CLostGuard ()
	{
		if (OnLost)
		{
			OnLost();
		}
	}
	void Dismiss () noexcept { OnLost = nullptr; }

private:
	TUniqueFunction<void ()> OnLost;
};

using FProblemsBySubject = std::map<FString, std::vector<SDiagnostic>, std::less<>>;

FProblemsBySubject GroupBySubject (std::vector<SDiagnostic>&& Diagnostics)
{
	FProblemsBySubject bySubject;
	for (SDiagnostic& diagnostic : Diagnostics)
	{
		bySubject[diagnostic.Subject].push_back(std::move(diagnostic));
	}
	return bySubject;
}

// Groups diagnostics by Subject into one SProblemsSet each, under Source.
void AddProblemSets (SRegistryDelta& Delta, EProblemSource Source, std::vector<SDiagnostic>&& Diagnostics)
{
	for (auto& [subject, list] : GroupBySubject(std::move(Diagnostics)))
	{
		Delta.Problems.push_back(SProblemsSet { subject, Source, std::move(list) });
	}
}

/**
 * Replaces Source's lists at or below Root (SProblemsClear's reach) with Diagnostics - nothing at all when the view
 * holds exactly those lists already, so a warm start commits no problem it had.
 */
void ReplaceSubtreeProblems (SRegistryDelta& Delta, const CRegistryView& View, FStringView Root, EProblemSource Source,
	std::vector<SDiagnostic>&& Diagnostics)
{
	FProblemsBySubject stored;
	View.ForEachProblem([&] (FStringView Subject, EProblemSource ProblemSource, const SDiagnostic& Diagnostic)
	{
		if (ProblemSource == Source && IsMountPathAtOrBelow(Subject, Root, true))
		{
			stored[FString(Subject)].push_back(Diagnostic);
		}
	});
	FProblemsBySubject found = GroupBySubject(std::move(Diagnostics));
	if (found == stored)
	{
		return;
	}
	Delta.ProblemClears.push_back(SProblemsClear { FString(Root), Source });
	for (auto& [subject, list] : found)
	{
		Delta.Problems.push_back(SProblemsSet { subject, Source, std::move(list) });
	}
}

// The RulesHash of a mount's Config/Assets.yml as a view's RULEFILES row holds it (0: no row).
uint64 GetStoredMountRulesHash (const CRegistryView& View, FStringView MountName)
{
	SRuleFileData data;
	const std::optional<uint32> row = View.FindRuleFile(MountName, FStringView());
	return row.has_value() && View.GetRuleFile(*row, data) ? data.RulesHash : 0u;
}

// Whether a delta changes what RULE002 is computed from: file rows, records (their companions) or folders.
bool ChangesFiles (const SRegistryDelta& Delta) noexcept
{
	return !Delta.FileUpserts.empty() || !Delta.FileRemoves.empty() || !Delta.FolderRemoves.empty() || !Delta.FolderUpserts.empty()
		|| !Delta.RecordUpserts.empty() || !Delta.RecordRemoves.empty();
}

template <typename T>
void AppendMoved (std::vector<T>& To, std::vector<T>& From)
{
	if (To.empty())
	{
		To = std::move(From);
		return;
	}
	To.reserve(To.size() + From.size());
	std::move(From.begin(), From.end(), std::back_inserter(To));
}

// Appends From to To, member by member, keeping the contract order within each list (one atomic delta).
void MergeDelta (SRegistryDelta& To, SRegistryDelta&& From)
{
	AppendMoved(To.Mounts, From.Mounts);
	AppendMoved(To.FolderRemoves, From.FolderRemoves);
	AppendMoved(To.FileRemoves, From.FileRemoves);
	AppendMoved(To.RecordRemoves, From.RecordRemoves);
	AppendMoved(To.RuleFileRemoves, From.RuleFileRemoves);
	AppendMoved(To.ProblemClears, From.ProblemClears);
	AppendMoved(To.FolderUpserts, From.FolderUpserts);
	AppendMoved(To.FileUpserts, From.FileUpserts);
	AppendMoved(To.RuleFileUpserts, From.RuleFileUpserts);
	AppendMoved(To.RecordUpserts, From.RecordUpserts);
	AppendMoved(To.Problems, From.Problems);
	AppendMoved(To.MetaCopies, From.MetaCopies);
	AppendMoved(To.MetaCopyRemoves, From.MetaCopyRemoves);
	AppendMoved(To.Tombstones, From.Tombstones);
	AppendMoved(To.TombstoneRemoves, From.TombstoneRemoves);
}


// ---------------- The work of one pass (on workers) ----------------

/**
 * A meta as a conversion read it (design §5.4 step 5, M4): its raw bytes' parse and hash (FILES' MetaHash), and an Ok
 * meta's bytes for METAS. Read on a worker only when the meta changed, or when its file is analyzed or re-evaluated and
 * needs its values: from the view's METAS copy when the meta did not change and the copy verifies (no disk read), else
 * from the file; shared by the requests that use it.
 */
struct SReadMeta
{
	SMetaParseResult Parse;
	uint64 Hash = 0u;
	std::vector<uint8> Bytes;                           // an Ok meta's raw bytes (empty for a Broken one)
	bool bFromCopy = false;                             // parsed from the view's METAS copy

	[[nodiscard]] bool IsOk () const noexcept { return Parse.State == EMetaParseState::Ok; }
	// The values the rules engine applies (SFileFacts::Meta): an Ok parse's; a Broken meta's are meaningless.
	[[nodiscard]] const SAssetMeta* GetValues () const noexcept { return IsOk() ? &Parse.Meta : nullptr; }
	// The FILES columns of the meta's source, and of the meta's own row.
	[[nodiscard]] SFileMetaState GetColumns () const noexcept { return SFileMetaState { IsOk() ? EMetaState::Paired : EMetaState::Broken, Hash }; }
};

// The meta at MountPath of Mount, read race-free (platform::ReadFileChunksStable); null when it changed while read or
// cannot be read (KeepsChanging: a meta every read of which changed, CAssetRegistry::SetSourceChangedForTest): the
// caller then commits nothing of it, so the next scan reads it again.
std::shared_ptr<const SReadMeta> ReadMetaFile (const SMount& Mount, FStringView MountPath, FStringView KeepsChanging)
{
	if (!KeepsChanging.IsEmpty() && MountPath == KeepsChanging)
	{
		return nullptr;
	}
	FStringView mountName;
	FStringView relative;
	if (!SplitMountPath(MountPath, mountName, relative))
	{
		return nullptr;
	}
	const std::optional<std::filesystem::path> path = Mount.ToAbsolute(relative);
	if (!path.has_value())
	{
		return nullptr;
	}
	FString bytes;
	const platform::EStableRead read = platform::ReadFileChunksStable(*path, [&bytes] (std::span<const uint8> Chunk)
	{
		bytes += FStringView(reinterpret_cast<const char*>(Chunk.data()), Chunk.size());
	});
	if (read != platform::EStableRead::Stable)
	{
		return nullptr;
	}
	auto meta = std::make_shared<SReadMeta>();
	ParseMeta(bytes, meta->Parse);
	meta->Hash = HashMetaBytes(bytes);
	if (meta->IsOk())
	{
		meta->Bytes.assign(reinterpret_cast<const uint8*>(bytes.GetData()), reinterpret_cast<const uint8*>(bytes.GetData()) + bytes.Len());
	}
	return meta;
}

// The meta of a file whose meta did not change, from the view's METAS copy of its GUID (§5.3: re-evaluation and a
// pinned file's class need the meta's values, never the disk): null when there is no copy whose hash is the file's
// MetaHash, which then reads the file.
std::shared_ptr<const SReadMeta> ReadMetaCopy (const CRegistryView& View, const SStoredFile& Stored)
{
	if (!Stored.Record.IsValid() || Stored.MetaState != EMetaState::Paired)
	{
		return nullptr;
	}
	const std::optional<SMetaCopyView> copy = View.FindMetaCopy(Stored.Guid);
	if (!copy.has_value() || copy->Hash != Stored.MetaHash)
	{
		return nullptr;
	}
	auto meta = std::make_shared<SReadMeta>();
	ParseMeta(FStringView(reinterpret_cast<const char*>(copy->Bytes.data()), static_cast<uint32>(copy->Bytes.size())), meta->Parse);
	if (!meta->IsOk() || meta->Parse.Meta.Guid != Stored.Guid)
	{
		return nullptr;
	}
	meta->Hash = copy->Hash;
	meta->Bytes.assign(copy->Bytes.begin(), copy->Bytes.end());
	meta->bFromCopy = true;
	return meta;
}

/**
 * The identity state a conversion commits beside a file's record (§5.3, M4): the METAS copy of an Ok meta read from the
 * file, when the view's differs (another hash or source); for a record whose meta-given GUID goes away - its file
 * removed, its GUID changed in place - a tombstone {GUID, last path, hash, identity, time} and the copy's removal, which
 * the store ignores while another live record holds the GUID. A provisional id (HasMetaGivenGuid: PendingMeta, or a
 * new file's Broken meta that salvaged none) is never tombstoned: no meta holds it.
 */
void AppendMetaCopy (const CRegistryView& View, const SReadMeta* Meta, FStringView SourcePath, SRegistryDelta& Delta)
{
	if (Meta == nullptr || !Meta->IsOk() || Meta->bFromCopy)
	{
		return;
	}
	const SGuid& guid = Meta->Parse.Meta.Guid;
	const std::optional<SMetaCopyView> stored = View.FindMetaCopy(guid);
	if (stored.has_value() && stored->Hash == Meta->Hash && stored->SourcePath == SourcePath)
	{
		return;
	}
	Delta.MetaCopies.push_back(SMetaCopy { guid, Meta->Hash, Meta->Bytes, FString(SourcePath) });
}

std::optional<STombstone> MakeTombstone (const CRegistryView& View, const SStoredFile& Stored, FStringView Path, int64 Now)
{
	if (!HasMetaGivenGuid(Stored, Path))
	{
		return std::nullopt;
	}
	// Sent whether or not another record holds the GUID (a copy, a dedupe loser re-GUIDed away from its winner): this
	// view may be older than the commit (another mount's pass, another record the same pass removes), so the store
	// decides at commit, against what is live then (RegistryDelta.h).
	SFileData file;
	(void)View.GetFile(Stored.FileRow, file);
	return STombstone { Stored.Guid, FString(Path), file.ContentHash, Stored.Stat.Identity, Now };
}

void AppendLostIdentity (std::optional<STombstone>&& Tombstone, SRegistryDelta& Delta)
{
	if (Tombstone.has_value())
	{
		Delta.MetaCopyRemoves.push_back(Tombstone->Guid);
		Delta.Tombstones.push_back(std::move(*Tombstone));
	}
}

// A record of a file whose meta is gone keeps what its meta gave it (§4.6: a lost meta never changes identity), until
// the lifecycle restores the meta: the stored consolidated aliases and former paths.
void KeepStoredRedirects (const CRegistryView& View, SRecordId Record, SRecordUpsert& Upsert)
{
	View.ForEachAlias(Record, [&] (const SAssetId& Alias)
	{
		if (Alias.SubId == 0u)
		{
			Upsert.Aliases.push_back(Alias);
		}
	});
	View.ForEachFormerPath(Record, [&] (FStringView FormerPath) { Upsert.FormerPaths.push_back(FString(FormerPath)); });
}

/**
 * A record's GUID (§4.1, M4), in order: an Ok meta's; a Broken meta's salvaged one - the stored record's GUID while the
 * broken bytes still name it, else the first in file order (a GUID edited by hand); else the stored record's when a
 * meta gave it, because a lost meta never changes identity; else the provisional id of a file with no meta yet
 * (PendingMeta, or a Broken meta that salvaged none), which never reaches a written meta. A conflict between two Guid:
 * lines keeps the record's own whichever side git wrote first (a rebase puts upstream's first): flipping it would
 * tombstone the GUID the local references name and drop its last good METAS copy, which "keep A, alias B" rebuilds from.
 */
SGuid ChooseRecordGuid (const SReadMeta* Meta, const std::optional<SStoredFile>& Stored, FStringView MountPath)
{
	if (Meta != nullptr && Meta->IsOk())
	{
		return Meta->Parse.Meta.Guid;
	}
	const bool bStoredGiven = Stored.has_value() && HasMetaGivenGuid(*Stored, MountPath);
	if (Meta != nullptr && !Meta->Parse.SalvagedGuids.empty())
	{
		const std::vector<SGuid>& salvaged = Meta->Parse.SalvagedGuids;
		if (bStoredGiven && std::find(salvaged.begin(), salvaged.end(), Stored->Guid) != salvaged.end())
		{
			return Stored->Guid;
		}
		return salvaged.front();
	}
	if (bStoredGiven)
	{
		return Stored->Guid;
	}
	return MakeProvisionalGuid(MountPath);
}

// A meta's own FILES row (EFileClass::Meta): its stat, and its pairing when Columns says (nullopt: keep it). Its content
// is in its MetaHash, so ContentHash stays as it was (zero for a new row).
SFileUpsert MakeMetaRow (FStringView MountPath, const SScanMeta& Meta, std::optional<SFileMetaState> Columns)
{
	SFileUpsert row;
	row.MountPath = FString(MountPath);
	row.Size = Meta.Size;
	row.MTime = Meta.MTime;
	row.Identity = Meta.Identity;
	row.Class = EFileClass::Meta;
	row.Meta = Columns;
	return row;
}

// One file to analyze: what the scan saw, the settings and the folder's scope the conversion resolved.
struct SAnalyzeRequest
{
	FString Path;
	uint32 Mount = 0u;                                  // index in the mount table
	SEffectiveFileSettings Settings;
	std::shared_ptr<const SScanScope> Scope;
	uint64 Size = 0u;
	int64 MTime = 0;
	platform::SFileIdentity Identity;
	EFileClass Class = EFileClass::Asset;
	bool bContentChanged = false;                       // new or changed: a hash Analyze does not compute is zeroed
	uint32 Retries = 0u;
	uint64 Sequence = 0u;                               // the registry's request sequence of this file
	SGuid Guid;                                         // the record's (ChooseRecordGuid)
	std::shared_ptr<const SReadMeta> Meta;              // the file's meta as read for it; null: none (or unreadable)
	std::optional<SFileMetaState> MetaColumns;          // the file row's meta columns to write; nullopt: keep the stored ones
	std::optional<SFileUpsert> MetaRow;                 // the meta's own row, committed with the record its values went into
	bool bHadRecord = false;                            // the view held a record of the file
	std::vector<FString> FileRemoves;                   // files of the pass this record moved from (ConvertBatch's move pairing)
	std::optional<STombstone> LostIdentity;             // the stored GUID, when the meta gave the record another one
	std::optional<SMetaCopy> MetaCopy;                  // METAS: the Ok meta read from the file, when the view's copy differs
	std::vector<SAssetId> KeptAliases;                  // a lost meta's: the stored consolidated aliases (KeepStoredRedirects)
	std::vector<FString> KeptFormerPaths;
};

// One analyzed file's delta: its file row (the stat the scan saw, the hash when Analyze computed one), record and
// Analyze problems.
struct SAnalyzedFile
{
	FString Path;
	uint64 Sequence = 0u;
	SRegistryDelta Delta;
};

struct SAnalyzeOutcome
{
	std::vector<SAnalyzedFile> Files;
	std::vector<SAnalyzeRequest> Retry;                 // changed while read: again after the settle window
	uint32 Analyzed = 0u;
	uint32 Hashed = 0u;
	uint32 EngineAnalyzed = 0u;
};

// KeepsChanging: a file whose every Analyze reports bSourceChanged (CAssetRegistry::SetSourceChangedForTest; empty:
// none).
SAnalyzeOutcome RunAnalyze (std::vector<SAnalyzeRequest>& Requests, const CRulesEngine& Rules, const CMountTable& Mounts, uint32 MaxRetries,
	FStringView KeepsChanging)
{
	SAnalyzeOutcome outcome;
	for (SAnalyzeRequest& request : Requests)
	{
		if (jobs::CWorkerPool::IsCancelled())
		{
			break;
		}
		const SMount& mount = Mounts.GetMounts()[request.Mount];
		const std::optional<SAnalyzeInput> input = SAnalyzeInput::FromMount(mount, request.Path);
		if (!input.has_value())
		{
			continue;
		}
		SAnalyzeResult result;
		(void)AnalyzeFile(*input, request.Settings, result);
		result.bSourceChanged = result.bSourceChanged || (!KeepsChanging.IsEmpty() && request.Path == KeepsChanging);
		++outcome.Analyzed;
		outcome.EngineAnalyzed += mount.Kind == EMountKind::Engine ? 1u : 0u;
		if (result.bSourceChanged && request.Retries < MaxRetries)
		{
			++request.Retries;
			outcome.Retry.push_back(std::move(request));
			continue;
		}
		if (result.bSourceChanged)
		{
			result.Status = EImportStatus::AnalyzeFailed;
			result.ContentHash.reset();
			SDiagnostic diagnostic;
			diagnostic.Severity = ESeverity::Error;
			diagnostic.Subject = request.Path;
			diagnostic.File = input->SourceFile;
			diagnostic.Message = FString::Format("the file kept changing while it was read ({} attempts); it is analyzed again when it changes",
				MaxRetries + 1u);
			result.Diagnostics.push_back(std::move(diagnostic));
		}
		outcome.Hashed += result.ContentHash.has_value() ? 1u : 0u;

		// Type and Label selectors see the record's type now (their rules may set cook rule, chunk and labels, never
		// the importer or an Analyze-relevant setting, so the stamp stays the request's).
		const SScanScope& scope = *request.Scope;
		const SAssetMeta* meta = request.Meta != nullptr ? request.Meta->GetValues() : nullptr;
		SEffectiveFileSettings settings = Rules.ResolveFile(scope, request.Path, SFileFacts { result.Type, meta }, false);
		if (settings.Importer != request.Settings.Importer)
		{
			settings = request.Settings;
		}
		settings.Settings.AnalyzeHash = request.Settings.Settings.AnalyzeHash;   // with the filter hash ConvertBatch folded in
		const bool bTypeIncluded = Rules.IsTypeIncluded(scope, result.Type);
		RemoveSubEntries(result, request.Path, [&] (reflect::TypeId Type) { return Rules.IsTypeIncluded(scope, Type); });
		if (meta != nullptr)
		{
			ApplySubAssetOverrides(result, *input, *meta, Rules.GetMetaProvenance(scope, request.Path));
		}
		for (SDiagnostic& diagnostic : result.Diagnostics)
		{
			Rules.ApplyValidation(scope, diagnostic);
		}

		SAnalyzedFile& file = outcome.Files.emplace_back();
		file.Path = request.Path;
		file.Sequence = request.Sequence;
		file.Delta.FileRemoves = request.FileRemoves;   // a move: removed in the commit that adds it
		// The file's new stat goes in with its record, never before it (ConvertBatch leaves it out of the conversion
		// for a file that has a record): a pass cut short before this delta leaves the stat the old record was made
		// from, so the next scan finds the file changed and analyzes it again.
		SFileUpsert row;
		row.MountPath = request.Path;
		row.Size = request.Size;
		row.MTime = request.MTime;
		row.Identity = request.Identity;
		if (result.ContentHash.has_value())
		{
			row.ContentHash = result.ContentHash;
		}
		else if (request.bContentChanged)
		{
			row.ContentHash = hash::SContentHash {};   // changed, not hashed (hashes are lazy, §5.4 step 5)
		}
		row.Class = bTypeIncluded ? request.Class : EFileClass::Excluded;
		row.Meta = request.MetaColumns;
		file.Delta.FileUpserts.push_back(std::move(row));
		if (request.MetaRow.has_value())
		{
			file.Delta.FileUpserts.push_back(std::move(*request.MetaRow));
		}
		AppendRecordUpsert(std::move(result), settings, request.Path, file.Delta, request.Guid, meta);
		SRecordUpsert& record = file.Delta.RecordUpserts.back();
		if (!bTypeIncluded)
		{
			record.Flags |= ERecordFlags::TypeExcluded;
		}
		if (meta == nullptr)
		{
			record.Aliases.insert(record.Aliases.end(), request.KeptAliases.begin(), request.KeptAliases.end());
			record.FormerPaths = std::move(request.KeptFormerPaths);
		}
		AppendLostIdentity(std::move(request.LostIdentity), file.Delta);
		if (request.MetaCopy.has_value())
		{
			file.Delta.MetaCopies.push_back(std::move(*request.MetaCopy));
		}
	}
	return outcome;
}

// The folders of a pass whose RULE002 is recomputed once its deltas are committed: the files the scan classified
// Unclaimed there, with their quiet flags.
struct SUnclaimedFolder
{
	FString Path;
	std::shared_ptr<const SScanScope> Scope;
	std::vector<std::pair<FString, bool>> Candidates;
};

/**
 * RULE002 for a pass's folders, once companions are known (§3.5.2): the Unclaimed lists that changed. Reads only an
 * acquired view (every delta of the pass committed) and the folders' immutable scopes, so it runs on a worker.
 */
SRegistryDelta MakeUnclaimedDelta (const CRegistryView& View, const CRulesEngine& Rules, std::span<const SUnclaimedFolder> Folders)
{
	SRegistryDelta delta;
	std::vector<SUnclaimedFile> files;
	for (const SUnclaimedFolder& folder : Folders)
	{
		files.clear();
		for (const auto& [path, bQuiet] : folder.Candidates)
		{
			const std::optional<SStoredFile> stored = FindStoredFile(View, path);
			if (stored.has_value() && stored->Class == EFileClass::Unclaimed)
			{
				files.push_back(SUnclaimedFile { path.View(), bQuiet });
			}
		}
		std::vector<SDiagnostic> diagnostics = ReportUnclaimedFiles(files);
		for (SDiagnostic& diagnostic : diagnostics)
		{
			if (folder.Scope != nullptr)
			{
				Rules.ApplyValidation(*folder.Scope, diagnostic);
			}
		}
		// Only a list that changed is set: a warm start rewrites nothing.
		std::vector<SDiagnostic> stored;
		View.ForEachDiagnosticAt(folder.Path, [&] (EProblemSource Source, const SDiagnostic& Diagnostic)
		{
			if (Source == EProblemSource::Unclaimed)
			{
				stored.push_back(Diagnostic);
			}
		});
		if (stored != diagnostics)
		{
			delta.Problems.push_back(SProblemsSet { folder.Path, EProblemSource::Unclaimed, std::move(diagnostics) });
		}
	}
	return delta;
}

struct SUnclaimedJob
{
	std::shared_ptr<const CRegistryView> View;
	std::vector<SUnclaimedFolder> Folders;
	SRegistryDelta Delta;
	double Milliseconds = 0.0;
};

// Files (or folders) per conversion delta: a pass of a whole mount is committed in pieces, each well inside a frame's
// commit budget (FrameBudgetMs), never in one delta.
constexpr uint32 ConversionDeltaFiles = 256u;

struct SConversion
{
	// In commit order: the removals first, then the new folders, then the files' rows and records in batch order
	// (each file's in one delta), the subtree's Scan and Rules problems in the last.
	std::vector<SRegistryDelta> Deltas;
	// The RULEFILES rows the batch changed (the mount's Config/Assets.yml included): committed last, with RULE002,
	// once every record they cover is committed (CheckPassDone), never by a pass cut short.
	SRegistryDelta RuleFiles;
	std::vector<SAnalyzeRequest> Analyze;
	std::vector<SUnclaimedFolder> Folders;
	std::vector<FString> RemovedFolders;
	std::vector<FString> RemovedFiles;
	uint32 ReEvaluated = 0u;
	uint32 MetasRead = 0u;
	bool bRuleFilesChanged = false;
	double Milliseconds = 0.0;
};

struct SConvertInput
{
	SScanBatch Batch;
	std::shared_ptr<const CRegistryView> View;          // the view the scan compared against
	const CRulesEngine* Rules = nullptr;
	const SMount* Mount = nullptr;
	jobs::CWorkerPool* Pool = nullptr;                  // the files are decided in chunks on it
	uint32 MountIndex = 0u;
	bool bReEvaluateAll = false;                        // the mount's chain changed (mount file, engine file, RulesBasis)
	// A scan of the mount root: the RulesHash of the mount's Config/Assets.yml the scan's scopes were made with, whose
	// RULEFILES row goes into the same delta as the records it re-evaluates.
	std::optional<uint64> MountRulesHash;
	FString KeepsChanging;                              // CAssetRegistry::SetSourceChangedForTest
};

/**
 * A scan batch as deltas of bounded size and the work that follows them (the header comment's step 2). Reads only
 * the view the scan compared against and the immutable rules scopes the batch carries, so it runs on a worker.
 */
SConversion ConvertBatch (SConvertInput& In)
{
	const uint64 start = platform::ReadTimestamp();
	SConversion out;
	SScanBatch& batch = In.Batch;
	const CRegistryView& view = *In.View;
	const int64 now = GetFileTimeNow();   // the removal time of the tombstones this batch writes
	const CRulesEngine& rules = *In.Rules;
	const SMount& mount = *In.Mount;
	const FString& root = batch.RootMountPath;
	const std::span<const FString> failed = batch.FailedFolders;
	SRegistryDelta& ruleFiles = out.RuleFiles;
	std::vector<SFolderUpsert> folderUpserts;

	if (In.MountRulesHash.has_value() && *In.MountRulesHash != GetStoredMountRulesHash(view, mount.Name))
	{
		out.bRuleFilesChanged = true;
		if (*In.MountRulesHash != 0u)
		{
			ruleFiles.RuleFileUpserts.push_back(SRuleFileUpsert { mount.Name, FString(), *In.MountRulesHash });
		}
		else
		{
			ruleFiles.RuleFileRemoves.push_back(SRuleFileUpsert { mount.Name, FString(), 0u });
		}
	}

	// Folders: rows the view lacks, RULEFILES rows, and which folders' rule chains changed (inherited downwards).
	std::vector<uint8> dirty(batch.Folders.size(), uint8(0));
	size_t knownFolders = 0u;   // batch folders the view holds (byte-exact)
	TStringMap<std::shared_ptr<const SScanScope>> scopeOf;
	FStringSet batchFolders;
	scopeOf.reserve(batch.Folders.size());
	batchFolders.reserve(batch.Folders.size());
	for (uint32 i = 0u; i < batch.Folders.size(); ++i)
	{
		const SScanFolder& folder = batch.Folders[i];
		batchFolders.insert(folder.MountPath);
		scopeOf.emplace(folder.MountPath, folder.Scope);
		const bool bParentDirty = folder.Parent != ~0u && dirty[folder.Parent] != 0u;
		if (IsAtOrBelowAny(folder.MountPath, failed))
		{
			dirty[i] = bParentDirty ? 1u : 0u;   // what is known at or below a failed folder stays
			continue;
		}
		uint64 storedHash = 0u;
		if (const std::optional<uint32> row = view.FindRuleFile(mount.Name, folder.MountPath); row.has_value())
		{
			SRuleFileData data;
			storedHash = view.GetRuleFile(*row, data) ? data.RulesHash : 0u;
		}
		const uint64 hash = folder.bHasRuleFile ? folder.RulesHash : 0u;
		if (hash != storedHash)
		{
			out.bRuleFilesChanged = true;
			if (hash != 0u)
			{
				ruleFiles.RuleFileUpserts.push_back(SRuleFileUpsert { mount.Name, folder.MountPath, hash });
			}
			else
			{
				ruleFiles.RuleFileRemoves.push_back(SRuleFileUpsert { mount.Name, folder.MountPath, 0u });
			}
		}
		dirty[i] = (In.bReEvaluateAll || hash != storedHash || bParentDirty) ? 1u : 0u;

		bool bKnown = false;
		if (const std::optional<uint32> row = view.FindFolder(folder.MountPath); row.has_value())
		{
			SFolderData data;
			bKnown = view.GetFolder(*row, data) && data.MountPath == folder.MountPath.View();
		}
		if (!bKnown)
		{
			folderUpserts.push_back(SFolderUpsert { folder.MountPath });
		}
		knownFolders += bKnown ? 1u : 0u;
		out.Folders.push_back(SUnclaimedFolder { folder.MountPath, folder.Scope, {} });
	}

	// Files, in chunks on the pool (each file's decision reads only the view and its folder's scope); the chunks'
	// results are joined in batch order into one delta per ConversionDeltaFiles files of the batch, so the deltas are
	// the same whatever the thread count.
	std::vector<uint32> unclaimedFolderOf(batch.Folders.size(), ~0u);   // batch folder -> index in out.Folders
	for (uint32 i = 0u, next = 0u; i < batch.Folders.size(); ++i)
	{
		if (next < out.Folders.size() && out.Folders[next].Path == batch.Folders[i].MountPath)
		{
			unclaimedFolderOf[i] = next++;
		}
	}
	struct SChunkOut
	{
		uint32 FirstPart = 0u;                              // Parts[i] is the batch's delta FirstPart + i
		std::vector<SRegistryDelta> Parts;                  // file rows, record removals and upserts
		std::vector<SAnalyzeRequest> Analyze;
		std::vector<std::pair<uint32, uint32>> Unclaimed;   // (index in out.Folders, file index)
		uint32 ReEvaluated = 0u;
		uint32 MetasRead = 0u;
		uint32 Known = 0u;                                  // files and metas the view holds (byte-exact)
	};
	std::vector<FString> paths(batch.Files.size());
	// Each file's meta (SScanMeta::SourceFile): the one spelled byte-exact wins when a case-sensitive folder holds two that
	// fold to its name; a meta no file takes is converted on its own below (an orphan, or one whose source is excluded).
	std::vector<uint32> metaOfFile(batch.Files.size(), ~0u);
	std::vector<FString> metaPaths(batch.Metas.size());
	for (const bool bExact : { true, false })
	{
		for (uint32 m = 0u; m < batch.Metas.size(); ++m)
		{
			const SScanMeta& meta = batch.Metas[m];
			if (meta.SourceFile == ~0u || metaOfFile[meta.SourceFile] != ~0u)
			{
				continue;
			}
			const bool bSpelledExactly = GetMetaSource(meta.Name) == batch.Files[meta.SourceFile].Name.View()
				&& meta.Name.View().EndsWith(FStringView(MetaFileSuffix));
			if (bSpelledExactly == bExact)
			{
				metaOfFile[meta.SourceFile] = m;
			}
		}
	}
	const std::vector<SChunkRange> chunks = SplitIntoChunks(In.Pool, static_cast<uint32>(batch.Files.size()), 256u);
	std::vector<SChunkOut> chunkOuts(chunks.size());
	RunChunks(In.Pool, chunks, [&] (uint32 Chunk, uint32 Begin, uint32 End)
	{
		SChunkOut& chunkOut = chunkOuts[Chunk];
		chunkOut.FirstPart = Begin / ConversionDeltaFiles;
		chunkOut.Parts.resize(End > Begin ? (End - 1u) / ConversionDeltaFiles - chunkOut.FirstPart + 1u : 0u);
		for (uint32 index = Begin; index < End; ++index)
		{
			SRegistryDelta& part = chunkOut.Parts[index / ConversionDeltaFiles - chunkOut.FirstPart];
			const SScanFile& file = batch.Files[index];
			const SScanFolder& folder = batch.Folders[file.Folder];
			paths[index] = MakeChildMountPath(folder.MountPath, file.Name);
			const FString& path = paths[index];
			const bool bDirty = dirty[file.Folder] != 0u;
			if (file.Verdict.Class == EFileClass::Unclaimed && unclaimedFolderOf[file.Folder] != ~0u)
			{
				chunkOut.Unclaimed.emplace_back(unclaimedFolderOf[file.Folder], index);
			}
			const std::optional<SStoredFile> stored = FindStoredFile(view, path);
			chunkOut.Known += stored.has_value() ? 1u : 0u;
			// The scan's Unchanged test (ScanTypes.h), against the view.
			const EScanFileState state = ClassifyKnown(stored.has_value() ? std::optional<SKnownFile>(stored->Stat) : std::nullopt, file.Size,
				file.MTime, file.Identity);
			const bool bHasRecord = stored.has_value() && stored->Record.IsValid();
			const bool bContentChanged = state == EScanFileState::New || state == EScanFileState::Changed;
			const bool bStatChanged = state != EScanFileState::Unchanged;
			const bool bStale = bHasRecord && HasAnyFlags(stored->Flags, ERecordFlags::StaleTags);
			const bool bTypeExcluded = bHasRecord && HasAnyFlags(stored->Flags, ERecordFlags::TypeExcluded);
			// The scan's verdict knows no meta: a meta's Importer pin can make the stored class differ from it, which the
			// meta read below decides.
			const bool bVerdictDiffers = stored.has_value() && stored->BaseClass != file.Verdict.Class
				&& !(bTypeExcluded && stored->BaseClass == EFileClass::Excluded);
			const bool bMissingRecord = stored.has_value() && !bHasRecord && file.Verdict.Class != EFileClass::Unclaimed;

			// Its meta: a FILES row of its own, compared like the file; its bytes are read only when it changed or when
			// the file is worked on and needs its values (§5.4: an unchanged meta of an unchanged file is never opened).
			const uint32 metaIndex = metaOfFile[index];
			const SScanMeta* meta = metaIndex != ~0u ? &batch.Metas[metaIndex] : nullptr;
			std::optional<SStoredFile> storedMeta;
			EScanFileState metaState = EScanFileState::Unchanged;
			if (meta != nullptr)
			{
				metaPaths[metaIndex] = MakeChildMountPath(folder.MountPath, meta->Name);
				storedMeta = FindStoredFile(view, metaPaths[metaIndex]);
				chunkOut.Known += storedMeta.has_value() ? 1u : 0u;
				metaState = ClassifyKnown(storedMeta.has_value() ? std::optional<SKnownFile>(storedMeta->Stat) : std::nullopt, meta->Size, meta->MTime,
					meta->Identity);
			}
			const bool bMetaBytesChanged = meta != nullptr && (metaState == EScanFileState::New || metaState == EScanFileState::Changed);
			if (!bStatChanged && stored.has_value() && !bDirty && !bStale && !bVerdictDiffers && !bMissingRecord && !bMetaBytesChanged)
			{
				if (meta != nullptr && metaState == EScanFileState::IdentityChanged)
				{
					part.FileUpserts.push_back(MakeMetaRow(metaPaths[metaIndex], *meta, std::nullopt));
				}
				continue;   // unchanged file, unchanged meta, unchanged rule chain: nothing to do (the warm start's path)
			}

			std::shared_ptr<const SReadMeta> read;
			if (meta != nullptr)
			{
				// An unchanged meta's values from the view's METAS copy (a rule-chain change, a pinned file's class); a
				// changed one, or one with no verified copy, from the file.
				if (!bMetaBytesChanged && storedMeta.has_value() && stored.has_value())
				{
					read = ReadMetaCopy(view, *stored);
				}
				if (read == nullptr)
				{
					read = ReadMetaFile(mount, metaPaths[metaIndex], In.KeepsChanging);
					++chunkOut.MetasRead;
				}
				if (read == nullptr)
				{
					// Changed while read (git or an editor writing it) or not opened: nothing of the file this pass, so
					// its stored row and record stand, and no record is made without its meta (a provisional id, no
					// Importer pin, a stamp the next read changes). The write's watcher event or the next scan reads it
					// again: its row still has the stat of the last read.
					continue;
				}
			}
			const SAssetMeta* metaValues = read != nullptr ? read->GetValues() : nullptr;
			const std::optional<SFileMetaState> metaColumns = read != nullptr ? std::optional<SFileMetaState>(read->GetColumns()) : std::nullopt;
			const SGuid guid = ChooseRecordGuid(read.get(), stored, path);
			// The meta's row goes with what used its values: an unreadable meta commits nothing, so it is read again.
			std::optional<SFileUpsert> metaRow;
			if (read != nullptr && (metaState != EScanFileState::Unchanged || storedMeta->MetaState != metaColumns->State
				|| storedMeta->MetaHash != metaColumns->Hash || storedMeta->BaseClass != EFileClass::Meta))
			{
				metaRow = MakeMetaRow(metaPaths[metaIndex], *meta, metaColumns);
			}
			const auto addMetaRow = [&]
			{
				if (metaRow.has_value())
				{
					part.FileUpserts.push_back(std::move(*metaRow));
					metaRow.reset();
				}
			};

			const SScanScope& scope = *folder.Scope;
			const SFileFacts facts { bHasRecord ? stored->Type : reflect::InvalidTypeId, metaValues };
			SEffectiveFileSettings settings = rules.ResolveFile(scope, path, facts, false);
			// The stamp covers what filters Analyze's result too (the Types filter of its sub-entries, Validation of its
			// problems: GetAnalyzeFilterHash), so an edit of either analyzes the scope's files again rather than leaving
			// their records filtered by the old lists. Nothing filtered: 0, the stamp of the settings alone.
			settings.Settings.AnalyzeHash ^= rules.GetAnalyzeFilterHash(scope);
			const bool bTypeIncluded = !bHasRecord || rules.IsTypeIncluded(scope, stored->Type);
			const EFileClass fileClass = bTypeIncluded ? settings.Class : EFileClass::Excluded;
			const bool bClassDiffers = stored.has_value() && stored->BaseClass != fileClass;
			const bool bMetaColumnsDiffer = metaColumns.has_value()
				&& (!stored.has_value() || stored->MetaState != metaColumns->State || stored->MetaHash != metaColumns->Hash);
			const bool bRowChanged = !stored.has_value() || bStatChanged || bClassDiffers || bMetaColumnsDiffer;
			const auto addRow = [&]
			{
				SFileUpsert& row = part.FileUpserts.emplace_back();
				row.MountPath = path;
				row.Size = file.Size;
				row.MTime = file.MTime;
				row.Identity = file.Identity;
				if (!stored.has_value() || bContentChanged)
				{
					row.ContentHash = hash::SContentHash {};   // changed, not hashed (hashes are lazy, §5.4 step 5)
				}
				row.Class = fileClass;
				row.Meta = metaColumns;
			};

			const SImporterDesc* importer = settings.Importer;
			if (importer == nullptr || settings.Class == EFileClass::Excluded || settings.Class == EFileClass::Unclaimed)
			{
				if (bRowChanged)
				{
					addRow();
				}
				addMetaRow();
				if (bHasRecord)
				{
					part.RecordRemoves.push_back(path);   // its importer went away (Importers.Exclude, a rule)
					if (HasMetaGivenGuid(*stored, path))
					{
						part.MetaCopyRemoves.push_back(stored->Guid);   // unless another record holds it; the meta stays on disk
					}
				}
				continue;
			}
			const uint64 stamp = MakeAnalyzeStamp(importer->AnalyzeVersion, settings.Settings.AnalyzeHash);
			const bool bAnalyze = !bHasRecord || bContentChanged || bStale || stored->AnalyzeStamp != stamp || stored->Importer != importer->Name.View();
			if (bAnalyze)
			{
				// A file with a record gets its new row with its Analyze result (RunAnalyze), so what is persisted never
				// pairs a new stat with the old record; a file without one gets it now (the next scan analyzes a file
				// that has no record whatever its row says).
				if (bRowChanged && !bHasRecord)
				{
					addRow();
				}
				SAnalyzeRequest& request = chunkOut.Analyze.emplace_back();
				request.Path = path;
				request.Mount = In.MountIndex;
				request.Class = settings.Class;
				request.Settings = std::move(settings);
				request.Scope = folder.Scope;
				request.Size = file.Size;
				request.MTime = file.MTime;
				request.Identity = file.Identity;
				request.bContentChanged = !stored.has_value() || bContentChanged;
				request.bHadRecord = bHasRecord;
				request.Guid = guid;
				if (bHasRecord && stored->Guid != guid)
				{
					request.LostIdentity = MakeTombstone(view, *stored, path, now);
				}
				if (read != nullptr && read->IsOk() && !read->bFromCopy)
				{
					SRegistryDelta copies;
					AppendMetaCopy(view, read.get(), path, copies);
					if (!copies.MetaCopies.empty())
					{
						request.MetaCopy = std::move(copies.MetaCopies.front());
					}
				}
				if (metaValues == nullptr && bHasRecord && HasMetaGivenGuid(*stored, path))
				{
					SRecordUpsert kept;
					KeepStoredRedirects(view, stored->Record, kept);
					request.KeptAliases = std::move(kept.Aliases);
					request.KeptFormerPaths = std::move(kept.FormerPaths);
				}
				request.Meta = std::move(read);
				request.MetaColumns = metaColumns;
				request.MetaRow = std::move(metaRow);
				continue;
			}
			if (bRowChanged)
			{
				addRow();
			}
			addMetaRow();
			AppendMetaCopy(view, read.get(), path, part);
			if (bDirty || bClassDiffers || bMetaBytesChanged)
			{
				// Re-evaluated in memory: the rules' and the meta's outputs, and the meta's GUID, against the record.
				++chunkOut.ReEvaluated;
				const ERecordFlags flags = (stored->Flags & ~ERecordFlags::TypeExcluded) | (bTypeIncluded ? ERecordFlags::None : ERecordFlags::TypeExcluded);
				if (RuleOutputsDiffer(view, stored->Record, settings, flags) || stored->Guid != guid
					|| (metaValues != nullptr && MetaRedirectsDiffer(view, stored->Record, *metaValues)))
				{
					SRecordUpsert upsert = MakeRecordUpsertFromView(view, stored->Record, settings, flags);
					upsert.Guid = guid;
					if (metaValues != nullptr)
					{
						SetMetaRedirects(upsert, *metaValues);
					}
					else
					{
						for (SAssetId& alias : upsert.Aliases)
						{
							alias.Guid = alias.SubId != 0u ? guid : alias.Guid;   // a renamed sub-asset's alias names its record's id
						}
					}
					if (stored->Guid != guid)
					{
						AppendLostIdentity(MakeTombstone(view, *stored, path, now), part);
					}
					part.RecordUpserts.push_back(std::move(upsert));
				}
			}
		}
	});
	uint32 knownFiles = 0u;
	std::vector<SRegistryDelta> fileDeltas((batch.Files.size() + ConversionDeltaFiles - 1u) / ConversionDeltaFiles);
	for (SChunkOut& chunkOut : chunkOuts)
	{
		for (uint32 i = 0u; i < chunkOut.Parts.size(); ++i)
		{
			MergeDelta(fileDeltas[chunkOut.FirstPart + i], std::move(chunkOut.Parts[i]));
		}
		AppendMoved(out.Analyze, chunkOut.Analyze);
		for (const auto& [folder, file] : chunkOut.Unclaimed)
		{
			out.Folders[folder].Candidates.emplace_back(paths[file], batch.Files[file].Verdict.bQuiet);
		}
		out.ReEvaluated += chunkOut.ReEvaluated;
		out.MetasRead += chunkOut.MetasRead;
		knownFiles += chunkOut.Known;
	}

	// The metas no file of the batch took: orphans, and metas of files the rules exclude. Their rows keep their stat and
	// say Orphan; their bytes are not read (the lifecycle reads the few it needs).
	SRegistryDelta orphanMetas;
	for (uint32 m = 0u; m < batch.Metas.size(); ++m)
	{
		const SScanMeta& meta = batch.Metas[m];
		if (meta.SourceFile != ~0u && metaOfFile[meta.SourceFile] == m)
		{
			continue;
		}
		metaPaths[m] = MakeChildMountPath(batch.Folders[meta.Folder].MountPath, meta.Name);
		const std::optional<SStoredFile> stored = FindStoredFile(view, metaPaths[m]);
		knownFiles += stored.has_value() ? 1u : 0u;
		const EScanFileState state = ClassifyKnown(stored.has_value() ? std::optional<SKnownFile>(stored->Stat) : std::nullopt, meta.Size, meta.MTime,
			meta.Identity);
		const bool bBytesChanged = state == EScanFileState::New || state == EScanFileState::Changed;
		const SFileMetaState columns { EMetaState::Orphan, bBytesChanged || !stored.has_value() ? 0u : stored->MetaHash };
		if (state != EScanFileState::Unchanged || stored->BaseClass != EFileClass::Meta || stored->MetaState != columns.State
			|| stored->MetaHash != columns.Hash)
		{
			orphanMetas.FileUpserts.push_back(MakeMetaRow(metaPaths[m], meta, columns));
		}
	}

	// Removals: what the view knows at or below the root that the batch does not hold, outside failed folders. Every
	// batch entry the view knows is a distinct row below the root, so when the counts agree nothing is gone and the
	// lists are not compared (a warm start).
	SFolderData rootData;
	const std::optional<uint32> rootRow = view.FindFolder(root);
	if (rootRow.has_value() && view.GetFolder(*rootRow, rootData) && rootData.MountPath == root.View() && !IsAtOrBelowAny(root, failed))
	{
		std::vector<uint32> folderRows;
		view.ForEachFolder([&] (uint32 Row)
		{
			SFolderData data;
			if (view.GetFolder(Row, data) && data.Mount == rootData.Mount && data.PreorderBegin >= rootData.PreorderBegin
				&& data.PreorderBegin < rootData.PreorderEnd)
			{
				folderRows.push_back(Row);
			}
		});
		uint32 viewFiles = 0u;
		view.ForEachFileUnder(*rootRow, [&] (uint32) { ++viewFiles; });
		if (!failed.empty() || folderRows.size() != knownFolders || viewFiles != knownFiles)
		{
			FStringSet batchFiles(paths.begin(), paths.end());
			batchFiles.insert(metaPaths.begin(), metaPaths.end());
			std::vector<FString> goneFolders;
			for (const uint32 row : folderRows)
			{
				SFolderData data;
				if (view.GetFolder(row, data) && !batchFolders.contains(data.MountPath) && !IsAtOrBelowAny(data.MountPath, failed))
				{
					goneFolders.emplace_back(data.MountPath);
				}
			}
			std::sort(goneFolders.begin(), goneFolders.end(), [] (const FString& A, const FString& B) { return A.View() < B.View(); });
			for (FString& folder : goneFolders)
			{
				if (out.RemovedFolders.empty() || !IsMountPathAtOrBelow(folder.View(), out.RemovedFolders.back().View()))
				{
					out.RemovedFolders.push_back(std::move(folder));
				}
			}
			view.ForEachFileUnder(*rootRow, [&] (uint32 Row)
			{
				SFileData data;
				if (!view.GetFile(Row, data) || batchFiles.contains(data.MountPath) || IsAtOrBelowAny(data.MountPath, failed))
				{
					return;
				}
				if (batchFolders.contains(GetMountPathParent(data.MountPath)))
				{
					out.RemovedFiles.emplace_back(data.MountPath);   // else its folder's removal takes it
				}
			});
		}
	}

	/**
	 * Moves inside the pass (a rename, or a move below its root, with the meta): a removed file whose record's GUID a new
	 * file of the batch takes from its meta is removed in that file's Analyze delta instead, so one commit removes and
	 * adds the asset, which listeners get as one move (SAssetChangeSet::Moved) and never as two records with one id. A
	 * removed folder's files, a case-only rename and a move between two passes (the watcher's Moved scans both ends)
	 * stay a removal and an addition.
	 */
	std::vector<FString> fileRemoves = out.RemovedFiles;
	if (!fileRemoves.empty() && !out.Analyze.empty())
	{
		std::map<SGuid, size_t> movedTo;
		for (size_t i = 0u; i < out.Analyze.size(); ++i)
		{
			const SAnalyzeRequest& request = out.Analyze[i];
			if (!request.bHadRecord && request.Meta != nullptr && request.Guid.IsValid())
			{
				movedTo.try_emplace(request.Guid, i);
			}
		}
		std::erase_if(fileRemoves, [&] (const FString& Removed)
		{
			const std::optional<SStoredFile> stored = FindStoredFile(view, Removed);
			if (!stored.has_value() || !HasMetaGivenGuid(*stored, Removed))
			{
				return false;
			}
			const auto it = movedTo.find(stored->Guid);
			// A case-only rename keeps its removal first (the two spellings never share a commit, ConvertBatch's order).
			if (it == movedTo.end() || Removed.Equals<ESearchCase::IgnoreCase>(out.Analyze[it->second].Path))
			{
				return false;
			}
			out.Analyze[it->second].FileRemoves.push_back(Removed);
			movedTo.erase(it);
			return true;
		});
	}

	// The deltas, in commit order. Removals first, as within one delta: a folder or file the batch spells otherwise
	// (a case-only rename) is removed before its new spelling is added. Then the new folders (parents first), then
	// the files.
	if (!out.RemovedFolders.empty() || !fileRemoves.empty())
	{
		SRegistryDelta& removals = out.Deltas.emplace_back();
		// Every record that goes with a meta-given GUID leaves a tombstone (§4.6, §4.7: the lifecycle's grace, its
		// orphan pairing), one per GUID (two copies of one asset removed together leave the first's); a GUID the pass
		// registers again (a move), or another live record holds, loses it in the store.
		std::set<SGuid> tombstoned;
		const auto appendLost = [&] (std::optional<STombstone>&& Tombstone)
		{
			if (Tombstone.has_value() && tombstoned.insert(Tombstone->Guid).second)
			{
				AppendLostIdentity(std::move(Tombstone), removals);
			}
		};
		for (const FString& removed : fileRemoves)
		{
			if (const std::optional<SStoredFile> stored = FindStoredFile(view, removed); stored.has_value())
			{
				appendLost(MakeTombstone(view, *stored, removed, now));
			}
		}
		for (const FString& folder : out.RemovedFolders)
		{
			const std::optional<uint32> folderRow = view.FindFolder(folder);
			SFolderData folderData;
			if (!folderRow.has_value() || !view.GetFolder(*folderRow, folderData) || folderData.MountPath != folder.View())
			{
				continue;
			}
			view.ForEachFileUnder(*folderRow, [&] (uint32 Row)
			{
				SFileData data;
				if (view.GetFile(Row, data) && data.Record.IsValid() && data.MetaState != EMetaState::PendingMeta)
				{
					if (const std::optional<SStoredFile> stored = FindStoredFile(view, data.MountPath); stored.has_value())
					{
						appendLost(MakeTombstone(view, *stored, data.MountPath, now));
					}
				}
			});
		}
		removals.FolderRemoves = out.RemovedFolders;
		removals.FileRemoves = std::move(fileRemoves);
	}
	for (size_t i = 0u; i < folderUpserts.size(); i += ConversionDeltaFiles)
	{
		const size_t end = std::min(folderUpserts.size(), i + ConversionDeltaFiles);
		SRegistryDelta& folders = out.Deltas.emplace_back();
		folders.FolderUpserts.assign(std::make_move_iterator(folderUpserts.begin() + static_cast<ptrdiff_t>(i)),
			std::make_move_iterator(folderUpserts.begin() + static_cast<ptrdiff_t>(end)));
	}
	for (SRegistryDelta& files : fileDeltas)
	{
		if (!files.IsEmpty())
		{
			out.Deltas.push_back(std::move(files));
		}
	}
	for (size_t i = 0u; i < orphanMetas.FileUpserts.size(); i += ConversionDeltaFiles)
	{
		const size_t end = std::min(orphanMetas.FileUpserts.size(), i + ConversionDeltaFiles);
		SRegistryDelta& metas = out.Deltas.emplace_back();
		metas.FileUpserts.assign(std::make_move_iterator(orphanMetas.FileUpserts.begin() + static_cast<ptrdiff_t>(i)),
			std::make_move_iterator(orphanMetas.FileUpserts.begin() + static_cast<ptrdiff_t>(end)));
	}

	// Problems of the subtree: the scan's own (with Validation: severities) and the rule files'.
	for (SDiagnostic& diagnostic : batch.Diagnostics)
	{
		auto it = scopeOf.find(diagnostic.Subject);
		if (it == scopeOf.end())
		{
			it = scopeOf.find(GetMountPathParent(diagnostic.Subject));
		}
		if (it != scopeOf.end() && it->second != nullptr)
		{
			rules.ApplyValidation(*it->second, diagnostic);
		}
	}
	SRegistryDelta problems;
	ReplaceSubtreeProblems(problems, view, root, EProblemSource::Scan, std::move(batch.Diagnostics));
	ReplaceSubtreeProblems(problems, view, root, EProblemSource::Rules, std::move(batch.RuleDiagnostics));
	if (!problems.IsEmpty())
	{
		if (out.Deltas.empty())
		{
			out.Deltas.emplace_back();
		}
		MergeDelta(out.Deltas.back(), std::move(problems));
	}
	out.Milliseconds = MillisecondsSince(start);
	return out;
}
}


// ---------------- The service ----------------

void AddRecordChanges (std::span<const SRecordChange> Changes, SAssetChangeSet& Out)
{
	// Per id and path, the window's first and last change there: the id held the path before the window when its first
	// change is no Added, and holds it after when its last is no Removed.
	struct SSpan
	{
		size_t First = 0u;
		size_t Last = 0u;
	};
	std::map<std::pair<SAssetId, FStringView>, SSpan> spans;
	for (size_t i = 0u; i < Changes.size(); ++i)
	{
		const auto [it, bInserted] = spans.try_emplace(std::pair<SAssetId, FStringView>(Changes[i].Id, Changes[i].MountPath.View()), SSpan { i, i });
		it->second.Last = i;
	}

	// An id that left paths it held and holds paths it did not: each such pair, oldest first, is one move. The first
	// Added at the new path goes to Moved and the last Removed at the old one is dropped; every other change stays.
	enum class EUse : uint8 { Listed, Moved, Dropped };
	std::vector<EUse> use(Changes.size(), EUse::Listed);
	std::vector<size_t> left;
	std::vector<size_t> arrived;
	for (auto it = spans.begin(); it != spans.end();)
	{
		left.clear();
		arrived.clear();
		const SAssetId id = it->first.first;
		for (; it != spans.end() && it->first.first == id; ++it)
		{
			const bool bBefore = Changes[it->second.First].Kind != ERecordChange::Added;
			const bool bAfter = Changes[it->second.Last].Kind != ERecordChange::Removed;
			if (bBefore && !bAfter)
			{
				left.push_back(it->second.Last);
			}
			else if (!bBefore && bAfter)
			{
				arrived.push_back(it->second.First);
			}
		}
		std::sort(left.begin(), left.end());
		std::sort(arrived.begin(), arrived.end());
		for (size_t k = 0u; k < left.size() && k < arrived.size(); ++k)
		{
			use[left[k]] = EUse::Dropped;
			use[arrived[k]] = EUse::Moved;
		}
	}

	for (size_t i = 0u; i < Changes.size(); ++i)
	{
		const SRecordChange& change = Changes[i];
		std::vector<SAssetChange>& list = use[i] == EUse::Moved ? Out.Moved
			: change.Kind == ERecordChange::Added ? Out.Added
			: change.Kind == ERecordChange::Removed ? Out.Removed : Out.Modified;
		if (use[i] != EUse::Dropped)
		{
			list.push_back(SAssetChange { change.Id, change.MountPath });
		}
	}
}


struct CAssetRegistry::SImpl final : IAssetRepairsHost
{
	struct SScanRequest
	{
		FString Root;
		bool bReEvaluateAll = false;
		bool bInitial = false;
		bool bUnclaimed = false;         // RULE002 recomputed whatever the pass finds (RequestCompanionFolderScans)
	};

	struct SMountQueue
	{
		std::deque<SScanRequest> Requests;
		uint64 ActivePass = 0u;
	};

	struct SPass
	{
		uint32 Mount = 0u;
		FString Root;
		bool bInitial = false;
		bool bReEvaluateAll = false;
		bool bUnclaimed = false;                       // SScanRequest::bUnclaimed
		std::shared_ptr<const CRegistryView> View;     // what the conversion compares the batch with
		std::optional<SScanBatch> Batch;               // scanned while the snapshots loaded: converted once they are
		std::optional<uint64> MountRulesHash;
		bool bConverted = false;
		bool bUnclaimedRunning = false;  // RULE002 on a worker
		bool bUnclaimedDone = false;     // ... asked for, or not needed
		bool bFinalQueued = false;       // the RULEFILES rows and RULE002 (CheckPassDone)
		bool bLost = false;              // cut short: a conversion or Analyze work dropped unrun
		bool bRuleFilesChanged = false;
		bool bFilesChanged = false;      // a delta of it changed files or records (RULE002 may differ)
		uint32 PendingDeltas = 0u;       // queued, not committed
		uint32 OutstandingAnalyze = 0u;  // tasks in the pool or waiting to be retried
		std::vector<SUnclaimedFolder> Folders;
		SRegistryDelta RuleFiles;        // SConversion::RuleFiles
		SRegistryDelta Unclaimed;        // RULE002's lists that changed
		std::vector<FString> Dirty;      // the paths its deltas touched (the lifecycle's events), with a lifecycle only
	};

	struct SQueuedDelta
	{
		SRegistryDelta Delta;
		uint64 Pass = 0u;                // 0: belongs to no pass
	};

	struct SRetry
	{
		uint64 Deadline = 0u;
		uint64 Pass = 0u;
		std::vector<SAnalyzeRequest> Requests;
	};

	struct SWatchEvent
	{
		uint32 Mount = 0u;
		EFileChange Kind = EFileChange::Modified;
		bool bDirectory = false;
		FString Path;
		std::optional<FString> OldPath;
	};

	struct SSnapshotJob
	{
		std::shared_ptr<const CRegistryView> View;
		std::vector<MountId> Mounts;
		std::filesystem::path Path;
		SGuid ProjectId;
		uint64 RulesBasis = 0u;
		CJournalRestart Restart;
		bool bHasRestart = false;
		bool bWritten = false;
		FString Error;
		SSnapshotWriteStats Stats;
	};

	explicit SImpl (SAssetRegistryDesc InDesc)
		: Desc(std::move(InDesc))
		, Mounts(Desc.Mounts)
		, Pool(*Desc.Pool)
		, Store(std::make_unique<CRegistryStore>())
		, Rules(std::make_unique<CRulesEngine>(*Desc.Mounts, Desc.RulesOptions))
	{
		frt_assert(Mounts != nullptr && Desc.Pool != nullptr && Pool.IsOwnerThread());
		CancelGroup = Pool.CreateCancelGroup();
		MountQueues.resize(Mounts->GetMounts().size());
		const SMount* engine = Mounts->Find(EngineMountId);
		const SMount* project = Mounts->Find(ProjectMountId);
		frt_assert(engine != nullptr);
		EngineDir = engine->OwnerRoot;
		if (Desc.Owner == EAssetRegistryOwner::Engine || project == nullptr)
		{
			Desc.Owner = EAssetRegistryOwner::Engine;
			OwnerRoot = EngineDir;
			SnapshotPath = GetEngineSnapshotPath(EngineDir);
			SnapshotMounts = { EngineMountId };
			SnapshotMountNames = { engine->Name };
			bWithEngineFallback = false;
		}
		else
		{
			OwnerRoot = project->OwnerRoot;
			SnapshotPath = GetRegistrySnapshotPath(OwnerRoot);
			for (const SMount& mount : Mounts->GetMounts())
			{
				if (mount.Kind != EMountKind::Engine)
				{
					SnapshotMounts.push_back(mount.Id);
					SnapshotMountNames.push_back(mount.Name);
				}
			}
			bWithEngineFallback = true;
		}
		if (Desc.Lifecycle != nullptr)
		{
			SAssetRepairsDesc repairs;
			repairs.Lifecycle = Desc.Lifecycle;
			repairs.Mounts = Mounts.get();
			repairs.Rules = Rules.get();
			repairs.Pool = &Pool;
			repairs.Watcher = Desc.Watcher;
			repairs.Host = this;
			repairs.OwnerRoot = OwnerRoot;
			repairs.bWriter = Desc.bWriter;
			// The mounts of the owner root: /Project and its plugins, or /Engine. Written only by a writer; a guest's problems
			// there name their owner.
			repairs.OwnedMounts = SnapshotMounts;
			if (Desc.bWriter && Desc.bEngineMetasWritable && Desc.Owner == EAssetRegistryOwner::Project)
			{
				// Engine-owner mode: metas repaired in /Engine through a writer of its own root, nothing else of it written.
				repairs.EngineRoot = EngineDir;
				repairs.EngineMounts = { EngineMountId };
			}
			Repairs = std::make_unique<CAssetRepairs>(std::move(repairs));
		}
	}

	~SImpl () override = default;

	// ---- the lifecycle's host (IAssetRepairsHost) ----

	[[nodiscard]] const CRegistryView& GetLiveView () const override
	{
		return Store->Live();
	}

	[[nodiscard]] std::shared_ptr<const CRegistryView> AcquireView () const override
	{
		return Store->Acquire();
	}

	[[nodiscard]] bool IsQuiet () const override
	{
		return State == EScanState::Ready && !bStopping && IsIdle();
	}

	void CommitNow (SRegistryDelta&& Delta) override
	{
		CommitDelta(std::move(Delta));
	}

	void RequestRescan (std::span<const FString> MountPaths) override
	{
		for (const FString& path : MountPaths)
		{
			FStringView mountName;
			FStringView relative;
			if (const SMount* mount = SplitMountPath(path, mountName, relative) ? Mounts->Find(mountName) : nullptr; mount != nullptr)
			{
				RequestScan(mount->Id, ScanRootOf(path), false);
			}
		}
	}

	bool ScanNow (std::span<const FString> MountPaths) override
	{
		for (const FString& path : MountPaths)
		{
			RequestRescan(std::span<const FString>(&path, 1u));
		}
		return WaitUntil([this] { return IsIdle(); }, -1.0);
	}

	bool WaitUntil (TFunctionRef<bool ()> Done, double TimeoutSeconds) override
	{
		return WaitUntilImpl(Done, TimeoutSeconds);
	}

	void CountRepairs (uint32 Applied, uint32 Refused) override
	{
		Counters.RepairsApplied += Applied;
		Counters.RepairsRefused += Refused;
	}

	// ---- start-up ----

	void Start ()
	{
		frt_assert(State == EScanState::NotStarted);
		StartTimestamp = platform::ReadTimestamp();
		Rules->LoadMountRules(&MountRuleDiagnostics);
		if (Repairs != nullptr)
		{
			Repairs->Start();   // an unfinished operation is finished before anything is scanned
		}
		Subscribe();

		// The ids the snapshot files are tied to: a writer creates its own, a reader only reads (a cold start
		// without one).
		std::vector<SSnapshotSource> sources;
		if (Desc.bLoadSnapshots)
		{
			if (Desc.Owner == EAssetRegistryOwner::Project)
			{
				const std::optional<SGuid> engineId = ReadProjectId(EngineDir);
				sources.push_back(SSnapshotSource { GetEngineSnapshotPath(EngineDir), engineId.value_or(SGuid {}), false, Rules->GetRulesBasis(false) });
			}
		}
		std::optional<SGuid> ownId;
		if (Desc.bWriter)
		{
			FString error;
			ownId = ReadOrCreateProjectId(OwnerRoot, &error);
			if (!ownId.has_value())
			{
				log::Warning(LogCategory, "no registry ProjectId under {}: {}; the registry writes no snapshot this run", FString::FromPath(OwnerRoot), error);
			}
		}
		else
		{
			ownId = ReadProjectId(OwnerRoot);
		}
		ProjectId = ownId.value_or(SGuid {});
		if (Desc.bLoadSnapshots)
		{
			sources.push_back(SSnapshotSource { SnapshotPath, ProjectId, false, Rules->GetRulesBasis(bWithEngineFallback) });
		}

		// The initial scan of every mount: the scans run while the snapshots load (a batch is compared with the loaded
		// view only when it is converted), so the start-up costs the longer of the two, not their sum.
		InitialPassesLeft = static_cast<uint32>(Mounts->GetMounts().size());
		for (const SMount& mount : Mounts->GetMounts())
		{
			MountQueues[mount.Id].Requests.push_back(SScanRequest { mount.GetRootMountPath(), false, true });
		}
		const bool bAnyId = std::any_of(sources.begin(), sources.end(), [] (const SSnapshotSource& Source) { return Source.ProjectId.IsValid(); });
		if (!bAnyId)
		{
			OnLoaded({}, 0.0);
			return;
		}
		State = EScanState::LoadingSnapshots;
		bLoading = true;
		struct SLoadJob
		{
			std::vector<SSnapshotSource> Sources;
			std::vector<SMountUpsert> Mounts;
			SSnapshotLoadResult Result;
			double Milliseconds = 0.0;
		};
		auto job = std::make_shared<SLoadJob>();
		job->Sources = std::move(sources);
		job->Mounts = MakeMountUpserts();
		// The store is handed to the worker for the load and back by the completion (CRegistryStore::ReleaseOwner).
		Store->ReleaseOwner();
		Pool.Submit(jobs::EWorkPriority::High, jobs::NoCancelGroup,
			[job, store = Store.get(), pool = &Pool] ()
			{
				const uint64 start = platform::ReadTimestamp();
				job->Result = LoadSnapshots(*store, job->Sources, job->Mounts, pool);
				store->ReleaseOwner();
				job->Milliseconds = MillisecondsSince(start);
			},
			[this, job] () { OnLoaded(std::move(job->Result), job->Milliseconds); });
		StartPasses();
	}

	[[nodiscard]] std::vector<SMountUpsert> MakeMountUpserts () const
	{
		std::vector<SMountUpsert> upserts;
		for (const SMount& mount : Mounts->GetMounts())
		{
			upserts.push_back(SMountUpsert { mount.Name, mount.Volume.bCaseSensitive, mount.Id });
		}
		return upserts;
	}

	void OnLoaded (SSnapshotLoadResult&& Result, double Milliseconds)
	{
		bLoading = false;
		SnapshotReports = std::move(Result.Files);
		Initial.LoadMs = Milliseconds;
		for (const SSnapshotFileReport& report : SnapshotReports)
		{
			if (report.State == ESnapshotFileState::Rejected || report.State == ESnapshotFileState::Unreadable)
			{
				log::Info(LogCategory, "{} not used: {}", FString::FromPath(report.Path.filename()), report.Message);
			}
		}
		if (State == EScanState::ShutDown)
		{
			return;
		}

		// The writer's journal continues the snapshot it loaded (or starts anew).
		if (Desc.bWriter && ProjectId.IsValid())
		{
			std::optional<uint64> continueFrom;
			for (const SSnapshotFileReport& report : SnapshotReports)
			{
				if (report.State == ESnapshotFileState::Loaded && report.Path == SnapshotPath)
				{
					continueFrom = report.JournalBase;
				}
			}
			Journal.emplace();
			FString error;
			if (!Journal->Open(GetJournalPath(SnapshotPath), ProjectId, continueFrom, SnapshotMountNames, &error))
			{
				log::Warning(LogCategory, "the registry journal cannot be written: {}", error);
				Journal.reset();
			}
		}

		// Which mounts re-evaluate every record: their Config/Assets.yml differs from its RULEFILES row (the engine's for
		// every mount but /Engine shows in the RulesBasis instead), or their snapshot file lost its RULEFILES rows.
		std::vector<uint8> reEvaluate(Mounts->GetMounts().size(), uint8(0));
		for (const SSnapshotFileReport& report : SnapshotReports)
		{
			if (report.bRuleFilesDropped)
			{
				for (const FString& name : report.Mounts)
				{
					if (const SMount* mount = Mounts->Find(name.View()); mount != nullptr)
					{
						reEvaluate[mount->Id] = 1u;
					}
				}
			}
		}
		for (const SMount& mount : Mounts->GetMounts())
		{
			if (Rules->GetMountRulesHash(mount.Name) != GetStoredMountRulesHash(Store->Live(), mount.Name))
			{
				reEvaluate[mount.Id] = 1u;
			}
		}
		SRegistryDelta delta;
		delta.Mounts = MakeMountUpserts();
		AddProblemSets(delta, EProblemSource::Rules, std::move(MountRuleDiagnostics));
		MountRuleDiagnostics.clear();
		CommitDelta(std::move(delta));

		State = EScanState::Scanning;
		std::vector<uint64> parked;
		for (auto& [id, pass] : Passes)
		{
			pass.bReEvaluateAll = pass.bReEvaluateAll || (pass.bInitial && reEvaluate[pass.Mount] != 0u);
			pass.View = Store->Acquire();
			if (pass.Batch.has_value())
			{
				parked.push_back(id);
			}
		}
		for (SMountQueue& queue : MountQueues)
		{
			for (SScanRequest& request : queue.Requests)
			{
				request.bReEvaluateAll = request.bReEvaluateAll || (request.bInitial && reEvaluate[&queue - MountQueues.data()] != 0u);
			}
		}
		for (const uint64 id : parked)
		{
			SPass& pass = Passes.at(id);
			SScanBatch batch = std::move(*pass.Batch);
			pass.Batch.reset();
			if (bStopping)
			{
				pass.bConverted = true;
				pass.bLost = true;
				CheckPassDone(id);
				continue;
			}
			SubmitConversion(id, std::move(batch));
		}
		StartPasses();
	}

	// ---- the watcher ----

	// Subscribed even while the watcher is off ([Assets] Watch=0, every scripted run), as its other subscribers are: it
	// tells nothing until it is on, and a scripted step that turns it on for its own files (the smoke test's scratch copy,
	// where it adds a file whose meta the lifecycle must create) is told about them like any other change.
	void Subscribe ()
	{
		CFileWatcherService* watcher = Desc.Watcher;
		if (watcher == nullptr)
		{
			return;
		}
		for (const SMount& mount : Mounts->GetMounts())
		{
			SWatchFilter content;
			content.bVerify = false;
			Subscriptions.push_back(watcher->Subscribe(mount.ContentDir, content, [this, index = static_cast<uint32>(mount.Id)] (std::span<const SFileChange> Changes)
			{
				OnContentChanges(index, Changes);
			}));
			SWatchFilter rules;
			rules.Prefix = FString(RuleFileName);
			rules.bVerify = false;
			Subscriptions.push_back(watcher->Subscribe(mount.RulesFile.parent_path(), rules, [this] (std::span<const SFileChange>)
			{
				bMountRulesChanged = true;
			}));
		}
	}

	void Unsubscribe ()
	{
		if (Desc.Watcher != nullptr)
		{
			for (const SDelegateHandle handle : Subscriptions)
			{
				Desc.Watcher->Unsubscribe(handle);
			}
		}
		Subscriptions.clear();
	}

	// Only queues (a watcher callback may not pump or flush); the paths are mount paths of the subscription's mount.
	void OnContentChanges (uint32 MountIndex, std::span<const SFileChange> Changes)
	{
		const SMount& mount = Mounts->GetMounts()[MountIndex];
		const FString root = mount.GetRootMountPath();
		for (const SFileChange& change : Changes)
		{
			SWatchEvent event;
			event.Mount = MountIndex;
			event.Kind = change.Kind;
			event.bDirectory = change.bDirectory;
			if (!change.RelativePath.IsEmpty())
			{
				event.Path = MakeChildMountPath(root, change.RelativePath);
			}
			else if (const std::optional<FString> path = Mounts->ToMountPath(change.Path); path.has_value())
			{
				event.Path = *path;
			}
			else
			{
				event.Path = root;
			}
			if (change.Kind == EFileChange::Moved)
			{
				event.OldPath = Mounts->ToMountPath(change.OldPath);
			}
			WatchEvents.push_back(std::move(event));
		}
	}

	void ProcessWatchEvents ()
	{
		if (bMountRulesChanged)
		{
			bMountRulesChanged = false;
			ReloadMountRules();
		}
		std::vector<SWatchEvent> events = std::move(WatchEvents);
		WatchEvents.clear();
		for (const SWatchEvent& event : events)
		{
			if (event.bDirectory && event.Kind == EFileChange::Rescan)
			{
				RequestScan(event.Mount, event.Path, false);
			}
			else
			{
				RequestScan(event.Mount, ScanRootOf(event.Path), false);
			}
			if (event.OldPath.has_value())
			{
				FStringView mountName;
				FStringView relative;
				if (SplitMountPath(*event.OldPath, mountName, relative))
				{
					if (const SMount* oldMount = Mounts->Find(mountName); oldMount != nullptr)
					{
						RequestScan(oldMount->Id, ScanRootOf(*event.OldPath), false);
					}
				}
			}
		}
	}

	// A mount's Config/Assets.yml (or the engine's) changed: the rules again, the RULEFILES rows and mount-root Rules
	// lists, and a stat rescan of every mount whose chain changed (a membership edit can add files FILES never held).
	void ReloadMountRules ()
	{
		std::vector<SDiagnostic> diagnostics;
		std::vector<FString> changed;
		Rules->LoadMountRules(&diagnostics, &changed);
		// The RULEFILES rows of the mount files change with the rescans' last deltas, once the records they cover are
		// committed (CheckPassDone): a pass cut short never leaves a row that claims records it did not reach.
		SRegistryDelta delta;
		for (const SMount& mount : Mounts->GetMounts())
		{
			const FString root = mount.GetRootMountPath();
			std::vector<SDiagnostic> list;
			for (const SDiagnostic& diagnostic : diagnostics)
			{
				if (diagnostic.Subject == root.View())
				{
					list.push_back(diagnostic);
				}
			}
			delta.Problems.push_back(SProblemsSet { root, EProblemSource::Rules, std::move(list) });
		}
		Queue.push_back(SQueuedDelta { std::move(delta), 0u });
		for (const FString& name : changed)
		{
			if (const SMount* mount = Mounts->Find(name.View()); mount != nullptr)
			{
				RequestScan(mount->Id, mount->GetRootMountPath(), true);
			}
		}
		WriteRulesCache();
	}

	// Queues a scan of Root in its mount, merged with a queued one that covers it (or that it covers). bUnclaimed: its
	// RULE002 is recomputed even when none of its own files changed (RequestCompanionFolderScans).
	void RequestScan (uint32 MountIndex, FStringView Root, bool bReEvaluateAll, bool bUnclaimed = false)
	{
		std::deque<SScanRequest>& requests = MountQueues[MountIndex].Requests;
		for (SScanRequest& request : requests)
		{
			if (IsMountPathAtOrBelow(Root, request.Root.View()))
			{
				request.bReEvaluateAll = request.bReEvaluateAll || bReEvaluateAll;
				request.bUnclaimed = request.bUnclaimed || bUnclaimed;
				return;
			}
		}
		bool bMerged = bReEvaluateAll;
		bool bInitial = false;
		bool bMergedUnclaimed = bUnclaimed;
		std::erase_if(requests, [&] (const SScanRequest& Request)
		{
			if (!IsMountPathAtOrBelow(Request.Root.View(), Root))
			{
				return false;
			}
			bMerged = bMerged || Request.bReEvaluateAll;
			bInitial = bInitial || Request.bInitial;
			bMergedUnclaimed = bMergedUnclaimed || Request.bUnclaimed;
			return true;
		});
		requests.push_back(SScanRequest { FString(Root), bMerged, bInitial, bMergedUnclaimed });
	}

	// ---- passes ----

	void StartPasses ()
	{
		if (State == EScanState::ShutDown || bStopping)
		{
			return;
		}
		for (uint32 index = 0u; index < MountQueues.size(); ++index)
		{
			SMountQueue& queue = MountQueues[index];
			if (queue.ActivePass != 0u || queue.Requests.empty())
			{
				continue;
			}
			SScanRequest request = std::move(queue.Requests.front());
			queue.Requests.pop_front();
			const uint64 id = NextPassId++;
			SPass& pass = Passes[id];
			pass.Mount = index;
			pass.Root = std::move(request.Root);
			pass.bInitial = request.bInitial;
			pass.bReEvaluateAll = request.bReEvaluateAll;
			pass.bUnclaimed = request.bUnclaimed;
			pass.View = Store->Acquire();   // the published view (the empty one while loading: refreshed by OnLoaded)
			const SMount& mount = Mounts->GetMounts()[index];
			if (pass.Root == mount.GetRootMountPath())
			{
				pass.MountRulesHash = Rules->GetMountRulesHash(mount.Name);
			}
			queue.ActivePass = id;
			++ScansInFlight;
			CMountScanner::ScanAsync(Pool, Mounts->GetMounts()[index], pass.Root, *Rules, nullptr, [this, id] (SScanBatch&& Batch)
			{
				OnScanned(id, std::move(Batch));
			});
		}
	}

	void OnScanned (uint64 PassId, SScanBatch&& Batch)
	{
		--ScansInFlight;
		SPass& pass = Passes.at(PassId);
		Counters.Scanned += Batch.Files.size();
		if (!Initial.bComplete)
		{
			Initial.ScanMs += Batch.Milliseconds;
		}
		if (bStopping)
		{
			pass.bConverted = true;
			pass.bLost = true;
			CheckPassDone(PassId);
			return;
		}
		if (bLoading)
		{
			pass.Batch = std::move(Batch);
			return;
		}
		SubmitConversion(PassId, std::move(Batch));
	}

	void SubmitConversion (uint64 PassId, SScanBatch&& Batch)
	{
		SPass& pass = Passes.at(PassId);
		auto job = std::make_shared<std::pair<SConvertInput, SConversion>>();
		job->first.Batch = std::move(Batch);
		job->first.View = pass.View;
		job->first.Rules = Rules.get();
		job->first.Mount = &Mounts->GetMounts()[pass.Mount];
		job->first.MountIndex = pass.Mount;
		job->first.Pool = &Pool;
		job->first.bReEvaluateAll = pass.bReEvaluateAll;
		job->first.MountRulesHash = pass.MountRulesHash;
		job->first.KeepsChanging = SourceChangedForTest;
		pass.View.reset();
		++ConversionsInFlight;
		Pool.Submit(jobs::EWorkPriority::High, CancelGroup,
			[job] () { job->second = ConvertBatch(job->first); },
			[this, PassId, job, guard = CLostGuard([this, PassId] { OnConversionLost(PassId); })] () mutable
			{
				guard.Dismiss();
				OnConverted(PassId, std::move(job->second));
			});
	}

	void OnConversionLost (uint64 PassId)
	{
		--ConversionsInFlight;
		SPass& pass = Passes.at(PassId);
		pass.bConverted = true;
		pass.bLost = true;
		CheckPassDone(PassId);
	}

	void OnConverted (uint64 PassId, SConversion&& Conversion)
	{
		--ConversionsInFlight;
		SPass& pass = Passes.at(PassId);
		pass.bConverted = true;
		pass.bRuleFilesChanged = Conversion.bRuleFilesChanged;
		pass.Folders = std::move(Conversion.Folders);
		pass.RuleFiles = std::move(Conversion.RuleFiles);
		Counters.ReEvaluated += Conversion.ReEvaluated;
		Counters.MetasRead += Conversion.MetasRead;
		if (!Initial.bComplete)
		{
			Initial.ConvertMs += Conversion.Milliseconds;
		}
		for (const FString& folder : Conversion.RemovedFolders)
		{
			Rules->ForgetFolders(folder);
		}
		for (const FString& file : Conversion.RemovedFiles)
		{
			LastRequested[file] = NextRequest++;   // a result still in flight for it is stale now
		}
		for (SRegistryDelta& delta : Conversion.Deltas)
		{
			pass.bFilesChanged = pass.bFilesChanged || ChangesFiles(delta);
			Queue.push_back(SQueuedDelta { std::move(delta), PassId });
			++pass.PendingDeltas;
		}

		std::vector<SAnalyzeRequest> chunk;
		const uint32 chunkSize = std::max(Desc.AnalyzeBatchFiles, 1u);
		for (SAnalyzeRequest& request : Conversion.Analyze)
		{
			request.Sequence = NextRequest++;
			LastRequested[request.Path] = request.Sequence;
			chunk.push_back(std::move(request));
			if (chunk.size() >= chunkSize)
			{
				SubmitAnalyze(PassId, std::move(chunk));
				chunk.clear();
			}
		}
		if (!chunk.empty())
		{
			SubmitAnalyze(PassId, std::move(chunk));
		}
		CheckPassDone(PassId);
	}

	void SubmitAnalyze (uint64 PassId, std::vector<SAnalyzeRequest>&& Requests)
	{
		SPass& pass = Passes.at(PassId);
		++pass.OutstandingAnalyze;
		++AnalyzeTasksInFlight;
		auto job = std::make_shared<std::pair<std::vector<SAnalyzeRequest>, SAnalyzeOutcome>>();
		job->first = std::move(Requests);
		Pool.Submit(jobs::EWorkPriority::Normal, CancelGroup,
			[job, rules = Rules.get(), mounts = Mounts.get(), retries = Desc.SourceChangedRetries, changing = SourceChangedForTest] ()
			{
				job->second = RunAnalyze(job->first, *rules, *mounts, retries, changing);
			},
			[this, PassId, job, guard = CLostGuard([this, PassId] { OnAnalyzeLost(PassId); })] () mutable
			{
				guard.Dismiss();
				OnAnalyzed(PassId, std::move(job->second));
			});
	}

	void OnAnalyzeLost (uint64 PassId)
	{
		--AnalyzeTasksInFlight;
		SPass& pass = Passes.at(PassId);
		--pass.OutstandingAnalyze;
		pass.bLost = true;
		CheckPassDone(PassId);
	}

	void OnAnalyzed (uint64 PassId, SAnalyzeOutcome&& Outcome)
	{
		--AnalyzeTasksInFlight;
		SPass& pass = Passes.at(PassId);
		Counters.Analyzed += Outcome.Analyzed;
		Counters.Hashed += Outcome.Hashed;
		if (!Initial.bComplete)
		{
			Initial.EngineAnalyzed += Outcome.EngineAnalyzed;
		}
		SRegistryDelta delta;
		for (SAnalyzedFile& file : Outcome.Files)
		{
			const auto it = LastRequested.find(file.Path);
			if (it == LastRequested.end() || it->second != file.Sequence)
			{
				continue;   // the file changed again (or went) since this was asked for: a newer result decides
			}
			LastRequested.erase(it);
			MergeDelta(delta, std::move(file.Delta));
		}
		if (!delta.IsEmpty())
		{
			pass.bFilesChanged = pass.bFilesChanged || ChangesFiles(delta);
			Queue.push_back(SQueuedDelta { std::move(delta), PassId });
			++pass.PendingDeltas;
		}
		if (!Outcome.Retry.empty())
		{
			// Still outstanding: retried after the watcher's settle window.
			Retries.push_back(SRetry { platform::MakeDeadline(Desc.RetryDelayMs / 1000.0), PassId, std::move(Outcome.Retry) });
			return;
		}
		--pass.OutstandingAnalyze;
		CheckPassDone(PassId);
	}

	void SubmitDueRetries ()
	{
		const uint64 now = platform::ReadTimestamp();
		for (size_t i = 0u; i < Retries.size();)
		{
			if (Retries[i].Deadline > now && !bStopping)
			{
				++i;
				continue;
			}
			SRetry retry = std::move(Retries[i]);
			Retries.erase(Retries.begin() + static_cast<ptrdiff_t>(i));
			SPass& pass = Passes.at(retry.Pass);
			--pass.OutstandingAnalyze;   // SubmitAnalyze counts it again
			if (bStopping)
			{
				pass.bLost = true;
				CheckPassDone(retry.Pass);
				continue;
			}
			SubmitAnalyze(retry.Pass, std::move(retry.Requests));
		}
	}

	[[nodiscard]] double SecondsToNextRetry () const
	{
		if (Retries.empty())
		{
			return -1.0;
		}
		uint64 next = ~0ull;
		for (const SRetry& retry : Retries)
		{
			next = std::min(next, retry.Deadline);
		}
		const uint64 now = platform::ReadTimestamp();
		return next <= now ? 0.0 : platform::TimestampToSeconds(next - now);
	}

	void CheckPassDone (uint64 PassId)
	{
		SPass& pass = Passes.at(PassId);
		if (!pass.bConverted || pass.PendingDeltas != 0u || pass.OutstandingAnalyze != 0u || pass.bUnclaimedRunning)
		{
			return;
		}
		// RULE002 can only differ when files or records changed, or the rules did (a Validation: severity, Scan.Quiet),
		// or a record elsewhere switched a file's companion status (bUnclaimed). It is recomputed on a worker from the
		// view every delta of the pass is in (a whole mount's folders take milliseconds), then goes into the pass's last
		// delta.
		const bool bUnclaimedMayDiffer = pass.bFilesChanged || pass.bRuleFilesChanged || pass.bReEvaluateAll || pass.bUnclaimed;
		if (!pass.bUnclaimedDone && !pass.bLost && bUnclaimedMayDiffer)
		{
			pass.bUnclaimedDone = true;
			pass.bUnclaimedRunning = true;
			++UnclaimedTasksInFlight;
			auto job = std::make_shared<SUnclaimedJob>();
			job->View = Store->Acquire();
			job->Folders = std::move(pass.Folders);
			Pool.Submit(jobs::EWorkPriority::High, jobs::NoCancelGroup,
				[job, rules = Rules.get()] ()
				{
					const uint64 start = platform::ReadTimestamp();
					job->Delta = MakeUnclaimedDelta(*job->View, *rules, job->Folders);
					job->Milliseconds = MillisecondsSince(start);
				},
				[this, PassId, job, guard = CLostGuard([this, PassId] { OnUnclaimed(PassId, nullptr); })] () mutable
				{
					guard.Dismiss();
					OnUnclaimed(PassId, job.get());
				});
			return;
		}
		if (!pass.bFinalQueued)
		{
			// The pass's last delta. Its RULEFILES rows only now, when every record they cover is committed, and only
			// when nothing of the pass was dropped: a pass cut short leaves the old rows, so the next start evaluates
			// (and analyzes) that interval again.
			pass.bFinalQueued = true;
			SRegistryDelta delta;
			if (!pass.bLost)
			{
				delta = std::move(pass.RuleFiles);
				MergeDelta(delta, std::move(pass.Unclaimed));
			}
			if (!delta.IsEmpty())
			{
				Queue.push_back(SQueuedDelta { std::move(delta), PassId });
				++pass.PendingDeltas;
				return;
			}
		}
		FinishPass(PassId);
	}

	// Job: null when the pool dropped it unrun (it is in no cancel group: only a drain does that).
	void OnUnclaimed (uint64 PassId, SUnclaimedJob* Job)
	{
		--UnclaimedTasksInFlight;
		SPass& pass = Passes.at(PassId);
		pass.bUnclaimedRunning = false;
		if (Job == nullptr)
		{
			pass.bLost = true;
		}
		else
		{
			pass.Unclaimed = std::move(Job->Delta);
			if (!Initial.bComplete)
			{
				Initial.UnclaimedMs += Job->Milliseconds;
			}
		}
		CheckPassDone(PassId);
	}

	void FinishPass (uint64 PassId)
	{
		const auto it = Passes.find(PassId);
		const SPass& pass = it->second;
		MountQueues[pass.Mount].ActivePass = 0u;
		++Counters.Passes;
		const bool bInitial = pass.bInitial;
		const bool bRuleFilesChanged = pass.bRuleFilesChanged;
		const bool bReEvaluated = pass.bReEvaluateAll;
		std::vector<FString> dirty = std::move(it->second.Dirty);
		Passes.erase(it);
		if (Repairs != nullptr)
		{
			Repairs->OnPassCommitted(dirty, bInitial);
			if (bRuleFilesChanged || bReEvaluated)
			{
				Repairs->OnRulesChanged();   // unchanged files emit no rows, yet their switches or severities may differ
			}
		}
		if (bRuleFilesChanged && State == EScanState::Ready)
		{
			WriteRulesCache();
		}
		if (bInitial && InitialPassesLeft > 0u && --InitialPassesLeft == 0u)
		{
			CompleteInitialScan();
		}
	}

	void CompleteInitialScan ()
	{
		if (State != EScanState::Scanning)
		{
			return;
		}
		State = EScanState::Ready;
		const CRegistryView& view = Store->Live();
		Initial.bComplete = true;
		Initial.Files = view.GetFileCount();
		Initial.Records = view.GetRecordCount();
		Initial.TotalMs = MillisecondsSince(StartTimestamp);
		Initial.Analyzed = Counters.Analyzed;
		Initial.Hashed = Counters.Hashed;
		Initial.ReEvaluated = Counters.ReEvaluated;
		bScanCompletedPending = true;
		if (Repairs != nullptr)
		{
			Repairs->OnInitialScanComplete();
		}
		if (Desc.Owner == EAssetRegistryOwner::Project && Initial.EngineAnalyzed > 0u)
		{
			const bool bLoaded = std::any_of(SnapshotReports.begin(), SnapshotReports.end(), [&] (const SSnapshotFileReport& Report)
			{
				return Report.State == ESnapshotFileState::Loaded && Report.Path == GetEngineSnapshotPath(EngineDir);
			});
			// The engine checkout's own FRT.bat: a project's names its project, which --engine refuses.
			log::Info(LogCategory, "{} ({} /Engine files analyzed): run {} assets scan --engine",
				bLoaded ? "Engine.snapshot is older than /Engine" : "no usable Engine.snapshot", Initial.EngineAnalyzed,
				FString::FromPath(EngineDir / "FRT.bat"));
		}
		WriteRulesCache();
	}

	// ---- commits ----

	void CommitDelta (SRegistryDelta&& Delta)
	{
		if (Delta.IsEmpty())
		{
			return;
		}
		const uint64 start = platform::ReadTimestamp();
		// Broken or not (Journal.h, protocol step 4): a broken journal keeps the commit in memory, and the restart after
		// the full write its break triggers writes it into the next journal.
		if (Journal.has_value())
		{
			FString error;
			if (!Journal->Append(Delta, Store->GetRevision() + 1u, &error) && !bJournalBreakLogged)
			{
				bJournalBreakLogged = true;
				log::Warning(LogCategory, "the registry journal broke ({}): a full snapshot is written instead", error);
			}
		}
		Store->Commit(std::move(Delta));
		++Counters.Committed;
		const double milliseconds = MillisecondsSince(start);
		Counters.CommitMs += milliseconds;
		if (!Initial.bComplete)
		{
			Initial.CommitMs += milliseconds;
		}
		PublishedRevision.store(Store->GetRevision(), std::memory_order_release);
		RecordsSinceSnapshot += Store->GetLastCommitStats().RecordsChanged;
		bSnapshotDirty = true;
		LastCommitTimestamp = platform::ReadTimestamp();
	}

	void CommitQueued (bool bUnbounded)
	{
		const uint64 start = platform::ReadTimestamp();
		while (!Queue.empty())
		{
			SQueuedDelta item = std::move(Queue.front());
			Queue.pop_front();
			if (Repairs != nullptr && item.Pass != 0u)
			{
				std::vector<FString>& dirty = Passes.at(item.Pass).Dirty;
				for (const SFileUpsert& upsert : item.Delta.FileUpserts)
				{
					dirty.push_back(upsert.MountPath);
				}
				dirty.insert(dirty.end(), item.Delta.FileRemoves.begin(), item.Delta.FileRemoves.end());
				dirty.insert(dirty.end(), item.Delta.FolderRemoves.begin(), item.Delta.FolderRemoves.end());
			}
			CommitDelta(std::move(item.Delta));
			RequestCompanionFolderScans(item.Pass);
			if (item.Pass != 0u)
			{
				--Passes.at(item.Pass).PendingDeltas;
				CheckPassDone(item.Pass);
			}
			if (bUnbounded)
			{
				if (Store->NeedsCompaction())
				{
					Compact();
				}
			}
			else if (MillisecondsSince(start) >= Desc.FrameBudgetMs)
			{
				break;
			}
		}
	}

	/**
	 * RULE002 of a folder depends on its files' classes, and a record elsewhere can switch one (a glTF that stops naming
	 * a buffer in another folder makes it Unclaimed). A folder the committing pass does not cover gets a subtree pass of
	 * its own, queued behind this one, which recomputes its RULE002 from a view that holds this pass's records.
	 */
	void RequestCompanionFolderScans (uint64 PassId)
	{
		const std::vector<FString>& changed = Store->GetLastCommitStats().CompanionClassChanges;
		if (changed.empty())
		{
			return;
		}
		const auto pass = Passes.find(PassId);
		for (const FString& file : changed)
		{
			const FStringView folder = GetMountPathParent(file);
			FStringView mountName;
			FStringView relative;
			if ((pass != Passes.end() && IsMountPathAtOrBelow(folder, pass->second.Root.View())) || !SplitMountPath(file, mountName, relative))
			{
				continue;
			}
			if (const SMount* mount = Mounts->Find(mountName); mount != nullptr)
			{
				RequestScan(mount->Id, folder, false, true);
			}
		}
	}

	void Compact ()
	{
		Store->Compact();
		++Counters.Compactions;
		PublishedRevision.store(Store->GetRevision(), std::memory_order_release);
	}

	[[nodiscard]] bool HasWorkInFlight () const noexcept
	{
		return bLoading || ScansInFlight != 0u || ConversionsInFlight != 0u || AnalyzeTasksInFlight != 0u || UnclaimedTasksInFlight != 0u;
	}

	[[nodiscard]] bool IsIdle () const noexcept
	{
		if (HasWorkInFlight() || !Queue.empty() || !Retries.empty() || !WatchEvents.empty() || bMountRulesChanged || !Passes.empty())
		{
			return false;
		}
		return std::all_of(MountQueues.begin(), MountQueues.end(), [] (const SMountQueue& Queue) { return Queue.Requests.empty(); });
	}

	void Pump (bool bUnbounded)
	{
		if (State == EScanState::NotStarted || State == EScanState::ShutDown || bLoading)
		{
			return;
		}
		if (State == EScanState::Ready && !bStopping)
		{
			ProcessWatchEvents();
		}
		SubmitDueRetries();
		StartPasses();
		CommitQueued(bUnbounded);
		StartPasses();
		if (Repairs != nullptr && Desc.Lifecycle->bLive && State == EScanState::Ready && !bStopping)
		{
			Repairs->Tick(GetLifecycleNowMs());
		}
		if (IsIdle())
		{
			if (Store->NeedsCompaction())
			{
				Compact();
			}
			MaybeWriteSnapshot(true);
		}
		else
		{
			MaybeWriteSnapshot(false);
		}
		Broadcast();
	}

	// The pump loop of every wait: completions, unbounded commits, retry deadlines.
	bool WaitUntilImpl (TFunctionRef<bool ()> Done, double TimeoutSeconds)
	{
		const uint64 deadline = TimeoutSeconds >= 0.0 ? platform::MakeDeadline(TimeoutSeconds) : ~0ull;
		for (;;)
		{
			Pool.PumpCompletions();
			Pump(true);
			if (Done())
			{
				return true;
			}
			const uint64 now = platform::ReadTimestamp();
			if (now >= deadline)
			{
				return false;
			}
			if (!HasWorkInFlight() && !bSnapshotWriteInFlight && !bRulesCacheWriteInFlight && Queue.empty() && Retries.empty()
				&& !Pool.GetStats().CompletionsWaiting
				&& Pool.IsIdle())
			{
				return Done();   // nothing could change any more
			}
			double wait = SecondsToNextRetry();
			if (deadline != ~0ull)
			{
				const double remaining = platform::TimestampToSeconds(deadline - now);
				wait = wait < 0.0 ? remaining : std::min(wait, remaining);
			}
			Pool.WaitForCompletions(wait);
		}
	}

	void Broadcast ()
	{
		const uint64 revision = Store->GetRevision();
		if (revision == BroadcastRevision && !bScanCompletedPending)
		{
			return;
		}
		if (AssetsChanged.IsEmpty())
		{
			BroadcastRevision = revision;   // nobody listens: no change set is built
			bScanCompletedPending = false;
			return;
		}
		SAssetChangeSet changes;
		changes.Revision = revision;
		changes.bScanCompleted = std::exchange(bScanCompletedPending, false);
		if (revision != BroadcastRevision)
		{
			std::vector<SRecordChange> window;
			const bool bComplete = Store->ForEachChangeSince(BroadcastRevision, [&] (const SRecordChange& Change) { window.push_back(Change); });
			if (!bComplete || window.empty())
			{
				// The ring cannot list it (an image, a compaction, an overflow) or no record changed (file rows,
				// problems): listeners re-run what they show.
				changes.bRefreshAll = true;
			}
			if (bComplete)
			{
				AddRecordChanges(window, changes);
			}
		}
		BroadcastRevision = revision;
		if (!changes.IsEmpty())
		{
			AssetsChanged.Broadcast(changes);
		}
	}

	// ---- writes (the writer only) ----

	/**
	 * Rules.cache, written on a worker like the snapshot (a flushed atomic write costs a frame on the owner thread): the
	 * worker takes the listing itself (CRulesEngine is thread-safe), so a write asked for while one runs is coalesced into
	 * one more write of the newest listing once it ends. Waits (Flush, ShutDown) count it as work in flight.
	 */
	void WriteRulesCache ()
	{
		if (!Desc.bWriter)
		{
			return;
		}
		if (bRulesCacheWriteInFlight)
		{
			bRulesCacheDirty = true;
			return;
		}
		bRulesCacheWriteInFlight = true;
		bRulesCacheDirty = false;
		auto error = std::make_shared<std::optional<FString>>();
		Pool.Submit(jobs::EWorkPriority::Low, jobs::NoCancelGroup,
			[error, rules = Rules.get(), path = GetRulesCachePath(OwnerRoot)] ()
			{
				FString text;
				if (!assets::WriteRulesCache(path, rules->GetListing(), &text))
				{
					*error = std::move(text);
				}
			},
			[this, error, guard = CLostGuard([this] { bRulesCacheWriteInFlight = false; })] () mutable
			{
				guard.Dismiss();
				bRulesCacheWriteInFlight = false;
				if (error->has_value())
				{
					log::Warning(LogCategory, "Rules.cache not written: {}", **error);
				}
				if (bRulesCacheDirty)
				{
					WriteRulesCache();
				}
			});
	}

	void MaybeWriteSnapshot (bool bIdle)
	{
		if (!Desc.bWriter || !ProjectId.IsValid() || bSnapshotWriteInFlight || State != EScanState::Ready || !bSnapshotDirty || bStopping)
		{
			return;
		}
		const bool bBatch = RecordsSinceSnapshot >= Desc.SnapshotBatchRecords;
		const bool bIdleLong = bIdle && platform::TimestampToSeconds(platform::ReadTimestamp() - LastCommitTimestamp) >= Desc.SnapshotIdleSeconds;
		const bool bJournalBroken = Journal.has_value() && Journal->IsBroken();
		if (bBatch || bIdleLong || bJournalBroken)
		{
			BeginSnapshotWrite(jobs::EWorkPriority::Low);
		}
	}

	void BeginSnapshotWrite (jobs::EWorkPriority Priority)
	{
		auto job = std::make_shared<SSnapshotJob>();
		job->View = Store->Acquire();
		job->Mounts = SnapshotMounts;
		job->Path = SnapshotPath;
		job->ProjectId = ProjectId;
		job->RulesBasis = Rules->GetRulesBasis(bWithEngineFallback);
		if (Journal.has_value())   // a broken one too: the restart mends it
		{
			job->Restart = Journal->BeginRestart(job->View->GetRevision());
			job->bHasRestart = true;
		}
		RecordsSinceSnapshot = 0u;
		bSnapshotDirty = false;
		bSnapshotWriteInFlight = true;
		Pool.Submit(Priority, jobs::NoCancelGroup,
			[job] ()
			{
				job->bWritten = WriteSnapshot(*job->View, job->Mounts, job->Path, job->ProjectId, job->RulesBasis, &job->Error, &job->Stats);
				if (job->bWritten && job->bHasRestart)
				{
					(void)job->Restart.Prepare();   // FinishRestart restarts on the owner thread when this failed
				}
			},
			[this, job, guard = CLostGuard([this] { bSnapshotWriteInFlight = false; bSnapshotDirty = true; })] () mutable
			{
				guard.Dismiss();
				OnSnapshotWritten(*job);
			});
	}

	void OnSnapshotWritten (SSnapshotJob& Job)
	{
		bSnapshotWriteInFlight = false;
		if (!Job.bWritten)
		{
			log::Warning(LogCategory, "{} not written: {}", FString::FromPath(Job.Path.filename()), Job.Error);
			bSnapshotDirty = true;
			return;
		}
		++Counters.SnapshotsWritten;
		LastSnapshotStats = Job.Stats;
		if (Job.bHasRestart && Journal.has_value())
		{
			FString error;
			if (!Journal->FinishRestart(std::move(Job.Restart), &error) && !error.IsEmpty())
			{
				log::Warning(LogCategory, "the registry journal was not restarted: {}", error);
			}
			if (!Journal->IsBroken())
			{
				bJournalBreakLogged = false;   // the next break is logged again
			}
		}
	}

	bool WriteSnapshotNow ()
	{
		if (!Desc.bWriter || !ProjectId.IsValid() || State == EScanState::NotStarted || State == EScanState::ShutDown || bLoading)
		{
			return false;
		}
		WaitUntil([&] { return !bSnapshotWriteInFlight; }, -1.0);
		const uint64 written = Counters.SnapshotsWritten;
		BeginSnapshotWrite(jobs::EWorkPriority::High);
		WaitUntil([&] { return !bSnapshotWriteInFlight; }, -1.0);
		return Counters.SnapshotsWritten > written;
	}

	// ---- shut-down ----

	/**
	 * The exit snapshot is written only when nothing was left undone. A pass cut short persists nothing that claims
	 * work it did not do (a file analyzed later keeps its old row, the RULEFILES rows wait for the pass's last
	 * delta), so what was committed is in the journal either way, and the next start's scan finds the rest.
	 */
	void ShutDown ()
	{
		if (State == EScanState::NotStarted)
		{
			State = EScanState::ShutDown;
			return;
		}
		if (State == EScanState::ShutDown)
		{
			return;
		}
		if (Repairs != nullptr)
		{
			// No new run or op starts; the one in flight finishes (an op is journalled either way).
			Repairs->ShutDown();
			WaitUntilImpl([this] { return !Repairs->IsWorking(); }, -1.0);
		}
		Unsubscribe();
		const bool bWasComplete = State == EScanState::Ready && IsIdle();
		bStopping = true;
		WatchEvents.clear();
		bMountRulesChanged = false;
		for (SMountQueue& queue : MountQueues)
		{
			queue.Requests.clear();
		}
		Pool.Cancel(CancelGroup);
		// Scans and the snapshot load cannot be cancelled; the cancelled tasks' completions report themselves lost.
		WaitUntil([&] { return !HasWorkInFlight() && Retries.empty() && Passes.empty() && !bSnapshotWriteInFlight && !bRulesCacheWriteInFlight; },
			-1.0);
		CommitQueued(true);
		if (bWasComplete && Desc.bSnapshotAtShutDown && Desc.bWriter && ProjectId.IsValid() && bSnapshotDirty)
		{
			BeginSnapshotWrite(jobs::EWorkPriority::High);
			WaitUntil([&] { return !bSnapshotWriteInFlight; }, -1.0);
		}
		// The last commits may have finished a pass that asked for Rules.cache: its write (and its completion, which
		// holds this) ends before the registry does.
		WaitUntil([&] { return !bRulesCacheWriteInFlight; }, -1.0);
		if (Journal.has_value())
		{
			Journal->Close();
			Journal.reset();
		}
		State = EScanState::ShutDown;
	}

	SAssetRegistryDesc Desc;
	std::shared_ptr<const CMountTable> Mounts;
	jobs::CWorkerPool& Pool;
	std::unique_ptr<CRegistryStore> Store;
	std::unique_ptr<CRulesEngine> Rules;
	uint64 CancelGroup = 0u;
	EScanState State = EScanState::NotStarted;
	bool bLoading = false;
	bool bStopping = false;

	std::filesystem::path EngineDir;
	std::filesystem::path OwnerRoot;
	std::filesystem::path SnapshotPath;
	std::vector<MountId> SnapshotMounts;
	std::vector<FString> SnapshotMountNames;
	bool bWithEngineFallback = true;
	SGuid ProjectId;
	std::optional<CRegistryJournal> Journal;
	bool bJournalBreakLogged = false;
	std::vector<SSnapshotFileReport> SnapshotReports;
	std::vector<SDiagnostic> MountRuleDiagnostics;

	std::vector<SMountQueue> MountQueues;
	std::map<uint64, SPass> Passes;
	uint64 NextPassId = 1u;
	uint32 InitialPassesLeft = 0u;
	std::deque<SQueuedDelta> Queue;
	std::vector<SRetry> Retries;
	TStringMap<uint64> LastRequested;
	uint64 NextRequest = 1u;
	uint32 ScansInFlight = 0u;
	uint32 ConversionsInFlight = 0u;
	uint32 AnalyzeTasksInFlight = 0u;
	uint32 UnclaimedTasksInFlight = 0u;

	std::vector<SDelegateHandle> Subscriptions;
	std::vector<SWatchEvent> WatchEvents;
	bool bMountRulesChanged = false;
	FString SourceChangedForTest;    // CAssetRegistry::SetSourceChangedForTest

	SAssetRegistryCounters Counters;
	SInitialScanReport Initial;
	uint64 StartTimestamp = 0u;

	bool bSnapshotWriteInFlight = false;
	bool bSnapshotDirty = false;
	bool bRulesCacheWriteInFlight = false;
	bool bRulesCacheDirty = false;     // asked for again while one was written
	uint64 RecordsSinceSnapshot = 0u;
	uint64 LastCommitTimestamp = 0u;
	SSnapshotWriteStats LastSnapshotStats;

	TMulticastDelegate<const SAssetChangeSet&> AssetsChanged;
	uint64 BroadcastRevision = 0u;
	bool bScanCompletedPending = false;
	std::atomic<uint64> PublishedRevision = 0u;

	// Last, so it goes first: it holds this registry as its host.
	std::unique_ptr<CAssetRepairs> Repairs;
};


CAssetRegistry::CAssetRegistry (SAssetRegistryDesc Desc)
	: Impl(std::make_unique<SImpl>(std::move(Desc)))
{
}

CAssetRegistry::~CAssetRegistry ()
{
	frt_assert(Impl->State == EScanState::NotStarted || Impl->State == EScanState::ShutDown);
}

void CAssetRegistry::Start ()
{
	Impl->Start();
}

void CAssetRegistry::ShutDown ()
{
	Impl->ShutDown();
}

void CAssetRegistry::Pump ()
{
	Impl->Pump(false);
}

bool CAssetRegistry::WaitForInitialScan (double TimeoutSeconds)
{
	if (Impl->State == EScanState::NotStarted || Impl->State == EScanState::ShutDown)
	{
		return Impl->Initial.bComplete;
	}
	return Impl->WaitUntil([this] { return Impl->State == EScanState::Ready; }, TimeoutSeconds);
}

double CAssetRegistry::WaitBeforeBuildWorld ()
{
	SImpl& impl = *Impl;
	const uint64 start = platform::ReadTimestamp();
	if (impl.State != EScanState::Ready && impl.State != EScanState::NotStarted && impl.State != EScanState::ShutDown)
	{
		// The M3 form of §5.4's progress bar: a line now and then while the wait lasts.
		uint64 nextLine = platform::MakeDeadline(1.0);
		log::Info(LogCategory, "waiting for the initial scan of {} mount(s)", impl.Mounts->GetMounts().size());
		impl.WaitUntil([&]
		{
			if (impl.State == EScanState::Ready)
			{
				return true;
			}
			if (platform::ReadTimestamp() >= nextLine)
			{
				nextLine = platform::MakeDeadline(1.0);
				log::Info(LogCategory, "initial scan: {} files listed, {} analyzed so far", impl.Counters.Scanned, impl.Counters.Analyzed);
			}
			return false;
		}, -1.0);
	}
	const double waited = MillisecondsSince(start);
	const SInitialScanReport& report = impl.Initial;
	log::Info(LogCategory, "initial scan: {} files, {} records, {:.1f} ms; BuildWorld waited {:.1f} ms (analyzed {}, hashed {})", report.Files,
		report.Records, report.TotalMs, waited, report.Analyzed, report.Hashed);
	return waited;
}

bool CAssetRegistry::IsInitialScanComplete () const noexcept
{
	return Impl->State == EScanState::Ready;
}

EScanState CAssetRegistry::GetScanState () const noexcept
{
	return Impl->State;
}

const SInitialScanReport& CAssetRegistry::GetInitialScanReport () const noexcept
{
	return Impl->Initial;
}

std::span<const SSnapshotFileReport> CAssetRegistry::GetSnapshotReports () const noexcept
{
	return Impl->SnapshotReports;
}

bool CAssetRegistry::ScanPathsSynchronous (std::span<const FString> MountPaths, double TimeoutSeconds)
{
	SImpl& impl = *Impl;
	const uint64 deadline = TimeoutSeconds >= 0.0 ? platform::MakeDeadline(TimeoutSeconds) : ~0ull;
	const auto remaining = [&] { return deadline == ~0ull ? -1.0 : std::max(0.0, platform::TimestampToSeconds(deadline - std::min(deadline, platform::ReadTimestamp()))); };
	if (!WaitForInitialScan(remaining()))
	{
		return false;
	}
	bool bAllInMounts = true;
	for (const FString& path : MountPaths)
	{
		FStringView mountName;
		FStringView relative;
		const SMount* mount = SplitMountPath(path, mountName, relative) ? impl.Mounts->Find(mountName) : nullptr;
		if (mount == nullptr || !IsWellFormedMountPath(path))
		{
			bAllInMounts = false;
			continue;
		}
		// A file scans its folder (its spelling then comes from the listing).
		bool bFile = impl.Store->Live().FindFileExact(path).has_value();
		if (!bFile)
		{
			const std::optional<std::filesystem::path> absolute = mount->ToAbsolute(relative);
			std::error_code ec;
			bFile = absolute.has_value() && std::filesystem::is_regular_file(*absolute, ec);
		}
		impl.RequestScan(mount->Id, bFile ? ScanRootOf(path).View() : path.View(), false);
	}
	const bool bDone = impl.WaitUntil([&] { return impl.IsIdle(); }, remaining());
	return bDone && bAllInMounts;
}

bool CAssetRegistry::Flush (double TimeoutSeconds)
{
	SImpl& impl = *Impl;
	const uint64 deadline = platform::MakeDeadline(TimeoutSeconds);
	bool bFenced = true;
	if (impl.Desc.bWriter && impl.Desc.Watcher != nullptr && impl.Desc.Watcher->IsEnabled() && !impl.Subscriptions.empty())
	{
		for (const SMount& mount : impl.Mounts->GetMounts())
		{
			// Only the mounts this writer owns get fence files (a Project owner never writes into /Engine, which may
			// be read-only); of the others, what the watcher reported already is waited for below.
			if (std::find(impl.SnapshotMounts.begin(), impl.SnapshotMounts.end(), mount.Id) == impl.SnapshotMounts.end())
			{
				continue;
			}
			std::error_code ec;
			if (std::filesystem::is_directory(mount.ContentDir, ec))
			{
				bFenced = impl.Desc.Watcher->Flush(mount.ContentDir, TimeoutSeconds) && bFenced;
			}
			if (std::filesystem::is_directory(mount.RulesFile.parent_path(), ec))
			{
				bFenced = impl.Desc.Watcher->Flush(mount.RulesFile.parent_path(), TimeoutSeconds) && bFenced;
			}
		}
	}
	const uint64 now = platform::ReadTimestamp();
	const double remaining = now >= deadline ? 0.0 : platform::TimestampToSeconds(deadline - now);
	if (!WaitForInitialScan(remaining))
	{
		return false;
	}
	return impl.WaitUntil([&] { return impl.IsIdle(); }, remaining) && bFenced;
}

bool CAssetRegistry::IsIdle () const noexcept
{
	return Impl->IsIdle();
}

bool CAssetRegistry::WriteSnapshotNow ()
{
	return Impl->WriteSnapshotNow();
}

const CRegistryView& CAssetRegistry::Live () const noexcept
{
	static const CRegistryView EmptyView;
	return Impl->bLoading ? EmptyView : Impl->Store->Live();
}

std::shared_ptr<const CRegistryView> CAssetRegistry::Acquire () const noexcept
{
	return Impl->Store->Acquire();
}

uint64 CAssetRegistry::GetRevision () const noexcept
{
	return Impl->PublishedRevision.load(std::memory_order_acquire);
}

bool CAssetRegistry::ForEachChangeSince (uint64 Revision, TFunctionRef<void (const SRecordChange&)> Visit) const
{
	if (Impl->bLoading)
	{
		return false;
	}
	return Impl->Store->ForEachChangeSince(Revision, Visit);
}

EExists CAssetRegistry::Exists (const SAssetId& Id) const
{
	if (Impl->State != EScanState::Ready)
	{
		return EExists::Unknown;
	}
	return Impl->Store->Live().Exists(Id);
}

TMulticastDelegate<const SAssetChangeSet&>& CAssetRegistry::OnAssetsChanged () noexcept
{
	return Impl->AssetsChanged;
}

CAssetRepairs* CAssetRegistry::GetRepairs () noexcept
{
	return Impl->Repairs.get();
}

const SAssetRegistryCounters& CAssetRegistry::GetCounters () const noexcept
{
	return Impl->Counters;
}

const CMountTable& CAssetRegistry::GetMounts () const noexcept
{
	return *Impl->Mounts;
}

const CRulesEngine& CAssetRegistry::GetRules () const noexcept
{
	return *Impl->Rules;
}

bool CAssetRegistry::IsWriter () const noexcept
{
	return Impl->Desc.bWriter;
}

EAssetRegistryOwner CAssetRegistry::GetOwner () const noexcept
{
	return Impl->Desc.Owner;
}

const std::filesystem::path& CAssetRegistry::GetSnapshotPath () const noexcept
{
	return Impl->SnapshotPath;
}

CRegistryJournal* CAssetRegistry::GetJournal () noexcept
{
	return Impl->Journal.has_value() ? &*Impl->Journal : nullptr;
}

void CAssetRegistry::SetSourceChangedForTest (FString MountPath)
{
	frt_assert(Impl->State == EScanState::NotStarted);
	Impl->SourceChangedForTest = std::move(MountPath);
}
}
