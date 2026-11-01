#pragma once

#include "Core.h"
#include "CoreTypes.h"
#include "Containers/String.h"


/**
 * Threads, as the OS names and schedules them (AssetManager-Design §8.15: an OS facility is a
 * frt::platform function with a Windows implementation in Platform/Windows/, so nothing above it
 * includes <windows.h>).
 *
 * The engine owns its threads (jobs::CWorkerPool, later the I/O backend's thread) instead of
 * borrowing an OS thread pool, so they are the same code on every platform; what differs per
 * platform is only how a thread gets its name, priority and processors, which is ConfigureThread.
 */
namespace frt::platform
{
// The calling thread's OS id: what debuggers, PIX and Task Manager show (Windows: GetCurrentThreadId).
// Never 0. A std::thread::id compares just as well, but has no number to print in a log line.
[[nodiscard]] FRT_CORE_API uint64 GetCurrentThreadId () noexcept;

// Scheduling priority relative to the process's other threads. Normal is what every thread starts
// with; the others are one or two steps either side of it (Windows THREAD_PRIORITY_LOWEST ...
// THREAD_PRIORITY_HIGHEST; POSIX nice values, macOS QoS classes later).
enum class EThreadPriority : uint8
{
	Lowest,
	BelowNormal,
	Normal,
	AboveNormal,
	Highest,
};

/**
 * Names the calling thread, sets its priority and, when AffinityMask is not 0, the processors it
 * prefers. Called by a thread on itself, first thing, so its name is in place before it does
 * anything a profiler or debugger could show.
 *
 * Name is what debuggers, PIX, crash dumps and Task Manager show ("FRT Worker 3"; Windows
 * SetThreadDescription, any text). AffinityMask selects logical processors by index, bit i for
 * the i-th one in the order the OS lists them (Windows: CPU sets, SetThreadSelectedCpuSets - a
 * preference the scheduler honours unless it has to break it, never a hard pin that could starve
 * the thread; the first 64 processors only). 0 leaves the thread on every processor.
 *
 * Returns false when any part did not apply (the rest still does): a mask that selects no
 * processor this machine has, or an OS without the call. Never fatal: a thread without its name
 * or priority still runs correctly.
 */
FRT_CORE_API bool ConfigureThread (FStringView Name, EThreadPriority Priority, uint64 AffinityMask = 0u) noexcept;

// The name ConfigureThread gave the calling thread; empty if it has none (Windows: GetThreadDescription).
[[nodiscard]] FRT_CORE_API FString GetCurrentThreadName ();
}
