#pragma once

#include <string>
#include <vector>

#include "CoreTypes.h"

namespace frt::graphics
{
	struct SDisplayOptions
	{
	public:
		uint8 OutputsNum = 0;
		std::vector<std::wstring> OutputsNames;
		std::vector<struct SRect> OutputsRects;
		std::vector<std::vector<struct SOutputModeInfo>> OutputsModes;

	public:
		std::vector<std::string> GetNames() const;
		std::vector<uint64> GetResolutionsEncoded(uint8 OutputIndex) const;
		uint64 GetFullscreenResolutionEncoded(uint8 OutputIndex) const;
		std::vector<uint64> GetRefreshRatesEncoded(uint8 OutputIndex, uint64 Resolution) const;
	};

	struct SOutputModeInfo
	{
	public:
		uint32 Width;
		uint32 Height;
		uint32 Numerator;
		uint32 Denominator;

	public:
		uint64 GetResolutionEncoded() const;
		uint64 GetRefreshRateEncoded() const;
	};

	struct SRect
	{
		int64 Left;
		int64 Top;
		int64 Right;
		int64 Bottom;
	};
}
