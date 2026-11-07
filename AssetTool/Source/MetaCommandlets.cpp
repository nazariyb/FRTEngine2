/**
 * The meta verbs (design §13.2, §13.4, §14.8; M4): validate, fix, new-meta, migrate and resave. Each opens its registry
 * through OpenRegistry (RegistryCommandlets.cpp) with a lifecycle of its own (SLifecycleDesc, SRegistryOpenOptions::bLifecycle)
 * over the run's own mount set, /Project and its plugins or /Engine with --engine, never both, and runs F4's
 * CAssetRepairs::RunNow: commands a person runs, with no delays. The work is Core's (Assets/Meta/Validation.h,
 * MetaVerbs.h, Migrations.h), so Core-Test runs it in-process; this file parses, prints and picks the exit code.
 *
 * Who runs (SCommandlet::bWritesProject and bGuestRuns, Commandlets.cpp). All five take the mount's writer lock unless the
 * run is read-only, so beside another owner they exit with 4. `validate` alone runs there as a read-only guest when
 * Safety.GuestProcesses says so (§5.9): it reports, naming the owner in its META001. `validate --fix`, `fix`, `new-meta`,
 * `migrate` and `resave` exist to write, so a guest run of them would only report: they exit with 4 instead, and with
 * --ci or -Assets:ReadOnly=1 they are a usage error. `migrate --dry-run` is the exception: read-only by request, it takes
 * no lock, runs beside an owner and writes nothing.
 *
 * Exit codes: 0 done (validate: clean), 1 problems (validate: an error, or with --ci a warning; the writing verbs: a file
 * left without what was asked), 2 usage, 3 the registry did not finish, 4 another process owns the mount.
 */
#include <algorithm>
#include <filesystem>
#include <iterator>
#include <memory>
#include <optional>
#include <vector>

#include "Assets/AssetDiagnosticCodes.h"
#include "Assets/Meta/AssetRepairs.h"
#include "Assets/Meta/Lifecycle.h"
#include "Assets/Meta/MetaPaths.h"
#include "Assets/Meta/MetaVerbs.h"
#include "Assets/Meta/Migrations.h"
#include "Assets/Meta/Safety.h"
#include "Assets/Meta/Validation.h"
#include "Assets/Registry/RegistryRecords.h"
#include "Assets/Vcs/VcsProvider.h"
#include "Containers/StringMap.h"

#include "Commandlet.h"
#include "RegistryRun.h"


