#pragma once

#include "Core.h"
#include "CoreTypes.h"


/**
 * Threads, as the OS names them (AssetManager-Design §8.15: an OS facility is a frt::platform
 * function with a Windows implementation in Platform/Windows/, so nothing above it includes
 * <windows.h>). M1 adds ConfigureThread (name, priority, affinity) for the worker pool here.
 */
namespace frt::platform
{
// The calling thread's OS id: what debuggers, PIX and Task Manager show (Windows: GetCurrentThreadId).
// Never 0. A std::thread::id compares just as well, but has no number to print in a log line.
[[nodiscard]] FRT_CORE_API uint64 GetCurrentThreadId () noexcept;
}
