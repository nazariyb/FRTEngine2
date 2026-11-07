#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <vector>

#include "CoreTypes.h"
#include "Containers/String.h"

#include "Text/JsonWriter.h"


namespace frt::config
{
class CConfig;
}

namespace frt::jobs
{
class CWorkerPool;
}


namespace frt::assettool
{
// AssetTool's --json lines are Core's JSON objects (Text/JsonWriter.h).
using json::AppendJsonString;
using json::CJsonObject;

/**
 * AssetTool's exit codes (design §13.1): what scripts, Premake Lua and hooks branch on, so they never
 * change meaning. FRT.bat assets forwards them as its own.
 */
inline constexpr int32 ExitSuccess = 0;
inline constexpr int32 ExitProblems = 1;        // the verb ran and found problems (validation errors, failed builds)
inline constexpr int32 ExitUsage = 2;           // a usage error: unknown verb, bad arguments, no project
inline constexpr int32 ExitInternalError = 3;   // an internal error or a crash (platform::ExitOnCrash)
inline constexpr int32 ExitOwned = 4;           // a run that writes, while another process owns the mount


/**
 * What a commandlet runs with: its arguments, how to print, and what it acts on. The host fills it
 * in (AssetTool.cpp) after finding the project, loading the config and taking the writer lock when
 * the commandlet writes, so a commandlet starts with all of that settled.
 *
 * Output: every verb prints human text and accepts --json (§13.1). A verb calls both Print and
 * PrintJson for what it reports; the context keeps the one the run asked for. Errors go through
 * Fail, which prints to stderr in either mode and, with --json, also as an object on stdout, so a
 * script reading stdout sees why the stream ended.
 */
class CCommandletContext
{
public:
	// The positional arguments after the verb: "newguid 3" -> { "3" }. Options (-Name, --Name,
	// -Name=Value, -Section:Key=Value) are not among them: FindOption and HasOption read those, and
	// the host has refused every option that neither it nor the commandlet reads (FindUnknownOption).
	// Every argument after "--" is one, and so is a query's negated term (GetPositionalArguments).
	std::vector<FString> Arguments;
	// Every argument of the command line, as given.
	std::vector<FString> CommandLine;

	bool bJson = false;          // --json: one object per line on stdout instead of text
	bool bCi = false;            // --ci: read-only, never prompts
	// --ci or -Assets:ReadOnly=1: writes nothing a mount owns (no metas, no Content/), never takes the
	// writer lock, and so runs beside an open editor (§13.1, O3DE RFC-77).
	bool bReadOnly = false;
	bool bEngineOwner = false;   // --engine: acts on /Engine as its owner, with no project (§4.7)
	// Another process owns the mount and Safety.GuestProcesses let this run go on as a read-only guest (§5.9):
	// bReadOnly is set as well, so a verb writes nothing; it may say so.
	bool bGuest = false;

	// The project's directory; empty with --engine, or when a verb that needs none runs without one.
	std::filesystem::path ProjectRoot;
	// The engine checkout this AssetTool belongs to.
	std::filesystem::path EngineRoot;

	// The mount the run acts on, as messages name it: "/Project" or "/Engine"; empty without either.
	[[nodiscard]] FStringView GetMountName () const;
	// The root of that mount: ProjectRoot, or EngineRoot with --engine; empty without either.
	[[nodiscard]] const std::filesystem::path& GetMountRoot () const;

	// The config layers (engine, project, user, command line), as the other hosts load them.
	[[nodiscard]] const config::CConfig& GetConfig () const { return *Config; }
	void SetConfig (const config::CConfig& InConfig) { Config = &InConfig; }

	// The host's worker pool, made after the memory pool as CEngine makes the engine's: a verb runs its
	// heavy work there and waits on this (the owner) thread with PumpUntil (Jobs/WorkerPool.h), which
	// keeps the thread free to report progress and is how every later verb builds and verifies.
	[[nodiscard]] jobs::CWorkerPool& GetWorkerPool () const { return *WorkerPool; }
	void SetWorkerPool (jobs::CWorkerPool& Pool) { WorkerPool = &Pool; }

