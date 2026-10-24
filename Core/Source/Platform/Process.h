#pragma once

#include "Core.h"
#include "CoreTypes.h"


/**
 * The running process, as the OS knows it (design §8.15: an OS facility is a frt::platform function
 * with a Windows implementation in Platform/Windows/).
 *
 * The project writer lock names its holder by these (Assets/OwnerLock.h): a PID alone may be reused
 * by the time someone reads it, the PID with the process's start time is one process for good.
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
}
