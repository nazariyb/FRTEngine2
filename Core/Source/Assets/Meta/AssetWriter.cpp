#include "Assets/Meta/AssetWriter.h"

#include <algorithm>
#include <span>
#include <system_error>

#include "Assets/Discovery/Mounts.h"
#include "Assets/FileHash.h"
#include "Assets/Meta/MetaPaths.h"
#include "Assets/MountPaths.h"
#include "Assets/Vcs/NoneVcsProvider.h"
#include "Log/Log.h"
#include "Platform/FileSystem.h"
#include "Platform/Platform.h"
#include "Unicode/DisplayText.h"


namespace frt::assets
{
namespace
{
// A file's step target: what a WriteFile or CopyFile leaves behind.
[[nodiscard]] bool IsWrite (EAssetOpStep Kind) noexcept
{
	return Kind == EAssetOpStep::WriteFile || Kind == EAssetOpStep::CopyFile;
}

[[nodiscard]] const FString& GetWriteTarget (const SAssetOpStep& Step) noexcept
{
	return Step.Kind == EAssetOpStep::CopyFile ? Step.ToPath : Step.Path;
}

// The other half of an asset/meta pair: a meta's source, or a source's meta.
[[nodiscard]] FString GetPairPath (FStringView MountPath)
{
	const std::optional<FStringView> source = GetMetaSource(MountPath);
	return source.has_value() ? FString(*source) : GetMetaPath(MountPath);
}

[[nodiscard]] bool Exists (const std::filesystem::path& Path)
{
	const std::optional<platform::SFileStat> stat = platform::Stat(Path);
	return stat.has_value() && !stat->bDirectory;
}

/**
 * Whether the folder lists Path's name in exactly that spelling. Stat alone cannot say: on NTFS the old spelling of a
 * case-only rename still opens, so "the Move is done" must compare what the listing holds (OpJournal.h).
 */
[[nodiscard]] bool IsSpelledOnDisk (const std::filesystem::path& Path)
{
	if (!Exists(Path))
	{
		return false;
	}
	const FString leaf = FString::FromPath(Path.filename());
	bool bFound = false;
	platform::EnumerateDirectory(Path.parent_path(), [&leaf, &bFound] (const platform::SDirectoryEntry& Entry)
	{
		bFound = Entry.Name == leaf.View();
		return !bFound;
	});
	return bFound;
}

[[nodiscard]] bool HoldsBytes (const std::filesystem::path& Path, std::span<const uint8> Bytes)
{
	if (!Exists(Path))
	{
		return false;
	}
	const std::optional<std::vector<uint8>> held = platform::ReadFileBytes(Path);
	return held.has_value() && std::equal(held->begin(), held->end(), Bytes.begin(), Bytes.end());
}

void AppendUnique (std::vector<FString>& Out, FStringView Path)
{
	if (std::find(Out.begin(), Out.end(), Path) == Out.end())
	{
		Out.emplace_back(Path);
	}
}

// Paths grouped by provider, in the order each provider first appears: one call per provider.
template <typename T>
struct SProviderBatch
{
	IVcsProvider* Provider = nullptr;
	std::vector<T> Items;
};

template <typename T>
void AddToBatch (std::vector<SProviderBatch<T>>& Batches, IVcsProvider* Provider, T Item)
{
	for (SProviderBatch<T>& batch : Batches)
	{
		if (batch.Provider == Provider)
		{
			batch.Items.push_back(std::move(Item));
			return;
		}
	}
	Batches.push_back(SProviderBatch<T> { Provider, { std::move(Item) } });
}

[[nodiscard]] SVcsResult Failure (FString Message)
{
	return { EVcsError::Failed, std::move(Message) };
}
}


/**
 * An op made ready to run: its steps as they will be journalled (reordered, filtered, hashed), each with its paths
 * resolved, and what goes to CheckOut and Add.
 */
struct CAssetWriter::SPlan
{
	struct SStep
	{
		std::filesystem::path From;      // Path, absolute
		std::filesystem::path To;        // ToPath, absolute (CopyFile, Move)
		IVcsProvider* Provider = nullptr;
	};

