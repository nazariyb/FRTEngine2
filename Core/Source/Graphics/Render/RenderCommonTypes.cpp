#include "RenderCommonTypes.h"

#include <numeric>
#include <optional>
#include <ranges>

#include "Math/Transform.h"


frt::graphics::SResolution frt::graphics::SDisplayOptions::GetFullscreenResolution(uint8 OutputIndex) const
{
	frt_assert(OutputIndex > -1);
	frt_assert(OutputIndex < OutputsNum);

	const SRect& monitorRect = OutputsRects[OutputIndex];
	const int32 width = (int32)(monitorRect.Right - monitorRect.Left);
	const int32 height = (int32)(monitorRect.Bottom - monitorRect.Top);
	frt_assert(width > 0 && height > 0);

	return { width, height };
}

std::span<const frt::graphics::SResolution> frt::graphics::SDisplayOptions::GetResolutions (uint8 OutputIndex) const
{
	frt_assert(OutputIndex < OutputsNum);

	const uint8 resolutionsNum = ResolutionOptionNums[OutputIndex];
	const auto prevOutputsResolutions = std::span(ResolutionOptionNums).subspan(0, OutputIndex);
	const uint16 prevResolutionsNum = static_cast<uint16>(std::accumulate(prevOutputsResolutions.begin(), prevOutputsResolutions.end(), 0u));
	return std::span(Resolutions).subspan(prevResolutionsNum, resolutionsNum);
}

std::span<const uint64> frt::graphics::SDisplayOptions::GetRefreshRates(uint8 OutputIndex, int32 ResolutionIndex) const
{
	frt_assert(OutputIndex < OutputsNum);

	const auto prevOutputsResoultionsNums = std::span(ResolutionOptionNums).subspan(0, OutputIndex);
	const uint16 prevResolutionsNum = static_cast<uint16>(std::accumulate(prevOutputsResoultionsNums.begin(), prevOutputsResoultionsNums.end(), 0u));

	uint16 offset = 0u;
	for (uint16 idx : std::views::iota(uint16{0}, prevResolutionsNum))
	{
		offset += RefreshRateOptionNums[static_cast<uint8>(idx)];
	}

	return std::span(RefreshRates).subspan(offset, RefreshRateOptionNums[ResolutionIndex]);
}

int32 frt::graphics::SDisplayOptions::GetResolutionIndex (uint8 OutputIndex, const struct SResolution& Resolution) const
{
	for (const auto&& [idx, value] : std::views::enumerate(GetResolutions(OutputIndex)))
	{
		if (value == Resolution)
		{
			return static_cast<int32>(idx);
		}
	}
	return DefaultIndex;}

int32 frt::graphics::SDisplayOptions::GetResolutionIndex (uint8 OutputIndex, uint16 Width, uint16 Height) const
{
	return GetResolutionIndex(OutputIndex, { Width, Height });
}

int32 frt::graphics::SDisplayOptions::GetRefreshRateIndex (uint8 OutputIndex, FStringView RefreshRateTxt) const
{
	// The rate overload compares whole hertz (Numerator / Denominator): "59.94" is 59. Text that is
	// no rate, or none a uint32 holds, selects the default.
	const std::optional<float> hertz = RefreshRateTxt.ToFloat();
	if (!hertz || !(*hertz >= 0.0f && *hertz < 4294967296.0f))
	{
		return DefaultIndex;
	}

	return GetRefreshRateIndex(OutputIndex, static_cast<uint32>(*hertz));
}

// TODO: OutputIndex and RefreshRate are currently ignored; this always returns DefaultIndex.
int32 frt::graphics::SDisplayOptions::GetRefreshRateIndex ([[maybe_unused]] uint8 OutputIndex, [[maybe_unused]] uint32 RefreshRate) const
{
	// for (const auto&& [idx, modeInfo] : std::views::enumerate(OutputsModes[OutputIndex]))
	// {
	// 	if ((modeInfo.Numerator / modeInfo.Denominator) == RefreshRate)
	// 	{
	// 		return static_cast<int32>(idx);
	// 	}
	// }

	return DefaultIndex;
}

// TODO: the parameters are currently ignored; this always returns DefaultIndex.
int32 frt::graphics::SDisplayOptions::GetRefreshRateIndex ([[maybe_unused]] uint8 OutputIndex, [[maybe_unused]] uint32 Numerator, [[maybe_unused]] uint32 Denominator) const
{
	// for (const auto&& [idx, modeInfo] : std::views::enumerate(OutputsModes[OutputIndex]))
	// {
	// 	if (modeInfo.Numerator == Numerator && modeInfo.Denominator == Denominator)
	// 	{
	// 		return static_cast<int32>(idx);
	// 	}
	// }

	return DefaultIndex;
}

uint8 frt::graphics::SDisplayOptions::ClampMonitorIndex (int32 InMonitorIndex) const
{
	// Clamped as int32: narrowing the config value to uint8 first would wrap -1 or 256 onto
	// another monitor. With no outputs at all this yields 0.
	return static_cast<uint8>(math::Clamp(InMonitorIndex, 0, math::Max(OutputsNum - 1, 0)));
}

FRT_CORE_API bool frt::graphics::SResolution::Parse (FStringView Text)
{
	// FindFirstOf, not a loop over the delimiters: the earliest delimiter in the STRING is
	// the separator, not the earliest one in this list. The hand-rolled walk this replaces
	// advanced a raw pointer guarded by `&& currentDelimeter`, which is never null once it
	// has been incremented - a string with no delimiter at all ran off the end of the
	// literal and read whatever followed it. "1920" parsed as 19x0; "" segfaulted, and ""
	// is exactly what LoadDisplaySettings passes when the Resolution key is absent.
	static constexpr FStringLiteral Delimiters = "xX:|-";

	bValid = false;
	Width = InvalidComponent;
	Height = InvalidHeight;

	const uint32 delimiterPos = Text.FindFirstOf(Delimiters);
	if (delimiterPos == FStringView::NotFound)
	{
		return bValid;
	}

	// Both sides must be consumed whole. A partial parse means the text was not a
	// resolution - "19 20x1080" is not 19, and silently accepting it is how a garbage
	// value ends up looking like a valid setting.
	const std::optional<uint16> width = Text.Left(delimiterPos).TryParse<uint16>();
	const std::optional<uint16> height = Text.Mid(delimiterPos + 1).TryParse<uint16>();
	if (!width || !height)
	{
		return bValid;
	}

	Set(*width, *height);
	return bValid;
}

FRT_CORE_API frt::FString frt::graphics::SResolution::ToString (char Delimiter) const
{
	if (!bValid)
	{
		return {};
	}

	return FString::Format("{}{}{}", Width, Delimiter, Height);
}
