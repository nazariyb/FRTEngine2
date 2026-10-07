#pragma once

#include "Core.h"
#include "Matrix3x3.h"
#include "Vector3.h"


namespace frt::math
{
/**
 * Affine 3x4 matrix: a 3x3 basis with translation in column 3. Row-major.
 *
 * Same conventions as TMatrix3x3 (column-vector, v' = M * v), and byte-for-byte the
 * layout D3D12_RAYTRACING_INSTANCE_DESC::Transform wants - FLOAT[3][4], row-major,
 * translation last column. DXRUtils memcpys straight into the instance desc, so this
 * is the handoff type between scene math and the render backend.
 *
 * There is no fourth row: it is always (0, 0, 0, 1) for an affine transform, and
 * storing it wastes 16 bytes per entity.
 */
template <concepts::Numerical T>
struct TMatrix3x4
{
	static_assert(std::is_floating_point_v<T>, "T must be a floating point number");

	using Real = T;

	Real M[3][4] =
	{
		{ Real(1), Real(0), Real(0), Real(0) },
		{ Real(0), Real(1), Real(0), Real(0) },
		{ Real(0), Real(0), Real(1), Real(0) },
	};


	static constexpr TMatrix3x4 FromBasisTranslation(const TMatrix3x3<Real>& InBasis, const TVector3<Real>& InTranslation)
	{
		TMatrix3x4 result;
		for (uint32 row = 0u; row < 3u; ++row)
		{
			result.M[row][0] = InBasis.M[row][0];
			result.M[row][1] = InBasis.M[row][1];
			result.M[row][2] = InBasis.M[row][2];
		}

		result.M[0][3] = InTranslation.x;
		result.M[1][3] = InTranslation.y;
		result.M[2][3] = InTranslation.z;

		return result;
	}

	constexpr TMatrix3x3<Real> GetBasis() const
	{
		TMatrix3x3<Real> result;
		for (uint32 row = 0u; row < 3u; ++row)
		{
			result.M[row][0] = M[row][0];
			result.M[row][1] = M[row][1];
			result.M[row][2] = M[row][2];
		}

		return result;
	}

	constexpr TVector3<Real> GetTranslation() const
	{
		return TVector3<Real>(M[0][3], M[1][3], M[2][3]);
	}

	static const TMatrix3x4 Identity;
};


template <concepts::Numerical T>
inline const TMatrix3x4<T> TMatrix3x4<T>::Identity = TMatrix3x4<T>{};


/**
 * Engine space (left-up-forward) -> DirectX space, by negating X of the transformed result.
 *
 * Left-multiply by diag(-1, 1, 1): negate row 0, translation included. That is the conversion
 * of world-space positions (ToDirectXCoordinates) applied after the transform, so instances,
 * lights, portals and the camera all agree. It matches STransform::GetMatrix (v * M * F).
 */
template <concepts::Numerical T>
constexpr TMatrix3x4<T> ToDirectXHandedness(const TMatrix3x4<T>& InMatrix)
{
	TMatrix3x4<T> result = InMatrix;
	result.M[0][0] = -result.M[0][0];
	result.M[0][1] = -result.M[0][1];
	result.M[0][2] = -result.M[0][2];
	result.M[0][3] = -result.M[0][3];

	return result;
}


template <concepts::Numerical TTo, concepts::Numerical TFrom>
constexpr TMatrix3x4<TTo> MatrixCast(const TMatrix3x4<TFrom>& InMatrix)
{
	TMatrix3x4<TTo> result;
	for (uint32 row = 0u; row < 3u; ++row)
	{
		for (uint32 col = 0u; col < 4u; ++col)
		{
			result.M[row][col] = static_cast<TTo>(InMatrix.M[row][col]);
		}
	}

	return result;
}
}