namespace frt::assettool
{
namespace
{
using assets::ToDisplayText;

// A writing verb's refusal of a read-only run: --ci and -Assets:ReadOnly=1 ask for nothing to be written.
int32 RefuseReadOnly (const CCommandletContext& Context, FStringView Verb)
{
	return Context.Fail(ExitUsage, FString::Format("{} writes metas: a read-only run (--ci, -Assets:ReadOnly=1) cannot run it", Verb));
}

// Positional arguments as mount paths of the run's own mounts. ExitSuccess, or the usage error reported.
int32 ResolveOwnFiles (const CCommandletContext& Context, FStringView Verb, const SRegistryRun& Run, std::vector<FString>& Out)
{
	for (const FString& argument : Context.Arguments)
	{
		int32 exitCode = ExitSuccess;
		const std::optional<FString> path = ResolveFileArgument(Context, Verb, *Run.Mounts, argument, exitCode);
		if (!path)
		{
			return exitCode;
		}
		const assets::SMount* mount = Run.Mounts->Find(assets::GetMountNameOf(*path));
		if (mount == nullptr || std::find(Run.OwnMounts.begin(), Run.OwnMounts.end(), mount->Id) == Run.OwnMounts.end())
		{
			return Context.Fail(ExitUsage, FString::Format("{}: {} is not in this run's mounts ({}{})", Verb, ToDisplayText(*path),
				Context.bEngineOwner ? "/Engine" : "/Project and its plugins", Context.bEngineOwner ? "" : "; /Engine's need --engine"));
		}
		Out.push_back(*path);
	}
	return ExitSuccess;
}

void PrintProblem (const CCommandletContext& Context, const SDiagnostic& Problem)
{
	const FString code = Problem.Code != 0u ? FormatDiagnosticCode(Problem.Code) : FString("-");
	FString where;
	if (!Problem.File.IsEmpty())
	{
		where = Problem.Line > 0u ? FString::Format(" ({}:{})", Problem.File, Problem.Line) : FString::Format(" ({})", Problem.File);
	}
	Context.Print(FString::Format("  {:<8} {:<8} {}{}: {}", code, GetSeverityName(Problem.Severity), ToDisplayText(Problem.Subject), where,
		Problem.Message));
	Context.PrintJson(CJsonObject().AddString("kind", "problem").AddString("code", Problem.Code != 0u ? code.View() : FStringView())
		.AddString("severity", GetSeverityName(Problem.Severity)).AddString("subject", Problem.Subject).AddString("file", Problem.File)
		.AddUInt("line", Problem.Line).AddUInt("column", Problem.Column).AddString("field", Problem.FieldPath).AddString("message", Problem.Message));
}

/**
 * validate --staged: what a commit stages outside every mount's content, which the mounts' providers (rooted at the content
 * directories) never list - each own mount's rule file (Config/Assets.yml, the engine's Core/Config/Assets.yml) and its
 * repository root's Schemas/Migrations.yml. Each repository root is asked once, through a provider rooted there from the
 * same factory and setting as the mounts' (so the hook's GIT_INDEX_FILE holds for it too); a root whose mounts' providers
 * stage nothing (None) is validated whole already. Out: root mount paths, as SValidateOptions names them.
 */
int32 FindStagedOutsideContent (const CCommandletContext& Context, FStringView Verb, const SRegistryRun& Run, std::vector<FString>& OutRuleMounts,
	std::vector<FString>& OutMigrationRoots)
{
	const auto fold = [] (const std::filesystem::path& Path) { return FString::FromPath(Path.lexically_normal().make_preferred()).FoldCase(); };
	const FString setting = assets::SAssetEditorSettings::FromConfig(Context.GetConfig()).VcsProvider;
	std::vector<std::filesystem::path> asked;
	for (const assets::MountId id : Run.OwnMounts)
	{
		const assets::SMount* mount = Run.Mounts->Find(id);
		assets::IVcsProvider* provider = Run.FindProvider(id);
		if (mount == nullptr || provider == nullptr || provider->GetName() == "None"
			|| std::find(asked.begin(), asked.end(), mount->OwnerRoot) != asked.end())
		{
			continue;
		}
		asked.push_back(mount->OwnerRoot);
		const std::unique_ptr<assets::IVcsProvider> rootProvider = assets::MakeVcsProvider(setting, FString::FromPath(mount->OwnerRoot));
		if (rootProvider == nullptr || rootProvider->GetName() == "None")
		{
			continue;
		}
		std::vector<FString> staged;
		if (const assets::SVcsResult result = rootProvider->GetStaged(staged); result.Error != assets::EVcsError::None)
		{
			return Context.Fail(ExitInternalError, FString::Format("{} --staged: {}", Verb, result.Message));
		}
		FStringSet folded;
		for (const FString& file : staged)
		{
			folded.insert(fold(file.ToPath()));
		}
		for (const assets::MountId other : Run.OwnMounts)
		{
			const assets::SMount* sibling = Run.Mounts->Find(other);
			if (sibling != nullptr && sibling->OwnerRoot == mount->OwnerRoot && !sibling->RulesFile.empty() && folded.contains(fold(sibling->RulesFile)))
			{
				OutRuleMounts.push_back(sibling->GetRootMountPath());
			}
		}
		if (folded.contains(fold(assets::GetMigrationsPath(mount->OwnerRoot))))
		{
			OutMigrationRoots.push_back(mount->GetRootMountPath());
		}
	}
	return ExitSuccess;
}

// The GUID a source's meta gives its record now ("" for none).
FString GuidOf (const assets::CRegistryView& View, FStringView Source)
{
	const std::optional<assets::SStoredFile> stored = assets::FindStoredFile(View, Source);
	return stored.has_value() && stored->MetaState == assets::EMetaState::Paired ? stored->Guid.ToString() : FString();
}

/**
 * validate [paths] [--staged] [--quick] [--fix] [--ci], and fix [paths] (bFix): one implementation (edit [8]). The lifecycle
 * runs over the scope first - reporting only, or with the safe repairs for a fix - then RunValidation lists what the
 * registry and the V-only rules find there.
 */
int32 Validate (CCommandletContext& Context, FStringView Verb, bool bFix)
{
	const bool bStaged = Context.HasOption("staged");
	const bool bQuick = Context.HasOption("quick");
	if (bFix && Context.bGuest)
	{
		return Context.Fail(ExitOwned, FString::Format("{} owns {}: {} writes metas, so it does not run as a read-only guest; close it, or run "
			"validate", Context.Owner, Context.GetMountName(), Verb), CJsonObject().AddString("owner", Context.Owner).AddString("mount", Context.GetMountName()));
	}
	if (bFix && Context.bReadOnly)
	{
		return RefuseReadOnly(Context, Verb);
	}
	if (bFix && bQuick)
	{
		return Context.Fail(ExitUsage, FString::Format("{}: --quick hashes nothing, and a meta's GUID is derived from its content: drop --quick", Verb));
	}
	if (bStaged && !Context.Arguments.empty())
	{
		return Context.Fail(ExitUsage, FString::Format("{}: --staged or paths, not both", Verb));
	}

	SRegistryRun run;
	if (const int32 result = OpenRegistry(Context, Verb, { .bWriter = bFix, .bLifecycle = true, .bRepairs = bFix }, run); result != ExitSuccess)
	{
		return result;
	}
	std::vector<FString> paths;
	if (const int32 result = ResolveOwnFiles(Context, Verb, run, paths); result != ExitSuccess)
	{
		return result;
	}
	std::vector<FString> stagedRuleMounts;
	std::vector<FString> stagedMigrationRoots;
	const bool bLimited = bStaged || !paths.empty();
	if (bStaged)
	{
		// The files the VCS has staged (Git: `git diff --cached`), as the hook commits them; a mount whose provider stages
		// nothing (None) is validated whole (edit [8]).
		for (const assets::MountId mount : run.OwnMounts)
		{
			assets::IVcsProvider* provider = run.FindProvider(mount);
			if (provider == nullptr || provider->GetName() == "None")
			{
				paths.push_back(run.Mounts->Find(mount)->GetRootMountPath());
				continue;
			}
			std::vector<FString> staged;
			const assets::SVcsResult result = provider->GetStaged(staged);
			if (result.Error != assets::EVcsError::None)
			{
				return Context.Fail(ExitInternalError, FString::Format("{} --staged: {}", Verb, result.Message));
			}
			for (const FString& file : staged)
			{
				if (const std::optional<FString> path = run.Mounts->ToMountPath(file.ToPath()); path.has_value())
				{
					paths.push_back(*path);
				}
			}
		}
		if (const int32 result = FindStagedOutsideContent(Context, Verb, run, stagedRuleMounts, stagedMigrationRoots); result != ExitSuccess)
		{
			return result;
		}
	}

	assets::CAssetRepairs& repairs = *run.Registry->GetRepairs();
	assets::SRepairReport report;
	if (!bLimited || !paths.empty())
	{
		assets::SRepairOptions options;
		options.Paths = bLimited ? paths : run.GetOwnRoots();
		if (bFix)
		{
			options.Kinds.assign(std::begin(assets::SafeFixKinds), std::end(assets::SafeFixKinds));
		}
		options.bNoHashing = bQuick;
		report = repairs.RunNow(options);
	}
	if (bFix)
	{
		const assets::CRegistryView& view = run.View();
		for (const assets::SLifecycleAction& action : report.Planned)
		{
			// Each as what it was: a refused one wrote nothing (its VCS002 is among what remains), and one no op ran neither.
			const assets::ERepairOutcome outcome = assets::GetRepairOutcome(report, action);
			const bool bApplied = outcome == assets::ERepairOutcome::Applied;
			const FStringView outcomeName = bApplied ? FStringView("applied") : outcome == assets::ERepairOutcome::Refused ? FStringView("refused")
				: FStringView("not-run");
			Context.Print(FString::Format("  {:<12} {}{}", enum_::ToString(action.Kind), ToDisplayText(action.Path),
				bApplied ? FString() : FString::Format("  ({}{})", outcomeName, outcome == assets::ERepairOutcome::Refused ? ": VCS002" : "")));
			Context.PrintJson(CJsonObject().AddString("kind", "repair").AddString("action", enum_::ToString(action.Kind)).AddString("path", action.Path)
				.AddString("to", action.ToPath).AddString("outcome", outcomeName)
				.AddString("guid", bApplied && action.Kind == assets::ELifecycleAction::CreateMeta ? GuidOf(view, action.Path).View() : FStringView()));
		}
		Context.Print(FString::Format("Repaired {}, refused {} (the VCS or the writer refused: VCS002)", report.Applied, report.Refused));
	}

	assets::SValidateOptions options;
	options.Mounts = run.Mounts.get();
	options.Rules = &run.Registry->GetRules();
	options.Scope = run.OwnMounts;
	if (bLimited)
	{
		options.Paths = paths;
	}
	options.StagedRuleMounts = std::move(stagedRuleMounts);
	options.StagedMigrationRoots = std::move(stagedMigrationRoots);
	options.bQuick = bQuick;
	options.Providers = run.Providers->GetProviders();
	std::vector<SDiagnostic> problems;
	assets::RunValidation(run.View(), options, problems);
	// A refusal of this run's plan (a file it does not write) is no registry problem: shown once, with the rest.
	for (const SDiagnostic& plan : report.Problems)
	{
		if (plan.Code == 0u && std::none_of(problems.begin(), problems.end(), [&] (const SDiagnostic& Problem) { return Problem == plan; }))
		{
			problems.push_back(plan);
		}
	}

	Context.Print(problems.empty() ? FString(bFix ? "What remains: nothing" : "Problems: none") : FString(bFix ? "What remains:" : "Problems:"));
	for (const SDiagnostic& problem : problems)
	{
		PrintProblem(Context, problem);
	}
	const assets::SValidationCounts counts = assets::CountValidation(problems);
	const bool bClean = assets::IsValidationClean(counts, Context.bCi);
	Context.Print(FString::Format("{}: {} error(s), {} warning(s), {} info{}", DescribeMounts(*run.Mounts), counts.Errors, counts.Warnings, counts.Infos,
		bClean ? "" : (Context.bCi && counts.Errors == 0u ? ": not clean (--ci allows no warning)" : "")));
	Context.PrintJson(CJsonObject().AddString("kind", "summary").AddString("verb", Verb).AddUInt("errors", counts.Errors).AddUInt("warnings", counts.Warnings)
		.AddUInt("infos", counts.Infos).AddBool("clean", bClean).AddBool("staged", bStaged).AddBool("quick", bQuick)
		.AddUInt("repaired", report.Applied).AddUInt("refused", report.Refused));
	return bClean ? ExitSuccess : ExitProblems;
}
}


// validate [paths] [--staged] [--quick] [--fix] [--ci]
int32 RunValidate (CCommandletContext& Context)
{
	return Validate(Context, "validate", Context.HasOption("fix"));
}

// fix [paths]: validate --fix under another name (§13.2).
int32 RunFix (CCommandletContext& Context)
{
	return Validate(Context, "fix", true);
}

// new-meta <file...>: a meta now for each file named; a file that has one is an error, and then nothing is written.
int32 RunNewMeta (CCommandletContext& Context)
{
	if (Context.Arguments.empty())
	{
		return Context.Fail(ExitUsage, "new-meta: takes one or more files (new-meta <file...>): mount paths or paths on disk");
	}
	if (Context.bReadOnly)
	{
		return RefuseReadOnly(Context, "new-meta");
	}
	SRegistryRun run;
	if (const int32 result = OpenRegistry(Context, "new-meta", { .bWriter = true, .bLifecycle = true, .bRepairs = true }, run); result != ExitSuccess)
	{
		return result;
	}
	std::vector<FString> paths;
	if (const int32 result = ResolveOwnFiles(Context, "new-meta", run, paths); result != ExitSuccess)
	{
		return result;
	}
	const assets::SNewMetasResult result = assets::NewMetas(*run.Registry, paths);
	const assets::CRegistryView& view = run.View();
	for (const FString& source : result.Created)
	{
		const FString guid = GuidOf(view, source);
		Context.Print(FString::Format("  created  {}  {{{}}}", ToDisplayText(assets::GetMetaPath(source)), guid));
		Context.PrintJson(CJsonObject().AddString("kind", "meta").AddString("path", source).AddString("meta", assets::GetMetaPath(source)).AddString("guid", guid));
	}
	for (const SDiagnostic& problem : result.Problems)
	{
		PrintProblem(Context, problem);
	}
	Context.Print(result.bRefused ? FString("new-meta: nothing written") : FString::Format("{} meta(s) created", result.Created.size()));
	Context.PrintJson(CJsonObject().AddString("kind", "summary").AddString("verb", "new-meta").AddUInt("created", result.Created.size())
		.AddBool("refused", result.bRefused));
	return result.Problems.empty() ? ExitSuccess : ExitProblems;
}

/**
 * migrate metas|--all [--dry-run]: the content migrations §14.8 lists, each recording itself in the root's
 * Schemas/Migrations.yml. M4 has `metas`; the later steps name their milestone.
 */
int32 RunMigrate (CCommandletContext& Context)
{
	const bool bAll = Context.HasOption("all");
	const bool bDryRun = Context.HasOption("dry-run");
	if (bAll == !Context.Arguments.empty() || Context.Arguments.size() > 1u)
	{
		return Context.Fail(ExitUsage, "migrate: takes one step or --all (migrate <step>|--all [--dry-run]; the steps: metas, material-refs, documents, "
			"texture-colorspaces, model-materials)");
	}
	std::vector<const assets::SMigrationStep*> steps;
	if (bAll)
	{
		for (const assets::SMigrationStep& step : assets::GetMigrationSteps())
		{
			if (step.bAvailable)
			{
				steps.push_back(&step);
			}
		}
	}
	else
	{
		const assets::SMigrationStep* step = assets::FindMigrationStep(Context.Arguments[0]);
		if (step == nullptr)
		{
			return Context.Fail(ExitUsage, FString::Format("migrate: no step '{}' (metas, material-refs, documents, texture-colorspaces, "
				"model-materials)", Context.Arguments[0]));
		}
		if (!step->bAvailable)
		{
			return Context.Fail(ExitUsage, FString::Format("migrate {}: arrives with {}; this engine has metas", step->Name, step->Milestone));
		}
		steps.push_back(step);
	}
	if (Context.bReadOnly && !bDryRun)
	{
		return RefuseReadOnly(Context, "migrate");
	}

	// A dry run is read-only by request (SCommandlet::ReadOnlyOption): the host took no lock, and its reader writes nothing,
	// neither metas nor the registry's own Local/Assets files, and rolls no unfinished operation forward.
	SRegistryRun run;
	const bool bWriter = !Context.bReadOnly && !bDryRun;
	if (const int32 result = OpenRegistry(Context, "migrate", { .bWriter = bWriter, .bLifecycle = true, .bRepairs = bWriter }, run); result != ExitSuccess)
	{
		return result;
	}
	const assets::MountId rootMount = Context.bEngineOwner ? assets::EngineMountId : assets::ProjectMountId;
	bool bComplete = true;
	for (const assets::SMigrationStep* step : steps)
	{
		// Only `metas` can be available in M4.
		const assets::SMigrateMetasResult result = assets::MigrateMetas(*run.Registry, run.OwnMounts, Context.GetMountRoot(), run.FindProvider(rootMount), bDryRun);
		const assets::CRegistryView& view = run.View();
		uint32 created = 0u;
		for (const assets::SLifecycleAction& action : result.Report.Planned)
		{
			if (action.Kind != assets::ELifecycleAction::CreateMeta)
			{
				continue;
			}
			const FString guid = bDryRun ? (action.Guid.IsValid() ? action.Guid.ToString() : FString("random")) : GuidOf(view, action.Path);
			if (!bDryRun && guid.IsEmpty())
			{
				continue;   // not written after all: among the problems
			}
			++created;
			Context.Print(FString::Format("  {}  {}  {{{}}}", bDryRun ? "would create" : "created", ToDisplayText(assets::GetMetaPath(action.Path)), guid));
			Context.PrintJson(CJsonObject().AddString("kind", "meta").AddString("path", action.Path).AddString("meta", assets::GetMetaPath(action.Path))
				.AddString("guid", guid).AddBool("dryRun", bDryRun));
		}
		for (const FString& remaining : result.Remaining)
		{
			Context.PrintJson(CJsonObject().AddString("kind", "remaining").AddString("path", remaining));
		}
		for (const SDiagnostic& problem : result.Report.Problems)
		{
			// A dry run's META001 is what it would create, listed above.
			const bool bPlanned = problem.Code == assets::Meta001 && std::any_of(result.Report.Planned.begin(), result.Report.Planned.end(),
				[&] (const assets::SLifecycleAction& Action) { return Action.Path == problem.Subject; });
			if (!bPlanned)
			{
				PrintProblem(Context, problem);
			}
		}
		if (!result.Error.IsEmpty())
		{
			Context.PrintError(FString::Format("migrate {}: {}", step->Name, result.Error));
		}
		const FString record = result.bRecorded ? (result.bWasRecorded ? FString::Format("{} already lists it", assets::MigrationsFile)
				: FString::Format("recorded in {}", assets::MigrationsFile))
			: bDryRun ? FString("a dry run: nothing written") : FString::Format("not recorded: {} file(s) still have no meta", result.Remaining.size());
		Context.Print(FString::Format("migrate {}: {} meta(s) {}, {}", step->Name, created, bDryRun ? "to create" : "created", record));
		Context.PrintJson(CJsonObject().AddString("kind", "summary").AddString("step", step->Name).AddUInt("created", created)
			.AddUInt("remaining", result.Remaining.size()).AddBool("recorded", result.bRecorded).AddBool("dryRun", bDryRun));
		bComplete = bComplete && result.Remaining.empty() && result.Error.IsEmpty() && (bDryRun || result.bRecorded);
	}
	return bComplete ? ExitSuccess : ExitProblems;
}

// resave --metas: every meta that parses and is not canonical, rewritten canonically (its own commit, §13.2).
int32 RunResave (CCommandletContext& Context)
{
	static constexpr FStringLiteral LaterModes[] = { "references", "redirected", "type", "drop-unknown" };
	for (const FStringLiteral later : LaterModes)
	{
		if (Context.HasOption(later) || Context.FindOption(later).has_value())
		{
			return Context.Fail(ExitUsage, FString::Format("resave --{}: arrives with M5 (references and reflected types); this engine has --metas", later));
		}
	}
	if (!Context.HasOption("metas") || !Context.Arguments.empty())
	{
		return Context.Fail(ExitUsage, "resave: takes --metas (resave --metas; --references, --redirected, --type and --drop-unknown arrive with M5)");
	}
	if (Context.bReadOnly)
	{
		return RefuseReadOnly(Context, "resave");
	}
	SRegistryRun run;
	if (const int32 result = OpenRegistry(Context, "resave", { .bWriter = true, .bLifecycle = true, .bRepairs = true }, run); result != ExitSuccess)
	{
		return result;
	}
	const assets::SResaveMetasResult result = assets::ResaveMetas(*run.Registry, run.OwnMounts, false);
	for (const FString& meta : result.Rewritten)
	{
		Context.Print(FString::Format("  rewritten  {}", ToDisplayText(meta)));
		Context.PrintJson(CJsonObject().AddString("kind", "meta").AddString("path", meta).AddBool("rewritten", true));
	}
	for (const FString& meta : result.Broken)
	{
		Context.Print(FString::Format("  broken     {} (left as it is: META004)", ToDisplayText(meta)));
		Context.PrintJson(CJsonObject().AddString("kind", "meta").AddString("path", meta).AddBool("rewritten", false).AddBool("broken", true));
	}
	for (const FString& meta : result.Newer)
	{
		Context.Print(FString::Format("  newer      {} (left as it is: written by a newer engine)", ToDisplayText(meta)));
		Context.PrintJson(CJsonObject().AddString("kind", "meta").AddString("path", meta).AddBool("rewritten", false).AddBool("newer", true));
	}
	for (const SDiagnostic& problem : result.Problems)
	{
		PrintProblem(Context, problem);
	}
	Context.Print(FString::Format("resave --metas: {} rewritten, {} already canonical, {} broken, {} newer", result.Rewritten.size(), result.Canonical,
		result.Broken.size(), result.Newer.size()));
	Context.PrintJson(CJsonObject().AddString("kind", "summary").AddString("verb", "resave").AddUInt("rewritten", result.Rewritten.size())
		.AddUInt("canonical", result.Canonical).AddUInt("broken", result.Broken.size()).AddUInt("newer", result.Newer.size()));
	return result.Problems.empty() ? ExitSuccess : ExitProblems;
}
}
