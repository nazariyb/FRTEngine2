/**
 * ddc stats|gc|verify: maintenance of the derived-data cache (design §7.4, §13.2) through
 * CDerivedDataCache, on the machine's store (CDerivedDataCache::GetDefaultRoot: FRT_DDC_DIR, else
 * [DerivedDataCache] Path, else the user's own) or the one --ddc=<dir> names.
 *
 * The store is shared by every process on the machine and is safe without locks, so these verbs take
 * no writer lock and run beside an open editor. They never create a store: a root without a FORMAT
 * file holds none, and there is nothing to report, trim or check. The work itself (a walk of every
 * file, hashing every value) runs on the host's worker pool while this thread waits by pumping.
 *
 *   stats    files and bytes of each kind of file
 *   gc       trims the content (manifests, records, values) to --max-gb (default [DerivedDataCache] MaxGB,
 *            else 20) GiB, least recently used first, never evicting a record that a BuildState of this
 *            machine names (the mount's, the engines', the recent projects': CollectPinnedKeys) or a value
 *            such a record uses; with --ci a dry run that deletes nothing and says what it would. Exit 1
 *            when the content stays over the budget or the collection could not finish.
 *   verify   re-checks every file and deletes the corrupt ones, as any read of them would. Exit 1 when it
 *            found any.
 *
 * --json prints one object per verb with stable field names (counts and byte sizes as numbers).
 */
#include <algorithm>
#include <filesystem>
#include <optional>
#include <span>
#include <system_error>
#include <utility>
#include <vector>

#include "Assets/BuildState.h"
#include "Assets/DerivedDataCache.h"
#include "Config/Config.h"
#include "Jobs/WorkerPool.h"
#include "Platform/FileSystem.h"
#include "Project/UserRegistry.h"

#include "Commandlet.h"


