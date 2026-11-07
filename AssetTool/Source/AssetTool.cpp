/**
 * AssetTool: the asset system's command line (design §13.1, §13.2), a console host beside the editor
 * and the launcher, which FRT.bat assets <verb> runs. One run is one commandlet:
 *
 *     AssetTool <verb> [arguments] [options]       AssetTool -Run=<Verb> [arguments] [options]
 *
 * It finds the project like the other hosts (-Project=<file> or --project=<file>, a bare *.frtproject
 * argument, else the working directory's; project::FindProjectToRun), or with --engine acts on /Engine
 * as its owner with no project (§4.7). A verb that writes nothing (help, newguid) runs with no project
 * when the working directory's cannot be used, with a warning: where it is run does not break it.
 * Options that neither the host nor the verb reads are usage errors (FindUnknownOption); "--" ends the
 * options, and a query's negated term (-label:Debug) is text (Commandlet.h, GetPositionalArguments).
 * It starts the engine as a tool host (SEngineDesc::bToolHost): CEngine::StartUp's first steps - components,
 * config layers and -Section:Key=Value, user settings, the memory pool, the worker pool
 * (CCommandletContext::GetWorkerPool) - in the one order every host shares, and no window, renderer or
 * game module: the game module joins with the game's types and importers (M5). Release builds no
 * AssetTool (it cannot load a game module).
 *
 * A commandlet that writes what only the mount's owner may write (SCommandlet::bWritesProject) runs
 * only while this process holds the mount's writer lock, Local/Assets/Owner.lock (§5.9): if another
 * process owns the mount, the run ends with exit code 4 and names it - unless the opened project's
 * Safety.GuestProcesses (the engine's with --engine) lets it run as a read-only guest, which says so on
 * stderr and writes nothing (lock never runs as one). --ci and -Assets:ReadOnly=1 make a run read-only
 * by request, as does a verb's own read-only option (SCommandlet::ReadOnlyOption: migrate --dry-run): it
 * never takes the lock, and so runs beside an open editor.
 *
 * A root behind the engine's content migrations (MIG001: its Schemas/Migrations.yml lacks a step, design §14.8) is
 * written only by migrate: every other run that would write exits with code 1 before it takes the lock, a run that
 * writes nothing runs and reports MIG001 (scan, status, validate). The gate is [Assets] MigrationGate
 * (assets::IsMigrationGateOn), off until H4b commits each repository's Schemas/Migrations.yml.
 *
 * Output: human text, or with --json one JSON object per line on stdout (stable field names); errors
 * and warnings on stderr in either mode. Exit codes: Commandlet.h (0 success, 1 problems, 2 usage,
 * 3 internal error or crash, 4 owned by another process).
 */
#include <cstdio>
#include <exception>
#include <filesystem>
#include <optional>
#include <span>
#include <system_error>
#include <vector>

#include "EnginePaths.h"
#include "Precision.h"
#include "Assets/Discovery/BuiltinExcludes.h"
#include "Assets/Meta/AssetRepairs.h"
#include "Assets/Meta/Migrations.h"
#include "Assets/MountPaths.h"
#include "Assets/OwnerLock.h"
#include "Engine/Engine.h"
#include "Jobs/WorkerPool.h"
#include "Platform/FileLock.h"
#include "Platform/Platform.h"
#include "Platform/Process.h"
#include "Project/Project.h"

#include "Commandlet.h"


