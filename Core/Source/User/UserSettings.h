#pragma once

#include <string>
#include <vector>

#include "CoreTypes.h"

enum class EDisplayMode : int32
{
	Fullscreen = 0,
	Windowed,
	Borderless,
};

struct SUserSettings
{
	int32 MonitorIndex = 0;
	std::vector<std::wstring> MonitorNames;
	int32 ResolutionIndex = 0;
	int32 RefreshRateIndex = 0;

	EDisplayMode DisplayMode = EDisplayMode::Fullscreen;
};
