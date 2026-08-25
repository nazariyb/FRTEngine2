#include "RenderCommonTypes.h"

#ifdef _WINDOWS
#include <Windows.h>
#endif

#include <charconv>
#include <format>
#include <numeric>
#include <ranges>

#include "Math/Transform.h"


namespace
{
std::string WideToUtf8 (const std::wstring& Value)
{
	if (Value.empty())
	{
		return {};
	}

#ifdef _WINDOWS
	const int byteCount = WideCharToMultiByte(
		CP_UTF8, 0, Value.data(), static_cast<int>(Value.size()), nullptr, 0, nullptr, nullptr);
	if (byteCount <= 0)
	{
		return {};
	}

	std::string utf8(static_cast<std::size_t>(byteCount), '\0');
	WideCharToMultiByte(
		CP_UTF8, 0, Value.data(), static_cast<int>(Value.size()),
		utf8.data(), byteCount, nullptr, nullptr);
	return utf8;
#else
	std::string narrow;
	narrow.reserve(Value.size());
	for (const wchar_t ch : Value)
	{
		narrow.push_back(static_cast<char>(ch));
	}
	return narrow;
#endif
}
}


std::vector<std::string> frt::graphics::SDisplayOptions::GetNames () const
{
	std::vector<std::string> names;
	names.reserve(OutputsNames.size());

	for (const auto& wStr : OutputsNames)
	{
		names.emplace_back(WideToUtf8(wStr));
	}

	return names;
}

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
	const uint16 prevResolutionsNum = std::accumulate(prevOutputsResolutions.begin(), prevOutputsResolutions.end(), 0u);
	return std::span(Resolutions).subspan(prevResolutionsNum, resolutionsNum);
}

std::span<const uint64> frt::graphics::SDisplayOptions::GetRefreshRates(uint8 OutputIndex, int32 ResolutionIndex) const
{
	frt_assert(OutputIndex < OutputsNum);

	const auto prevOutputsResoultionsNums = std::span(ResolutionOptionNums).subspan(0, OutputIndex);
	const uint16 prevResolutionsNum = std::accumulate(prevOutputsResoultionsNums.begin(), prevOutputsResoultionsNums.end(), 0u);

	uint16 offset = 0u;
	for (uint16 idx : std::views::iota(0u, prevResolutionsNum))
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

int32 frt::graphics::SDisplayOptions::GetRefreshRateIndex (uint8 OutputIndex, const std::string_view& RefreshRateTxt) const
{
	const char* begin = RefreshRateTxt.data();
	const char* end = begin + RefreshRateTxt.size();

	int32 refreshRateInt;
	const auto [ptr, error] = std::from_chars(begin, end, refreshRateInt);

	if (error != std::errc())
	{
		refreshRateInt = -1;

		float refreshRateFloat;
		const auto [ptrF, errorF] = std::from_chars(begin, end, refreshRateFloat);
		if (errorF == std::errc())
		{
			refreshRateInt = static_cast<int32>(refreshRateFloat * 100.f);
		}
	}

	if (refreshRateInt < 0)
	{
		return DefaultIndex;
	}

	return GetRefreshRateIndex(OutputIndex, refreshRateInt);
}

int32 frt::graphics::SDisplayOptions::GetRefreshRateIndex (uint8 OutputIndex, uint32 RefreshRate) const
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

int32 frt::graphics::SDisplayOptions::GetRefreshRateIndex (uint8 OutputIndex, uint32 Numerator, uint32 Denominator) const
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

uint8 frt::graphics::SDisplayOptions::ClampMonitorIndex (uint8 InMonitorIndex) const
{
	return math::ClampIndex(InMonitorIndex, OutputsNum);
}

FRT_CORE_API bool frt::graphics::SResolution::Parse (std::string_view Str)
{
	// find_first_of, not a loop over the delimiters: the earliest delimiter in the STRING is
	// the separator, not the earliest one in this list. The hand-rolled walk this replaces
	// advanced a raw pointer guarded by `&& currentDelimeter`, which is never null once it
	// has been incremented - a string with no delimiter at all ran off the end of the
	// literal and read whatever followed it. "1920" parsed as 19x0; "" segfaulted, and ""
	// is exactly what LoadDisplaySettings passes when the Resolution key is absent.
	static constexpr std::string_view Delimiters = "xX:|-";

	bValid = false;
	Width = InvalidComponent;
	Height = InvalidComponent;

	const size_t delimiterPos = Str.find_first_of(Delimiters);
	if (delimiterPos == std::string_view::npos)
	{
		return bValid;
	}

	const char* const strStart = Str.data();
	const char* const widthEnd = Str.data() + delimiterPos;
	const char* const heightEnd = Str.data() + Str.size();

	uint16 width, height;
	const auto [ptrW, errorW] = std::from_chars(strStart, widthEnd, width);
	const auto [ptrH, errorH] = std::from_chars(strStart + delimiterPos + 1, heightEnd, height);

	// Both sides must be consumed whole. A partial parse means the text was not a
	// resolution - "19 20x1080" is not 19, and silently accepting it is how a garbage
	// value ends up looking like a valid setting.
	if (errorW != std::errc() || errorH != std::errc() || ptrW != widthEnd || ptrH != heightEnd)
	{
		Width = InvalidComponent;
		Height = InvalidComponent;
		return bValid;
	}

	Set(width, height);
	return bValid;
}

FRT_CORE_API std::string frt::graphics::SResolution::ToString (char Delimiter) const
{
	if (!bValid)
	{
		return {};
	}

	return std::format("{}{}{}", Width, Delimiter, Height);
}
