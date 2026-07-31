#pragma once

#include <cmath>
#include <type_traits>

#include "Core.h"
#include "MathUtility.h"
#include "Vector3.h"


namespace frt::math
{
/**
 * 3x3 basis matrix - rotation and scale, no translation.
 *
 * Conventions, stated up front because mixing them up is the single most common source
 * of transform bugs:
 *
 *   - Column-vector: v' = M * v.
 *   - Row-major storage: M[Row][Column].
 *   - A * B applies B first, then A.
 *
 * This is deliberately the layout D3D12_RAYTRACING_INSTANCE_DESC::Transform expects
 * (and nvrhi::rt::InstanceDesc after that), so building a GPU instance transform is a
 * copy rather than a transpose.
 *
 * DirectXMath uses the opposite, row-vector convention, so interop with it transposes.
 * That conversion belongs at the render-backend boundary and nowhere else.
 */
template <concepts::Numerical T>
struct TMatrix3x3
{
	static_assert(std::is_floating_point_v<T>, "T must be a floating point number");

	using Real = T;

	Real M[3][3] =
	{
		{ Real(1), Real(0), Real(0) },
		{ Real(0), Real(1), Real(0) },
		{ Real(0), Real(0), Real(1) },
	};


	// ----- Construction -----

	/**
	 * Euler angles in radians, matching STransform's convention:
	 * X = pitch, Y = yaw, Z = roll, applied roll -> pitch -> yaw.
	 * Equivalent to the transpose of XMMatrixRotationRollPitchYaw(X, Y, Z).
	 */
	static TMatrix3x3 FromEulerRadians(const TVector3<Real>& InEuler)
	{
		const Real sp = std::sin(InEuler.x), cp = std::cos(InEuler.x); // pitch
		const Real sy = std::sin(InEuler.y), cy = std::cos(InEuler.y); // yaw
		const Real sr = std::sin(InEuler.z), cr = std::cos(InEuler.z); // roll

		TMatrix3x3 result;
		result.M[0][0] = cy * cr + sy * sp * sr;
		result.M[0][1] = -cy * sr + sy * sp * cr;
		result.M[0][2] = sy * cp;
		result.M[1][0] = cp * sr;
		result.M[1][1] = cp * cr;
		result.M[1][2] = -sp;
		result.M[2][0] = -sy * cr + cy * sp * sr;
		result.M[2][1] = sy * sr + cy * sp * cr;
		result.M[2][2] = cy * cp;

		return result;
	}

	static constexpr TMatrix3x3 FromScale(const TVector3<Real>& InScale)
	{
		TMatrix3x3 result;
		result.M[0][0] = InScale.x;
		result.M[1][1] = InScale.y;
		result.M[2][2] = InScale.z;

		return result;
	}

	/**
	 * Rotation composed with scale as R * S - scale is applied first, in local space.
	 *
	 * NOTE: STransform composes rotation * scale in row-vector order, which is scale
	 * applied AFTER rotation, i.e. along world axes. That shears a non-uniformly scaled
	 * object once it is rotated off its scale axis. The scene does not currently expose
	 * the difference (the only non-uniform scale, on Ceiling, is around the same axis it
	 * is rotated about, so the two commute), but they are not the same operation. This
	 * uses the local-space convention every other engine does.
	 */
	static TMatrix3x3 FromRotationScale(const TVector3<Real>& InEulerRadians, const TVector3<Real>& InScale)
	{
		TMatrix3x3 result = FromEulerRadians(InEulerRadians);

		// Column j scaled by InScale[j] == result * FromScale(InScale), without the multiply.
		for (uint32 row = 0u; row < 3u; ++row)
		{
			result.M[row][0] *= InScale.x;
			result.M[row][1] *= InScale.y;
			result.M[row][2] *= InScale.z;
		}

		return result;
	}


	// ----- Queries -----

	/** Column j is the image of basis vector j - i.e. the object's transformed axes. */
	constexpr TVector3<Real> GetColumn(uint32 InIndex) const
	{
		return TVector3<Real>(M[0][InIndex], M[1][InIndex], M[2][InIndex]);
	}

	constexpr TVector3<Real> GetRow(uint32 InIndex) const
	{
		return TVector3<Real>(M[InIndex][0], M[InIndex][1], M[InIndex][2]);
	}

	constexpr TMatrix3x3 Transposed() const
	{
		TMatrix3x3 result;
		for (uint32 row = 0u; row < 3u; ++row)
		{
			for (uint32 col = 0u; col < 3u; ++col)
			{
				result.M[row][col] = M[col][row];
			}
		}

		return result;
	}