	// The value of the last -Name= / --Name= option before any "--", IgnoreCaseAscii; nullopt without one.
	[[nodiscard]] std::optional<FString> FindOption (FStringView Name) const;
	// -Name / --Name is on the command line, before any "--".
	[[nodiscard]] bool HasOption (FStringView Name) const;

	// A line of human text on stdout; nothing with --json.
	void Print (FStringView Line) const;
	// With --json, Object as one line on stdout; nothing without.
	void PrintJson (const CJsonObject& Object) const;
	// With --json, a line that already is one JSON object (Rules.cache's lines, a registry dump's); nothing without.
	void PrintJsonLine (FStringView Line) const;
	// Text as it is, line breaks included, on stdout in one write; nothing with --json (a registry dump).
	void PrintText (FStringView Text) const;
	// A line on stderr, in either mode: warnings and progress that are not the verb's result.
	void PrintError (FStringView Line) const;
	// Reports a failure and returns ExitCode, for "return Context.Fail(ExitUsage, ...)": the message
	// on stderr, and with --json also {"error":Message,"exitCode":ExitCode} on stdout.
	int32 Fail (int32 ExitCode, FStringView Message) const;
	// The same with extra fields for the JSON object (the owner of a refused lock).
	int32 Fail (int32 ExitCode, FStringView Message, const CJsonObject& Extra) const;

private:
	const config::CConfig* Config = nullptr;
	jobs::CWorkerPool* WorkerPool = nullptr;
};


/**
 * A commandlet: a verb of FRT.bat assets, run headless (§13.1). A plain table, Editor style, with no
 * registration framework: GetCommandlets() in Commandlets.cpp lists every one, and a new verb is a
 * function and a line there.
 *
 *   Name            the verb, lowercase: "AssetTool <verb>", "AssetTool -Run=<Verb>" (any case), FRT.bat assets <verb>
 *   Run             returns an exit code (above)
 *   Usage           "<verb> <arguments>: what it does", one line for help
 *   bWritesProject  writes what the mount's owner alone may write (metas, snapshots, BuildState): the
 *                   host takes the mount's writer lock first, and exits with ExitOwned if another
 *                   process owns it - unless the run is read-only, in which case it runs without the
 *                   lock and the verb must write nothing of the kind (it sees bReadOnly). Verbs that
 *                   touch only shared, process-safe stores (the DDC, §7.4) leave it false. A verb
 *                   that leaves it false also runs where the working directory holds a project that
 *                   cannot be used (broken, or another engine's): with a warning, and no project.
 *   Options         the options of its own, by name without dashes ({ "fix", "max-gb" }): -Name,
 *                   --Name, -Name=Value and --Name=Value are accepted for each, and the verb reads
 *                   them with FindOption / HasOption and checks their values itself. Any other option
 *                   that the host does not read is a usage error, so a typo (--jsno) or a negative
 *                   number (newguid -3) never runs a verb in a mode nobody asked for. Empty: none.
 *   bTakesText      its arguments are text in a syntax where a term may start with '-' (query's
 *                   -label:Debug): an argument with one dash that is no option of the host or the verb
 *                   is one of its arguments, not an unknown option. Two dashes still make an option, so
 *                   a typo of one (--limt=3) is still refused.
 *   bGuestRuns      a bWritesProject verb with a read-only form worth running beside the owner: with the
 *                   project's Safety.GuestProcesses on it runs as a guest instead of exiting with
 *                   ExitOwned (§5.9). False for lock, whose point is the lock itself.
 */
struct SCommandlet
{
	FStringLiteral Name;
	int32 (*Run) (CCommandletContext& Context) = nullptr;
	FStringLiteral Usage;
	bool bWritesProject = false;
	std::span<const FStringLiteral> Options;
	bool bTakesText = false;
	bool bGuestRuns = true;
};

// Verbs with a file of their own (their table rows, options included, are in Commandlets.cpp).
// DdcCommandlets.cpp: ddc stats|gc|verify.
int32 RunDdc (CCommandletContext& Context);
// RegistryCommandlets.cpp: the asset registry's verbs (design §13.2, M3).
int32 RunScan (CCommandletContext& Context);
int32 RunStatus (CCommandletContext& Context);
int32 RunQuery (CCommandletContext& Context);
int32 RunRefs (CCommandletContext& Context);
int32 RunReferencers (CCommandletContext& Context);
int32 RunExplain (CCommandletContext& Context);
int32 RunListRules (CCommandletContext& Context);
int32 RunDump (CCommandletContext& Context);

// "123 B", "4.5 KiB", "6.7 MiB", "8.9 GiB" (DdcCommandlets.cpp).
[[nodiscard]] FString FormatBytes (uint64 Bytes);

// The machine's derived-data cache in one line, as `status` shows it (DdcCommandlets.cpp): the store the ddc
// verbs use by default (CDerivedDataCache::GetDefaultRoot), counted on the worker pool; nothing is created.
struct SDdcSummary
{
	FString Root;
	bool bExists = false;      // the root holds a store
	FString Error;             // it does, and could not be opened
	uint64 ContentBytes = 0u;  // manifests, records and values
	uint64 TotalBytes = 0u;    // everything under the root
	uint64 Files = 0u;
};
[[nodiscard]] SDdcSummary SummarizeDdc (CCommandletContext& Context);

// The table, in the order help lists it.
[[nodiscard]] std::span<const SCommandlet> GetCommandlets ();
// The commandlet named Name (IgnoreCaseAscii), or nullptr.
[[nodiscard]] const SCommandlet* FindCommandlet (FStringView Name);

/**
 * The parts of AssetTool's command line (design §13.1):
 *
 *     AssetTool <verb> [arguments] [options] [-- arguments]       AssetTool -Run=<Verb> ...
 *
 * An option starts with '-' (-Run=, -Project=, --json, -Section:Key=Value, a verb's own). "--" ends the
 * options: every argument after it is positional whatever it starts with, so a verb's text or name may
 * start with '-' (query -- -json). A '/' starts no option, so mount paths ("/Project/Content/Rock.png")
 * stay arguments. A bare *.frtproject argument names the project wherever it is (FindProjectToRun reads
 * it), never a verb or an argument. For a verb with bTakesText, an argument with one dash before "--"
 * that is no option of the host or the verb is one of its arguments (query -label:Debug).
 */
// The arguments that can be options: those before the first "--" (all of them without one).
[[nodiscard]] std::span<const FString> GetOptionArguments (std::span<const FString> CommandLine);
// The verb: -Run=<Verb>, else the first positional argument (an argument with a dash before "--" is
// never one). nullopt when there is none.
[[nodiscard]] std::optional<FString> FindVerb (std::span<const FString> CommandLine);
// Commandlet's positional arguments in order, the verb left out when FindVerb took it from them.
// Commandlet null: no verb's text is known (an argument with a dash before "--" is never one).
[[nodiscard]] std::vector<FString> GetPositionalArguments (std::span<const FString> CommandLine, const SCommandlet* Commandlet);

/**
 * The first argument before any "--" that is an option (it starts with '-') and that neither the host
 * nor Commandlet reads; nullopt when there is none. The host reads, for every verb:
 *   -Run=<Verb>, -Project=<file or directory>             with a value
 *   --json, --ci, --engine, --help, -?                    without one
 *   -Section:Key=Value                                    a config override (-Assets:ReadOnly=1); its
 *                                                         section and key hold no whitespace
 * each with one dash or two, the name in any case (platform::FindCommandLineValue's rules). A host
 * option in the wrong form (--json=1, -Project) is unknown too: it would be ignored otherwise. With no
 * Commandlet (no verb yet), only the host's options are known. A bTakesText verb's text with one dash
 * is never unknown (GetPositionalArguments).
 */
[[nodiscard]] std::optional<FString> FindUnknownOption (std::span<const FString> CommandLine, const SCommandlet* Commandlet);
}
