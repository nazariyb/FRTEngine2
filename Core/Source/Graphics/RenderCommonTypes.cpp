#include "RenderCommonTypes.h"

#include "Math/Transform.h"

uint64 frt::graphics::SOutputModeInfo::GetResolutionEncoded() const
{
	return frt::math::EncodeTwoIntoOne<uint32, uint64>(Width, Height);
}

uint64 frt::graphics::SOutputModeInfo::GetRefreshRateEncoded() const
{
	return frt::math::EncodeTwoIntoOne<uint32, uint64>(Numerator, Denominator);
}
