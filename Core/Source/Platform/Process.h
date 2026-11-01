#pragma once

#include <filesystem>
#include <vector>

#include "Core.h"
#include "CoreTypes.h"
#include "Containers/String.h"


/**
 * The running process, as the OS knows it (design §8.15: an OS facility is a frt::platform function
 * with a Windows implementation in Platform/Windows/).
 *
 * The project writer lock names its holder by these (Assets/OwnerLock.h): a PID alone may be reused
 * by the time someone reads it, the PID with the process's start time is one process for good.
 *
 * Also a console tool run to its end (RunToolProcess), which is all the VCS providers need. It blocks, so
 * it belongs on a worker, never on a frame. The long-lived SpawnToolProcess and IToolPipe of the design
 * wait for their first client, the Blender bridge (M9c).
 */
namespace frt::platform
{
// This process's OS id (Windows: GetCurrentProcessId). Never 0.
[[nodiscard]] FRT_CORE_API uint32 GetCurrentProcessId () noexcept;

// When this process started, as a file time: 100 ns ticks since 1601-01-01 UTC, the unit of
// SFileStat's times (Platform/FileSystem.h). Fixed for the life of the process. Windows: the
// creation time GetProcessTimes reports.
[[nodiscard]] FRT_CORE_API int64 GetProcessStartTime () noexcept;

/**
 * For console tools that scripts run (AssetTool): the process never stops at a dialog or a crash
 * report, and a crash ends it with ExitCode, which a script can tell apart from a result.
 *
 * After this call, each of these writes one line saying what happened to stderr and ends the process
 * with ExitCode: an unhandled structured exception (an access violation, a stack overflow, a failed
 * frt_assert's breakpoint), abort() (std::terminate: an exception that left a thread), and a CRT
 * function given an invalid parameter (which would otherwise end the process with a fast fail).
 * The debug CRT's assertion and error reports go to stderr instead of a dialog. Under a debugger
 * nothing changes for exceptions: the debugger sees them first, as before. Call it first thing in
 * main; it is per process, and the last call's ExitCode counts.
 *
 * Windows: SetUnhandledExceptionFilter, SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX),
 * signal(SIGABRT) with _set_abort_behavior, _set_invalid_parameter_handler and, in Debug builds,
 * _CrtSetReportMode / _CrtSetReportFile.
 */
FRT_CORE_API void ExitOnCrash (int32 ExitCode) noexcept;

struct SToolProcessDesc
{
	std::filesystem::path Executable;   // absolute: FindOnPath, never a bare name
	std::vector<FString> Arguments;
	std::filesystem::path WorkingDir;
	uint32 TimeoutMs = 30000u;
};

struct SToolProcessResult
{
	bool bStarted = false;
	bool bTimedOut = false;
	int32 ExitCode = -1;
	std::vector<uint8> Stdout;
	FString Stderr;
};

/**
 * Runs Executable to its end, or until TimeoutMs, when it is killed. No shell and no window. Both streams are drained
 * while it runs, so a chatty tool cannot block on a full pipe (`git status` easily exceeds a pipe's buffer). It runs in
 * a job that kills it when this process goes, so a killed host does not leave a git behind holding index.lock. Worker-safe.
 */
[[nodiscard]] FRT_CORE_API SToolProcessResult RunToolProcess (const SToolProcessDesc& Desc);

/**
 * The executable Name resolves to on PATH alone, or empty. Never the working directory: CreateProcess's own search
 * would look there first, so a git.exe in a project folder would run.
 */
[[nodiscard]] FRT_CORE_API std::filesystem::path FindOnPath (FStringView Name);
}
