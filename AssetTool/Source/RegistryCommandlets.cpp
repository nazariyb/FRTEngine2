/**
 * The asset registry's verbs (design §13.2; M3, registry v0, read-only): scan, status, query, refs, referencers,
 * explain, list-rules and dump registry. Each builds its own CAssetRegistry (Assets/AssetRegistry.h) over the
 * mounts of the run - the project's /Engine, /Project and plugins, or /Engine alone with --engine - loads the
 * snapshots, runs the incremental scan, answers from the registry and shuts it down before it returns, so nothing
 * of it outlives the verb (the host drains the pool right after).
 *
 * Writes. Of these verbs only `scan` writes, and only while the host holds the mount's writer lock
 * (SCommandlet::bWritesProject, not a read-only run): its registry is then the writer and writes the snapshot of the
 * mounts it owns (/Project and its plugins: Local/Assets/AssetRegistry.snapshot; with --engine:
 * <engine>/Local/Registry/Engine.snapshot), its journal, Rules.cache and ProjectId - and never a meta: it opens no
 * lifecycle (edit [66]). Every other verb here builds a reader that writes nothing at all, beside an open editor too.
 * `list-rules` therefore prints Rules.cache's lines without writing the file (an M3 deviation from §13.2: the lock
 * holder writes it). The meta verbs (MetaCommandlets.cpp) open their registry here with a lifecycle of their own.
 * `status` names each mount's VCS provider, so it alone of these detects them (git runs once per mount); the others
 * spawn nothing.
 *
 * Every verb needs a mount: a project, or --engine (the host requires one only for verbs that write). Deltas commit
 * strictly in sequence order (jobs::ECommitOrder::Sequence, §5.9: tools that promise reproducible output), and
 * the dumps are the same however the registry got there - fresh, warm or incremental.
 *
 * Exit codes (§13.1): 0 success; 1 when an error-severity problem exists (scan, status); 2 for a usage error,
 * including a query that does not parse or compile and an <asset> that names no record or several (PATH001), with
 * the positioned message; 3 when the registry could not finish.
 */
#include <algorithm>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#include "EnginePaths.h"
#include "Assets/AssetDiagnosticCodes.h"
#include "Assets/AssetRegistry.h"
#include "Assets/AssetTypes.h"
#include "Assets/Discovery/Mounts.h"
#include "Assets/Meta/Lifecycle.h"
#include "Assets/Meta/MetaFile.h"
#include "Assets/Meta/MetaPaths.h"
#include "Assets/Pipeline/ImporterTable.h"
#include "Assets/Pipeline/Rules.h"
#include "Assets/Registry/AssetQuery.h"
#include "Assets/Registry/QueryEngine.h"
#include "Assets/Registry/RegistryView.h"
#include "Jobs/WorkerPool.h"
#include "Platform/FileSystem.h"
#include "Project/Project.h"

#include "Commandlet.h"
#include "RegistryRun.h"


