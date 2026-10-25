#include "Commandlet.h"

#include <cstdio>
#include <iterator>

#include "Assets/AssetId.h"
#include "Assets/OwnerLock.h"
#include "Platform/Process.h"


namespace frt::assettool
{
namespace
{
// More than anyone pastes into code at once; a typo of 1000000 does not flood the console.
constexpr uint32 MaxNewGuids = 10'000u;

int32 RunHelp (CCommandletContext& Context);
int32 RunNewGuid (CCommandletContext& Context);
int32 RunLock (CCommandletContext& Context);

constexpr FStringLiteral DdcOptions[] = { "max-gb", "ddc" };

/**
 * Every verb, in the order help lists them. A new verb is a function and a line here (§13.1: no
 * registration framework); a verb with a file of its own declares its Run function in Commandlet.h.
 */
constexpr SCommandlet Commandlets[] =
{
	{ "help", &RunHelp, "help [verb]: lists the verbs, or says what one does", false },
	{ "newguid", &RunNewGuid, "newguid [n]: n fresh GUIDs (default 1) for builder versions and built-in assets, each as meta "
		"text and as a C++ initializer", false },
	{ "lock", &RunLock, "lock: takes the writer lock of the project (with --engine, of /Engine) and holds it until Enter or "
		"the end of input, so no other process that writes the mount can run meanwhile", true },
	// The DDC is shared by every process on the machine and safe without locks (§7.4): no writer lock.
	{ "ddc", &RunDdc, "ddc stats|gc [--max-gb=<n>]|verify [--ddc=<dir>]: the derived-data cache (default: FRT_DDC_DIR, "
		"[DerivedDataCache] Path, the user's own): stats counts its files and bytes; gc trims its content to n GiB "
		"([DerivedDataCache] MaxGB, default 20), least recently used first, keeping what the mount's BuildState names - "
		"with --ci a dry run that deletes nothing; verify re-checks every file and deletes the corrupt ones", false,
		DdcOptions },
};

// What help says beyond the verbs: the options every verb takes (FindUnknownOption knows the same
// ones), and the exit codes. FRT.bat assets and AssetTool itself take every spelling of them.
constexpr FStringLiteral HelpHeader =
	"Usage: FRT.bat assets <verb> [arguments] [options], or AssetTool <verb> ... / AssetTool -Run=<verb> ...";
constexpr FStringLiteral HelpOptions[] =
{
	"  --project=<file>    the project to act on, its .frtproject file or directory (default: the working directory's; "
		"-Project=<file> too)",
	"  --engine            act on /Engine as its owner, with no project",
	"  --help              with a verb: what it does, as help <verb>",
	"  --json              one JSON object per line on stdout instead of text",
	"  --ci                read-only: writes nothing the mount's owner writes, never takes the writer lock",
	"  -Assets:ReadOnly=1  read-only, as with --ci",
	"  -Section:Key=Value  a config override, as for every host",
	"Exit codes: 0 success, 1 problems found, 2 usage error, 3 internal error or crash, 4 the mount is owned by "
		"another process",
};

void PrintUsage (const CCommandletContext& Context, const SCommandlet& Commandlet)
{
	Context.Print(FString::Format("  {}", Commandlet.Usage));
	Context.PrintJson(CJsonObject().AddString("verb", Commandlet.Name).AddString("usage", Commandlet.Usage)
		.AddBool("writes", Commandlet.bWritesProject));
}

// help [verb]
int32 RunHelp (CCommandletContext& Context)
{
	if (Context.Arguments.size() > 1u)
	{
		return Context.Fail(ExitUsage, "help: takes at most one argument, a verb (help [verb])");
	}
	if (!Context.Arguments.empty())
	{
		const SCommandlet* commandlet = FindCommandlet(Context.Arguments[0]);
		if (commandlet == nullptr)
		{
			return Context.Fail(ExitUsage, FString::Format("help: no verb '{}' (FRT.bat assets help lists them)", Context.Arguments[0]));
		}
		PrintUsage(Context, *commandlet);
		return ExitSuccess;
	}

	Context.Print(HelpHeader);
	Context.Print("Verbs:");
	for (const SCommandlet& commandlet : GetCommandlets())
	{
		PrintUsage(Context, commandlet);
	}
	Context.Print("Options:");
	for (const FStringLiteral line : HelpOptions)
	{
		Context.Print(line);
	}
	return ExitSuccess;
}

// SGuid's bytes as the aggregate initializer C++ code declares a constant with:
// "SGuid { { 0x9f, 0xc0, ... } }", in byte order, so it equals the meta text's GUID.
FString FormatGuidInitializer (const assets::SGuid& Guid)
{
	FString text("SGuid { { ");
	for (uint32 i = 0u; i < assets::SGuid::Size; ++i)
	{
		text.AppendFormat("{}0x{:02x}", i == 0u ? "" : ", ", Guid.Bytes[i]);
	}
	text.Append(" } }");
	return text;
}

// newguid [n]: version 4 GUIDs from the OS generator (SGuid::NewRandom, never all-digit text, so no
// YAML reader types one as a number), for the version GUIDs of builders (§7.3) and the metas of
// built-in assets. Nothing is written anywhere.
int32 RunNewGuid (CCommandletContext& Context)
{
	if (Context.Arguments.size() > 1u)
	{
		return Context.Fail(ExitUsage, "newguid: takes at most one argument, the number of GUIDs (newguid [n])");
	}
	uint32 count = 1u;
	if (!Context.Arguments.empty())
	{
		const std::optional<uint32> parsed = Context.Arguments[0].View().ToUInt32();
		if (!parsed.has_value() || *parsed == 0u || *parsed > MaxNewGuids)
		{
			return Context.Fail(ExitUsage, FString::Format("newguid: '{}' is not a number from 1 to {}", Context.Arguments[0], MaxNewGuids));
		}
		count = *parsed;
	}

	for (uint32 i = 0u; i < count; ++i)
	{
		const assets::SGuid guid = assets::SGuid::NewRandom();
		const FString text = guid.ToString();
		const FString initializer = FormatGuidInitializer(guid);
		Context.Print(FString::Format("{}  {}", text, initializer));
		Context.PrintJson(CJsonObject().AddString("guid", text).AddString("cpp", initializer));
	}
	return ExitSuccess;
}

// lock: the host took the writer lock before running it (bWritesProject); this holds it until the
// standard input ends - Enter in a console, the end of a pipe or a closed one - or the process is
// killed, which lets go of it as well (platform::TryLockFileExclusive). For editing a mount by hand
// while no tool may write it, and for testing tools against an owned project.
int32 RunLock (CCommandletContext& Context)
{
	if (!Context.Arguments.empty())
	{
		return Context.Fail(ExitUsage, "lock: takes no arguments");
	}
	if (Context.bReadOnly)
	{
		return Context.Fail(ExitUsage, "lock: a read-only run (--ci, -Assets:ReadOnly=1) takes no lock");
	}

	const FStringView mount = Context.GetMountName();
	const std::filesystem::path lockFile = assets::GetOwnerLockPath(Context.GetMountRoot());
	Context.Print(FString::Format("{} is locked by this process (pid {}) until Enter or the end of input: {}", mount,
		platform::GetCurrentProcessId(), FString::FromPath(lockFile)));
	Context.PrintJson(CJsonObject().AddString("locked", mount).AddString("lockFile", FString::FromPath(lockFile))
		.AddUInt("pid", platform::GetCurrentProcessId()));

	for (int c = std::fgetc(stdin); c != EOF && c != '\n'; c = std::fgetc(stdin))
	{
	}
	Context.Print(FString::Format("{} is unlocked", mount));
	Context.PrintJson(CJsonObject().AddString("unlocked", mount));
	return ExitSuccess;
}
}


std::span<const SCommandlet> GetCommandlets ()
{
	return std::span<const SCommandlet>(Commandlets, std::size(Commandlets));
}

const SCommandlet* FindCommandlet (FStringView Name)
{
	for (const SCommandlet& commandlet : Commandlets)
	{
		if (Name.Equals<ESearchCase::IgnoreCaseAscii>(commandlet.Name))
		{
			return &commandlet;
		}
	}
	return nullptr;
}
}
