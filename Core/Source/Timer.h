#pragma once

#include <chrono>

#include "Core.h"
#include "Singleton.h"


NAMESPACE_FRT_START

class Timer : public Singleton<Timer>
{
	using TimePoint = std::chrono::high_resolution_clock::time_point;
public:
	Timer();

	FRT_SINGLETON_GETTERS(Timer)

	float FRT_CORE_API GetTotalSeconds() const;
	float FRT_CORE_API GetDeltaSeconds() const;
	bool FRT_CORE_API IsPaused() const;

	void FRT_CORE_API Reset();
	void FRT_CORE_API Start(bool bReset = false);
	void FRT_CORE_API Pause();
	void FRT_CORE_API Tick();

private:
	static long long GetDurationInMicroSeconds(const TimePoint& Start, const TimePoint& End);
	static double MicroSecondsToSeconds(long long MicroSeconds);

private:
	double _deltaSeconds;

	long long _pausedDuration;
	bool _bPaused;

	TimePoint _baseTimePoint;
	TimePoint _prevTimePoint;
	TimePoint _currTimePoint;
	TimePoint _stopTimePoint;
};

NAMESPACE_FRT_END
