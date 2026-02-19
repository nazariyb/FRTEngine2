#pragma once

#include <chrono>

#include "Core.h"
#include "Singleton.h"


namespace frt
{
class CTimer : public Singleton<CTimer>
{
	using TimePoint = std::chrono::high_resolution_clock::time_point;

public:
	CTimer ();

	FRT_SINGLETON_GETTERS(CTimer)

	[[nodiscard]] float FRT_CORE_API GetTotalSeconds () const;
	[[nodiscard]] float FRT_CORE_API GetDeltaSeconds () const;
	[[nodiscard]] bool FRT_CORE_API IsPaused () const;

	void FRT_CORE_API Reset ();
	void FRT_CORE_API Start (bool bReset = false);
	void FRT_CORE_API Pause ();
	void FRT_CORE_API Tick ();

private:
	static long long GetDurationInMicroSeconds (const TimePoint& Start, const TimePoint& End);
	static double MicroSecondsToSeconds (long long MicroSeconds);

private:
	double DeltaSeconds;

	long long PausedDuration;
	bool bPaused;

	TimePoint BaseTimePoint;
	TimePoint PrevTimePoint;
	TimePoint CurrTimePoint;
	TimePoint StopTimePoint;
};
}
