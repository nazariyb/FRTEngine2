#include "UserSettings.h"

#include <cmath>
#include <format>
#include <optional>
#include <span>

#include "Config/Config.h"

using namespace frt::names;

namespace
{
// The panel edits indices into the option lists. Config stores the value the index points
// at, because an index means something different on another monitor, another machine, or
// after a display is unplugged.
//
// nullopt when the index addresses nothing - a monitor with no enumerated modes, or an index
// left over from a different display setup. Writing nothing is better than writing a
// resolution the user never chose.
std::optional<frt::graphics::SResolution> ResolveResolution (
	const frt::graphics::SDisplayOptions& Options,
	const frt::SDisplaySettings& Settings)
{
	if (Settings.MonitorIndex < 0 || Settings.MonitorIndex >= Options.OutputsNum)
	{
		return std::nullopt;
	}

	const std::span<const uint32> resolutions =
		Options.GetResolutions(static_cast<uint8>(Settings.MonitorIndex));

	if (Settings.ResolutionIndex < 0
		|| static_cast<size_t>(Settings.ResolutionIndex) >= resolutions.size())
	{
		return std::nullopt;
	}

	return frt::graphics::SResolution::FromEncoded(resolutions[Settings.ResolutionIndex]);
}


// Reads back through the same span the panel listed, so whatever the user selected is what
// gets written - if that span is wrong, the list they picked from was wrong too.
//
// NOTE: SDisplayOptions::GetRefreshRates still indexes RefreshRateOptionNums with an
// output-local resolution index against a globally-flattened array. Until that is fixed this
// value is only as right as the list the panel showed.
std::optional<std::string> ResolveRefreshRate (
	const frt::graphics::SDisplayOptions& Options,
	const frt::SDisplaySettings& Settings)
{
	if (Settings.MonitorIndex < 0 || Settings.MonitorIndex >= Options.OutputsNum)
	{
		return std::nullopt;
	}

	const std::span<const uint64> refreshRates = Options.GetRefreshRates(
		static_cast<uint8>(Settings.MonitorIndex), Settings.ResolutionIndex);

	if (Settings.RefreshRateIndex < 0
		|| static_cast<size_t>(Settings.RefreshRateIndex) >= refreshRates.size())
	{
		return std::nullopt;
	}

	uint32 numerator = 0;
	uint32 denominator = 0;
	frt::math::DecodeTwoFromOne(refreshRates[Settings.RefreshRateIndex], numerator, denominator);
	if (denominator == 0u)
	{
		return std::nullopt;
	}

	// Whole rates stay whole ("60", not "60.00") so the common case matches what is already
	// hand-written in Engine.ini and the file does not churn on every save. Two decimals
	// otherwise, which is what the panel displays and what fits RefreshRateTextBufferSize.
	const float hz = static_cast<float>(numerator) / static_cast<float>(denominator);
	if (std::round(hz) == hz)
	{
		return std::format("{}", static_cast<uint32>(hz));
	}

	return std::format("{:.2f}", hz);
}
}


namespace frt
{
const graphics::SResolution& SDisplaySettings::GetResolution() const
{
	if (!Resolution.bValid)
	{
		Resolution.Parse(ResolutionTxt);
	}

	return Resolution;
}

void LoadDisplaySettings (const config::CConfig& Config, SDisplaySettings& OutSettings)
{
	OutSettings.MonitorIndex = Config.Get(DisplaySection, "MonitorIndex", OutSettings.MonitorIndex);

	Config.Get(OutSettings.ResolutionTxt, SDisplaySettings::ResolutionTextBufferSize, 
		DisplaySection, "Resolution", "");
	Config.Get(OutSettings.RefreshRateTxt, SDisplaySettings::RefreshRateTextBufferSize,
		DisplaySection, "RefreshRate", "");

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
}


void SaveDisplaySettings (
	config::CConfig& Config,
	const SDisplaySettings& Settings,
	const graphics::SDisplayOptions& Options)
{
	Config.Set(DisplaySection, "MonitorIndex", Settings.MonitorIndex);

	// Must be the same keys LoadDisplaySettings reads. Writing ResolutionIndex /
	// RefreshRateIndex here left Load with nothing to find, so it fell through to the engine
	// defaults and the user's choice was silently discarded on every launch.
	if (const auto resolution = ResolveResolution(Options, Settings))
	{
		Config.Set(DisplaySection, "Resolution", resolution->ToString());
	}

	if (const auto refreshRate = ResolveRefreshRate(Options, Settings))
	{
		Config.Set(DisplaySection, "RefreshRate", *refreshRate);
	}

	Config.Set(DisplaySection, "FullscreenMode", Settings.FullscreenMode);
	Config.Set(DisplaySection, "RenderMode", Settings.RenderMode);
	Config.Set(DisplaySection, "VSync", Settings.bVSync);
}


void LoadUserSettings (const config::CConfig& Config, SUserSettings& OutSettings)
{
	LoadDisplaySettings(Config, OutSettings.DisplaySettings);
}


void SaveUserSettings (
	config::CConfig& Config,
	const SUserSettings& Settings,
	const graphics::SDisplayOptions& Options)
{
	SaveDisplaySettings(Config, Settings.DisplaySettings, Options);
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
