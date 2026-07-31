#pragma once

#include "Core.h"
#include "Matrix3x3.h"
#include "Matrix3x4.h"
#include "MathUtility.h"
#include "Precision.h"
#include "Vector2.h"
#include "Vector3.h"

using Vector2i = frt::math::TVector2<int>;
using Vector2u = frt::math::TVector2<unsigned>;
using Vector2f = frt::math::TVector2<float>;
using Vector2d = frt::math::TVector2<double>;
using Vector3f = frt::math::TVector3<float>;
using Vector3d = frt::math::TVector3<double>;

// Configured-precision aliases. Use these for positions, distances, and other world
// measurements; use the explicit f/d variants where a fixed width is required (GPU
// upload structs, DirectXMath interop, file formats).
using Vector2r = frt::math::TVector2<frt::Real>;
using Vector3r = frt::math::TVector3<frt::Real>;

using Matrix3x3f = frt::math::TMatrix3x3<float>;
using Matrix3x3d = frt::math::TMatrix3x3<double>;
using Matrix3x3r = frt::math::TMatrix3x3<frt::Real>;

// The GPU handoff type is always 32-bit - see TMatrix3x4's comment.
using Matrix3x4f = frt::math::TMatrix3x4<float>;
using Matrix3x4d = frt::math::TMatrix3x4<double>;

namespace frt::math
{
	// TVector3 has no cross-type converting constructor, so precision changes are
	// always explicit. Element-wise, so a narrowing cast is visible at the call site.
	template <concepts::Numerical TTo, concepts::Numerical TFrom>
	constexpr TVector3<TTo> VectorCast (const TVector3<TFrom>& InVector)
	{
		return TVector3<TTo>(
			static_cast<TTo>(InVector.x),
			static_cast<TTo>(InVector.y),
			static_cast<TTo>(InVector.z));
	}

	template <concepts::Numerical TTo, concepts::Numerical TFrom>
	constexpr TVector2<TTo> VectorCast (const TVector2<TFrom>& InVector)
	{
		return TVector2<TTo>(
			static_cast<TTo>(InVector.x),
			static_cast<TTo>(InVector.y));
	}

	template <typename T>
	inline TVector3<T> ToDirectXCoordinates (const TVector3<T>& Value)
	{
		return TVector3<T>(-Value.x, Value.y, Value.z);
	}

	template <typename T>
	inline TVector3<T> RubToLuf (const TVector3<T>& Value)
	{
		return TVector3<T>(-Value.x, Value.y, Value.z);
	}
}
