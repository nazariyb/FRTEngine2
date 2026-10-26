#include "Assets/AssetRegistry.h"

#include <algorithm>
#include <atomic>
#include <deque>
#include <map>
#include <optional>
#include <system_error>
#include <utility>

#include "Assets/AssetDiagnosticCodes.h"
#include "Assets/Discovery/MountScanner.h"
#include "Assets/FileWatcherService.h"
#include "Assets/MountPaths.h"
#include "Assets/Pipeline/ImporterTable.h"
#include "Assets/Pipeline/Importers/AssetImporter.h"
#include "Assets/Registry/Journal.h"
#include "Assets/Registry/RegistryParallel.h"
#include "Assets/Registry/RegistryRecords.h"
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

// Path is Ancestor or lies below it ('/' separated, byte-exact: mount paths are keyed by their on-disk spelling).
bool IsAtOrBelow (FStringView Path, FStringView Ancestor) noexcept
{
	return Path == Ancestor || (Path.Len() > Ancestor.Len() && Path.StartsWith(Ancestor) && Path[Ancestor.Len()] == '/');
}

bool IsAtOrBelowAny (FStringView Path, std::span<const FString> Ancestors) noexcept
{
	return std::any_of(Ancestors.begin(), Ancestors.end(), [Path] (const FString& Ancestor) { return IsAtOrBelow(Path, Ancestor.View()); });
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
		if (ProblemSource == Source && Subject.StartsWith(Root)
			&& (Subject.Len() == Root.Len() || Subject[Root.Len()] == '/' || Subject[Root.Len()] == '#'))
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
}


// ---------------- The work of one pass (on workers) ----------------

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

