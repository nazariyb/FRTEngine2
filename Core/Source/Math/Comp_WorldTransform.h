#pragma once
#include "ECS/ComponentRegistry.h"
#include "Math/Comp_LocalTransform.h"
#include "Math/Math.h"


namespace frt
{
/**
 * World-space transform component, written once per frame by the transform system and
 * read by bounds, culling, and every extraction system.
 *
 * Basis and translation are stored separately because only translation needs range: a
 * rotation-and-scale basis is bounded no matter how large the world gets, so it stays
 * 32-bit while translation follows the build's configured precision. At 200k entities
 * that is 12 MB instead of 19 MB for a uniformly-double 3x4, and it states the
 * large-world argument structurally rather than in a comment.
 *
 * Stored in ENGINE space (left-up-forward). The handedness flip lives in
 * math::ToDirectXHandedness and is applied at upload, not here - otherwise every CPU
 * consumer silently works in a flipped space.
 *
 * Absolute translation never reaches the GPU. ToInstanceTransform subtracts the camera
 * to produce the camera-relative 32-bit affine matrix the acceleration structure wants.
 */
template <concepts::Numerical TReal>
struct TComp_WorldTransform
{
	using RealType		= TReal;
	using BasisType		= math::TMatrix3x3<float>;
	using VectorType	= math::TVector3<TReal>;

	BasisType	Basis		= BasisType::Identity;
	VectorType	Translation	= VectorType::ZeroVector;


	// ----- Composition -----

	/** For an entity with no parent. */
	static TComp_WorldTransform FromLocal(const TComp_LocalTransform<TReal>& InLocal)
	{
		return TComp_WorldTransform
		{
			math::MatrixCast<float>(
				math::TMatrix3x3<TReal>::FromRotationScale(InLocal.Rotation, InLocal.Scale)),
			InLocal.Translation
		};
	}

	/**
	 * Applies InLocal within InParent. Requires the parent to already hold its final
	 * world transform, which is why the transform system walks the hierarchy in depth
	 * order.
	 */
	static TComp_WorldTransform Compose(
		const TComp_WorldTransform& InParent,
		const TComp_LocalTransform<TReal>& InLocal)
	{
		const TComp_WorldTransform local = FromLocal(InLocal);

		return TComp_WorldTransform
		{
			InParent.Basis * local.Basis,
			InParent.Basis * local.Translation + InParent.Translation
		};
	}


	// ----- Queries -----

	constexpr VectorType TransformPoint(const VectorType& InPoint) const
	{
		return Basis * InPoint + Translation;
	}

	/** No translation involved, so directions stay 32-bit. */
	constexpr math::TVector3<float> TransformDirection(const math::TVector3<float>& InDirection) const
	{
		return Basis * InDirection;
	}

	/** Correct for normals under non-uniform scale, unlike TransformDirection. */
	math::TVector3<float> TransformNormal(const math::TVector3<float>& InNormal) const
	{
		return Basis.InverseTransposed() * InNormal;
	}

	/** The object's world-space axes: column 0 is X, 1 is Y, 2 is Z, each scaled. */
	constexpr math::TVector3<float> GetAxis(uint32 InIndex) const
	{
		return Basis.GetColumn(InIndex);
	}


	// ----- Render handoff -----

	/**
	 * Camera-relative 32-bit affine transform, ready for
	 * D3D12_RAYTRACING_INSTANCE_DESC::Transform.
	 *
	 * The subtraction happens at TReal precision and only its result is narrowed, which
	 * is the whole point of storing absolute translation wide: near the camera the
	 * relative offset is small, so 32 bits carry it accurately no matter how far from
	 * the origin the entity actually sits.
	 *
	 * Still engine space - run math::ToDirectXHandedness on the result at the backend
	 * boundary.
	 */
	constexpr math::TMatrix3x4<float> ToInstanceTransform(const VectorType& InCameraTranslation) const
	{
		const VectorType relative = Translation - InCameraTranslation;

		return math::TMatrix3x4<float>::FromBasisTranslation(Basis, math::VectorCast<float>(relative));
	}
};


using Comp_WorldTransform	= TComp_WorldTransform<Real>;
using Comp_WorldTransformF	= TComp_WorldTransform<float>;
using Comp_WorldTransformD	= TComp_WorldTransform<double>;
}

// Named for the same reason as Comp_LocalTransform: the alias target follows
// FRT_REAL_PRECISION.
FRT_DECLARE_COMPONENT_NAMED(frt::Comp_WorldTransform, "Comp_WorldTransform");
