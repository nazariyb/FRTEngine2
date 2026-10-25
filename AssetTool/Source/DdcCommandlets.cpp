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
 *            else 20) GiB, least recently used first, never evicting a record the mount's BuildState
 *            names or a value such a record uses; with --ci a dry run that deletes nothing and says what
 *            it would. Exit 1 when the content stays over the budget or the collection could not finish.
 *   verify   re-checks every file and deletes the corrupt ones, as any read of them would. Exit 1 when it
 *            found any.
 *
 * --json prints one object per verb with stable field names (counts and byte sizes as numbers).
 */
#include <filesystem>
#include <optional>
#include <span>
#include <system_error>
#include <vector>

#include "Assets/BuildState.h"
#include "Assets/DerivedDataCache.h"
#include "Config/Config.h"
#include "Jobs/WorkerPool.h"
#include "Platform/BlockingFileReader.h"
#include "Platform/FileSystem.h"

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

// The keys the mount's BuildState names: what garbage collection must keep (§7.4 "GC"). Only from a log
// that exists and is one - opening anything else would start it anew, a write - and none without a
// mount. OutNote says where they came from, or why there are none.
std::vector<hash::SContentHash> CollectPinnedKeys (const CCommandletContext& Context, FString& OutNote)
{
	if (Context.GetMountRoot().empty())
	{
		OutNote = "no mount, so no BuildState to keep";
		return {};
	}
	const std::filesystem::path file = assets::GetBuildStatePath(Context.GetMountRoot() / "Local");
	constexpr FStringLiteral Header = "FRTBSLG1";   // BuildState.h: File = "FRTBSLG1", Record*
	std::vector<uint8> header(Header.Len());
	platform::CBlockingFileReader reader;
	if (!reader.Open(file) || reader.GetSize() < header.size() || !reader.ReadExactAt(0u, header)
		|| FStringView(reinterpret_cast<const char*>(header.data()), header.size()) != FStringView(Header))
	{
		OutNote = FString::Format("{} has no BuildState to keep", Context.GetMountName());
		return {};
	}
	reader.Close();

	assets::CBuildStateLog log;
	FString error;
	if (!log.Open(file, &error))
	{
		OutNote = FString::Format("{}'s BuildState cannot be read ({}): nothing of it is kept", Context.GetMountName(), error);
		return {};
	}
	std::vector<hash::SContentHash> keys = log.CollectKeys();
	OutNote = FString::Format("{} key(s) of {}'s BuildState kept", keys.size(), Context.GetMountName());
	return keys;
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
	FString pinNote;
	const std::vector<hash::SContentHash> pinned = CollectPinnedKeys(Context, pinNote);
	const assets::EDdcGcMode mode = Context.bReadOnly ? assets::EDdcGcMode::DryRun : assets::EDdcGcMode::Delete;
	assets::SDdcGcResult result;
	RunOnWorker(Context, [&] () { result = Ddc.CollectGarbage(MaxBytes, pinned, mode); });

	const FStringView verb = result.bDryRun ? FStringView("would evict") : FStringView("evicted");
	Context.Print(FString::Format("Derived-data cache: {}{}", Root, result.bDryRun ? " (read-only: a dry run, nothing deleted)" : ""));
	Context.Print(FString::Format("  content {} -> {} (budget {}); {} pinned: {}", FormatBytes(result.ContentBytesBefore),
		FormatBytes(result.ContentBytesAfter), FormatBytes(MaxBytes), FormatBytes(result.PinnedBytes), pinNote));
	Context.Print(FString::Format("  {} {} record(s), {} value(s), {} manifest(s); {} {} Tmp/ leftover(s), {} old access log(s)", verb,
		result.RecordsEvicted, result.ValuesEvicted, result.ManifestsEvicted, result.bDryRun ? "would delete" : "deleted",
		result.TmpFilesDeleted, result.AccessLogsDeleted));
	Context.PrintJson(CJsonObject().AddString("ddc", Root).AddBool("exists", true).AddBool("dryRun", result.bDryRun)
		.AddUInt("maxBytes", MaxBytes).AddUInt("contentBytesBefore", result.ContentBytesBefore)
		.AddUInt("contentBytesAfter", result.ContentBytesAfter).AddUInt("pinnedKeys", pinned.size())
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
		Context.PrintError(FString::Format("ddc gc: the content stays over the budget of {}: what the BuildState keeps is {}",
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

	// The store: --ddc, else the machine's.
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
	const FString rootText = FString::FromPath(root);

	// No FORMAT, no store: nothing to do, and nothing is created to find that out.
	if (!platform::Stat(root / "FORMAT").has_value())
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
