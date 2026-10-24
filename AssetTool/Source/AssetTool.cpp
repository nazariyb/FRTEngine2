/**
 * AssetTool: the asset system's command line (design §13.1, §13.2), a console host beside the editor
 * and the launcher, which FRT.bat assets <verb> runs. One run is one commandlet:
 *
 *     AssetTool <verb> [arguments] [options]       AssetTool -Run=<Verb> [arguments] [options]
 *
 * It finds the project like the other hosts (-Project=<file>, else the working directory's;
 * project::FindProjectToRun), or with --engine acts on /Engine as its owner with no project (§4.7).
 * It loads the config layers and makes a memory pool primary as the other hosts do, and opens no
 * window, renderer or game module: the game module joins with the game's types and importers (M5),
 * through SEngineDesc::GameModule. Release builds no AssetTool (it cannot load a game module).
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
#include "Config/Config.h"
#include "Memory/Memory.h"
#include "Memory/MemoryPool.h"
#include "Platform/FileLock.h"
#include "Platform/Platform.h"
#include "Platform/Process.h"
#include "Project/Project.h"

#include "Commandlet.h"


namespace frt::assettool
{
namespace
{
using namespace memory::literals;

// The primary pool, as CEngine sizes it: reserved and committed whole, so pages cost nothing until
// used. Nothing in AssetTool allocates through it yet; game types (M5) will.
constexpr uint64 MemoryPoolBytes = 2_Gb;

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

// The project the run acts on, into Context and paths::SetProject. ExitSuccess, or the exit code to
// end with (the failure already reported). A verb that writes needs a mount: a project or --engine.
int32 FindProject (CCommandletContext& Context, const SCommandlet& Commandlet)
{
	if (Context.bEngineOwner)
	{
		if (platform::FindCommandLineValue(Context.CommandLine, "Project").has_value())
		{
			return Context.Fail(ExitUsage, "--engine acts on /Engine without a project: give -Project= or --engine, not both");
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
				"-Project=<file>, or with --engine for the engine's own content", Commandlet.Name));
		}
		return ExitSuccess;
	}

	project::SProjectDescriptor descriptor;
	if (!project::SProjectDescriptor::Load(file, descriptor, &error))
	{
		return Context.Fail(ExitUsage, error);
	}
	const std::filesystem::path engineDir = project::ResolveEngineDir(descriptor, &error);
	if (engineDir.empty())
	{
		return Context.Fail(ExitUsage, descriptor.Name + ": " + error);
	}
	std::error_code ec;
	if (!std::filesystem::equivalent(engineDir, Context.EngineRoot, ec))
	{
		// Its metas, keys and DDC values are that engine's to write.
		return Context.Fail(ExitUsage, FString::Format("{} uses the engine in {}: run that engine's AssetTool (the project's "
			"FRT.bat assets does)", descriptor.Name, FString::FromPath(engineDir)));
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
	context.Arguments = std::move(verb.Arguments);

	// The project, before the config: its Game.ini is a layer of it.
	context.EngineRoot = paths::GetEngineDir();
	if (const int32 result = FindProject(context, *commandlet); result != ExitSuccess)
	{
		return result;
	}

	// Config, then memory, in CEngine::StartUp's order: layers weakest first, then -Section:Key=Value.
	config::CConfig config;
	config.LoadDefaultLayers();
	config.ApplyCommandLineArguments(context.CommandLine);
	context.SetConfig(config);
	context.bReadOnly = context.bCi || config.Get("Assets", "ReadOnly", false);

	memory::CMemoryPool memoryPool(MemoryPoolBytes);
	memoryPool.MakeThisPrimaryInstance();

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

	return commandlet->Run(context);
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
