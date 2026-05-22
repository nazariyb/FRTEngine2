#include "Profiler/MetricsAggregator.h"

#include <cstring>

#include "Profiler/GpuProfiler.h"


namespace frt::profiler
{
namespace
{
	double SafeRatio (uint64 Num, uint64 Den)
	{
		return Den > 0u ? static_cast<double>(Num) / static_cast<double>(Den) : 0.0;
	}

	void DeriveRatios (SFrameMetrics& M, uint32 W, uint32 H)
	{
		const uint64 attempted = M.Counters.Values[RC_SkyShadowAttempted];
		const uint64 rejected = M.Counters.Values[RC_SkyShadowPortalRej];
		const uint64 reached = M.Counters.Values[RC_SkyShadowReachedSky];
		const uint64 passed = M.Counters.Values[RC_SkyShadowPortalPass];
		const double pixels = static_cast<double>(W) * static_cast<double>(H);

		M.PctFiltered = SafeRatio(rejected, attempted);
		M.SkyEfficiency = SafeRatio(reached, attempted);
		M.RaysPerPixel = pixels > 0.0 ? static_cast<double>(attempted) / pixels : 0.0;
		M.TlasDispatches = static_cast<double>(passed);
		M.MeanPortalTests = SafeRatio(M.Counters.Values[RC_PortalTests], attempted);
	}
}


void CMetricsAggregator::SetWindow (uint32 N)
{
	WindowSize = N == 0u ? 1u : N;
	Reset();
}

void CMetricsAggregator::Reset ()
{
	Ring.Clear();
	Head = 0u;
	Count = 0u;
	LatestFrame = SFrameMetrics{};
	AveragedFrame = SFrameMetrics{};
}

void CMetricsAggregator::Sample (
	const SRayCounters& Counters,
	const TArray<SGpuScopeResult>& GpuResults,
	double FrameMs,
	uint32 RenderWidth,
	uint32 RenderHeight)
{
	SFrameMetrics m;
	m.Counters = Counters;
	m.FrameMs = FrameMs;

	for (uint32 i = 0; i < GpuResults.Count(); ++i)
	{
		const SGpuScopeResult& r = GpuResults[i];
		m.GpuTotalMs += r.DurationMs;
		if (r.Name && std::strcmp(r.Name, "RT_Dispatch") == 0)
		{
			m.RtDispatchMs = r.DurationMs;
		}
	}

	DeriveRatios(m, RenderWidth, RenderHeight);
	LatestFrame = m;

	// Grow ring lazily to WindowSize.
	if (Ring.Count() < WindowSize)
	{
		Ring.Add(m);
	}
	else
	{
		Ring[Head] = m;
	}
	Head = (Head + 1u) % WindowSize;
	if (Count < WindowSize)
	{
		++Count;
	}

	Recompute();
}

void CMetricsAggregator::Recompute ()
{
	if (Count == 0u)
	{
		AveragedFrame = SFrameMetrics{};
		return;
	}

	SFrameMetrics avg;
	double rtMs = 0.0, gpuMs = 0.0, frameMs = 0.0;
	double accum[RC_Count] = {};

	for (uint32 i = 0; i < Count; ++i)
	{
		const SFrameMetrics& s = Ring[i];
		rtMs += s.RtDispatchMs;
		gpuMs += s.GpuTotalMs;
		frameMs += s.FrameMs;
		for (uint32 c = 0; c < RC_Count; ++c)
		{
			accum[c] += static_cast<double>(s.Counters.Values[c]);
		}
	}

	const double inv = 1.0 / static_cast<double>(Count);
	avg.RtDispatchMs = rtMs * inv;
	avg.GpuTotalMs = gpuMs * inv;
	avg.FrameMs = frameMs * inv;
	for (uint32 c = 0; c < RC_Count; ++c)
	{
		avg.Counters.Values[c] = static_cast<uint64>(accum[c] * inv + 0.5);
	}

	// Re-derive ratios from the averaged raw counters (more stable than averaging ratios).
	// W*H recoverable from averaged attempted / per-frame rays-per-pixel is circular, so
	// instead derive ratios that don't need W/H, and copy RaysPerPixel from the latest frame
	// (resolution is fixed within a session).
	const uint64 attempted = avg.Counters.Values[RC_SkyShadowAttempted];
	avg.PctFiltered = SafeRatio(avg.Counters.Values[RC_SkyShadowPortalRej], attempted);
	avg.SkyEfficiency = SafeRatio(avg.Counters.Values[RC_SkyShadowReachedSky], attempted);
	avg.TlasDispatches = static_cast<double>(avg.Counters.Values[RC_SkyShadowPortalPass]);
	avg.MeanPortalTests = SafeRatio(avg.Counters.Values[RC_PortalTests], attempted);
	avg.RaysPerPixel = LatestFrame.RaysPerPixel > 0.0 && LatestFrame.Counters.Values[RC_SkyShadowAttempted] > 0u
							? (static_cast<double>(attempted)
								* LatestFrame.RaysPerPixel
								/ static_cast<double>(LatestFrame.Counters.Values[RC_SkyShadowAttempted]))
							: LatestFrame.RaysPerPixel;

	AveragedFrame = avg;
}
}
