#pragma once

#include "CoreTypes.h"
#include "Graphics/Render/GraphicsCoreTypes.h"


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
	graphics::ERenderMode RenderMode = graphics::ERenderMode::Raytracing;
	bool bVSync = true;

	bool IsFullscreen () const { return FullscreenMode == EFullscreenMode::Fullscreen; }
};


struct SUserSettings
{
	SDisplaySettings DisplaySettings;
};
}


bool operator== (const frt::SDisplaySettings& Lhs, const frt::SDisplaySettings& Rhs);
