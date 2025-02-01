#include "Timer.h"

#include "Window.h"


using namespace std::chrono;

NAMESPACE_FRT_START

FRT_SINGLETON_DEFINE_INSTANCE(Timer)

Timer::Timer()
	: _deltaSeconds(0)
	, _pausedDuration(0)
	, _bPaused(false)
{
}

// Returns the total time elapsed since Reset() was called, NOT counting any
// time when the clock is stopped.
float Timer::GetTotalSeconds() const
{
	// If we are stopped, do not count the time that has passed since we stopped.
	// Moreover, if we previously already had a pause, the distance 
	// mStopTime - mBaseTime includes paused time, which we do not want to count.
	// To correct this, we can subtract the paused time from mStopTime:  
	//
	//                     |<--paused time-->|
	// ----*---------------*-----------------*------------*------------*------> time
	//  mBaseTime       mStopTime        startTime     mStopTime    mCurrTime

	if (_bPaused)
	{
		return static_cast<float>(MicroSecondsToSeconds(GetDurationInMicroSeconds(_baseTimePoint, _stopTimePoint) - _pausedDuration));
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
		return static_cast<float>(MicroSecondsToSeconds(GetDurationInMicroSeconds(_baseTimePoint, _currTimePoint) - _pausedDuration));
	}
}

float Timer::GetDeltaSeconds() const
{
	return static_cast<float>(_deltaSeconds);
}

bool Timer::IsPaused() const
{
	return _bPaused;
}

void Timer::Reset()
{
	const auto currentTimePoint = high_resolution_clock::now();
	_baseTimePoint = currentTimePoint;
	_prevTimePoint = currentTimePoint;
	_stopTimePoint = {};
	_bPaused = false;
}

void Timer::Start(bool bReset)
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

	if (_bPaused)
	{
		_pausedDuration += GetDurationInMicroSeconds(_stopTimePoint, startTimePoint);

		_prevTimePoint = startTimePoint;
		_stopTimePoint = {};
		_bPaused = false;
	}
}

void Timer::Pause()
{
	if(!_bPaused)
	{
		_stopTimePoint = high_resolution_clock::now();
		_bPaused  = true;
	}
}

void Timer::Tick()
{
	if (_bPaused)
	{
		_deltaSeconds = 0.0;
		return;
	}

	const auto currentTimePoint = high_resolution_clock::now();
	_currTimePoint = currentTimePoint;

	// Time difference between this frame and the previous.
	_deltaSeconds = MicroSecondsToSeconds(GetDurationInMicroSeconds(_prevTimePoint, _currTimePoint));

	// Prepare for next frame.
	_prevTimePoint = _currTimePoint;

	// Force nonnegative.  The DXSDK's CDXUTTimer mentions that if the 
	// processor goes into a power save mode or we get shuffled to another
	// processor, then mDeltaTime can be negative.
	if (_deltaSeconds < 0.0)
	{
		_deltaSeconds = 0.0;
	}
}

long long Timer::GetDurationInMicroSeconds(const TimePoint& Start, const TimePoint& End)
{
	return duration_cast<microseconds>(End - Start).count();
}

double Timer::MicroSecondsToSeconds(long long MicroSeconds)
{
	return static_cast<double>(MicroSeconds) / 1'000'000.;
}

NAMESPACE_FRT_END
