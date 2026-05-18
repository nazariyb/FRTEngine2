#pragma once

#include "Core.h"
#include "CoreTypes.h"
#include "Containers/Array.h"
#include "Profiler/RayCounters.h"


namespace frt::profiler
{
struct SGpuScopeResult;


// One frame's worth of metrics — raw counters + timing + derived ratios.
struct SFrameMetrics
{
	SRayCounters Counters;

	double RtDispatchMs = 0.0; // "RT_Dispatch" scope from CGpuProfiler
	double GpuTotalMs = 0.0;   // sum of all GPU scopes
	double FrameMs = 0.0;      // CPU frame delta

	// Derived (from this frame's counters).
	double PctFiltered = 0.0;    // SkyShadowPortalRej / SkyShadowAttempted
	double SkyEfficiency = 0.0;  // SkyShadowReachedSky / SkyShadowAttempted
	double RaysPerPixel = 0.0;   // SkyShadowAttempted / (W*H)
	double TlasDispatches = 0.0; // SkyShadowPortalPass (sky NEE TLAS queries)
};


// Per-frame sampler + rolling window. Latest() = newest frame. Average() = mean over the
// last N sampled frames (raw fields averaged, ratios re-derived from the averaged raw).
class FRT_CORE_API CMetricsAggregator
{
public:
	void SetWindow (uint32 N);
	uint32 GetWindow () const { return WindowSize; }

	void Sample (
		const SRayCounters& Counters,
		const TArray<SGpuScopeResult>& GpuResults,
		double FrameMs,
		uint32 RenderWidth,
		uint32 RenderHeight);

	bool HasData () const { return Count > 0u; }
	const SFrameMetrics& Latest () const { return LatestFrame; }
	const SFrameMetrics& Average () const { return AveragedFrame; }
	uint32 SampleCount () const { return Count; } // frames currently in the window

	void Reset ();

private:
	void Recompute ();

	TArray<SFrameMetrics> Ring;
	uint32 WindowSize = 500u;
	uint32 Head = 0u;  // next write slot
	uint32 Count = 0u; // valid entries (<= WindowSize)

	SFrameMetrics LatestFrame;
	SFrameMetrics AveragedFrame;
};
}