	constexpr Real Determinant() const
	{
		return M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1])
		     - M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0])
		     + M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0]);
	}

	/** Undefined for a singular basis (zero scale on any axis); asserts in debug. */
	constexpr TMatrix3x3 Inverse() const
	{
		const Real det = Determinant();
		frt_assert(det != Real(0));

		const Real invDet = Real(1) / det;

		TMatrix3x3 result;
		result.M[0][0] = (M[1][1] * M[2][2] - M[1][2] * M[2][1]) * invDet;
		result.M[0][1] = (M[0][2] * M[2][1] - M[0][1] * M[2][2]) * invDet;
		result.M[0][2] = (M[0][1] * M[1][2] - M[0][2] * M[1][1]) * invDet;
		result.M[1][0] = (M[1][2] * M[2][0] - M[1][0] * M[2][2]) * invDet;
		result.M[1][1] = (M[0][0] * M[2][2] - M[0][2] * M[2][0]) * invDet;
		result.M[1][2] = (M[0][2] * M[1][0] - M[0][0] * M[1][2]) * invDet;
		result.M[2][0] = (M[1][0] * M[2][1] - M[1][1] * M[2][0]) * invDet;
		result.M[2][1] = (M[0][1] * M[2][0] - M[0][0] * M[2][1]) * invDet;
		result.M[2][2] = (M[0][0] * M[1][1] - M[0][1] * M[1][0]) * invDet;

		return result;
	}

	/** For transforming normals under non-uniform scale. */
	constexpr TMatrix3x3 InverseTransposed() const
	{
		return Inverse().Transposed();
	}

	static const TMatrix3x3 Identity;
};


template <concepts::Numerical T>
inline const TMatrix3x3<T> TMatrix3x3<T>::Identity = TMatrix3x3<T>{};


/** Applies Rhs first, then Lhs. */
template <concepts::Numerical T>
constexpr TMatrix3x3<T> operator*(const TMatrix3x3<T>& Lhs, const TMatrix3x3<T>& Rhs)
{
	TMatrix3x3<T> result;
	for (uint32 row = 0u; row < 3u; ++row)
	{
		for (uint32 col = 0u; col < 3u; ++col)
		{
			result.M[row][col] = Lhs.M[row][0] * Rhs.M[0][col]
			                   + Lhs.M[row][1] * Rhs.M[1][col]
			                   + Lhs.M[row][2] * Rhs.M[2][col];
		}
	}

	return result;
}


/**
 * Promotes to the wider of the two types, so a float basis transforming a double
 * translation yields a double result rather than silently truncating. This is what
 * makes a float Basis + double Translation world transform correct.
 */
template <concepts::Numerical TM, concepts::Numerical TV>
constexpr TVector3<std::common_type_t<TM, TV>> operator*(const TMatrix3x3<TM>& Lhs, const TVector3<TV>& Rhs)
{
	using TResult = std::common_type_t<TM, TV>;

	return TVector3<TResult>(
		static_cast<TResult>(Lhs.M[0][0]) * static_cast<TResult>(Rhs.x)
		+ static_cast<TResult>(Lhs.M[0][1]) * static_cast<TResult>(Rhs.y)
		+ static_cast<TResult>(Lhs.M[0][2]) * static_cast<TResult>(Rhs.z),

		static_cast<TResult>(Lhs.M[1][0]) * static_cast<TResult>(Rhs.x)
		+ static_cast<TResult>(Lhs.M[1][1]) * static_cast<TResult>(Rhs.y)
		+ static_cast<TResult>(Lhs.M[1][2]) * static_cast<TResult>(Rhs.z),

		static_cast<TResult>(Lhs.M[2][0]) * static_cast<TResult>(Rhs.x)
		+ static_cast<TResult>(Lhs.M[2][1]) * static_cast<TResult>(Rhs.y)
		+ static_cast<TResult>(Lhs.M[2][2]) * static_cast<TResult>(Rhs.z));
}


template <concepts::Numerical TTo, concepts::Numerical TFrom>
constexpr TMatrix3x3<TTo> MatrixCast(const TMatrix3x3<TFrom>& InMatrix)
{
	TMatrix3x3<TTo> result;
	for (uint32 row = 0u; row < 3u; ++row)
	{
		for (uint32 col = 0u; col < 3u; ++col)
		{
			result.M[row][col] = static_cast<TTo>(InMatrix.M[row][col]);
		}
	}

	return result;
}
}
