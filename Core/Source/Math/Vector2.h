#pragma once

#include <cmath>

#include "Core.h"
#include "CoreTypes.h"
#include "MathUtility.h"


NAMESPACE_FRT_START

namespace math
{

template<concepts::Numerical T>
struct TVector2
{
public:
	using Real = T;

	Real x;
	Real y;

	constexpr TVector2() : x(0), y(0) {}
	constexpr explicit TVector2(Real F) : x(F), y(F) {}
	constexpr TVector2(Real X, Real Y) : x(X), y(Y) {}

	TVector2(const TVector2<Real>&) = default;
	TVector2(TVector2<Real>&&) = default;

	TVector2<Real>& operator=(const TVector2<Real>&) = default;
	TVector2<Real>& operator=(TVector2<Real>&&) = default;

	TVector2<Real>& operator-();

	TVector2<Real>& operator+=(const TVector2<Real>& rhs);
	TVector2<Real>& operator-=(const TVector2<Real>& rhs);
	TVector2<Real>& operator*=(const TVector2<Real>& rhs);
	TVector2<Real>& operator/=(const TVector2<Real>& rhs);
	TVector2<Real>& operator+=(const Real& rhs);
	TVector2<Real>& operator-=(const Real& rhs);
	TVector2<Real>& operator*=(const Real& rhs);
	TVector2<Real>& operator/=(const Real& rhs);

	template<concepts::Numerical N> friend constexpr TVector2<N> operator+(const TVector2<N>& lhs, const TVector2<N>& rhs) noexcept;
	template<concepts::Numerical N> friend constexpr TVector2<N> operator-(const TVector2<N>& lhs, const TVector2<N>& rhs) noexcept;
	template<concepts::Numerical N> friend constexpr TVector2<N> operator*(const TVector2<N>& lhs, const TVector2<N>& rhs) noexcept;
	template<concepts::Numerical N> friend constexpr TVector2<N> operator/(const TVector2<N>& lhs, const TVector2<N>& rhs) noexcept;
	template<concepts::Numerical L, concepts::Numerical R> constexpr friend TVector2<L> operator+(const TVector2<L>& lhs, const R& rhs) noexcept;
	template<concepts::Numerical L, concepts::Numerical R> constexpr friend TVector2<L> operator-(const TVector2<L>& lhs, const R& rhs) noexcept;
	template<concepts::Numerical L, concepts::Numerical R> constexpr friend TVector2<L> operator*(const TVector2<L>& lhs, const R& rhs) noexcept;
	template<concepts::Numerical L, concepts::Numerical R> constexpr friend TVector2<L> operator/(const TVector2<L>& lhs, const R& rhs) noexcept;
	template<concepts::Numerical L, concepts::Numerical R> constexpr friend TVector2<R> operator+(const L& lhs, const TVector2<R>& rhs) noexcept;
	template<concepts::Numerical L, concepts::Numerical R> constexpr friend TVector2<R> operator-(const L& lhs, const TVector2<R>& rhs) noexcept;
	template<concepts::Numerical L, concepts::Numerical R> constexpr friend TVector2<R> operator*(const L& lhs, const TVector2<R>& rhs) noexcept;

	TVector2<T>& NormalizeUnsafe();
	TVector2<T> GetNormalizedUnsafe() const;

	Real Dot(const TVector2<Real>& rhs);

	// dot product between this vector and rhs, but both are normalized before
	Real CosDegrees(const TVector2<Real>& rhs);
	// dot product between this vector and rhs, but both are normalized before
	Real CosRadians(const TVector2<Real>& rhs);

	// dot product between this vector and normalized Rhs
	Real ProjectOnTo(const TVector2<Real>& rhs);
	// dot product between this vector and Rhs which is assumed to be normalized
	Real ProjectOnToNormal(const TVector2<Real>& rhs);

	static Real Dot(const TVector2<Real>& lhs, const TVector2<Real>& rhs);
	static Real Cos(const TVector2<Real>& lhs, const TVector2<Real>& rhs);

	// TODO: pseudo-cross
	TVector2<Real> Cross(const TVector2<Real>& rhs);
	static TVector2<Real> Cross(const TVector2<Real>& lhs, const TVector2<Real>& rhs);

	Real Size() const;
	Real SizeSquared() const;