namespace frt::assettool
{
namespace
{
constexpr uint64 BytesPerGiB = 1024ull * 1024ull * 1024ull;
// A budget of more than this many GiB is a typo, not a cache size (and would overflow the bytes).
constexpr double MaxBudgetGiB = 1024.0 * 1024.0;

// "123 B", "4.5 KiB", "6.7 MiB", "8.9 GiB".
FString FormatBytes (uint64 Bytes)
{
	if (Bytes < 1024u)
	{
		return FString::Format("{} B", Bytes);
	}
	constexpr FStringLiteral Units[] = { "KiB", "MiB", "GiB", "TiB" };
	double value = static_cast<double>(Bytes) / 1024.0;
	uint32 unit = 0u;
	while (value >= 1024.0 && unit + 1u < std::size(Units))
	{
		value /= 1024.0;
		++unit;
	}
	return FString::Format("{:.1f} {}", value, Units[unit]);
}

// A budget in GiB as given (--max-gb=, [DerivedDataCache] MaxGB): a number from 0 up; nullopt otherwise.
std::optional<uint64> ParseBudget (FStringView Text)
{
	const std::optional<double> gib = Text.TrimStartAndEnd().ToDouble();
	if (!gib || !(*gib >= 0.0) || *gib > MaxBudgetGiB)
	{
		return std::nullopt;
	}
	return static_cast<uint64>(*gib * static_cast<double>(BytesPerGiB));
}

// Runs Work on the host's worker pool and waits for it here, pumping (Jobs/WorkerPool.h).
template <typename F>
void RunOnWorker (CCommandletContext& Context, F&& Work)
{
	jobs::CWorkerPool& pool = Context.GetWorkerPool();
	bool bDone = false;
	pool.Submit(jobs::EWorkPriority::High, jobs::NoCancelGroup, [&Work] () { Work(); }, [&bDone] () { bDone = true; });
	pool.PumpUntil([&bDone] () { return bDone; });
}

// What garbage collection must keep (§7.4 "GC": never a value that any open project's BuildState points
// at), and from how many logs.
struct SPinnedKeys
{
	std::vector<hash::SContentHash> Keys;
	uint32 LogsRead = 0u;
	std::vector<FString> Unreadable;   // "<file> (<why>)" of each log that is there but could not be read
};

// The keys every BuildState log this machine knows of names. The store is machine-wide, so not only the
// mount's own log: the engine's (its /Engine content), every engine FRT.bat setup registered, and the log of
// every project in the user's recent list, to which the project manager and the editor add each project
// they open (project::GetRecentProjects). A log is read once and never opened for writing
// (CBuildStateLog::ReadKeys): another process may own it, and a missing or foreign file is left as it is,
// never started anew. The same log reached by two paths is read once.
SPinnedKeys CollectPinnedKeys (const CCommandletContext& Context)
{
	std::vector<std::filesystem::path> mountRoots;
	if (!Context.GetMountRoot().empty())
	{
		mountRoots.push_back(Context.GetMountRoot());
	}
	if (!Context.EngineRoot.empty())
	{
		mountRoots.push_back(Context.EngineRoot);
	}
	for (const std::pair<FString, std::filesystem::path>& engine : project::GetRegisteredEngines())
	{
		if (!engine.second.empty())
		{
			mountRoots.push_back(engine.second);
		}
	}
	for (const project::SRecentProject& recent : project::GetRecentProjects())
	{
		if (!recent.File.empty())
		{
			mountRoots.push_back(recent.File.parent_path());
		}
	}

	SPinnedKeys pinned;
	std::vector<platform::SFileIdentity> seen;
	for (const std::filesystem::path& mountRoot : mountRoots)
	{
		const std::filesystem::path file = assets::GetBuildStatePath(mountRoot);
		const std::optional<platform::SFileStat> stat = platform::Stat(file);
		if (!stat || stat->bDirectory)
		{
			continue;   // no BuildState there: nothing to keep
		}
		if (stat->Identity.IsValid())
		{
			if (std::find(seen.begin(), seen.end(), stat->Identity) != seen.end())
			{
				continue;
			}
			seen.push_back(stat->Identity);
		}
		FString error;
		const std::optional<std::vector<hash::SContentHash>> keys = assets::CBuildStateLog::ReadKeys(file, &error);
		if (!keys)
		{
			pinned.Unreadable.push_back(FString::Format("{} ({})", FString::FromPath(file), error));
			continue;
		}
		++pinned.LogsRead;
		pinned.Keys.insert(pinned.Keys.end(), keys->begin(), keys->end());
	}
	std::sort(pinned.Keys.begin(), pinned.Keys.end());
	pinned.Keys.erase(std::unique(pinned.Keys.begin(), pinned.Keys.end()), pinned.Keys.end());
	return pinned;
}

int32 RunStats (CCommandletContext& Context, assets::CDerivedDataCache& Ddc, const FString& Root)
{
	assets::SDdcStats stats;
	std::vector<FString> verdicts;
	RunOnWorker(Context, [&] ()
	{
		stats = Ddc.GetStats();
		verdicts = Ddc.ReadToolVerdicts();
	});

	const uint64 total = stats.GetContentBytes() + stats.Tmp.Bytes + stats.Access.Bytes + stats.Other.Bytes;
	Context.Print(FString::Format("Derived-data cache: {}", Root));
	const auto line = [&Context] (FStringView Kind, const assets::SDdcFileStats& Files)
	{
		Context.Print(FString::Format("  {:<12} {:>8} file(s)  {:>10}", Kind, Files.Files, FormatBytes(Files.Bytes)));
	};
	line("manifests", stats.Manifests);
	line("records", stats.Records);
	line("values", stats.Values);
	line("tmp", stats.Tmp);
	line("access logs", stats.Access);
	line("other", stats.Other);
	Context.Print(FString::Format("  content {} (manifests, records, values), {} in all; {} tool verdict(s)", FormatBytes(stats.GetContentBytes()),
		FormatBytes(total), verdicts.size()));
	Context.PrintJson(CJsonObject().AddString("ddc", Root).AddBool("exists", true)
		.AddUInt("manifestFiles", stats.Manifests.Files).AddUInt("manifestBytes", stats.Manifests.Bytes)
		.AddUInt("recordFiles", stats.Records.Files).AddUInt("recordBytes", stats.Records.Bytes)
		.AddUInt("valueFiles", stats.Values.Files).AddUInt("valueBytes", stats.Values.Bytes)
		.AddUInt("tmpFiles", stats.Tmp.Files).AddUInt("tmpBytes", stats.Tmp.Bytes)
		.AddUInt("accessFiles", stats.Access.Files).AddUInt("accessBytes", stats.Access.Bytes)
		.AddUInt("otherFiles", stats.Other.Files).AddUInt("otherBytes", stats.Other.Bytes)
		.AddUInt("contentBytes", stats.GetContentBytes()).AddUInt("totalBytes", total).AddUInt("toolVerdicts", verdicts.size()));
	return ExitSuccess;
}

int32 RunGc (CCommandletContext& Context, assets::CDerivedDataCache& Ddc, const FString& Root, uint64 MaxBytes)
{
	const SPinnedKeys pinned = CollectPinnedKeys(Context);
	for (const FString& unreadable : pinned.Unreadable)
	{
		Context.PrintError(FString::Format("warning: the BuildState log {} cannot be read: nothing of it is kept",
			unreadable));
	}
	const FString pinNote = FString::Format("{} key(s) of {} BuildState log(s) kept", pinned.Keys.size(),
		pinned.LogsRead);
	const assets::EDdcGcMode mode = Context.bReadOnly ? assets::EDdcGcMode::DryRun : assets::EDdcGcMode::Delete;
	assets::SDdcGcResult result;
	RunOnWorker(Context, [&] () { result = Ddc.CollectGarbage(MaxBytes, pinned.Keys, mode); });

	const FStringView verb = result.bDryRun ? FStringView("would evict") : FStringView("evicted");
	Context.Print(FString::Format("Derived-data cache: {}{}", Root, result.bDryRun ? " (read-only: a dry run, nothing deleted)" : ""));
	Context.Print(FString::Format("  content {} -> {} (budget {}); {} pinned: {}", FormatBytes(result.ContentBytesBefore),
		FormatBytes(result.ContentBytesAfter), FormatBytes(MaxBytes), FormatBytes(result.PinnedBytes), pinNote));
	Context.Print(FString::Format("  {} {} record(s), {} value(s), {} manifest(s); {} {} Tmp/ leftover(s), {} old access log(s)", verb,
		result.RecordsEvicted, result.ValuesEvicted, result.ManifestsEvicted, result.bDryRun ? "would delete" : "deleted",
		result.TmpFilesDeleted, result.AccessLogsDeleted));
	Context.PrintJson(CJsonObject().AddString("ddc", Root).AddBool("exists", true).AddBool("dryRun", result.bDryRun)
		.AddUInt("maxBytes", MaxBytes).AddUInt("contentBytesBefore", result.ContentBytesBefore)
		.AddUInt("contentBytesAfter", result.ContentBytesAfter).AddUInt("pinnedKeys", pinned.Keys.size())
		.AddUInt("buildStateLogs", pinned.LogsRead).AddUInt("buildStateLogsUnreadable", pinned.Unreadable.size())
		.AddUInt("pinnedBytes", result.PinnedBytes).AddUInt("recordsEvicted", result.RecordsEvicted)
		.AddUInt("valuesEvicted", result.ValuesEvicted).AddUInt("manifestsEvicted", result.ManifestsEvicted)
		.AddUInt("tmpFilesDeleted", result.TmpFilesDeleted).AddUInt("accessLogsDeleted", result.AccessLogsDeleted)
		.AddUInt("recordsUnreadable", result.RecordsUnreadable).AddBool("withinBudget", result.bWithinBudget)
		.AddBool("complete", result.bComplete));

	if (!result.bComplete)
	{
		Context.PrintError(FString::Format("ddc gc: {} record(s) could not be read now (in use?), so only leftovers were trimmed: "
			"run it again later", result.RecordsUnreadable));
		return ExitProblems;
	}
	if (!result.bWithinBudget)
	{
		Context.PrintError(FString::Format("ddc gc: the content stays over the budget of {}: what the BuildState logs keep is {}",
			FormatBytes(MaxBytes), FormatBytes(result.PinnedBytes)));
		return ExitProblems;
	}
	return ExitSuccess;
}

int32 RunVerify (CCommandletContext& Context, assets::CDerivedDataCache& Ddc, const FString& Root)
{
	assets::SDdcVerifyResult result;
	RunOnWorker(Context, [&] () { result = Ddc.Verify(); });

	Context.Print(FString::Format("Derived-data cache: {}", Root));
	Context.Print(FString::Format("  checked {} manifest(s), {} record(s), {} value(s)", result.ManifestsChecked, result.RecordsChecked,
		result.ValuesChecked));
	Context.Print(result.GetProblemCount() == 0u ? FString("  every file is intact") : FString::Format("  deleted {} corrupt manifest(s), "
		"{} corrupt record(s), {} corrupt value(s), {} record(s) whose value is gone: {}", result.CorruptManifests, result.CorruptRecords,
		result.CorruptValues, result.DanglingRecords, FormatBytes(result.BytesDeleted)));
	Context.PrintJson(CJsonObject().AddString("ddc", Root).AddBool("exists", true)
		.AddUInt("manifestsChecked", result.ManifestsChecked).AddUInt("recordsChecked", result.RecordsChecked)
		.AddUInt("valuesChecked", result.ValuesChecked).AddUInt("corruptManifests", result.CorruptManifests)
		.AddUInt("corruptRecords", result.CorruptRecords).AddUInt("corruptValues", result.CorruptValues)
		.AddUInt("danglingRecords", result.DanglingRecords).AddUInt("bytesDeleted", result.BytesDeleted));
	return result.GetProblemCount() == 0u ? ExitSuccess : ExitProblems;
}
}


int32 RunDdc (CCommandletContext& Context)
{
	constexpr FStringLiteral Usage = "ddc stats|gc [--max-gb=<n>]|verify [--ddc=<dir>]";
	if (Context.Arguments.size() != 1u)
	{
		return Context.Fail(ExitUsage, FString::Format("ddc: takes one of stats, gc and verify ({})", Usage));
	}
	const FString& action = Context.Arguments[0];
	const bool bStats = action.View().Equals<ESearchCase::IgnoreCaseAscii>("stats");
	const bool bGc = action.View().Equals<ESearchCase::IgnoreCaseAscii>("gc");
	const bool bVerify = action.View().Equals<ESearchCase::IgnoreCaseAscii>("verify");
	if (!bStats && !bGc && !bVerify)
	{
		return Context.Fail(ExitUsage, FString::Format("ddc: no '{}': stats, gc or verify ({})", action, Usage));
	}

	// The budget: --max-gb (gc only), else [DerivedDataCache] MaxGB, else the default.
	const std::optional<FString> maxGb = Context.FindOption("max-gb");
	if (maxGb && !bGc)
	{
		return Context.Fail(ExitUsage, FString::Format("ddc {}: --max-gb is for gc ({})", action, Usage));
	}
	uint64 maxBytes = static_cast<uint64>(assets::DdcDefaultMaxGB * static_cast<double>(BytesPerGiB));
	if (maxGb)
	{
		const std::optional<uint64> parsed = ParseBudget(*maxGb);
		if (!parsed)
		{
			return Context.Fail(ExitUsage, FString::Format("ddc gc: --max-gb={} is not a number of GiB from 0 up", *maxGb));
		}
		maxBytes = *parsed;
	}
	else if (const FString configured = Context.GetConfig().Get("DerivedDataCache", "MaxGB", FStringView()); !configured.IsEmpty())
	{
		if (const std::optional<uint64> parsed = ParseBudget(configured))
		{
			maxBytes = *parsed;
		}
		else
		{
			Context.PrintError(FString::Format("warning: [DerivedDataCache] MaxGB={} is not a number of GiB from 0 up: using {}",
				configured, assets::DdcDefaultMaxGB));
		}
	}

	// The store: --ddc, a path argument, so relative to the working directory like every other (it wins
	// over FRT_DDC_DIR); else the machine's (GetDefaultRoot: FRT_DDC_DIR, then [DerivedDataCache] Path=,
	// a config value, which -DerivedDataCache:Path= overrides like any other and which, as in a config
	// file, is relative to the per-user data directory).
	std::filesystem::path root;
	if (const std::optional<FString> dir = Context.FindOption("ddc"))
	{
		const std::optional<std::filesystem::path> path = dir->View().TrimStartAndEnd().TryToPath();
		if (!path || path->empty())
		{
			return Context.Fail(ExitUsage, FString::Format("ddc: --ddc='{}' names no directory", *dir));
		}
		std::error_code ec;
		root = std::filesystem::absolute(*path, ec);
		if (ec)
		{
			root = *path;
		}
	}
	else
	{
		root = assets::CDerivedDataCache::GetDefaultRoot(&Context.GetConfig());
	}
	// One separator style in what scripts read, whichever way the parts were joined.
	root = root.lexically_normal();
	root.make_preferred();
	const FString rootText = FString::FromPath(root);

	// No store: nothing to do, and nothing is created to find that out.
	if (!assets::CDerivedDataCache::HoldsStore(root))
	{
		Context.Print(FString::Format("No derived-data cache at {}: nothing to {}", rootText, bStats ? "count" : (bGc ? "trim" : "check")));
		Context.PrintJson(CJsonObject().AddString("ddc", rootText).AddBool("exists", false));
		return ExitSuccess;
	}

	assets::CDerivedDataCache ddc;
	FString error;
	if (!ddc.Open(root, &error))
	{
		return Context.Fail(ExitProblems, FString::Format("ddc {}: cannot open the derived-data cache at {}: {}", action, rootText, error));
	}
	if (bStats)
	{
		return RunStats(Context, ddc, rootText);
	}
	if (bGc)
	{
		return RunGc(Context, ddc, rootText, maxBytes);
	}
	return RunVerify(Context, ddc, rootText);
}
}
