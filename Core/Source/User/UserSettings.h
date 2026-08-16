#pragma once

#include "Core.h"
#include "CoreTypes.h"
#include "Enum.h"
#include "Graphics/Render/GraphicsCoreTypes.h"
#include "Graphics/Render/RenderCommonTypes.h"


namespace frt::config
{
class CConfig;
}


namespace frt::names
{
inline constexpr const char* DisplaySection = "Settings.Display";
inline constexpr const char* VideoSection = "Settings.Video";
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

	static constexpr uint8 ResolutionTextBufferSize		= 9u;
	static constexpr uint8 RefreshRateTextBufferSize	= 6u;
	char ResolutionTxt[ResolutionTextBufferSize + 1u]	= {};
	char RefreshRateTxt[RefreshRateTextBufferSize + 1u]	= {};

	bool bVSync = true;
	EFullscreenMode FullscreenMode = EFullscreenMode::Windowed;
	graphics::ERenderMode RenderMode = graphics::ERenderMode::Raytracing;

private:
	mutable graphics::SResolution Resolution{};

public:
	bool IsFullscreen () const { return FullscreenMode == EFullscreenMode::Fullscreen; }
	const graphics::SResolution& GetResolution () const;
};


struct SUserSettings
{
	SDisplaySettings DisplaySettings;
};


// Config round-trip, over the [Settings.Display] section of the layered stack.
//
// Load leaves anything the config does not mention exactly as it found it, so the struct's
// own member initializers are the real defaults and a partial, stale or absent file costs
// nothing. Enums go through the reflection below, by name.
//
// Resolution and refresh rate are stored as VALUES ("3840x2160", "60"), not as indices: the
// option lists differ per monitor and per machine, so an index does not survive a monitor
// change. Save therefore needs SDisplayOptions to turn the panel's index back into the value
// it points at. Load leaves the text in ResolutionTxt / RefreshRateTxt for the caller to
// resolve once the adapter has been queried.
FRT_CORE_API void LoadDisplaySettings (const config::CConfig& Config, SDisplaySettings& OutSettings);
FRT_CORE_API void SaveDisplaySettings (
	config::CConfig& Config,
	const SDisplaySettings& Settings,
	const graphics::SDisplayOptions& Options);

FRT_CORE_API void LoadUserSettings (const config::CConfig& Config, SUserSettings& OutSettings);
FRT_CORE_API void SaveUserSettings (
	config::CConfig& Config,
	const SUserSettings& Settings,
	const graphics::SDisplayOptions& Options);
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
