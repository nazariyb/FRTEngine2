#include "RenderCommonTypes.h"

#ifdef _WINDOWS
#include <Windows.h>
#endif

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

std::vector<uint64> frt::graphics::SDisplayOptions::GetResolutionsEncoded (uint8 OutputIndex) const
{
	frt_assert(OutputIndex > -1);
	frt_assert(OutputIndex < OutputsNum);

	std::vector<uint64> resolutions;
	resolutions.reserve(OutputsModes[OutputIndex].size());

	for (const auto& modeInfo : OutputsModes[OutputIndex])
	{
		uint64 resEncoded = modeInfo.GetResolutionEncoded();
		if (std::ranges::find(resolutions, resEncoded) == resolutions.end())
		{
			resolutions.emplace_back(resEncoded);
		}
	}

	return resolutions;
}

uint64 frt::graphics::SDisplayOptions::GetFullscreenResolutionEncoded (uint8 OutputIndex) const
{
	frt_assert(OutputIndex > -1);
	frt_assert(OutputIndex < OutputsNum);

	const SRect& monitorRect = OutputsRects[OutputIndex];
	const int32 width = (int32)(monitorRect.Right - monitorRect.Left);
	const int32 height = (int32)(monitorRect.Bottom - monitorRect.Top);
	frt_assert(width > 0 && height > 0);

	return math::EncodeTwoIntoOne<int32, uint64>(width, height);
}

std::vector<uint64> frt::graphics::SDisplayOptions::GetRefreshRatesEncoded (uint8 OutputIndex, uint64 Resolution) const
{
	frt_assert(OutputIndex > -1);
	frt_assert(OutputIndex < OutputsNum);

	std::vector<uint64> refreshRates;
	refreshRates.reserve(OutputsModes[OutputIndex].size());

	for (const auto& info : OutputsModes[OutputIndex])
	{
		if (info.GetResolutionEncoded() == Resolution)
		{
			uint64 rrEncoded = info.GetRefreshRateEncoded();
			if (std::ranges::find(refreshRates, rrEncoded) == refreshRates.end())
			{
				refreshRates.emplace_back(rrEncoded);
			}
		}
	}

	// TODO: cache result
	return refreshRates;
}

uint64 frt::graphics::SOutputModeInfo::GetResolutionEncoded () const
{
	return frt::math::EncodeTwoIntoOne<uint32, uint64>(Width, Height);
}

uint64 frt::graphics::SOutputModeInfo::GetRefreshRateEncoded () const
{
	return frt::math::EncodeTwoIntoOne<uint32, uint64>(Numerator, Denominator);
}
