#pragma once

#include "Core.h"
#include "CoreTypes.h"
#include "Enum.h"
#include "Graphics/Render/GraphicsCoreTypes.h"


namespace frt::config
{
class CConfig;
}


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


// Config round-trip, over the [Display] section of the layered stack.
//
// Load leaves anything the config does not mention exactly as it found it, so the struct's
// own member initializers are the real defaults and a partial, stale or absent file costs
// nothing. Enums go through the reflection below, by name.
//
// The monitor / resolution / refresh-rate indices are indices into lists that only exist
// once the adapter has been queried, so they are NOT validated here - the caller clamps
// them against SDisplayOptions after the renderer comes up.
FRT_CORE_API void LoadDisplaySettings (const config::CConfig& Config, SDisplaySettings& OutSettings);
FRT_CORE_API void SaveDisplaySettings (config::CConfig& Config, const SDisplaySettings& Settings);

FRT_CORE_API void LoadUserSettings (const config::CConfig& Config, SUserSettings& OutSettings);
FRT_CORE_API void SaveUserSettings (config::CConfig& Config, const SUserSettings& Settings);
}


bool operator== (const frt::SDisplaySettings& Lhs, const frt::SDisplaySettings& Rhs);


// Needed by the config round-trip, which writes enums by name rather than by integer
// value, and reused by the display settings panel for its slider labels.
FRT_DECLARE_ENUM_REFLECTION(
	frt::EFullscreenMode,
	FRT_ENUM_ENTRY(frt::EFullscreenMode, Minimized),
	FRT_ENUM_ENTRY(frt::EFullscreenMode, Fullscreen),
	FRT_ENUM_ENTRY(frt::EFullscreenMode, Windowed),
	FRT_ENUM_ENTRY(frt::EFullscreenMode, Borderless));
