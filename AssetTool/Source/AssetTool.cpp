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
 * Options that neither the host nor the verb reads are usage errors (FindUnknownOption).
 * It starts the engine as a tool host (SEngineDesc::bToolHost): CEngine::StartUp's first steps - components,
 * config layers and -Section:Key=Value, user settings, the memory pool, the worker pool
 * (CCommandletContext::GetWorkerPool) - in the one order every host shares, and no window, renderer or
 * game module: the game module joins with the game's types and importers (M5). Release builds no
 * AssetTool (it cannot load a game module).
 *
 * A commandlet that writes what only the mount's owner may write (SCommandlet::bWritesProject) runs
 * only while this process holds the mount's writer lock, Local/Assets/Owner.lock (§5.9): if another
 * process owns the mount, the run ends with exit code 4 and names it. --ci and -Assets:ReadOnly=1 make
 * a run read-only by request: it never takes the lock, and so runs beside an open editor.
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
struct SVerbAndArguments
{
	FString Verb;
	std::vector<FString> Arguments;
};

/**
 * The verb and its positional arguments. Options start with '-' (-Run=, -Project=, --json,
 * -Section:Key=Value); a '/' starts no option here, so arguments that are mount paths
 * ("/Project/Content/Rock.png") stay arguments. -Run=<Verb> names the verb, else the first positional
 * argument does. A bare *.frtproject argument names the project (FindProjectToRun reads it), never a
 * verb or an argument.
 */
SVerbAndArguments SplitVerbAndArguments (std::span<const FString> CommandLine)
{
	SVerbAndArguments result;
	const std::optional<FString> run = platform::FindCommandLineValue(CommandLine, "Run");
	bool bVerbFound = run.has_value();
	if (bVerbFound)
	{
		result.Verb = *run;
	}
	for (const FString& argument : CommandLine)
	{
		if (argument.StartsWith('-') || project::HasProjectFileExtension(argument.ToPath()))
		{
			continue;
		}
		if (!bVerbFound)
		{
			result.Verb = argument;
			bVerbFound = true;
			continue;
		}
		result.Arguments.push_back(argument);
	}
	return result;
}

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
	context.bJson = platform::HasCommandLineSwitch(context.CommandLine, "json");
	context.bCi = platform::HasCommandLineSwitch(context.CommandLine, "ci");
	context.bEngineOwner = platform::HasCommandLineSwitch(context.CommandLine, "engine");

	SVerbAndArguments verb = SplitVerbAndArguments(context.CommandLine);
	const bool bHelpSwitch = platform::HasCommandLineSwitch(context.CommandLine, "help")
		|| platform::HasCommandLineSwitch(context.CommandLine, "?");
	if (verb.Verb.IsEmpty() && bHelpSwitch)
	{
		verb.Verb = "help";
	}
	if (verb.Verb.IsEmpty())
	{
		return context.Fail(ExitUsage, "No verb: FRT.bat assets <verb> [arguments]. FRT.bat assets help lists the verbs.");
	}
	const SCommandlet* commandlet = FindCommandlet(verb.Verb);
	if (commandlet == nullptr)
	{
		return context.Fail(ExitUsage, FString::Format("No verb '{}': FRT.bat assets help lists the verbs.", verb.Verb));
	}
	if (const std::optional<FString> unknown = FindUnknownOption(context.CommandLine, commandlet); unknown.has_value())
	{
		return context.Fail(ExitUsage, FString::Format("{}: no option '{}' (FRT.bat assets help {} says what it takes; the options "
			"of every verb are in FRT.bat assets help)", commandlet->Name, *unknown, commandlet->Name));
	}
	context.Arguments = std::move(verb.Arguments);
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
	context.bReadOnly = context.bCi || engine.GetConfig().Get("Assets", "ReadOnly", false);
	context.SetWorkerPool(engine.GetWorkerPool());

	// Held until the commandlet returns; the OS lets go of it if the process dies first.
	std::optional<platform::CExclusiveFileLock> ownerLock;
	if (commandlet->bWritesProject && !context.bReadOnly)
	{
		FString owner;
		FString error;
		ownerLock = assets::TryTakeOwnerLock(context.GetMountRoot(), &owner, &error);
		if (!ownerLock.has_value())
		{
			if (owner.IsEmpty())
			{
				return context.Fail(ExitInternalError, FString::Format("{}: cannot take the writer lock of {}: {}", commandlet->Name,
					context.GetMountName(), error));
			}
			return context.Fail(ExitOwned, FString::Format("{} owns {}: close it, or run with --ci", owner, context.GetMountName()),
				CJsonObject().AddString("owner", owner).AddString("mount", context.GetMountName()));
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
