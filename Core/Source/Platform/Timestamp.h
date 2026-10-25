#pragma once

#include "Core.h"
#include "CoreTypes.h"


/**
 * A monotonic, high-resolution tick counter for measuring short intervals: CPU profiling scopes
 * (Profiler/CpuProfiler.h) take two reads per scope, so it must be cheap and must never go back.
 *
 * Windows: QueryPerformanceCounter. On every machine Windows 10 supports it reads the invariant
 * TSC, scaled to a fixed frequency (10 MHz on current versions), consistent across cores, in
 * well under 100 ns ("Acquiring high-resolution time stamps", Microsoft Learn). std::chrono's
 * steady_clock is the same counter converted to nanoseconds on every read; raw ticks convert once,
 * when results are shown.
 */
namespace frt::platform
{
// Ticks since an arbitrary point fixed at boot. Monotonic; comparable across threads.
[[nodiscard]] FRT_CORE_API uint64 ReadTimestamp () noexcept;
// Ticks per second. Fixed for the life of the process.
[[nodiscard]] FRT_CORE_API uint64 GetTimestampFrequency () noexcept;

// A tick count in milliseconds or seconds.
[[nodiscard]] inline double TimestampToMilliseconds (uint64 Ticks) noexcept
{
	return static_cast<double>(Ticks) * 1000.0 / static_cast<double>(GetTimestampFrequency());
}

[[nodiscard]] inline double TimestampToSeconds (uint64 Ticks) noexcept
{
	return static_cast<double>(Ticks) / static_cast<double>(GetTimestampFrequency());
}

// The other way: a duration in milliseconds as ticks.
[[nodiscard]] inline uint64 MillisecondsToTimestamp (uint64 Milliseconds) noexcept
{
	return GetTimestampFrequency() * Milliseconds / 1000u;
}

// ReadTimestamp() SecondsFromNow from now (now itself for a negative count): a deadline to compare
// later reads with.
[[nodiscard]] inline uint64 MakeDeadline (double SecondsFromNow) noexcept
{
	const double ticks = SecondsFromNow * static_cast<double>(GetTimestampFrequency());
	return ReadTimestamp() + (ticks > 0.0 ? static_cast<uint64>(ticks) : 0u);
}
}
