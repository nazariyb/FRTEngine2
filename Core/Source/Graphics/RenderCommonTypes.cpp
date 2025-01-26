#include "RenderCommonTypes.h"

#include "Math/Transform.h"

std::vector<std::string> frt::graphics::SDisplayOptions::GetNames() const
{
	std::vector<std::string> names;
	names.reserve(OutputsNames.size());

	for (const auto& wStr : OutputsNames)
	{
		names.emplace_back(wStr.begin(), wStr.end());
	}

	return names;
}

std::vector<uint64> frt::graphics::SDisplayOptions::GetResolutionsEncoded(uint8 OutputIndex) const
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

std::vector<uint64> frt::graphics::SDisplayOptions::GetRefreshRatesEncoded(uint8 OutputIndex, uint64 Resolution) const
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

	return refreshRates;
}

uint64 frt::graphics::SOutputModeInfo::GetResolutionEncoded() const
{
	return frt::math::EncodeTwoIntoOne<uint32, uint64>(Width, Height);
}

uint64 frt::graphics::SOutputModeInfo::GetRefreshRateEncoded() const
{
	return frt::math::EncodeTwoIntoOne<uint32, uint64>(Numerator, Denominator);
}