SAnalyzeOutcome RunAnalyze (std::vector<SAnalyzeRequest>& Requests, const CRulesEngine& Rules, const CMountTable& Mounts, uint32 MaxRetries)
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
		SEffectiveFileSettings settings = Rules.ResolveFile(scope, request.Path, SFileFacts { result.Type, {} }, false);
		if (settings.Importer != request.Settings.Importer)
		{
			settings = request.Settings;
		}
		settings.Settings.AnalyzeHash = request.Settings.Settings.AnalyzeHash;   // with the filter hash ConvertBatch folded in
		const bool bTypeIncluded = Rules.IsTypeIncluded(scope, result.Type);
		RemoveSubEntries(result, request.Path, [&] (reflect::TypeId Type) { return Rules.IsTypeIncluded(scope, Type); });
		for (SDiagnostic& diagnostic : result.Diagnostics)
		{
			Rules.ApplyValidation(scope, diagnostic);
		}

		SAnalyzedFile& file = outcome.Files.emplace_back();
		file.Path = request.Path;
		file.Sequence = request.Sequence;
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
		file.Delta.FileUpserts.push_back(std::move(row));
		AppendRecordUpsert(std::move(result), settings, request.Path, file.Delta);
		if (!bTypeIncluded)
		{
			file.Delta.RecordUpserts.back().Flags |= ERecordFlags::TypeExcluded;
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
		uint32 Known = 0u;                                  // files the view holds (byte-exact)
	};
	std::vector<FString> paths(batch.Files.size());
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
			// The scan's Unchanged test (ScanTypes.h), against the view: size and mtime equal (never "newer than");
			// only the identity differing is refreshed without a hash.
			const EScanFileState state = !stored.has_value() ? EScanFileState::New
				: stored->Size != file.Size || stored->MTime != file.MTime ? EScanFileState::Changed
				: !(stored->Identity == file.Identity) ? EScanFileState::IdentityChanged : EScanFileState::Unchanged;
			const bool bHasRecord = stored.has_value() && stored->Record.IsValid();
			const bool bContentChanged = state == EScanFileState::New || state == EScanFileState::Changed;
			const bool bStatChanged = state != EScanFileState::Unchanged;
			const bool bStale = bHasRecord && HasAnyFlags(stored->Flags, ERecordFlags::StaleTags);
			const bool bTypeExcluded = bHasRecord && HasAnyFlags(stored->Flags, ERecordFlags::TypeExcluded);
			const bool bClassDiffers = stored.has_value() && stored->BaseClass != file.Verdict.Class
				&& !(bTypeExcluded && stored->BaseClass == EFileClass::Excluded);
			const bool bMissingRecord = stored.has_value() && !bHasRecord && file.Verdict.Class != EFileClass::Unclaimed;
			if (!bStatChanged && stored.has_value() && !bDirty && !bStale && !bClassDiffers && !bMissingRecord)
			{
				continue;   // unchanged file, unchanged rule chain: nothing to do (the warm start's path)
			}

			const SScanScope& scope = *folder.Scope;
			const SFileFacts facts { bHasRecord ? stored->Type : reflect::InvalidTypeId, {} };
			SEffectiveFileSettings settings = rules.ResolveFile(scope, path, facts, false);
			// The stamp covers what filters Analyze's result too (the Types filter of its sub-entries, Validation of its
			// problems: GetAnalyzeFilterHash), so an edit of either analyzes the scope's files again rather than leaving
			// their records filtered by the old lists. Nothing filtered: 0, the stamp of the settings alone.
			settings.Settings.AnalyzeHash ^= rules.GetAnalyzeFilterHash(scope);
			const bool bTypeIncluded = !bHasRecord || rules.IsTypeIncluded(scope, stored->Type);
			const EFileClass fileClass = bTypeIncluded ? settings.Class : EFileClass::Excluded;
			const bool bRowChanged = !stored.has_value() || bStatChanged || stored->BaseClass != fileClass;
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
			};

			const SImporterDesc* importer = settings.Importer;
			if (importer == nullptr || settings.Class == EFileClass::Excluded || settings.Class == EFileClass::Unclaimed)
			{
				if (bRowChanged)
				{
					addRow();
				}
				if (bHasRecord)
				{
					part.RecordRemoves.push_back(path);   // its importer went away (Importers.Exclude, a rule)
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
				request.Settings = std::move(settings);
				request.Scope = folder.Scope;
				request.Size = file.Size;
				request.MTime = file.MTime;
				request.Identity = file.Identity;
				request.Class = file.Verdict.Class;
				request.bContentChanged = !stored.has_value() || bContentChanged;
				continue;
			}
			if (bRowChanged)
			{
				addRow();
			}
			if (bDirty || bClassDiffers)
			{
				++chunkOut.ReEvaluated;
				const ERecordFlags flags = (stored->Flags & ~ERecordFlags::TypeExcluded) | (bTypeIncluded ? ERecordFlags::None : ERecordFlags::TypeExcluded);
				if (RuleOutputsDiffer(view, stored->Record, settings, flags))
				{
					part.RecordUpserts.push_back(MakeRecordUpsertFromView(view, stored->Record, settings, flags));
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
		knownFiles += chunkOut.Known;
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
			const FStringSet batchFiles(paths.begin(), paths.end());
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
				if (out.RemovedFolders.empty() || !IsAtOrBelow(folder.View(), out.RemovedFolders.back().View()))
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

	// The deltas, in commit order. Removals first, as within one delta: a folder or file the batch spells otherwise
	// (a case-only rename) is removed before its new spelling is added. Then the new folders (parents first), then
	// the files.
	if (!out.RemovedFolders.empty() || !out.RemovedFiles.empty())
	{
		SRegistryDelta& removals = out.Deltas.emplace_back();
		removals.FolderRemoves = out.RemovedFolders;
		removals.FileRemoves = out.RemovedFiles;
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

struct CAssetRegistry::SImpl
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
	}

	// ---- start-up ----

	void Start ()
	{
		frt_assert(State == EScanState::NotStarted);
		StartTimestamp = platform::ReadTimestamp();
		Rules->LoadMountRules(&MountRuleDiagnostics);
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

	void Subscribe ()
	{
		CFileWatcherService* watcher = Desc.Watcher;
		if (watcher == nullptr || !watcher->IsEnabled())
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
			if (IsAtOrBelow(Root, request.Root.View()))
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
			if (!IsAtOrBelow(Request.Root.View(), Root))
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
			[job, rules = Rules.get(), mounts = Mounts.get(), retries = Desc.SourceChangedRetries] ()
			{
				job->second = RunAnalyze(job->first, *rules, *mounts, retries);
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
		Passes.erase(it);
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
		if (Desc.Owner == EAssetRegistryOwner::Project && Initial.EngineAnalyzed > 0u)
		{
			const bool bLoaded = std::any_of(SnapshotReports.begin(), SnapshotReports.end(), [&] (const SSnapshotFileReport& Report)
			{
				return Report.State == ESnapshotFileState::Loaded && Report.Path == GetEngineSnapshotPath(EngineDir);
			});
			log::Info(LogCategory, "{} ({} /Engine files analyzed): run FRT.bat assets scan --engine",
				bLoaded ? "Engine.snapshot is older than /Engine" : "no usable Engine.snapshot", Initial.EngineAnalyzed);
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
			if ((pass != Passes.end() && IsAtOrBelow(folder, pass->second.Root.View())) || !SplitMountPath(file, mountName, relative))
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
	bool WaitUntil (TFunctionRef<bool ()> Done, double TimeoutSeconds)
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
			if (!HasWorkInFlight() && !bSnapshotWriteInFlight && Queue.empty() && Retries.empty() && !Pool.GetStats().CompletionsWaiting
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
			bool bAny = false;
			const bool bComplete = Store->ForEachChangeSince(BroadcastRevision, [&] (const SRecordChange& Change)
			{
				bAny = true;
				std::vector<SAssetChange>& list = Change.Kind == ERecordChange::Added ? changes.Added
					: Change.Kind == ERecordChange::Removed ? changes.Removed : changes.Modified;
				list.push_back(SAssetChange { Change.Id, Change.MountPath });
			});
			if (!bComplete || !bAny)
			{
				// The ring cannot list it (an image, a compaction, an overflow) or no record changed (file rows,
				// problems): listeners re-run what they show.
				changes.bRefreshAll = true;
				if (!bComplete)
				{
					changes.Added.clear();
					changes.Removed.clear();
					changes.Modified.clear();
				}
			}
		}
		BroadcastRevision = revision;
		if (!changes.IsEmpty())
		{
			AssetsChanged.Broadcast(changes);
		}
	}

	// ---- writes (the writer only) ----

	void WriteRulesCache ()
	{
		if (!Desc.bWriter)
		{
			return;
		}
		FString error;
		if (!assets::WriteRulesCache(GetRulesCachePath(OwnerRoot), Rules->GetListing(), &error))
		{
			log::Warning(LogCategory, "Rules.cache not written: {}", error);
		}
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
		WaitUntil([&] { return !HasWorkInFlight() && Retries.empty() && Passes.empty() && !bSnapshotWriteInFlight; }, -1.0);
		CommitQueued(true);
		if (bWasComplete && Desc.bSnapshotAtShutDown && Desc.bWriter && ProjectId.IsValid() && bSnapshotDirty)
		{
			BeginSnapshotWrite(jobs::EWorkPriority::High);
			WaitUntil([&] { return !bSnapshotWriteInFlight; }, -1.0);
		}
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

	SAssetRegistryCounters Counters;
	SInitialScanReport Initial;
	uint64 StartTimestamp = 0u;

	bool bSnapshotWriteInFlight = false;
	bool bSnapshotDirty = false;
	uint64 RecordsSinceSnapshot = 0u;
	uint64 LastCommitTimestamp = 0u;
	SSnapshotWriteStats LastSnapshotStats;

	TMulticastDelegate<const SAssetChangeSet&> AssetsChanged;
	uint64 BroadcastRevision = 0u;
	bool bScanCompletedPending = false;
	std::atomic<uint64> PublishedRevision = 0u;
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
}