namespace frt::assettool
{
namespace
{
using assets::ToDisplayText;

// A diagnostic's position in a query text, as the search box shows it: the message, then the text with a caret.
void PrintQueryError (const CCommandletContext& Context, FStringView Verb, FStringView Text, const SDiagnostic& Error)
{
	Context.PrintError(FString::Format("{}: {}{}", Verb, Error.Column > 0u ? FString::Format("column {}: ", Error.Column) : FString(),
		Error.Message));
	if (Error.Column > 0u && !Text.IsEmpty())
	{
		Context.PrintError(FString::Format("  {}", Text));
		FString caret("  ");
		for (uint32 i = 1u; i < Error.Column && i <= Text.Len(); ++i)
		{
			// One caret column per code point of the text before it.
			if ((static_cast<unsigned char>(Text[i - 1u]) & 0xC0u) != 0x80u)
			{
				caret += ' ';
			}
		}
		caret += '^';
		Context.PrintError(caret);
	}
}

int32 FailQuery (const CCommandletContext& Context, FStringView Verb, FStringView Text, const SDiagnostic& Error)
{
	PrintQueryError(Context, Verb, Text, Error);
	if (Context.bJson)
	{
		Context.PrintJson(CJsonObject().AddString("error", Error.Message).AddInt("exitCode", ExitUsage).AddUInt("column", Error.Column));
	}
	return ExitUsage;
}

std::vector<FString> MountNames (const assets::CMountTable& Mounts)
{
	std::vector<FString> names;
	for (const assets::SMount& mount : Mounts.GetMounts())
	{
		names.push_back(mount.GetRootMountPath());
	}
	return names;
}

struct SProblemCounts
{
	uint32 Errors = 0u;
	uint32 Warnings = 0u;
	uint32 Infos = 0u;
};

SProblemCounts CountProblems (const assets::CRegistryView& View)
{
	SProblemCounts counts;
	View.ForEachProblem([&] (FStringView, assets::EProblemSource, const SDiagnostic& Diagnostic)
	{
		switch (Diagnostic.Severity)
		{
		case ESeverity::Error: ++counts.Errors; break;
		case ESeverity::Warning: ++counts.Warnings; break;
		case ESeverity::Info: ++counts.Infos; break;
		}
	});
	return counts;
}

// A record's mount path ("#label" for a sub-entry), "" for an invalid one.
FString PathOf (const assets::CRegistryView& View, assets::SRecordId Record)
{
	assets::SAssetData data;
	return View.GetRecord(Record, data) ? data.MountPath : FString();
}

FStringView TypeNameOf (const assets::CRegistryView& View, assets::SRecordId Record)
{
	assets::SAssetData data;
	return View.GetRecord(Record, data) && data.Type != reflect::InvalidTypeId ? FStringView(assets::CAssetTypeRegistry::Get(data.Type).Name) : FStringView();
}

// ---- scan ----

}


// ---- The registry of one run ----

std::vector<FString> SRegistryRun::GetOwnRoots () const
{
	std::vector<FString> roots;
	for (const assets::MountId mount : OwnMounts)
	{
		if (const assets::SMount* found = Mounts->Find(mount); found != nullptr)
		{
			roots.push_back(found->GetRootMountPath());
		}
	}
	return roots;
}

int32 OpenRegistry (CCommandletContext& Context, FStringView Verb, const SRegistryOpenOptions& Options, SRegistryRun& Out)
{
	if (Context.ProjectRoot.empty() && !Context.bEngineOwner)
	{
		return Context.Fail(ExitUsage, FString::Format("{}: no project to act on. Run it in a project's directory, with --project=<file>, "
			"or with --engine for the engine's own content", Verb));
	}
	jobs::CWorkerPool& pool = Context.GetWorkerPool();
	// Reproducible output (§5.9): deltas commit strictly in sequence order, inside the waits below.
	pool.SetCommitOrder(jobs::ECommitOrder::Sequence);

	assets::SMountRoots roots;
	roots.EngineDir = Context.EngineRoot;
	roots.EngineContentDir = paths::GetEngineContentDir();
	roots.EngineConfigDir = paths::GetEngineConfigPath().parent_path();
	if (!Context.bEngineOwner)
	{
		roots.ProjectDir = Context.ProjectRoot;
	}
	std::vector<SDiagnostic> diagnostics;
	Out.Mounts = std::make_shared<const assets::CMountTable>(assets::CMountTable::Resolve(roots, &diagnostics));
	for (const SDiagnostic& diagnostic : diagnostics)
	{
		Context.PrintError(FString::Format("warning: {}: {}", ToDisplayText(diagnostic.Subject), diagnostic.Message));
	}
	for (const assets::SMount& mount : Out.Mounts->GetMounts())
	{
		if ((mount.Kind == assets::EMountKind::Engine) == Context.bEngineOwner)
		{
			Out.OwnMounts.push_back(mount.Id);
		}
	}

	const assets::SAssetEditorSettings settings = assets::SAssetEditorSettings::FromConfig(Context.GetConfig());
	if (Options.bProviders || Options.bLifecycle)
	{
		// Git is spawned here, once per mount asked for: a lifecycle needs its own mounts' only (§12.9 Detection).
		Out.Providers = std::make_unique<assets::CVcsProviderSet>();
		Out.Providers->DetectNow(*Out.Mounts, Options.bLifecycle ? std::span<const assets::MountId>(Out.OwnMounts) : std::span<const assets::MountId>(),
			settings.VcsProvider);
	}

	assets::SAssetRegistryDesc desc;
	desc.Mounts = Out.Mounts;
	desc.Pool = &pool;
	desc.Owner = Context.bEngineOwner ? assets::EAssetRegistryOwner::Engine : assets::EAssetRegistryOwner::Project;
	desc.bWriter = Options.bWriter;
	if (Options.bLifecycle)
	{
		// A tool host's own lifecycle: providers settled before the registry opens, no live ticks (RunNow alone acts), a
		// reader's or a guest's only reporting. A guest's META001 names the process that owns the mount.
		Out.Lifecycle = std::make_unique<assets::SLifecycleDesc>();
		Out.Lifecycle->Providers = Out.Providers->GetProviders();
		Out.Lifecycle->Settings = settings;
		if (!Context.ProjectRoot.empty())
		{
			Out.Lifecycle->ProjectSalt = FString(paths::GetProject().GetAssetSalt());
		}
		Out.Lifecycle->Owner = Context.bGuest ? Context.Owner : FString();
		Out.Lifecycle->bRepairs = Options.bRepairs && Options.bWriter;
		desc.Lifecycle = Out.Lifecycle.get();
	}
	Out.Registry = std::make_unique<assets::CAssetRegistry>(std::move(desc));
	Out.Registry->Start();
	if (!Out.Registry->WaitForInitialScan())
	{
		return Context.Fail(ExitInternalError, FString::Format("{}: the registry's initial scan did not complete", Verb));
	}
	return ExitSuccess;
}

FString DescribeMounts (const assets::CMountTable& Mounts)
{
	FString text;
	for (const assets::SMount& mount : Mounts.GetMounts())
	{
		text += text.IsEmpty() ? "" : ", ";
		text += mount.GetRootMountPath();
	}
	return text;
}

std::optional<FString> ResolveFileArgument (const CCommandletContext& Context, FStringView Verb, const assets::CMountTable& Mounts,
	FStringView Argument, int32& OutExit)
{
	FString mountPath;
	if (Argument.StartsWith('/'))
	{
		FStringView mountName;
		FStringView relative;
		if (!assets::IsWellFormedMountPath(Argument) || !assets::SplitMountPath(Argument, mountName, relative) || Mounts.Find(mountName) == nullptr)
		{
			OutExit = Context.Fail(ExitUsage, FString::Format("{}: {} is no file of the mounts ({})", Verb, Argument, DescribeMounts(Mounts)));
			return std::nullopt;
		}
		mountPath = FString(Argument);
	}
	else
	{
		const std::optional<std::filesystem::path> path = Argument.TryToPath();
		std::error_code ec;
		const std::filesystem::path absolute = path ? std::filesystem::absolute(*path, ec) : std::filesystem::path();
		const std::optional<FString> converted = absolute.empty() ? std::nullopt : Mounts.ToMountPath(absolute);
		if (!converted)
		{
			OutExit = Context.Fail(ExitUsage, FString::Format("{}: {} is in none of the mounts ({})", Verb, Argument, DescribeMounts(Mounts)));
			return std::nullopt;
		}
		mountPath = *converted;
	}
	if (assets::GetMountPathParent(mountPath).IsEmpty())
	{
		OutExit = Context.Fail(ExitUsage, FString::Format("{}: {} is a mount root, not a file", Verb, mountPath));
		return std::nullopt;
	}
	return mountPath;
}


// scan: the mounts scanned into the registry; a writer writes its snapshot. Exit 1 on an error-severity problem.
int32 RunScan (CCommandletContext& Context)
{
	if (!Context.Arguments.empty())
	{
		return Context.Fail(ExitUsage, "scan: takes no arguments (scan [--engine] [--ci])");
	}
	// The host took the mount's writer lock exactly when the run is not read-only (bWritesProject).
	const bool bWriter = !Context.bReadOnly;
	SRegistryRun run;
	if (const int32 result = OpenRegistry(Context, "scan", { .bWriter = bWriter }, run); result != ExitSuccess)
	{
		return result;
	}
	assets::CAssetRegistry& registry = *run.Registry;
	bool bWritten = false;
	if (bWriter)
	{
		bWritten = registry.WriteSnapshotNow();
		if (!bWritten)
		{
			return Context.Fail(ExitInternalError, FString::Format("scan: could not write {}", FString::FromPath(registry.GetSnapshotPath())));
		}
	}

	const assets::CRegistryView& view = run.View();
	const assets::SInitialScanReport& report = registry.GetInitialScanReport();
	const assets::SAssetRegistryCounters& counters = registry.GetCounters();
	const SProblemCounts problems = CountProblems(view);
	const FString snapshot = FString::FromPath(registry.GetSnapshotPath());

	Context.Print(FString::Format("Scanned {}: {} file(s), {} record(s), {} sub-entr{}, {} folder(s) in {:.1f} ms", DescribeMounts(*run.Mounts),
		view.GetFileCount(), view.GetRecordCount(), view.GetSubEntryCount(), view.GetSubEntryCount() == 1u ? "y" : "ies", view.GetFolderCount(),
		report.TotalMs));
	Context.Print(FString::Format("  analyzed {}, hashed {}, re-evaluated {}, {} file(s) listed, {} delta(s) committed", counters.Analyzed,
		counters.Hashed, counters.ReEvaluated, counters.Scanned, counters.Committed));
	Context.Print(FString::Format("  snapshot load {:.1f} ms, scan {:.1f} ms, commits {:.1f} ms", report.LoadMs, report.ScanMs, report.CommitMs));
	for (const assets::SSnapshotFileReport& file : registry.GetSnapshotReports())
	{
		Context.Print(FString::Format("  {}: {}, {} record(s){}{}", FString::FromPath(file.Path.filename()), enum_::ToString(file.State), file.Records,
			file.bJournalUsed ? FString::Format(", journal {} commit(s)", file.JournalCommits) : FString(),
			file.Message.IsEmpty() ? FString() : FString(" (") + file.Message + ")"));
	}
	Context.Print(FString::Format("  problems: {} error(s), {} warning(s), {} info", problems.Errors, problems.Warnings, problems.Infos));
	Context.Print(bWriter ? FString::Format("  wrote {}", snapshot) : FString("  read-only: wrote nothing"));

	const std::vector<FString> mounts = MountNames(*run.Mounts);
	Context.PrintJson(CJsonObject().AddString("verb", "scan").AddStringArray("mounts", mounts)
		.AddString("owner", enum_::ToString(registry.GetOwner())).AddBool("writer", bWriter)
		.AddUInt("files", view.GetFileCount()).AddUInt("records", view.GetRecordCount()).AddUInt("subEntries", view.GetSubEntryCount())
		.AddUInt("folders", view.GetFolderCount()).AddUInt("analyzed", counters.Analyzed).AddUInt("hashed", counters.Hashed)
		.AddUInt("reEvaluated", counters.ReEvaluated).AddUInt("scanned", counters.Scanned).AddUInt("committed", counters.Committed)
		.AddUInt("loadMs", static_cast<uint64>(report.LoadMs + 0.5)).AddUInt("totalMs", static_cast<uint64>(report.TotalMs + 0.5))
		.AddUInt("errors", problems.Errors).AddUInt("warnings", problems.Warnings).AddUInt("infos", problems.Infos)
		.AddString("snapshot", bWritten ? snapshot.View() : FStringView()));
	return problems.Errors > 0u ? ExitProblems : ExitSuccess;
}

// ---- status ----

// status: problems by code and source, unclaimed files by folder and extension, the scan state, the DDC. Exit 1 on an
// error-severity problem.
int32 RunStatus (CCommandletContext& Context)
{
	if (!Context.Arguments.empty())
	{
		return Context.Fail(ExitUsage, "status: takes no arguments");
	}
	SRegistryRun run;
	if (const int32 result = OpenRegistry(Context, "status", { .bProviders = true }, run); result != ExitSuccess)
	{
		return result;
	}
	const assets::CAssetRegistry& registry = *run.Registry;
	const assets::CRegistryView& view = run.View();
	const assets::SInitialScanReport& report = registry.GetInitialScanReport();

	Context.Print(FString::Format("{}: scan {}; {} file(s), {} record(s), {} sub-entr{}; initial scan {:.1f} ms (analyzed {}, hashed {})",
		DescribeMounts(*run.Mounts), enum_::ToString(registry.GetScanState()), view.GetFileCount(), view.GetRecordCount(), view.GetSubEntryCount(),
		view.GetSubEntryCount() == 1u ? "y" : "ies", report.TotalMs, report.Analyzed, report.Hashed));
	const std::vector<FString> mounts = MountNames(*run.Mounts);
	Context.PrintJson(CJsonObject().AddString("kind", "scan").AddStringArray("mounts", mounts)
		.AddString("state", enum_::ToString(registry.GetScanState())).AddUInt("files", view.GetFileCount())
		.AddUInt("records", view.GetRecordCount()).AddUInt("subEntries", view.GetSubEntryCount()).AddUInt("folders", view.GetFolderCount())
		.AddUInt("totalMs", static_cast<uint64>(report.TotalMs + 0.5)).AddUInt("analyzed", report.Analyzed).AddUInt("hashed", report.Hashed));

	// Per mount: its VCS provider (§12.9, edit [15]) and the files that wait for a meta (PendingMeta: no meta yet).
	std::vector<uint32> pending(run.Mounts->GetMounts().size(), 0u);
	view.ForEachFile([&] (uint32 Row)
	{
		assets::SFileData file;
		if (view.GetFile(Row, file) && assets::GetsMeta(file.Class) && file.MetaState == assets::EMetaState::PendingMeta && file.Mount < pending.size())
		{
			++pending[file.Mount];
		}
	});
	Context.Print("Mounts (provider, files waiting for a meta):");
	for (const assets::SMount& mount : run.Mounts->GetMounts())
	{
		const assets::IVcsProvider* provider = run.FindProvider(mount.Id);
		const FStringView name = provider != nullptr ? provider->GetName() : FStringView("None");
		Context.Print(FString::Format("  {:<16} {:<6} {} pending meta(s)", mount.GetRootMountPath(), name, pending[mount.Id]));
		Context.PrintJson(CJsonObject().AddString("kind", "mount").AddString("mount", mount.GetRootMountPath()).AddString("provider", name)
			.AddUInt("pendingMetas", pending[mount.Id]));
	}

	// Problems by (code, severity, source); a code-0 problem (an Analyze failure, an unlistable folder) as "-".
	using SProblemKey = std::tuple<FString, uint8, uint8>;
	std::map<SProblemKey, uint32> byCode;
	SProblemCounts counts;
	view.ForEachProblem([&] (FStringView, assets::EProblemSource Source, const SDiagnostic& Diagnostic)
	{
		++byCode[SProblemKey(Diagnostic.Code != 0u ? FormatDiagnosticCode(Diagnostic.Code) : FString("-"), static_cast<uint8>(Diagnostic.Severity),
			static_cast<uint8>(Source))];
		switch (Diagnostic.Severity)
		{
		case ESeverity::Error: ++counts.Errors; break;
		case ESeverity::Warning: ++counts.Warnings; break;
		case ESeverity::Info: ++counts.Infos; break;
		}
	});
	Context.Print(byCode.empty() ? FString("Problems: none") : FString("Problems by code, severity and source:"));
	for (const auto& [key, count] : byCode)
	{
		const FStringView severity = GetSeverityName(static_cast<ESeverity>(std::get<1>(key)));
		const FStringView source = enum_::ToString(static_cast<assets::EProblemSource>(std::get<2>(key)));
		Context.Print(FString::Format("  {:<8} {:<8} {:<10} {}", std::get<0>(key), severity, source, count));
		Context.PrintJson(CJsonObject().AddString("kind", "problems").AddString("code", std::get<0>(key) == "-" ? FStringView() : std::get<0>(key).View())
			.AddString("severity", severity).AddString("source", source).AddUInt("count", count));
	}

	// Unclaimed files (no importer claimed them, and no record names them as a companion), by folder and extension.
	std::map<std::pair<FString, FString>, uint32, std::less<>> unclaimed;
	view.ForEachFile([&] (uint32 Row)
	{
		assets::SFileData file;
		if (view.GetFile(Row, file) && file.Class == assets::EFileClass::Unclaimed)
		{
			const FStringView leaf = assets::GetMountPathLeaf(file.MountPath);
			const uint32 dot = leaf.FindLast('.');
			const FString extension = dot == FStringView::NotFound || dot == 0u ? FString() : FString(leaf.RightChop(dot)).FoldCase();
			++unclaimed[{ FString(assets::GetMountPathParent(file.MountPath)), extension }];
		}
	});
	Context.Print(unclaimed.empty() ? FString("Unclaimed files: none") : FString("Unclaimed files by folder and extension:"));
	for (const auto& [key, count] : unclaimed)
	{
		Context.Print(FString::Format("  {}  {}  {}", ToDisplayText(key.first), key.second.IsEmpty() ? FStringView("(none)") : key.second.View(), count));
		Context.PrintJson(CJsonObject().AddString("kind", "unclaimed").AddString("folder", key.first).AddString("extension", key.second).AddUInt("count", count));
	}

	const SDdcSummary ddc = SummarizeDdc(Context);
	if (!ddc.bExists)
	{
		Context.Print(FString::Format("Derived-data cache: none at {}", ddc.Root));
	}
	else if (!ddc.Error.IsEmpty())
	{
		Context.Print(FString::Format("Derived-data cache: {} cannot be opened: {}", ddc.Root, ddc.Error));
	}
	else
	{
		Context.Print(FString::Format("Derived-data cache: {}: content {} (manifests, records, values), {} in all, {} file(s)", ddc.Root,
			FormatBytes(ddc.ContentBytes), FormatBytes(ddc.TotalBytes), ddc.Files));
	}
	Context.PrintJson(CJsonObject().AddString("kind", "ddc").AddString("ddc", ddc.Root).AddBool("exists", ddc.bExists)
		.AddString("error", ddc.Error).AddUInt("contentBytes", ddc.ContentBytes).AddUInt("totalBytes", ddc.TotalBytes).AddUInt("files", ddc.Files));

	Context.Print(FString::Format("{} error(s), {} warning(s), {} info", counts.Errors, counts.Warnings, counts.Infos));
	Context.PrintJson(CJsonObject().AddString("kind", "summary").AddUInt("errors", counts.Errors).AddUInt("warnings", counts.Warnings)
		.AddUInt("infos", counts.Infos));
	return counts.Errors > 0u ? ExitProblems : ExitSuccess;
}

// ---- query ----

namespace
{
// A result column: a system column ("$Size") or a tag key ("Width"), with the field name --json gives it.
struct SQueryColumn
{
	FString Key;
	FString JsonName;      // "$ImportStatus" -> "importStatus"; a tag key as it is
	bool bList = false;    // every value, as a JSON array ($Labels, MultiValue tag keys)
	bool bNumber = false;  // a count or a size: a JSON number
};

constexpr FStringLiteral DefaultQueryColumns[] = { "$Name", "$Type", "$Path", "$Size", "$Problems" };

// The column named Name, or nullopt when no system column or tag key of any type has that name.
std::optional<SQueryColumn> FindQueryColumn (FStringView Name)
{
	SQueryColumn column;
	if (Name.StartsWith('$'))
	{
		const std::optional<assets::ESystemColumn> system = assets::FindSystemColumn(Name);
		if (!system)
		{
			return std::nullopt;
		}
		column.Key = FString(assets::GetSystemColumnName(*system));
		const FStringView bare = column.Key.View().RightChop(1u);
		column.JsonName = FString(bare.Left(1u)).ToLower() + bare.RightChop(1u);
		column.bList = *system == assets::ESystemColumn::Labels;
		column.bNumber = *system == assets::ESystemColumn::Size || *system == assets::ESystemColumn::Problems
			|| *system == assets::ESystemColumn::Referencers || *system == assets::ESystemColumn::Dependencies || *system == assets::ESystemColumn::Chunk;
		return column;
	}
	bool bFound = false;
	const FString folded = FString(Name).FoldCase();
	for (const assets::SAssetTypeInfo& type : assets::CAssetTypeRegistry::GetTypes())
	{
		for (const assets::SStaticTagKey& key : type.Keys)
		{
			if (FString(key.Name).FoldCase() == folded)
			{
				bFound = true;
				column.Key = FString(key.Name);
				column.bList = column.bList || (static_cast<uint32>(key.Flags) & static_cast<uint32>(assets::ETagFlags::MultiValue)) != 0u;
			}
		}
	}
	if (!bFound)
	{
		return std::nullopt;
	}
	column.JsonName = column.Key;
	return column;
}

void AddColumn (CJsonObject& Object, const assets::CRegistryView& View, assets::SRecordId Record, const SQueryColumn& Column)
{
	std::vector<assets::STagValue> values;
	assets::ForEachColumnValue(View, Record, Column.Key, [&] (const assets::STagValue& Value) { values.push_back(Value); });
	if (Column.bList)
	{
		std::vector<FString> texts;
		for (const assets::STagValue& value : values)
		{
			texts.push_back(assets::FormatColumnValue(value));
		}
		Object.AddStringArray(Column.JsonName, texts);
		return;
	}
	if (values.empty())
	{
		return;   // a tag the record's type does not have, or has no value for
	}
	const assets::STagValue& value = values.front();
	switch (value.Type)
	{
	case assets::ETagType::Int64:
		Object.AddInt(Column.JsonName, value.AsInt64());
		return;
	case assets::ETagType::Bool:
		Object.AddBool(Column.JsonName, value.AsBool());
		return;
	default:
		Object.AddString(Column.JsonName, assets::FormatColumnValue(value));
		return;
	}
}
}

/**
 * query "<text>" [--columns=a,b] [--limit=n] [--rows]: the records matching a query in the §5.8 syntax, in the query
 * engine's deterministic order, one line (or JSON object) each with the columns asked for (default $Name, $Type,
 * $Path, $Size, $Problems), then {"matched","shown"}. Several arguments are one query text joined by spaces. A
 * query that does not parse or compile is a usage error with its position.
 */
int32 RunQuery (CCommandletContext& Context)
{
	if (Context.Arguments.empty())
	{
		return Context.Fail(ExitUsage, "query: takes a query text (query \"<text>\" [--columns=a,b] [--limit=n] [--rows])");
	}
	FString text;
	for (const FString& argument : Context.Arguments)
	{
		text += text.IsEmpty() ? "" : " ";
		text += argument;
	}

	std::vector<SQueryColumn> columns;
	if (const std::optional<FString> option = Context.FindOption("columns"))
	{
		for (const FStringView name : option->View().Split(','))
		{
			const FStringView trimmed = name.TrimStartAndEnd();
			if (trimmed.IsEmpty())
			{
				continue;
			}
			const std::optional<SQueryColumn> column = FindQueryColumn(trimmed);
			if (!column)
			{
				return Context.Fail(ExitUsage, FString::Format("query: no column '{}' (a system column such as $Path, $Type, $Size, $Labels, or a "
					"tag key such as Width)", trimmed));
			}
			columns.push_back(*column);
		}
		if (columns.empty())
		{
			return Context.Fail(ExitUsage, "query: --columns names no column");
		}
	}
	else
	{
		for (const FStringLiteral name : DefaultQueryColumns)
		{
			columns.push_back(*FindQueryColumn(name));
		}
	}

	std::optional<uint32> limit;
	if (const std::optional<FString> option = Context.FindOption("limit"))
	{
		limit = option->View().TrimStartAndEnd().ToUInt32();
		if (!limit || *limit == 0u)
		{
			return Context.Fail(ExitUsage, FString::Format("query: --limit={} is not a number from 1 up", *option));
		}
	}

	SDiagnostic error;
	std::optional<assets::SAssetQuery> query = assets::SAssetQuery::Parse(text, &error);
	if (!query)
	{
		return FailQuery(Context, "query", text, error);
	}
	const bool bRows = Context.HasOption("rows");
	if (bRows)
	{
		query->Scope(assets::ERecordScope::Rows);
	}
	if (limit)
	{
		query->LimitCount = *limit;
	}

	SRegistryRun run;
	if (const int32 result = OpenRegistry(Context, "query", {}, run); result != ExitSuccess)
	{
		return result;
	}
	const assets::CRegistryView& view = run.View();
	assets::SCompiledQuery compiled;
	if (!view.Compile(*query, compiled, &error))
	{
		return FailQuery(Context, "query", text, error);
	}
	for (const SDiagnostic& note : compiled.Notes)
	{
		Context.PrintError(FString::Format("note: {}", note.Message));
	}
	if (bRows)
	{
		Context.PrintError("note: --rows: data-table rows arrive with M5; this matches nothing yet");
	}
	std::vector<assets::SRecordId> records;
	const uint32 matched = view.Run(compiled, records);

	if (!Context.bJson)
	{
		// A table: a header, then one line per record, each column as wide as its widest value.
		std::vector<std::vector<FString>> cells;
		std::vector<uint32> widths;
		for (const SQueryColumn& column : columns)
		{
			widths.push_back(column.Key.View().CountCodePoints());
		}
		for (const assets::SRecordId record : records)
		{
			std::vector<FString>& row = cells.emplace_back();
			for (size_t i = 0u; i < columns.size(); ++i)
			{
				row.push_back(ToDisplayText(assets::FormatColumnValues(view, record, columns[i].Key)));
				widths[i] = std::max(widths[i], row.back().View().CountCodePoints());
			}
		}
		const auto printRow = [&] (std::span<const FString> Values)
		{
			FString line;
			for (size_t i = 0u; i < Values.size(); ++i)
			{
				line += Values[i];
				if (i + 1u < Values.size())
				{
					for (uint32 pad = Values[i].View().CountCodePoints(); pad < widths[i] + 2u; ++pad)
					{
						line += ' ';
					}
				}
			}
			Context.Print(line);
		};
		std::vector<FString> header;
		for (const SQueryColumn& column : columns)
		{
			header.push_back(column.Key);
		}
		printRow(header);
		for (const std::vector<FString>& row : cells)
		{
			printRow(row);
		}
	}
	for (const assets::SRecordId record : records)
	{
		if (Context.bJson)
		{
			CJsonObject object;
			for (const SQueryColumn& column : columns)
			{
				AddColumn(object, view, record, column);
			}
			Context.PrintJson(object);
		}
	}
	Context.Print(FString::Format("{} of {} record(s)", records.size(), matched));
	Context.PrintJson(CJsonObject().AddUInt("matched", matched).AddUInt("shown", records.size()));
	return ExitSuccess;
}

// ---- refs, referencers ----

namespace
{
struct SEdgeLine
{
	FString From;       // the record that declares the edge (a sub-entry's "file#label")
	FString Target;     // as declared: the path written, or {guid} [hint]
	FString Resolved;   // the record it reaches, "" when it reaches none
	assets::EDepFlags Flags {};
	FString Field;
	FStringLiteral State;
};

// How an edge ended up (MSVC 14.38 needs the literals bound to constants to choose between them).
constexpr FStringLiteral StateResolved = "Resolved";
constexpr FStringLiteral StateUnresolved = "Unresolved";
constexpr FStringLiteral StateAmbiguous = "Ambiguous";
constexpr FStringLiteral StateCompanion = "Companion";

SEdgeLine MakeEdgeLine (const assets::CRegistryView& View, const assets::SDependencyView& Edge)
{
	SEdgeLine line;
	line.From = PathOf(View, Edge.From);
	line.Target = Edge.bGuidTarget ? FString("{") + Edge.TargetId.Guid.ToString() + "}" + (Edge.TargetPath.IsEmpty() ? FString() : " " + FString(Edge.TargetPath))
		: FString(Edge.TargetPath);
	line.Resolved = PathOf(View, Edge.To);
	line.Flags = Edge.Flags;
	line.Field = FString(Edge.FieldPath);
	line.State = Edge.bAmbiguous ? StateAmbiguous : Edge.bCompanion ? StateCompanion
		: (static_cast<uint32>(Edge.Flags) & static_cast<uint32>(assets::EDepFlags::Unresolved)) != 0u ? StateUnresolved : StateResolved;
	return line;
}

void PrintEdges (const CCommandletContext& Context, std::vector<SEdgeLine>& Edges, bool bByTarget)
{
	std::sort(Edges.begin(), Edges.end(), [bByTarget] (const SEdgeLine& A, const SEdgeLine& B)
	{
		const FString& a = bByTarget ? A.Target : A.From;
		const FString& b = bByTarget ? B.Target : B.From;
		if (a != b)
		{
			return assets::LessPortableThenBytes(a, b);
		}
		return std::tie(A.From, A.Target, A.Field, A.Flags) < std::tie(B.From, B.Target, B.Field, B.Flags);
	});
	for (const SEdgeLine& edge : Edges)
	{
		FString text = FString::Format("  {} -> {}", ToDisplayText(edge.From), ToDisplayText(edge.Target));
		if (!edge.Resolved.IsEmpty() && edge.Resolved != edge.Target)
		{
			text += FString::Format(" (= {})", ToDisplayText(edge.Resolved));
		}
		text += FString::Format("  flags={}", enum_::FormatFlags(edge.Flags));
		if (!edge.Field.IsEmpty())
		{
			text += FString::Format(" field={}", edge.Field);
		}
		text += FString::Format("  {}", edge.State);
		Context.Print(text);
		const std::vector<FString> flags = enum_::GetFlagNames(edge.Flags);
		Context.PrintJson(CJsonObject().AddString("from", edge.From).AddString("target", edge.Target).AddString("resolved", edge.Resolved)
			.AddStringArray("flags", flags).AddString("field", edge.Field).AddString("state", edge.State));
	}
	Context.Print(FString::Format("{} edge(s)", Edges.size()));
}

/**
 * The record Asset names, resolved by the query engine as the search box resolves ref:/dep: targets (a mount path
 * with an optional "#label", else a unique name). False with the failure reported (a usage error).
 */
bool ResolveAsset (const CCommandletContext& Context, FStringView Verb, const assets::CRegistryView& View, const assets::SAssetQuery& Query,
	FStringView Asset, assets::SCompiledQuery& OutCompiled, assets::SRecordId& OutRecord, int32& OutExit)
{
	SDiagnostic error;
	if (!View.Compile(Query, OutCompiled, &error))
	{
		error.Column = 0u;
		OutExit = FailQuery(Context, Verb, Asset, error);
		return false;
	}
	const std::vector<assets::SRecordId> targets = assets::GetGraphTargets(OutCompiled);
	if (targets.empty() || !View.IsValid(targets.front()))
	{
		OutExit = Context.Fail(ExitInternalError, FString::Format("{}: {} resolved to no record", Verb, Asset));
		return false;
	}
	OutRecord = targets.front();
	return true;
}
}

// refs <asset>: the asset's edges (every edge of its file for an asset record, the sub-entry's own for "file#label"),
// with flags, field path and whether each resolved.
int32 RunRefs (CCommandletContext& Context)
{
	if (Context.Arguments.size() != 1u)
	{
		return Context.Fail(ExitUsage, "refs: takes one asset: a mount path (with #label for a sub-asset) or a unique name");
	}
	const FString& asset = Context.Arguments[0];
	SRegistryRun run;
	if (const int32 result = OpenRegistry(Context, "refs", {}, run); result != ExitSuccess)
	{
		return result;
	}
	const assets::CRegistryView& view = run.View();
	assets::SCompiledQuery compiled;
	assets::SRecordId record;
	int32 exitCode = ExitSuccess;
	if (!ResolveAsset(Context, "refs", view, assets::SAssetQuery().ReferencedBy(asset), asset, compiled, record, exitCode))
	{
		return exitCode;
	}
	std::vector<SEdgeLine> edges;
	view.ForEachDependency(record, assets::EDepFlags {}, assets::EDepFlags {}, [&] (const assets::SDependencyView& Edge) { edges.push_back(MakeEdgeLine(view, Edge)); });
	Context.Print(FString::Format("{} references:", ToDisplayText(PathOf(view, record))));
	PrintEdges(Context, edges, true);
	return ExitSuccess;
}

// referencers <asset> [--transitive]: the edges that resolve to the asset; with --transitive every record that reaches
// it over resolved edges (ref:~X), in the query engine's order.
int32 RunReferencers (CCommandletContext& Context)
{
	if (Context.Arguments.size() != 1u)
	{
		return Context.Fail(ExitUsage, "referencers: takes one asset: a mount path (with #label for a sub-asset) or a unique name");
	}
	const FString& asset = Context.Arguments[0];
	const bool bTransitive = Context.HasOption("transitive");
	SRegistryRun run;
	if (const int32 result = OpenRegistry(Context, "referencers", {}, run); result != ExitSuccess)
	{
		return result;
	}
	const assets::CRegistryView& view = run.View();
	assets::SCompiledQuery compiled;
	assets::SRecordId record;
	int32 exitCode = ExitSuccess;
	if (!ResolveAsset(Context, "referencers", view, assets::SAssetQuery().References(asset, bTransitive), asset, compiled, record, exitCode))
	{
		return exitCode;
	}
	if (bTransitive)
	{
		std::vector<assets::SRecordId> records;
		view.Run(compiled, records);
		Context.Print(FString::Format("{} is reached by:", ToDisplayText(PathOf(view, record))));
		for (const assets::SRecordId found : records)
		{
			const FString path = PathOf(view, found);
			const FStringView type = TypeNameOf(view, found);
			Context.Print(FString::Format("  {}  {}", ToDisplayText(path), type));
			Context.PrintJson(CJsonObject().AddString("path", path).AddString("type", type));
		}
		Context.Print(FString::Format("{} record(s)", records.size()));
		return ExitSuccess;
	}
	std::vector<SEdgeLine> edges;
	view.ForEachReferencer(record, assets::EDepFlags {}, assets::EDepFlags {}, [&] (const assets::SDependencyView& Edge) { edges.push_back(MakeEdgeLine(view, Edge)); });
	Context.Print(FString::Format("{} is referenced by:", ToDisplayText(PathOf(view, record))));
	PrintEdges(Context, edges, false);
	return ExitSuccess;
}

// ---- explain ----

namespace
{
// "Content/Art/Assets.yml:4 (rule 2)", "Config/Assets.yml:24", or the default's name.
FString FormatProvenance (const assets::SProvenance& Source, FStringView Default)
{
	if (Source.File.IsEmpty())
	{
		return FString(Default);
	}
	FString text = Source.File;
	if (Source.Line > 0u)
	{
		text.AppendFormat(":{}", Source.Line);
	}
	if (Source.RuleIndex >= 0)
	{
		text.AppendFormat(" (rule {})", Source.RuleIndex + 1);
	}
	return text;
}

// The meta beside a file, as explain shows it: its spelling on disk (empty: none), the registry's state of the pair, and
// the parse of its bytes.
struct SExplainMeta
{
	FString File;                     // its mount path
	assets::EMetaState State = assets::EMetaState::PendingMeta;
	assets::SMetaParseResult Parsed;
};

SExplainMeta ReadExplainMeta (const assets::CRegistryView& View, const assets::CMountTable& Mounts, FStringView MountPath)
{
	SExplainMeta out;
	if (const std::optional<uint32> row = View.FindFile(MountPath); row.has_value())
	{
		assets::SFileData file;
		if (View.GetFile(*row, file))
		{
			out.State = file.MetaState;
		}
	}
	const std::optional<uint32> metaRow = View.FindFile(assets::GetMetaPath(MountPath));
	assets::SFileData meta;
	if (!metaRow.has_value() || !View.GetFile(*metaRow, meta) || meta.Class != assets::EFileClass::Meta)
	{
		return out;
	}
	out.File = FString(meta.MountPath);
	const std::optional<std::filesystem::path> path = Mounts.FromMountPath(meta.MountPath);
	FString bytes;
	if (path.has_value() && platform::ReadFileChunksStable(*path, [&bytes] (std::span<const uint8> Chunk)
		{
			bytes += FStringView(reinterpret_cast<const char*>(Chunk.data()), static_cast<uint32>(Chunk.size()));
		}) == platform::EStableRead::Stable)
	{
		assets::ParseMeta(bytes, out.Parsed);
	}
	else
	{
		out.Parsed.State = assets::EMetaParseState::Broken;
	}
	return out;
}

const assets::SProvenance* FindProvenance (const assets::SEffectiveFileSettings& Settings, FStringView Field)
{
	for (const auto& [name, source] : Settings.Provenance)
	{
		if (name == Field)
		{
			return &source;
		}
	}
	return nullptr;
}

CJsonObject ProvenanceObject (const assets::SProvenance* Source)
{
	CJsonObject object;
	object.AddString("file", Source != nullptr ? Source->File.View() : FStringView()).AddUInt("line", Source != nullptr ? Source->Line : 0u)
		.AddInt("rule", Source != nullptr && Source->RuleIndex >= 0 ? Source->RuleIndex + 1 : 0);
	return object;
}
}

/**
 * explain <file>: the effective importer, class, settings, cook rule, chunk and labels of one file, each with the rule
 * file and line that set it (§13.2; no step list until M6), the rule files consulted, the notes, and what the registry
 * holds for it. <file> is a mount path ("/Project/Textures/Rock.png") or a disk path, absolute or relative to the
 * working directory.
 */
int32 RunExplain (CCommandletContext& Context)
{
	if (Context.Arguments.size() != 1u)
	{
		return Context.Fail(ExitUsage, "explain: takes one file: a mount path (/Project/...) or a path on disk");
	}
	const FString& argument = Context.Arguments[0];
	SRegistryRun run;
	if (const int32 result = OpenRegistry(Context, "explain", {}, run); result != ExitSuccess)
	{
		return result;
	}
	int32 exitCode = ExitSuccess;
	const std::optional<FString> resolved = ResolveFileArgument(Context, "explain", *run.Mounts, argument, exitCode);
	if (!resolved)
	{
		return exitCode;
	}
	FString mountPath = *resolved;

	const assets::CRegistryView& view = run.View();
	const assets::CRulesEngine& rules = run.Registry->GetRules();
	// The registry's spelling of the file when it holds it (the one its records use), and its type for Type rules.
	assets::SFileFacts facts;
	assets::SAssetData record;
	bool bRecord = false;
	if (const std::optional<assets::SRecordId> found = view.FindByPath(mountPath))
	{
		bRecord = view.GetRecord(*found, record);
		if (bRecord)
		{
			mountPath = record.MountPath;
			facts.Type = record.Type;
		}
	}
	// The file's meta applies last (importer default < rules < meta), so its fields name it as their provenance.
	const SExplainMeta meta = ReadExplainMeta(view, *run.Mounts, mountPath);
	if (meta.Parsed.State == assets::EMetaParseState::Ok && !meta.File.IsEmpty())
	{
		facts.Meta = &meta.Parsed.Meta;
	}
	std::vector<SDiagnostic> diagnostics;
	const std::shared_ptr<const assets::SScanScope> scope = rules.OpenFolderScope(assets::GetMountPathParent(mountPath), diagnostics);
	const assets::SRuleExplanation explanation = rules.Explain(*scope, mountPath, facts);
	const assets::SEffectiveFileSettings& settings = explanation.Settings;

	Context.Print(ToDisplayText(mountPath));
	CJsonObject json;
	json.AddString("path", mountPath);
	if (!explanation.Verdict)
	{
		Context.Print(FString::Format("  excluded: {}", explanation.ExcludedBy));
		json.AddBool("excluded", true).AddString("excludedBy", explanation.ExcludedBy);
	}
	else
	{
		json.AddBool("excluded", false);
		const FStringView importer = settings.Importer != nullptr ? FStringView(settings.Importer->Name) : FStringView();
		const assets::SProvenance* importerSource = FindProvenance(settings, "Importer");
		Context.Print(FString::Format("  importer  {:<16} {}", importer.IsEmpty() ? FStringView("-") : importer,
			FormatProvenance(importerSource != nullptr ? *importerSource : assets::SProvenance {}, importer.IsEmpty() ? FStringView("no importer claims it") : FStringView("by extension"))));
		Context.Print(FString::Format("  class     {}{}", enum_::ToString(settings.Class), settings.bQuiet ? " (quiet: Scan.Quiet)" : ""));
		const assets::SProvenance* cookSource = FindProvenance(settings, "Cook");
		const assets::SProvenance* chunkSource = FindProvenance(settings, "Chunk");
		Context.Print(FString::Format("  cook      {:<16} {}", enum_::ToString(settings.CookRule),
			FormatProvenance(cookSource != nullptr ? *cookSource : assets::SProvenance {}, "default")));
		Context.Print(FString::Format("  chunk     {:<16} {}", settings.Chunk, FormatProvenance(chunkSource != nullptr ? *chunkSource : assets::SProvenance {}, "default")));
		std::vector<CJsonObject> labels;
		for (const FString& label : settings.RuleLabels)
		{
			const assets::SProvenance* source = FindProvenance(settings, FString("Labels.") + label);
			Context.Print(FString::Format("  label     {:<16} {}", label, FormatProvenance(source != nullptr ? *source : assets::SProvenance {}, "-")));
			labels.push_back(CJsonObject().AddString("label", label).AddObject("source", ProvenanceObject(source)).AddBool("authored", false));
		}
		// The meta's own labels (§5.2 LABELS, "authored"): what Label selectors match.
		for (const FString& label : settings.AuthoredLabels)
		{
			const assets::SProvenance* source = FindProvenance(settings, FString("Labels.") + label);
			Context.Print(FString::Format("  label     {:<16} {} (authored)", label, FormatProvenance(source != nullptr ? *source : assets::SProvenance {}, "-")));
			labels.push_back(CJsonObject().AddString("label", label).AddObject("source", ProvenanceObject(source)).AddBool("authored", true));
		}
		std::vector<CJsonObject> fields;
		if (settings.Settings.Table != nullptr)
		{
			Context.Print(FString::Format("  settings ({}):", settings.Settings.Table->Name));
			for (const assets::SSettingValue& value : settings.Settings.Values)
			{
				const FStringView field = settings.Settings.Table->Fields[value.Field].Name;
				Context.Print(FString::Format("    {:<14} {:<12} {}", field, value.Text, FormatProvenance(value.Source, "default")));
				fields.push_back(CJsonObject().AddString("field", field).AddString("value", value.Text).AddObject("source", ProvenanceObject(&value.Source)));
			}
		}
		json.AddString("importer", importer).AddObject("importerSource", ProvenanceObject(importerSource))
			.AddString("class", enum_::ToString(settings.Class)).AddBool("quiet", settings.bQuiet)
			.AddString("cook", enum_::ToString(settings.CookRule)).AddObject("cookSource", ProvenanceObject(cookSource))
			.AddUInt("chunk", settings.Chunk).AddObject("chunkSource", ProvenanceObject(chunkSource))
			.AddObjectArray("labels", labels).AddString("settingsTable", settings.Settings.Table != nullptr ? FStringView(settings.Settings.Table->Name) : FStringView())
			.AddObjectArray("settings", fields).AddBool("typeIncluded", explanation.bTypeIncluded);
	}
	std::vector<CJsonObject> chain;
	Context.Print("  rule files consulted, deepest first:");
	for (const assets::SRuleChainEntry& entry : explanation.Chain)
	{
		Context.Print(FString::Format("    {} ({}){}", entry.Origin.File, entry.Origin.MountName, entry.RulesHash == 0u ? " - no rules" : ""));
		chain.push_back(CJsonObject().AddString("file", entry.Origin.File).AddString("mount", entry.Origin.MountName)
			.AddString("hash", FString::Format("{:016x}", entry.RulesHash)));
	}
	for (const FString& note : explanation.Notes)
	{
		Context.Print(FString::Format("  note: {}", note));
	}
	// The meta: where it is, what the registry made of the pair, the GUID it gives the record; then the ids and paths that
	// resolve to the record besides its own (a meta's Aliases, a renamed sub-asset's old label; FormerPaths).
	CJsonObject metaJson;
	if (meta.File.IsEmpty())
	{
		Context.Print(FString::Format("  meta      none ({})", enum_::ToString(meta.State)));
	}
	else
	{
		const bool bOk = meta.Parsed.State == assets::EMetaParseState::Ok;
		const FString guid = bOk ? meta.Parsed.Meta.Guid.ToString() : FString();
		const FString file = rules.GetMetaProvenance(*scope, mountPath);
		Context.Print(FString::Format("  meta      {}  {}{}", file, enum_::ToString(meta.State), bOk ? FString::Format("  {{{}}}", guid) : FString("  (does not parse)")));
		metaJson.AddString("file", file).AddString("path", meta.File).AddString("guid", guid).AddBool("parses", bOk);
	}
	metaJson.AddString("state", enum_::ToString(meta.State));
	std::vector<FString> aliases;
	std::vector<FString> formerPaths;
	if (bRecord)
	{
		view.ForEachAlias(record.Record, [&] (const assets::SAssetId& Alias)
		{
			aliases.push_back(Alias.SubId == 0u ? Alias.Guid.ToString() : FString::Format("{}#{:016x}", Alias.Guid.ToString(), Alias.SubId));
		});
		view.ForEachFormerPath(record.Record, [&] (FStringView Path) { formerPaths.emplace_back(Path); });
		for (const FString& alias : aliases)
		{
			Context.Print(FString::Format("  alias     {{{}}}", alias));
		}
		for (const FString& path : formerPaths)
		{
			Context.Print(FString::Format("  formerly  {}", ToDisplayText(path)));
		}
		Context.Print(FString::Format("  registry: {}, importer {}, id {{{}}}{}", TypeNameOf(view, record.Record), view.GetImporterName(record.Record),
			record.Id.Guid.ToString(), meta.State == assets::EMetaState::PendingMeta ? " (provisional: no meta yet)" : ""));
	}
	else
	{
		Context.Print("  registry: no record");
	}
	json.AddObjectArray("chain", chain).AddStringArray("notes", explanation.Notes).AddBool("record", bRecord)
		.AddString("recordType", bRecord ? TypeNameOf(view, record.Record) : FStringView()).AddObject("meta", metaJson)
		.AddStringArray("aliases", aliases).AddStringArray("formerPaths", formerPaths);
	Context.PrintJson(json);
	return ExitSuccess;
}

// ---- list-rules ----

// list-rules: every rule file the mounts have (each mount's Config/Assets.yml, every folder Assets.yml the scan entered)
// with its normalised entries; --json prints exactly Rules.cache's lines. Writes no Rules.cache (it holds no lock).
int32 RunListRules (CCommandletContext& Context)
{
	if (!Context.Arguments.empty())
	{
		return Context.Fail(ExitUsage, "list-rules: takes no arguments");
	}
	SRegistryRun run;
	if (const int32 result = OpenRegistry(Context, "list-rules", {}, run); result != ExitSuccess)
	{
		return result;
	}
	const assets::SRulesListing listing = run.Registry->GetRules().GetListing();
	if (Context.bJson)
	{
		const FString cache = assets::FormatRulesCache(listing);
		for (const FStringView line : cache.View().Split('\n'))
		{
			if (!line.TrimStartAndEnd().IsEmpty())
			{
				Context.PrintJsonLine(line.TrimEnd());
			}
		}
		return ExitSuccess;
	}
	uint32 malformed = 0u;
	for (const assets::SRuleFileListing& file : listing.Files)
	{
		Context.Print(FString::Format("{} ({}, {}) {:016x}{}", ToDisplayText(file.Origin.File), ToDisplayText(file.Origin.FolderMountPath),
			enum_::ToString(file.Origin.Level), file.RulesHash, file.bMalformed ? " MALFORMED (its last good rules apply)" : ""));
		for (const assets::SRuleListEntry& entry : file.Entries)
		{
			Context.Print(FString::Format("  {:>4}  {:<18} {}{}", entry.Line, entry.Kind, entry.Text,
				entry.RuleIndex >= 0 ? FString::Format("  (rule {})", entry.RuleIndex + 1) : FString()));
		}
		for (const SDiagnostic& diagnostic : file.Diagnostics)
		{
			Context.Print(FString::Format("  {} {}:{}:{}: {}", FormatDiagnosticCode(diagnostic.Code), diagnostic.File, diagnostic.Line, diagnostic.Column,
				diagnostic.Message));
		}
		malformed += file.bMalformed ? 1u : 0u;
	}
	Context.Print(FString::Format("{} rule file(s){}", listing.Files.size(), malformed > 0u ? FString::Format(", {} malformed", malformed) : FString()));
	return ExitSuccess;
}

// ---- dump ----

/**
 * dump registry [--text|--json] [--mount=<name>[,<name>...]]: the registry's deterministic dump (§5.3; the golden
 * files), of every mount or of the ones named ("Project", "/Plugin.X"). --json prints CRegistryView::DumpJson's lines.
 * Equal content dumps identically, fresh or warm, on any machine.
 */
int32 RunDump (CCommandletContext& Context)
{
	if (Context.Arguments.size() != 1u || !Context.Arguments[0].View().Equals<ESearchCase::IgnoreCaseAscii>("registry"))
	{
		return Context.Fail(ExitUsage, "dump: takes 'registry' (dump registry [--text|--json] [--mount=<name>,...]); asset and chunk dumps "
			"come with cooked data (M6)");
	}
	if (Context.HasOption("text") && Context.bJson)
	{
		return Context.Fail(ExitUsage, "dump registry: --text or --json, not both");
	}
	const std::optional<FString> mountOption = Context.FindOption("mount");
	if (Context.HasOption("mount") && !mountOption)
	{
		return Context.Fail(ExitUsage, "dump registry: --mount=<name>[,<name>...] names the mounts");
	}
	SRegistryRun run;
	if (const int32 result = OpenRegistry(Context, "dump", {}, run); result != ExitSuccess)
	{
		return result;
	}
	std::vector<assets::MountId> mounts;
	if (mountOption)
	{
		for (const FStringView name : mountOption->View().Split(','))
		{
			FStringView trimmed = name.TrimStartAndEnd();
			if (trimmed.StartsWith('/'))
			{
				trimmed = trimmed.RightChop(1u);
			}
			const assets::SMount* mount = trimmed.IsEmpty() ? nullptr : run.Mounts->Find(trimmed);
			if (mount == nullptr)
			{
				return Context.Fail(ExitUsage, FString::Format("dump registry: no mount '{}' (the mounts: {})", name.TrimStartAndEnd(), DescribeMounts(*run.Mounts)));
			}
			if (std::find(mounts.begin(), mounts.end(), mount->Id) == mounts.end())
			{
				mounts.push_back(mount->Id);
			}
		}
	}
	const assets::CRegistryView& view = run.View();
	if (Context.bJson)
	{
		view.DumpJson([&] (FStringView Line) { Context.PrintJsonLine(Line); }, assets::EDumpFlags::None, mounts);
	}
	else
	{
		FString text;
		view.DumpText(text, assets::EDumpFlags::None, mounts);
		Context.PrintText(text);
	}
	return ExitSuccess;
}
}
