#include "Profiler/ProfilingSession.h"

#include <cstdio>
#include <utility>


namespace frt::profiler
{
// ---- Hardcoded sweep axes. Edit here; UI exposure comes later. ----
namespace
{
	constexpr uint32 kSpp[] = { 4u, 16u, 24u, 32u };
	constexpr uint32 kBounces[] = { 2u, 4u, 6u, 8u };
	constexpr uint32 kRrDepth[] = { 2u, 3u, 4u };
	constexpr bool kPortal[] = { false, true };
}


void CProfilingSession::Start ()
{
	Queue.clear();
	Results.clear();
	for (uint32 spp : kSpp)
	{
		for (uint32 b : kBounces)
		{
			for (uint32 rr : kRrDepth)
			{
				for (bool portal : kPortal)
				{
					SProfileConfig c;
					c.SampleCount = spp;
					c.MaxBounces = b;
					c.RussianRouletteDepth = rr;
					c.bPortalPreFilter = portal;
					Queue.push_back(c);
				}
			}
		}
	}

	if (Queue.empty())
	{
		State = EState::Idle;
		return;
	}

	Index = 0u;
	State = EState::Warmup;
	WarmupLeft = WarmupFrames;
	bProfileChanged = true;
	bFinished = false;

	std::printf(
		"[profiler] session start: %zu profiles, warmup=%u, window=(metrics)\n",
		Queue.size(), WarmupFrames);
	std::fflush(stdout);
}

void CProfilingSession::Stop ()
{
	State = EState::Idle;
	WarmupLeft = 0u;
}

const char* CProfilingSession::PhaseName () const
{
	switch (State)
	{
		case EState::Warmup: return "warmup";
		case EState::Measure: return "measure";
		default: return "idle";
	}
}

bool CProfilingSession::ConsumeProfileChanged ()
{
	const bool v = bProfileChanged;
	bProfileChanged = false;
	return v;
}

bool CProfilingSession::ConsumeFinished ()
{
	const bool v = bFinished;
	bFinished = false;
	return v;
}

int32 CProfilingSession::ConsumeCompletedProfile ()
{
	const int32 v = CompletedIndex;
	CompletedIndex = -1;
	return v;
}


void CProfilingSession::Update (CMetricsAggregator& Metrics)
{
	if (State == EState::Idle)
	{
		return;
	}

	if (State == EState::Warmup)
	{
		// Keep the window empty while transients (SBT/AS rebuild, RNG warm) settle.
		Metrics.Reset();
		if (WarmupLeft > 0u)
		{
			--WarmupLeft;
		}
		if (WarmupLeft == 0u)
		{
			State = EState::Measure;
		}
		return;
	}

	// Measure: Metrics.Sample() already ran this frame (caller order). Wait for the
	// rolling window to fill completely, then snapshot its average.
	if (Metrics.SampleCount() >= Metrics.GetWindow())
	{
		RecordAndAdvance(Metrics);
	}
}

void CProfilingSession::RecordAndAdvance (CMetricsAggregator& Metrics)
{
	SProfileResult r;
	r.Config = Queue[Index];
	r.Avg = Metrics.Average();
	const uint32 frameCount = Metrics.SampleCount();
	r.Frames.reserve(frameCount);
	for (uint32 i = 0; i < frameCount; ++i)
	{
		r.Frames.push_back(Metrics.Frame(i));
	}
	Results.push_back(std::move(r));
	CompletedIndex = static_cast<int32>(Index); // profile just measured

	const SProfileResult& rec = Results.back();
	const SFrameMetrics& m = rec.Avg;
	std::printf(
		"[profiler] %u/%zu spp=%u b=%u rr=%u portal=%d | "
		"RTms=%.3f filt=%.1f%% skyEff=%.1f%% raysPP=%.2f "
		"att=%llu ok=%llu rej=%llu reach=%llu blk=%llu\n",
		Index + 1u, Queue.size(),
		rec.Config.SampleCount, rec.Config.MaxBounces, rec.Config.RussianRouletteDepth,
		rec.Config.bPortalPreFilter ? 1 : 0,
		m.RtDispatchMs, m.PctFiltered * 100.0, m.SkyEfficiency * 100.0, m.RaysPerPixel,
		static_cast<unsigned long long>(m.Counters.Values[RC_SkyShadowAttempted]),
		static_cast<unsigned long long>(m.Counters.Values[RC_SkyShadowPortalPass]),
		static_cast<unsigned long long>(m.Counters.Values[RC_SkyShadowPortalRej]),
		static_cast<unsigned long long>(m.Counters.Values[RC_SkyShadowReachedSky]),
		static_cast<unsigned long long>(m.Counters.Values[RC_SkyShadowBlocked]));
	std::fflush(stdout);

	++Index;
	if (Index >= Queue.size())
	{
		State = EState::Idle;
		bFinished = true;
		std::printf("[profiler] session complete: %zu rows\n", Results.size());
		std::fflush(stdout);
		return;
	}

	State = EState::Warmup;
	WarmupLeft = WarmupFrames;
	bProfileChanged = true;
	Metrics.Reset();
}
}
