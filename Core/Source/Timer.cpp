#include "Timer.h"

#include "Window.h"


using namespace std::chrono;

NAMESPACE_FRT_START
FRT_SINGLETON_DEFINE_INSTANCE(CTimer)

CTimer::CTimer ()
	: DeltaSeconds(0)
	, PausedDuration(0)
	, BPaused(false)
{}

// Returns the total time elapsed since Reset() was called, NOT counting any
// time when the clock is stopped.
float CTimer::GetTotalSeconds () const
{
	// If we are stopped, do not count the time that has passed since we stopped.
	// Moreover, if we previously already had a pause, the distance 
	// mStopTime - mBaseTime includes paused time, which we do not want to count.
	// To correct this, we can subtract the paused time from mStopTime:  
	//
	//                     |<--paused time-->|
	// ----*---------------*-----------------*------------*------------*------> time
	//  mBaseTime       mStopTime        startTime     mStopTime    mCurrTime

	if (BPaused)
	{
		return static_cast<float>(MicroSecondsToSeconds(
			GetDurationInMicroSeconds(BaseTimePoint, StopTimePoint) - PausedDuration));
	}

	// The distance mCurrTime - mBaseTime includes paused time,
	// which we do not want to count.  To correct this, we can subtract 
	// the paused time from mCurrTime:  
	//
	//  (mCurrTime - mPausedTime) - mBaseTime 
	//
	//                     |<--paused time-->|
	// ----*---------------*-----------------*------------*------> time
	//  mBaseTime       mStopTime        startTime     mCurrTime

	else
	{
		return static_cast<float>(MicroSecondsToSeconds(
			GetDurationInMicroSeconds(BaseTimePoint, CurrTimePoint) - PausedDuration));
	}
}

float CTimer::GetDeltaSeconds () const
{
	return static_cast<float>(DeltaSeconds);
}

bool CTimer::IsPaused () const
{
	return BPaused;
}

void CTimer::Reset ()
{
	const auto currentTimePoint = high_resolution_clock::now();
	BaseTimePoint = currentTimePoint;
	PrevTimePoint = currentTimePoint;
	StopTimePoint = {};
	BPaused = false;
}

void CTimer::Start (bool bReset)
{
	if (bReset)
	{
		Reset();
	}

	const auto startTimePoint = high_resolution_clock::now();

	// Accumulate the time elapsed between stop and start pairs.
	//
	//                     |<-------d------->|
	// ----*---------------*-----------------*------------> time
	//  mBaseTime       mStopTime        startTime     

	if (BPaused)
	{
		PausedDuration += GetDurationInMicroSeconds(StopTimePoint, startTimePoint);

		PrevTimePoint = startTimePoint;
		StopTimePoint = {};
		BPaused = false;
	}
}

void CTimer::Pause ()
{
	if (!BPaused)
	{
		StopTimePoint = high_resolution_clock::now();
		BPaused = true;
	}
}

void CTimer::Tick ()
{
	if (BPaused)
	{
		DeltaSeconds = 0.0;
		return;
	}

	const auto currentTimePoint = high_resolution_clock::now();
	CurrTimePoint = currentTimePoint;

	// Time difference between this frame and the previous.
	DeltaSeconds = MicroSecondsToSeconds(GetDurationInMicroSeconds(PrevTimePoint, CurrTimePoint));

	// Prepare for next frame.
	PrevTimePoint = CurrTimePoint;

	// Force nonnegative.  The DXSDK's CDXUTTimer mentions that if the 
	// processor goes into a power save mode or we get shuffled to another
	// processor, then mDeltaTime can be negative.
	if (DeltaSeconds < 0.0)
	{
		DeltaSeconds = 0.0;
	}
}

long long CTimer::GetDurationInMicroSeconds (const TimePoint& Start, const TimePoint& End)
{
	return duration_cast<microseconds>(End - Start).count();
}

double CTimer::MicroSecondsToSeconds (long long MicroSeconds)
{
	return static_cast<double>(MicroSeconds) / 1'000'000.;
}

NAMESPACE_FRT_END