namespace frt::assettool
{
namespace
{
// The command line names the project, the ways FindProjectToRun reads one: -Project= with a value, or
// an argument naming a *.frtproject file.
bool NamesProject (std::span<const FString> CommandLine)
{
	if (const std::optional<FString> value = platform::FindCommandLineValue(CommandLine, "Project"); value && !value->IsEmpty())
	{
		return true;
	}
	for (const FString& argument : CommandLine)
	{
		if (project::HasProjectFileExtension(argument.ToPath()))
		{
			return true;
		}
	}
	return false;
}

// The project the run acts on, into Context and paths::SetProject. ExitSuccess, or the exit code to
// end with (the failure already reported). A verb that writes needs a mount: a project or --engine.
//
// A project that cannot be used - its file does not load, it names no engine that exists, or another
// engine - ends a run that needs it: one that writes, or that named it. A verb that writes nothing
// and was run in that project's directory without naming it (help, newguid, the ddc verbs) does not
// need it: it warns on stderr and runs with no project, so where it is run never breaks it.
int32 FindProject (CCommandletContext& Context, const SCommandlet& Commandlet)
{
	const bool bNamed = NamesProject(Context.CommandLine);
	if (Context.bEngineOwner)
	{
		if (bNamed || platform::FindCommandLineValue(Context.CommandLine, "Project").has_value())
		{
			return Context.Fail(ExitUsage, "--engine acts on /Engine without a project: name a project (--project=<file>, "
				"-Project=<file> or a .frtproject argument) or give --engine, not both");
		}
		return ExitSuccess;
	}

	FString error;
	const std::filesystem::path file = project::FindProjectToRun(false, &error);
	if (file.empty())
	{
		if (!error.IsEmpty())
		{
			return Context.Fail(ExitUsage, error);   // named, but not there
		}
		if (Commandlet.bWritesProject)
		{
			return Context.Fail(ExitUsage, FString::Format("{}: no project to act on. Run it in a project's directory, with "
				"--project=<file>, or with --engine for the engine's own content", Commandlet.Name));
		}
		return ExitSuccess;
	}

	const auto unusable = [&] (FStringView Why) -> int32
	{
		if (Commandlet.bWritesProject || bNamed)
		{
			return Context.Fail(ExitUsage, Why);
		}
		Context.PrintError(FString::Format("warning: the working directory's project cannot be used, so {} runs without one: {}",
			Commandlet.Name, Why));
		return ExitSuccess;
	};

	project::SProjectDescriptor descriptor;
	if (!project::SProjectDescriptor::Load(file, descriptor, &error))
	{
		return unusable(error);
	}
	const std::filesystem::path engineDir = project::ResolveEngineDir(descriptor, &error);
	if (engineDir.empty())
	{
		return unusable(descriptor.Name + ": " + error);
	}
	std::error_code ec;
	if (!std::filesystem::equivalent(engineDir, Context.EngineRoot, ec))
	{
		// Its metas, keys and DDC values are that engine's to write.
		return unusable(FString::Format("{} uses the engine in {}: run that engine's AssetTool (the project's FRT.bat assets "
			"does)", descriptor.Name, FString::FromPath(engineDir)));
	}
	paths::SetProject(descriptor);
	Context.ProjectRoot = descriptor.GetDir();
	return ExitSuccess;
}

int32 RunAssetTool ()
{
	CCommandletContext context;
	context.CommandLine = platform::GetCommandLineArguments();
	// Options end at "--" (Commandlet.h, GetOptionArguments): what follows is the verb's text.
	const std::span<const FString> options = GetOptionArguments(context.CommandLine);
	context.bJson = platform::HasCommandLineSwitch(options, "json");
	context.bCi = platform::HasCommandLineSwitch(options, "ci");
	context.bEngineOwner = platform::HasCommandLineSwitch(options, "engine");

	FString verb = FindVerb(context.CommandLine).value_or(FString());
	const bool bHelpSwitch = platform::HasCommandLineSwitch(options, "help") || platform::HasCommandLineSwitch(options, "?");
	if (verb.IsEmpty() && bHelpSwitch)
	{
		verb = "help";
	}
	if (verb.IsEmpty())
	{
		return context.Fail(ExitUsage, "No verb: FRT.bat assets <verb> [arguments]. FRT.bat assets help lists the verbs.");
	}
	const SCommandlet* commandlet = FindCommandlet(verb);
	if (commandlet == nullptr)
	{
		return context.Fail(ExitUsage, FString::Format("No verb '{}': FRT.bat assets help lists the verbs.", verb));
	}
	if (const std::optional<FString> unknown = FindUnknownOption(context.CommandLine, commandlet); unknown.has_value())
	{
		return context.Fail(ExitUsage, FString::Format("{}: no option '{}' (FRT.bat assets help {} says what it takes; the options "
			"of every verb are in FRT.bat assets help; after -- every argument is the verb's)", commandlet->Name, *unknown, commandlet->Name));
	}
	context.Arguments = GetPositionalArguments(context.CommandLine, commandlet);
	// <verb> --help is help <verb>: the verb itself does not run.
	if (bHelpSwitch && commandlet != FindCommandlet("help"))
	{
		context.Arguments = { FString(commandlet->Name) };
		commandlet = FindCommandlet("help");
	}

	// The project, before the config: its Game.ini is a layer of it.
	context.EngineRoot = paths::GetEngineDir();
	if (const int32 result = FindProject(context, *commandlet); result != ExitSuccess)
	{
		return result;
	}

	// The engine's first steps (SEngineDesc::bToolHost), before the lock: config layers weakest first,
	// then -Section:Key=Value, then memory and the worker pool. Verbs run their heavy work on the pool
	// (the ddc verbs' walks and hashing; builds later) and wait by pumping here. It shuts down after
	// the lock is gone.
	SEngineDesc desc;
	desc.bToolHost = true;
	CEngine engine;
	if (!engine.StartUp(desc))
	{
		return context.Fail(ExitInternalError, "the engine did not start");
	}
	context.SetConfig(engine.GetConfig());
	context.bReadOnly = context.bCi || engine.GetConfig().Get("Assets", "ReadOnly", false)
		|| (!commandlet->ReadOnlyOption.IsEmpty() && context.HasOption(commandlet->ReadOnlyOption));
	context.SetWorkerPool(engine.GetWorkerPool());

	// The Safety switches that govern a process (GuestProcesses, BuilderSourceGuard) come from one file: the opened
	// project's, the engine's with --engine (and with no project). BuilderSourceGuard is set before the verb keys any
	// builder's step (assets::ApplySafetyOfRuleFile).
	const std::filesystem::path rulesFile = context.bEngineOwner || context.ProjectRoot.empty()
		? paths::GetEngineConfigPath().parent_path() / assets::RuleFileName.ToPath()
		: context.ProjectRoot / "Config" / assets::RuleFileName.ToPath();
	(void)assets::ApplySafetyOfRuleFile(rulesFile);

	// The MIG001 gate (design §14.8; assets::IsMigrationGateOn, off until H4b): a root behind the engine's migrations - the
	// project's, the engine's with --engine - is written by nothing but its migration. A run that would write ends here,
	// before the lock, with exit code 1; one that writes nothing (--ci, -Assets:ReadOnly=1, migrate --dry-run, validate
	// without --fix) runs, and the verbs that report problems list MIG001 with them (CCommandletContext::MigrationGap).
	if (!context.GetMountRoot().empty() && assets::IsMigrationGateOn(engine.GetConfig()))
	{
		context.MigrationGap = assets::FindMigrationGap(context.GetMountRoot(), context.GetMountName(), context.bEngineOwner);
		const bool bWrites = commandlet->bWritesProject && !context.bReadOnly
			&& (commandlet->WriteOption.IsEmpty() || context.HasOption(commandlet->WriteOption));
		if (context.MigrationGap.has_value() && bWrites && !commandlet->bRunsBehind)
		{
			const SDiagnostic& gap = *context.MigrationGap;
			return context.Fail(ExitProblems, FString::Format("{}: MIG001 {}; {} writes nothing until then", commandlet->Name, gap.Message,
				commandlet->Name), CJsonObject().AddString("code", "MIG001").AddString("subject", gap.Subject).AddString("file", gap.File));
		}
	}

	// Held until the commandlet returns; the OS lets go of it if the process dies first. The decision is the one the
	// editor makes (assets::ClaimAssetOwnership), for the run's own mount set only: never /Project and /Engine together.
	std::optional<platform::CExclusiveFileLock> ownerLock;
	if (commandlet->bWritesProject)
	{
		assets::SAssetOwnershipRequest request;
		request.Root = context.GetMountRoot();
		request.RulesFile = rulesFile;
		request.bReadOnly = context.bReadOnly;
		assets::SAssetOwnership ownership = assets::ClaimAssetOwnership(request);
		// A process without the lock rolls nothing forward (§4.7): the owner finishes it. With the lock, the verb's own
		// lifecycle does at its Start (the request passes no Mounts): until then, say one waits, whatever the role.
		const std::vector<FString> unfinished = assets::DescribeUnfinishedOperations(request, ownership, false);
		const FString& owner = ownership.Root.Owner;
		if (ownership.Role == assets::EOwnerRole::Writer)
		{
			ownerLock = std::move(ownership.Root.Lock);
		}
		else if (ownership.Role == assets::EOwnerRole::Guest && commandlet->bGuestRuns)
		{
			// §5.9: it loads, scans and reports as any read-only run does, and writes nothing a mount owns.
			context.bReadOnly = true;
			context.bGuest = true;
			context.Owner = owner;
			context.PrintError(FString::Format("warning: {} owns {}: {} runs as a read-only guest (Safety.GuestProcesses) and writes "
				"nothing", owner, context.GetMountName(), commandlet->Name));
		}
		else if (ownership.Role == assets::EOwnerRole::Guest || ownership.Role == assets::EOwnerRole::Refused)
		{
			return context.Fail(ExitOwned, FString::Format("{} owns {}: close it, or run with --ci", owner, context.GetMountName()),
				CJsonObject().AddString("owner", owner).AddString("mount", context.GetMountName()));
		}
		else if (ownership.Role == assets::EOwnerRole::Failed)
		{
			return context.Fail(ExitInternalError, FString::Format("{}: cannot take the writer lock of {}: {}", commandlet->Name,
				context.GetMountName(), ownership.Root.Error));
		}
		for (const FString& line : unfinished)
		{
			context.PrintError("warning: " + line);
		}
	}

	const int32 result = commandlet->Run(context);
	// No work of the verb outlives it: what it wrote under the lock is finished before the lock goes.
	engine.GetWorkerPool().CancelAllAndDrain();
	return result;
}
}
}


int main ()
{
	frt::platform::InitConsoleUtf8();
	// Scripts read the exit code: a crash is 3, never a dialog.
	frt::platform::ExitOnCrash(frt::assettool::ExitInternalError);
	FRT_VERIFY_REAL_PRECISION_ABI();

	try
	{
		return frt::assettool::RunAssetTool();
	}
	catch (const std::exception& e)
	{
		const frt::FString message = "AssetTool: internal error: " + frt::FString::FromUtf8Lossy(e.what());
		std::fwrite(message.GetData(), 1u, message.Len(), stderr);
		std::fputc('\n', stderr);
	}
	catch (...)
	{
		std::fputs("AssetTool: internal error: an unknown exception\n", stderr);
	}
	return frt::assettool::ExitInternalError;
}
