#pragma once

#include <vector>

#include "Core.h"
#include "CoreTypes.h"
#include "Profiler/MetricsAggregator.h"


namespace frt::profiler
{
// One parameter combination under test.
struct SProfileConfig
{
	uint32 SampleCount = 1u;
	uint32 MaxBounces = 4u;
	uint32 RussianRouletteDepth = 2u;
	bool bPortalPreFilter = false;
};


struct SProfileResult
{
	SProfileConfig Config;
	SFrameMetrics Avg;                 // window average (console log convenience)
	std::vector<SFrameMetrics> Frames; // every measured frame — full CSV history
};


// Batch sweep driver. Cartesian product of hardcoded axes × portal{off,on}.
// Per profile: Warmup (discard transient frames) -> Measure (fill the Metrics window)
// -> record averaged row -> advance. Caller owns RtSettings/Metrics; this only sequences.
class FRT_CORE_API CProfilingSession
{
public:
	bool IsActive () const { return State != EState::Idle; }

	void Start (); // build queue, begin at profile 0
	void Stop ();  // abort, go Idle

	// Drive the state machine. Call once per frame AFTER Metrics.Sample().
	void Update (CMetricsAggregator& Metrics);

	bool HasCurrent () const { return IsActive() && Index < Queue.size(); }
	const SProfileConfig& Current () const { return Queue[Index]; }

	// Set true on the frame a new profile begins; caller resets accumulation then clears it.
	bool ConsumeProfileChanged ();
	// Set true on the frame the whole session finishes; caller restores saved settings.
	bool ConsumeFinished ();
	// Returns the index of the profile just measured (one-shot), or -1. Caller writes that
	// profile's file before the next configuration starts.
	int32 ConsumeCompletedProfile ();

	uint32 ProfileIndex () const { return Index; }
	uint32 ProfileCount () const { return static_cast<uint32>(Queue.size()); }
	const char* PhaseName () const;
	uint32 WarmupRemaining () const { return WarmupLeft; }

	const std::vector<SProfileResult>& History () const { return Results; }

	static constexpr uint32 WarmupFrames = 60u;

private:
	enum class EState { Idle, Warmup, Measure };


	void RecordAndAdvance (CMetricsAggregator& Metrics);

	EState State = EState::Idle;
	std::vector<SProfileConfig> Queue;
	std::vector<SProfileResult> Results;
	uint32 Index = 0u;
	uint32 WarmupLeft = 0u;
	bool bProfileChanged = false;
	bool bFinished = false;
	int32 CompletedIndex = -1; // profile index just recorded, consumed once
};
}