	Real Dist(const TVector2<Real>& rhs) const;
	Real DistSquared(const TVector2<Real>& rhs) const;
	static Real Dist(const TVector2<Real>& lhs, const TVector2<Real>& rhs);
	static Real DistSquared(const TVector2<Real>& lhs, const TVector2<Real>& rhs);

	FRT_CORE_API static const TVector2<Real> ZeroVector;
	FRT_CORE_API static const TVector2<Real> OneVector;
	FRT_CORE_API static const TVector2<Real> UpVector;
	FRT_CORE_API static const TVector2<Real> DownVector;
	FRT_CORE_API static const TVector2<Real> ForwardVector;
	FRT_CORE_API static const TVector2<Real> DownwardVector;
	FRT_CORE_API static const TVector2<Real> RightVector;
	FRT_CORE_API static const TVector2<Real> LeftVector;
	FRT_CORE_API static const TVector2<Real> XAxisVector;
	FRT_CORE_API static const TVector2<Real> YAxisVector;
	FRT_CORE_API static const TVector2<Real> ZAxisVector;
	
};

template <concepts::Numerical Real>
TVector2<Real>& TVector2<Real>::operator-()
{
	return TVector2<Real>(-x, -y);
}

template <concepts::Numerical Real>
TVector2<Real>& TVector2<Real>::operator+=(const TVector2<Real>& rhs)
{
	x += rhs.x;
	y += rhs.y;
	return *this;
}

template <concepts::Numerical Real>
TVector2<Real>& TVector2<Real>::operator-=(const TVector2<Real>& rhs)
{
	x -= rhs.x;
	y -= rhs.y;
	return *this;
}

template <concepts::Numerical Real>
TVector2<Real>& TVector2<Real>::operator*=(const TVector2<Real>& rhs)
{
	x *= rhs.x;
	y *= rhs.y;
	return *this;
}

template <concepts::Numerical Real>
TVector2<Real>& TVector2<Real>::operator/=(const TVector2<Real>& rhs)
{
	x /= rhs.x;
	y /= rhs.y;
	return *this;
}

template <concepts::Numerical Real>
TVector2<Real>& TVector2<Real>::operator+=(const Real& rhs)
{
	x += rhs;
	y += rhs;
	return *this;
}

template <concepts::Numerical Real>
TVector2<Real>& TVector2<Real>::operator-=(const Real& rhs)
{
	x -= rhs;
	y -= rhs;
	return *this;
}

template <concepts::Numerical Real>
TVector2<Real>& TVector2<Real>::operator*=(const Real& rhs)
{
	x *= rhs;
	y *= rhs;
	return *this;
}

template <concepts::Numerical Real>
TVector2<Real>& TVector2<Real>::operator/=(const Real& rhs)
{
	x /= rhs;
	y /= rhs;
	return *this;
}

template<concepts::Numerical N>
constexpr TVector2<N> operator+(const TVector2<N>& lhs, const TVector2<N>& rhs) noexcept
{
	return TVector2<N>(lhs.x + rhs.x, lhs.y + rhs.y);
}

template<concepts::Numerical N>
constexpr TVector2<N> operator-(const TVector2<N>& lhs, const TVector2<N>& rhs) noexcept
{
	return TVector2<N>(lhs.x - rhs.x, lhs.y - rhs.y);
}

template<concepts::Numerical N>
constexpr TVector2<N> operator*(const TVector2<N>& lhs, const TVector2<N>& rhs) noexcept
{
	return TVector2<N>(lhs.x * rhs.x, lhs.y * rhs.y);
}

template<concepts::Numerical N>
constexpr TVector2<N> operator/(const TVector2<N>& lhs, const TVector2<N>& rhs) noexcept
{
	return TVector2<N>(lhs.x / rhs.x, lhs.y / rhs.y);
}

template<concepts::Numerical L, concepts::Numerical R>
constexpr TVector2<L> operator+(const TVector2<L>& lhs, const R& rhs) noexcept
{
	return TVector2<L>(lhs.x + rhs, lhs.y + rhs);
}

template<concepts::Numerical L, concepts::Numerical R>
constexpr TVector2<L> operator-(const TVector2<L>& lhs, const R& rhs) noexcept
{
	return TVector2<L>(lhs.x - rhs, lhs.y - rhs);
}

template<concepts::Numerical L, concepts::Numerical R>
constexpr TVector2<L> operator*(const TVector2<L>& lhs, const R& rhs) noexcept
{
	return TVector2<L>(lhs.x * rhs, lhs.y * rhs);
}

template<concepts::Numerical L, concepts::Numerical R>
constexpr TVector2<L> operator/(const TVector2<L>& lhs, const R& rhs) noexcept
{
	return TVector2<L>(lhs.x / rhs, lhs.y / rhs);
}

template<concepts::Numerical L, concepts::Numerical R>
constexpr TVector2<R> operator+(const L& lhs, const TVector2<R>& rhs) noexcept
{
	return rhs + lhs;
}

template<concepts::Numerical L, concepts::Numerical R>
constexpr TVector2<R> operator-(const L& lhs, const TVector2<R>& rhs) noexcept
{
	return -rhs + lhs;
}

template<concepts::Numerical L, concepts::Numerical R>
constexpr TVector2<R> operator*(const L& lhs, const TVector2<R>& rhs) noexcept
{
	return rhs * lhs;
}

template <concepts::Numerical T>
TVector2<T>& TVector2<T>::NormalizeUnsafe()
{
	const float magnitude = Size();
	x /= magnitude;
	y /= magnitude;
	return *this;
}

template <concepts::Numerical T>
TVector2<T> TVector2<T>::GetNormalizedUnsafe() const
{
	return TVector2<T>(NormalizeUnsafe());
}

template <concepts::Numerical Real>
Real TVector2<Real>::Dot(const TVector2<Real>& rhs)
{
	return x * rhs.x + y * rhs.y;
}

template <concepts::Numerical T>
typename TVector2<T>::Real TVector2<T>::CosDegrees(const TVector2<Real>& rhs)
{
	return frt::math::RadiansToDegrees(CosRadians(rhs));
}

template <concepts::Numerical T>
typename TVector2<T>::Real TVector2<T>::CosRadians(const TVector2<Real>& rhs)
{
	return Dot(GetNormalizedUnsafe(), rhs.GetNormalizedUnsafe());
}

template <concepts::Numerical T>
typename TVector2<T>::Real TVector2<T>::ProjectOnTo(const TVector2<Real>& rhs)
{
	return Dot(rhs);
}

template <concepts::Numerical T>
typename TVector2<T>::Real TVector2<T>::ProjectOnToNormal(const TVector2<Real>& rhs)
{
	return Dot(rhs.GetNormalizedUnsafe());
}

template <concepts::Numerical Real>
TVector2<Real> TVector2<Real>::Cross(const TVector2<Real>& rhs)
{
	return TVector2<Real>();
}

template <concepts::Numerical Real>
Real TVector2<Real>::Dot(const TVector2<Real>& lhs, const TVector2<Real>& rhs)
{
	return lhs.x * rhs.x + lhs.y * rhs.y;
}

template <concepts::Numerical Real>
Real TVector2<Real>::Cos(const TVector2<Real>& lhs, const TVector2<Real>& rhs)
{
	return lhs.x * rhs.x + lhs.y * rhs.y;
}

template <concepts::Numerical Real>
TVector2<Real> TVector2<Real>::Cross(const TVector2<Real>& lhs, const TVector2<Real>& rhs)
{
	return lhs.Cross(rhs);
}

template <concepts::Numerical Real>
Real TVector2<Real>::Size() const
{
	return std::sqrt(x * x + y * y);
}

template <concepts::Numerical Real>
Real TVector2<Real>::SizeSquared() const
{
	return x * x + y * y;
}

template <concepts::Numerical Real>
Real TVector2<Real>::Dist(const TVector2<Real>& rhs) const
{
	return std::sqrt(x * x + y * y);
}

template <concepts::Numerical Real>
Real TVector2<Real>::DistSquared(const TVector2<Real>& rhs) const
{
	return (*this - rhs).Size();
}

template <concepts::Numerical Real>
Real TVector2<Real>::Dist(const TVector2<Real>& lhs, const TVector2<Real>& rhs)
{
	return lhs.Dist(rhs);
}

template <concepts::Numerical Real>
Real TVector2<Real>::DistSquared(const TVector2<Real>& lhs, const TVector2<Real>& rhs)
{
	return lhs.DistSquared(rhs);
}

}

NAMESPACE_FRT_END
