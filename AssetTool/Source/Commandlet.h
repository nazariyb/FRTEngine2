#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <vector>

#include "CoreTypes.h"
#include "Containers/String.h"

#include "JsonWriter.h"


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
	std::vector<FString> Arguments;
	// Every argument of the command line, as given.
	std::vector<FString> CommandLine;

	bool bJson = false;          // --json: one object per line on stdout instead of text
	bool bCi = false;            // --ci: read-only, never prompts
	// --ci or -Assets:ReadOnly=1: writes nothing a mount owns (no metas, no Content/), never takes the
	// writer lock, and so runs beside an open editor (§13.1, O3DE RFC-77).
	bool bReadOnly = false;
	bool bEngineOwner = false;   // --engine: acts on /Engine as its owner, with no project (§4.7)

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

	// The value of the last -Name= / --Name= option, IgnoreCaseAscii; nullopt without one.
	[[nodiscard]] std::optional<FString> FindOption (FStringView Name) const;
	// -Name / --Name is on the command line.
	[[nodiscard]] bool HasOption (FStringView Name) const;

	// A line of human text on stdout; nothing with --json.
	void Print (FStringView Line) const;
	// With --json, Object as one line on stdout; nothing without.
	void PrintJson (const CJsonObject& Object) const;
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
 */
struct SCommandlet
{
	FStringLiteral Name;
	int32 (*Run) (CCommandletContext& Context) = nullptr;
	FStringLiteral Usage;
	bool bWritesProject = false;
	std::span<const FStringLiteral> Options;
};

// Verbs with a file of their own (their table rows, options included, are in Commandlets.cpp).
// DdcCommandlets.cpp: ddc stats|gc|verify.
int32 RunDdc (CCommandletContext& Context);

// The table, in the order help lists it.
[[nodiscard]] std::span<const SCommandlet> GetCommandlets ();
// The commandlet named Name (IgnoreCaseAscii), or nullptr.
[[nodiscard]] const SCommandlet* FindCommandlet (FStringView Name);

/**
 * The first argument of CommandLine that is an option (it starts with '-') and that neither the host
 * nor Commandlet reads; nullopt when there is none. The host reads, for every verb:
 *   -Run=<Verb>, -Project=<file or directory>             with a value
 *   --json, --ci, --engine, --help, -?                    without one
 *   -Section:Key=Value                                    a config override (-Assets:ReadOnly=1)
 * each with one dash or two, the name in any case (platform::FindCommandLineValue's rules). A host
 * option in the wrong form (--json=1, -Project) is unknown too: it would be ignored otherwise. With no
 * Commandlet (no verb yet), only the host's options are known.
 */
[[nodiscard]] std::optional<FString> FindUnknownOption (std::span<const FString> CommandLine, const SCommandlet* Commandlet);
}
