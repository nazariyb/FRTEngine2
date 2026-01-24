#pragma once

#include "Core.h"
#include "MathUtility.h"
#include "Vector2.h"
#include "Vector3.h"

using Vector2i = frt::math::TVector2<int>;
using Vector2u = frt::math::TVector2<unsigned>;
using Vector2f = frt::math::TVector2<float>;
using Vector2d = frt::math::TVector2<double>;
using Vector3f = frt::math::TVector3<float>;
using Vector3d = frt::math::TVector3<double>;

namespace frt::math
{
template <typename T>
inline TVector3<T> ToDirectXCoordinates (const TVector3<T>& Value)
{
	return TVector3<T>(-Value.x, Value.y, Value.z);
}
}
