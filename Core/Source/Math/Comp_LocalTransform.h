#pragma once
#include "ECS/ComponentRegistry.h"
#include "Math/Math.h"


namespace frt
{
/**
 * Local-space transform component. Plain data: no cached matrices and no lazy
 * recompute. Comp_WorldTransform holds the composed matrix,
 * and the transform system is what writes it.
 *
 * Rotation is Euler in radians, matching STransform's convention
 * (XMMatrixRotationRollPitchYaw: x = pitch, y = yaw, z = roll).
 *
 * Setters come in two flavours per operation:
 *
 *  - Matching precision, or widening (float into a double transform) - silent.
 *  - Narrowing (double into a float transform) - compiles and converts, but warns at
 *    the call site, because that loss is usually accidental rather than intended.
 *
 * Where the narrowing really is intended, say so and the warning goes away:
 *
 *     transform.SetTranslation(math::VectorCast<float>(WorldPosition));
 */
template <concepts::Numerical TReal>
struct TComp_LocalTransform
{
	using RealType		= TReal;
	using VectorType	= math::TVector3<TReal>;

	VectorType Translation	= VectorType::ZeroVector;
	VectorType Rotation		= VectorType::ZeroVector;
	VectorType Scale		= VectorType::OneVector;

	constexpr const VectorType& GetTranslation()	const { return Translation; }
	constexpr const VectorType& GetRotation()		const { return Rotation; }
	constexpr const VectorType& GetScale()			const { return Scale; }

	constexpr void SetTranslation(TReal X, TReal Y, TReal Z)		{ Translation = VectorType(X, Y, Z); }
	constexpr void SetTranslation(const VectorType& InTranslation)	{ Translation = InTranslation; }

	constexpr void SetRotation(TReal X, TReal Y, TReal Z)		{ Rotation = VectorType(X, Y, Z); }
	constexpr void SetRotation(const VectorType& InRotation)	{ Rotation = InRotation; }

	constexpr void SetScale(TReal InScale)				{ Scale = VectorType(InScale); }
	constexpr void SetScale(const VectorType& InScale)	{ Scale = InScale; }

	constexpr void MoveBy(const VectorType& InDelta)	{ Translation += InDelta; }
	constexpr void RotateBy(const VectorType& InDelta)	{ Rotation += InDelta; }
	constexpr void ScaleBy(TReal InDelta)				{ Scale *= InDelta; }


	// ----- Setters, narrowing -----
	// An exact match on TOther beats the conversion the overloads above would need, so
	// these win overload resolution precisely when the argument is wider than TReal.
	// TVector3 has no cross-type converting constructor, so for the vector forms these
	// are not merely preferred - they are the only viable candidate.

	template <concepts::Numerical TOther> requires concepts::LosesPrecision<TOther, TReal>
	FRT_WARN_ON_USE(
		"SetTranslation: argument is wider than this transform's precision. static_cast it to make the narrowing explicit.")
	constexpr void SetTranslation(TOther X, TOther Y, TOther Z)
	{
		Translation = VectorType(static_cast<TReal>(X), static_cast<TReal>(Y), static_cast<TReal>(Z));
	}

	template <concepts::Numerical TOther> requires concepts::LosesPrecision<TOther, TReal>
	FRT_WARN_ON_USE(
		"SetTranslation: argument is wider than this transform's precision. Wrap it in math::VectorCast to make the narrowing explicit.")
	constexpr void SetTranslation(const math::TVector3<TOther>& InTranslation)
	{
		Translation = math::VectorCast<TReal>(InTranslation);
	}

	template <concepts::Numerical TOther> requires concepts::LosesPrecision<TOther, TReal>
	FRT_WARN_ON_USE(
		"SetRotation: argument is wider than this transform's precision. static_cast it to make the narrowing explicit.")
	constexpr void SetRotation(TOther X, TOther Y, TOther Z)
	{
		Rotation = VectorType(static_cast<TReal>(X), static_cast<TReal>(Y), static_cast<TReal>(Z));
	}

	template <concepts::Numerical TOther> requires concepts::LosesPrecision<TOther, TReal>
	FRT_WARN_ON_USE(
		"SetRotation: argument is wider than this transform's precision. Wrap it in math::VectorCast to make the narrowing explicit.")
	constexpr void SetRotation(const math::TVector3<TOther>& InRotation)
	{
		Rotation = math::VectorCast<TReal>(InRotation);
	}

	template <concepts::Numerical TOther> requires concepts::LosesPrecision<TOther, TReal>
	FRT_WARN_ON_USE(
		"SetScale: argument is wider than this transform's precision. static_cast it to make the narrowing explicit.")
	constexpr void SetScale(TOther InScale)
	{
		Scale = VectorType(static_cast<TReal>(InScale));
	}

	template <concepts::Numerical TOther> requires concepts::LosesPrecision<TOther, TReal>
	FRT_WARN_ON_USE(
		"SetScale: argument is wider than this transform's precision. Wrap it in math::VectorCast to make the narrowing explicit.")
	constexpr void SetScale(const math::TVector3<TOther>& InScale)
	{
		Scale = math::VectorCast<TReal>(InScale);
	}

	template <concepts::Numerical TOther> requires concepts::LosesPrecision<TOther, TReal>
	FRT_WARN_ON_USE(
		"MoveBy: argument is wider than this transform's precision. Wrap it in math::VectorCast to make the narrowing explicit.")
	constexpr void MoveBy(const math::TVector3<TOther>& InDelta)
	{
		Translation += math::VectorCast<TReal>(InDelta);
	}

	template <concepts::Numerical TOther> requires concepts::LosesPrecision<TOther, TReal>
	FRT_WARN_ON_USE(
		"RotateBy: argument is wider than this transform's precision. Wrap it in math::VectorCast to make the narrowing explicit.")
	constexpr void RotateBy(const math::TVector3<TOther>& InDelta)
	{
		Rotation += math::VectorCast<TReal>(InDelta);
	}

	template <concepts::Numerical TOther> requires concepts::LosesPrecision<TOther, TReal>
	FRT_WARN_ON_USE(
		"ScaleBy: argument is wider than this transform's precision. static_cast it to make the narrowing explicit.")
	constexpr void ScaleBy(TOther InDelta)
	{
		Scale *= static_cast<TReal>(InDelta);
	}


	// ----- Precision conversion -----
	// Explicit, so it never warns. This is the intended way to hand a double-precision
	// transform to the 32-bit render and upload path.

	template <concepts::Numerical TOther>
	constexpr TComp_LocalTransform<TOther> To() const
	{
		return TComp_LocalTransform<TOther>
		{
			math::VectorCast<TOther>(Translation),
			math::VectorCast<TOther>(Rotation),
			math::VectorCast<TOther>(Scale)
		};
	}
};


using Comp_LocalTransform	= TComp_LocalTransform<Real>;
using Comp_LocalTransformF	= TComp_LocalTransform<float>;
using Comp_LocalTransformD	= TComp_LocalTransform<double>;
}

// Named rather than stringized: the type is an alias whose target depends on
// FRT_REAL_PRECISION, so a stringized name would differ between float and double builds
// and invalidate saved scenes across a precision switch.
FRT_DECLARE_COMPONENT_NAMED(frt::Comp_LocalTransform, "Comp_LocalTransform");
