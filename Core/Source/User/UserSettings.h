#pragma once

#include <string>
#include <vector>

#include "CoreTypes.h"


namespace frt
{
enum class EFullscreenMode : int32
{
	Minimized = 0,
	Fullscreen,
	Windowed,
	Borderless,
};


struct SDisplaySettings
{
	int32 MonitorIndex = 0;
	int32 ResolutionIndex = 0;
	int32 RefreshRateIndex = 0;

	EFullscreenMode FullscreenMode = EFullscreenMode::Windowed;

	bool IsFullscreen () const { return FullscreenMode == EFullscreenMode::Fullscreen; }
};


struct SUserSettings
{
	SDisplaySettings DisplaySettings;
};
}


bool operator== (const frt::SDisplaySettings& Lhs, const frt::SDisplaySettings& Rhs);
