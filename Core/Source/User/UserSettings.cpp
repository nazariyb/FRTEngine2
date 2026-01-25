#include "UserSettings.h"

bool operator== (const frt::SDisplaySettings& Lhs, const frt::SDisplaySettings& Rhs)
{
	return Lhs.MonitorIndex == Rhs.MonitorIndex
			&& Lhs.ResolutionIndex == Rhs.ResolutionIndex
			&& Lhs.RefreshRateIndex == Rhs.RefreshRateIndex
			&& Lhs.FullscreenMode == Rhs.FullscreenMode
			&& Lhs.bVSync == Rhs.bVSync;
}
