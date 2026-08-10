#include "UserSettings.h"

#include "Config/Config.h"


namespace
{
constexpr const char* DisplaySection = "Display";
}


namespace frt
{
void LoadDisplaySettings (const config::CConfig& Config, SDisplaySettings& OutSettings)
{
	OutSettings.MonitorIndex = Config.Get(DisplaySection, "MonitorIndex", OutSettings.MonitorIndex);
	OutSettings.ResolutionIndex = Config.Get(DisplaySection, "ResolutionIndex", OutSettings.ResolutionIndex);
	OutSettings.RefreshRateIndex = Config.Get(DisplaySection, "RefreshRateIndex", OutSettings.RefreshRateIndex);
	OutSettings.FullscreenMode = Config.Get(DisplaySection, "FullscreenMode", OutSettings.FullscreenMode);
	OutSettings.RenderMode = Config.Get(DisplaySection, "RenderMode", OutSettings.RenderMode);
	OutSettings.bVSync = Config.Get(DisplaySection, "VSync", OutSettings.bVSync);

	// Minimized is a transient window state, not something to restore into. A config that
	// says so - written by a game that was minimized when it saved, or hand-edited - would
	// start with no visible window and no way to fix it but editing the file back.
	if (OutSettings.FullscreenMode == EFullscreenMode::Minimized)
	{
		OutSettings.FullscreenMode = EFullscreenMode::Windowed;
	}

	// Negative indices would index out of the option lists before anything got the chance
	// to clamp them against the real ones. The upper bound needs SDisplayOptions and is
	// applied by the caller once the adapter has been queried.
	OutSettings.MonitorIndex = OutSettings.MonitorIndex < 0 ? 0 : OutSettings.MonitorIndex;
	OutSettings.ResolutionIndex = OutSettings.ResolutionIndex < 0 ? 0 : OutSettings.ResolutionIndex;
	OutSettings.RefreshRateIndex = OutSettings.RefreshRateIndex < 0 ? 0 : OutSettings.RefreshRateIndex;
}


void SaveDisplaySettings (config::CConfig& Config, const SDisplaySettings& Settings)
{
	Config.Set(DisplaySection, "MonitorIndex", Settings.MonitorIndex);
	Config.Set(DisplaySection, "ResolutionIndex", Settings.ResolutionIndex);
	Config.Set(DisplaySection, "RefreshRateIndex", Settings.RefreshRateIndex);
	Config.Set(DisplaySection, "FullscreenMode", Settings.FullscreenMode);
	Config.Set(DisplaySection, "RenderMode", Settings.RenderMode);
	Config.Set(DisplaySection, "VSync", Settings.bVSync);
}


void LoadUserSettings (const config::CConfig& Config, SUserSettings& OutSettings)
{
	LoadDisplaySettings(Config, OutSettings.DisplaySettings);
}


void SaveUserSettings (config::CConfig& Config, const SUserSettings& Settings)
{
	SaveDisplaySettings(Config, Settings.DisplaySettings);
}
}


bool operator== (const frt::SDisplaySettings& Lhs, const frt::SDisplaySettings& Rhs)
{
	return Lhs.MonitorIndex == Rhs.MonitorIndex
			&& Lhs.ResolutionIndex == Rhs.ResolutionIndex
			&& Lhs.RefreshRateIndex == Rhs.RefreshRateIndex
			&& Lhs.FullscreenMode == Rhs.FullscreenMode
			&& Lhs.bVSync == Rhs.bVSync;
}