	SOpJournalEntry Entry;
	std::vector<SStep> Steps;            // beside Entry.Op.Steps
	std::vector<FString> CheckOut;       // mount paths
};


CAssetWriter::CAssetWriter (SAssetWriterDesc InDesc)
	: Desc(std::move(InDesc))
	, NormalizedOwnerRoot(platform::NormalizePath(Desc.OwnerRoot))
	, NoneProvider(std::make_unique<CNoneVcsProvider>())
	, Journal(GetOpJournalPath(Desc.OwnerRoot))
{
}

CAssetWriter::~CAssetWriter () = default;

bool CAssetWriter::Resolve (FStringView MountPath, std::filesystem::path& OutAbsolute, IVcsProvider*& OutProvider) const
{
	if (Desc.Mounts == nullptr)
	{
		return false;
	}
	FStringView mountName;
	FStringView relative;
	if (!SplitMountPath(MountPath, mountName, relative) || relative.IsEmpty())
	{
		return false;
	}
	const SMount* mount = Desc.Mounts->Find(mountName);
	const std::optional<std::filesystem::path> absolute = Desc.Mounts->FromMountPath(MountPath);
	if (mount == nullptr || !absolute.has_value())
	{
		return false;
	}
	const std::optional<FString> fromOwner = platform::GetRelativePath(mount->OwnerRoot, NormalizedOwnerRoot);
	if (!fromOwner.has_value() || !fromOwner->IsEmpty())
	{
		return false;
	}
	OutAbsolute = *absolute;
	OutProvider = NoneProvider.get();
	for (const auto& [id, provider] : Desc.Providers)
	{
		if (id == mount->Id && provider != nullptr)
		{
			OutProvider = provider;
		}
	}
	return true;
}

IVcsProvider* CAssetWriter::FindProvider (FStringView MountPath) const
{
	std::filesystem::path absolute;
	IVcsProvider* provider = nullptr;
	return Resolve(MountPath, absolute, provider) ? provider : nullptr;
}

std::optional<SOpJournalEntry> CAssetWriter::GetUnfinished () const
{
	SOpJournalContents contents;
	if (!ReadOpJournal(Journal.GetPath(), contents))
	{
		return std::nullopt;
	}
	return std::move(contents.Unfinished);
}

void CAssetWriter::SetStopAfterStepForTest (uint32 Steps, TUniqueFunction<void ()> OnStop)
{
	StopAfterStep = Steps;
	OnStopForTest = std::move(OnStop);
}


/**
 * Resolves and checks Op. A replay (roll-forward) takes the journalled steps as they are: they were checked and ordered
 * when the op first ran, and the disk has moved on since, so nothing is dropped, refused or reordered then.
 */
SVcsResult CAssetWriter::Prepare (const SAssetOp& Op, bool bReplay, SPlan& Out, SAssetWriteReport& Report) const
{
	Out = SPlan();
	Out.Entry.Op.Id = Op.Id;
	Out.Entry.Op.Verb = Op.Verb;
	Out.Entry.Op.PlannedGuids = Op.PlannedGuids;

	std::vector<SAssetOpStep> steps = Op.Steps;
	if (!bReplay)
	{
		// Metas before sources, within each run of consecutive writes.
		for (size_t begin = 0u; begin < steps.size();)
		{
			size_t end = begin;
			while (end < steps.size() && IsWrite(steps[end].Kind))
			{
				++end;
			}
			std::stable_partition(steps.begin() + static_cast<ptrdiff_t>(begin), steps.begin() + static_cast<ptrdiff_t>(end),
				[] (const SAssetOpStep& Step) { return GetMetaSource(GetWriteTarget(Step)).has_value(); });
			begin = end == begin ? begin + 1u : end;
		}
	}

	std::vector<FString> touched;   // mount paths an earlier step of this op writes, moves away, moves to or trashes
	std::vector<FString> written;   // write targets
	for (const SAssetOpStep& step : steps)
	{
		if (IsWrite(step.Kind))
		{
			AppendUnique(written, GetWriteTarget(step));
		}
	}

	for (SAssetOpStep& step : steps)
	{
		SPlan::SStep planned;
		if (!Resolve(step.Path, planned.From, planned.Provider))
		{
			return Failure(FString::Format("'{}' names no file in a mount of '{}'", ToDisplayText(step.Path), ToDisplayText(FString::FromPath(Desc.OwnerRoot))));
		}
		if (step.Kind == EAssetOpStep::Move || step.Kind == EAssetOpStep::CopyFile)
		{
			IVcsProvider* toProvider = nullptr;
			if (!Resolve(step.ToPath, planned.To, toProvider))
			{
				return Failure(FString::Format("'{}' names no file in a mount of '{}'", ToDisplayText(step.ToPath), ToDisplayText(FString::FromPath(Desc.OwnerRoot))));
			}
			if (step.Kind == EAssetOpStep::Move && toProvider != planned.Provider)
			{
				Report.bRefusedByVcs = true;
				return Failure(FString::Format("cannot move '{}' to '{}': they are under different version control", ToDisplayText(step.Path),
					ToDisplayText(step.ToPath)));
			}
		}

		const bool bTouched = std::find(touched.begin(), touched.end(), step.Path) != touched.end();
		switch (step.Kind)
		{
		case EAssetOpStep::WriteFile:
			step.Hash = hash::HashContent(step.Bytes);
			if (!bReplay && !bTouched && HoldsBytes(planned.From, step.Bytes))
			{
				++Report.StepsSkipped;   // the bytes are there already: no write, no check-out
				continue;
			}
			break;
		case EAssetOpStep::CopyFile:
			if (!bReplay)
			{
				if (!bTouched && !Exists(planned.From))
				{
					return Failure(FString::Format("cannot copy '{}': it does not exist", ToDisplayText(step.Path)));
				}
				if (Exists(planned.To))
				{
					return Failure(FString::Format("cannot copy '{}': '{}' exists", ToDisplayText(step.Path), ToDisplayText(step.ToPath)));
				}
				if (!step.Hash.IsValid())
				{
					const std::optional<hash::SContentHash> content = HashFileStable(planned.From);
					if (!content.has_value())
					{
						return Failure(FString::Format("cannot copy '{}': it cannot be read, or changed while it was read", ToDisplayText(step.Path)));
					}
					step.Hash = *content;
				}
			}
			break;
		case EAssetOpStep::Move:
			if (!bReplay && !bTouched && step.Path != step.ToPath)
			{
				const std::optional<platform::SFileStat> from = platform::Stat(planned.From);
				if (!from.has_value() || from->bDirectory)
				{
					return Failure(FString::Format("cannot move '{}': it does not exist", ToDisplayText(step.Path)));
				}
				// The same file in another spelling is a case-only rename, not an occupied target.
				const std::optional<platform::SFileStat> to = platform::Stat(planned.To);
				if (to.has_value() && !(to->Identity == from->Identity))
				{
					return Failure(FString::Format("cannot move '{}': '{}' exists", ToDisplayText(step.Path), ToDisplayText(step.ToPath)));
				}
			}
			break;
		case EAssetOpStep::Trash:
			break;
		}

		AppendUnique(touched, step.Path);
		if (!step.ToPath.IsEmpty())
		{
			AppendUnique(touched, step.ToPath);
		}
		Out.Entry.Op.Steps.push_back(std::move(step));
		Out.Steps.push_back(std::move(planned));
	}

	// CheckOut: each written file as it is named before the op (walking back through the Moves before it), and its pair.
	const std::vector<SAssetOpStep>& kept = Out.Entry.Op.Steps;
	for (size_t index = 0u; index < kept.size(); ++index)
	{
		if (!IsWrite(kept[index].Kind))
		{
			continue;
		}
		FString before = GetWriteTarget(kept[index]);
		for (size_t back = index; back-- > 0u;)
		{
			if (kept[back].Kind == EAssetOpStep::Move && kept[back].ToPath == before)
			{
				before = kept[back].Path;
			}
		}
		std::vector<FString> names { before };
		if (bReplay && before != GetWriteTarget(kept[index]))
		{
			names.push_back(GetWriteTarget(kept[index]));   // the Move may be done already
		}
		for (const FString& name : names)
		{
			AppendUnique(Out.CheckOut, name);
			const FString pair = GetPairPath(name);
			std::filesystem::path pairAbsolute;
			IVcsProvider* pairProvider = nullptr;
			if (std::find(written.begin(), written.end(), pair) != written.end()
				|| (Resolve(pair, pairAbsolute, pairProvider) && Exists(pairAbsolute)))
			{
				AppendUnique(Out.CheckOut, pair);
			}
		}
	}

	// Add: the files the op creates, which exist neither before it nor as an earlier Move's target, and their pairs.
	if (!bReplay)
	{
		for (size_t index = 0u; index < kept.size(); ++index)
		{
			if (!IsWrite(kept[index].Kind))
			{
				continue;
			}
			const FString& target = GetWriteTarget(kept[index]);
			const std::filesystem::path& absolute = kept[index].Kind == EAssetOpStep::CopyFile ? Out.Steps[index].To : Out.Steps[index].From;
			bool bMovedHere = false;
			for (size_t back = 0u; back < index; ++back)
			{
				bMovedHere = bMovedHere || (kept[back].Kind == EAssetOpStep::Move && kept[back].ToPath == target);
			}
			if (bMovedHere || Exists(absolute))
			{
				continue;
			}
			AppendUnique(Out.Entry.AddFiles, target);
			const FString pair = GetPairPath(target);
			std::filesystem::path pairAbsolute;
			IVcsProvider* pairProvider = nullptr;
			if (std::find(written.begin(), written.end(), pair) != written.end()
				|| (Resolve(pair, pairAbsolute, pairProvider) && Exists(pairAbsolute)))
			{
				AppendUnique(Out.Entry.AddFiles, pair);
			}
		}
	}
	return {};
}

SVcsResult CAssetWriter::CheckOutPlanned (const SPlan& Plan, SAssetWriteReport& Report)
{
	std::vector<SProviderBatch<FString>> batches;
	for (const FString& path : Plan.CheckOut)
	{
		std::filesystem::path absolute;
		IVcsProvider* provider = nullptr;
		if (Resolve(path, absolute, provider))
		{
			AddToBatch(batches, provider, FString::FromPath(absolute));
		}
	}
	for (const SProviderBatch<FString>& batch : batches)
	{
		SVcsResult result = batch.Provider->CheckOut(batch.Items);
		if (result.Error != EVcsError::None)
		{
			Report.bRefusedByVcs = true;
			return result;
		}
	}
	return {};
}

SVcsResult CAssetWriter::StopForTest (SAssetWriteReport& Report)
{
	StopAfterStep = ~0u;
	Report.bLeftUnfinished = true;
	TUniqueFunction<void ()> onStop = std::move(OnStopForTest);
	if (onStop)
	{
		onStop();
	}
	return Failure("stopped by a test");
}

/**
 * The steps, then Add, then End. Every step checks what is on disk first, so this is both the first run and the replay.
 */
SVcsResult CAssetWriter::Execute (const SPlan& Plan, SAssetWriteReport& Report)
{
	const std::vector<SAssetOpStep>& steps = Plan.Entry.Op.Steps;
	const size_t count = steps.size();
	size_t index = 0u;
	while (index < count)
	{
		if (index == StopAfterStep)
		{
			return StopForTest(Report);
		}
		const SAssetOpStep& step = steps[index];
		const SPlan::SStep& planned = Plan.Steps[index];

		if (step.Kind == EAssetOpStep::Move || step.Kind == EAssetOpStep::Trash)
		{
			// The run of steps of this kind, cut where a test stops.
			size_t end = index + 1u;
			while (end < count && steps[end].Kind == step.Kind && end != StopAfterStep)
			{
				++end;
			}
			std::vector<SProviderBatch<SVcsMove>> moves;
			std::vector<SProviderBatch<FString>> trashes;
			std::vector<size_t> pending;
			for (size_t at = index; at < end; ++at)
			{
				const SPlan::SStep& one = Plan.Steps[at];
				if (step.Kind == EAssetOpStep::Move)
				{
					const bool bFromThere = IsSpelledOnDisk(one.From);
					if (!bFromThere && IsSpelledOnDisk(one.To))
					{
						++Report.StepsSkipped;   // moved already
						continue;
					}
					if (!bFromThere && !Exists(one.From))
					{
						log::Warning(log::Assets, "The operation '{}' cannot move '{}' to '{}': the file is gone.", Plan.Entry.Op.Verb,
							ToDisplayText(steps[at].Path), ToDisplayText(steps[at].ToPath));
						++Report.StepsSkipped;
						continue;
					}
					AddToBatch(moves, one.Provider, SVcsMove { FString::FromPath(one.From), FString::FromPath(one.To) });
				}
				else
				{
					if (!Exists(one.From))
					{
						++Report.StepsSkipped;   // gone already
						continue;
					}
					AddToBatch(trashes, one.Provider, FString::FromPath(one.From));
				}
				pending.push_back(at);
			}
			for (const SProviderBatch<SVcsMove>& batch : moves)
			{
				SVcsResult result = batch.Provider->Move(batch.Items);
				if (result.Error != EVcsError::None)
				{
					Report.bRefusedByVcs = true;
					for (const SVcsMove& move : batch.Items)
					{
						Report.bChangedDisk = Report.bChangedDisk || !IsSpelledOnDisk(move.From.ToPath());
					}
					return result;
				}
				Report.bChangedDisk = true;
			}
			for (const SProviderBatch<FString>& batch : trashes)
			{
				SVcsResult result = batch.Provider->Delete(batch.Items);
				if (result.Error != EVcsError::None)
				{
					Report.bRefusedByVcs = true;
					for (const FString& file : batch.Items)
					{
						Report.bChangedDisk = Report.bChangedDisk || !Exists(file.ToPath());
					}
					return result;
				}
				Report.bChangedDisk = true;
			}
			Report.StepsRun += static_cast<uint32>(pending.size());
			index = end;
			continue;
		}

		if (step.Kind == EAssetOpStep::WriteFile)
		{
			if (HoldsBytes(planned.From, step.Bytes))
			{
				++Report.StepsSkipped;
			}
			else
			{
				FString error;
				if (!platform::WriteFileAtomic(planned.From, step.Bytes, true, &error))
				{
					return Failure(FString::Format("cannot write '{}': {}", ToDisplayText(step.Path), ToDisplayText(error)));
				}
				Report.bChangedDisk = true;
				++Report.StepsRun;
			}
		}
		else   // CopyFile
		{
			const std::optional<hash::SContentHash> there = Exists(planned.To) ? HashFileStable(planned.To) : std::nullopt;
			if (there.has_value() && *there == step.Hash)
			{
				++Report.StepsSkipped;
			}
			else if (!Exists(planned.From))
			{
				log::Warning(log::Assets, "The operation '{}' cannot copy '{}' to '{}': the file is gone.", Plan.Entry.Op.Verb,
					ToDisplayText(step.Path), ToDisplayText(step.ToPath));
				++Report.StepsSkipped;
			}
			else
			{
				FString error;
				const std::optional<std::vector<uint8>> bytes = platform::ReadFileBytes(planned.From, &error);
				if (!bytes.has_value() || !platform::WriteFileAtomic(planned.To, *bytes, true, &error))
				{
					return Failure(FString::Format("cannot copy '{}' to '{}': {}", ToDisplayText(step.Path), ToDisplayText(step.ToPath),
						ToDisplayText(error)));
				}
				Report.bChangedDisk = true;
				++Report.StepsRun;
			}
		}
		++index;
	}
	if (count == StopAfterStep)
	{
		return StopForTest(Report);
	}

	std::vector<SProviderBatch<FString>> adds;
	for (const FString& path : Plan.Entry.AddFiles)
	{
		std::filesystem::path absolute;
		IVcsProvider* provider = nullptr;
		if (Resolve(path, absolute, provider) && Exists(absolute))
		{
			AddToBatch(adds, provider, FString::FromPath(absolute));
		}
	}
	for (const SProviderBatch<FString>& batch : adds)
	{
		SVcsResult result = batch.Provider->Add(batch.Items);
		if (result.Error != EVcsError::None)
		{
			Report.bRefusedByVcs = true;
			return result;
		}
	}

	FString error;
	if (!Journal.End(Plan.Entry.Op.Id, &error))
	{
		// Every step is done; the log then holds a finished op or one whose replay changes nothing.
		log::Warning(log::Assets, "The operation journal '{}' could not be closed: {}", ToDisplayText(FString::FromPath(Journal.GetPath())),
			ToDisplayText(error));
	}
	return {};
}


SVcsResult CAssetWriter::Run (const SAssetOp& Op)
{
	SAssetWriteReport report;
	return Run(Op, report);
}

SVcsResult CAssetWriter::Run (const SAssetOp& Op, SAssetWriteReport& OutReport)
{
	OutReport = SAssetWriteReport();
	if (!Desc.bHoldsLock)
	{
		return Failure(FString::Format("this process does not hold the writer lock of '{}', so it writes nothing",
			ToDisplayText(FString::FromPath(Desc.OwnerRoot))));
	}
	if (GetUnfinished().has_value())
	{
		FString error;
		if (!RollForward(&error))
		{
			return Failure(FString::Format("an earlier operation could not be finished, so '{}' does not start: {}", Op.Verb, error));
		}
	}

	SPlan plan;
	SVcsResult result = Prepare(Op, false, plan, OutReport);
	if (result.Error != EVcsError::None || plan.Entry.Op.Steps.empty())
	{
		return result;
	}
	result = CheckOutPlanned(plan, OutReport);
	if (result.Error != EVcsError::None)
	{
		return result;
	}
	FString error;
	if (!Journal.Begin(plan.Entry, &error))
	{
		Journal.Clear();
		return Failure(FString::Format("cannot write the operation journal '{}', so nothing was written: {}",
			ToDisplayText(FString::FromPath(Journal.GetPath())), ToDisplayText(error)));
	}
	result = Execute(plan, OutReport);
	if (result.Error != EVcsError::None)
	{
		if (!OutReport.bLeftUnfinished && !OutReport.bChangedDisk)
		{
			Journal.Clear();   // nothing changed: the op is given up, not left for a replay
		}
		else
		{
			OutReport.bLeftUnfinished = true;
		}
	}
	return result;
}

bool CAssetWriter::RollForward (FString* OutError)
{
	SAssetWriteReport report;
	return RollForward(report, OutError);
}

bool CAssetWriter::RollForward (SAssetWriteReport& OutReport, FString* OutError)
{
	OutReport = SAssetWriteReport();
	SOpJournalContents contents;
	if (!ReadOpJournal(Journal.GetPath(), contents, OutError))
	{
		return false;
	}
	if (!contents.Unfinished.has_value())
	{
		if (!contents.bEmpty && Desc.bHoldsLock)
		{
			Journal.Clear();   // a finished op or a torn tail: nothing to do
		}
		return true;
	}
	const SOpJournalEntry& entry = *contents.Unfinished;
	if (!Desc.bHoldsLock)
	{
		if (OutError != nullptr)
		{
			*OutError = FString::Format("the operation '{}' in '{}' is unfinished, and only the holder of the writer lock finishes it", entry.Op.Verb,
				ToDisplayText(FString::FromPath(Journal.GetPath())));
		}
		return false;
	}

	SPlan plan;
	SVcsResult result = Prepare(entry.Op, true, plan, OutReport);
	plan.Entry.AddFiles = entry.AddFiles;
	if (result.Error == EVcsError::None)
	{
		result = CheckOutPlanned(plan, OutReport);
	}
	if (result.Error == EVcsError::None)
	{
		result = Execute(plan, OutReport);
	}
	if (result.Error != EVcsError::None)
	{
		OutReport.bLeftUnfinished = true;
		if (OutError != nullptr)
		{
			*OutError = FString::Format("the operation '{}' could not be finished: {}", entry.Op.Verb, result.Message);
		}
		return false;
	}
	log::Info(log::Assets, "Finished the operation '{}' that an earlier process left unfinished ({} steps run, {} done already).",
		entry.Op.Verb, OutReport.StepsRun, OutReport.StepsSkipped);
	return true;
}


void ForEachExpectedWrite (const SAssetOp& Op, TFunctionRef<void (FStringView MountPath, const hash::SContentHash& Hash, uint64 Size)> Visit)
{
	for (const SAssetOpStep& step : Op.Steps)
	{
		if (step.Kind == EAssetOpStep::WriteFile)
		{
			Visit(step.Path, hash::HashContent(step.Bytes), step.Bytes.size());
		}
	}
}
}
