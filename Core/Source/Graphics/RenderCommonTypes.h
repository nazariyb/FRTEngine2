#pragma once
#include "CoreTypes.h"

namespace frt::graphics
{
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
}
