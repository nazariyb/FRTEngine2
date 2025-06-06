#pragma once

#include <cmath>

#include "Core.h"
#include "Math.h"
#include "MathUtility.h"


NAMESPACE_FRT_START

namespace math
{

template<typename T>
struct TVector3
{
	static_assert(std::is_floating_point_v<T>, "T must be a floating point number");

public:
	using Real = T;

	Real x;
	Real y;
	Real z;

	constexpr TVector3() : x(0), y(0), z(0) {}
	constexpr explicit TVector3(Real F) : x(F), y(F), z(F) {}
	constexpr TVector3(Real X, Real Y) : x(X), y(Y), z(0) {}
	constexpr TVector3(Real X, Real Y, Real Z) : x(X), y(Y), z(Z) {}

	TVector3(const TVector3<Real>&) = default;
	TVector3(TVector3<Real>&&) = default;

	TVector3<Real>& operator=(const TVector3<Real>&) = default;
	TVector3<Real>& operator=(TVector3<Real>&&) = default;

	TVector3<Real>& operator-();

	TVector3<Real>& operator+=(const TVector3<Real>& rhs);
	TVector3<Real>& operator-=(const TVector3<Real>& rhs);
	TVector3<Real>& operator*=(const TVector3<Real>& rhs);
	TVector3<Real>& operator/=(const TVector3<Real>& rhs);
	TVector3<Real>& operator+=(const Real& rhs);
	TVector3<Real>& operator-=(const Real& rhs);
	TVector3<Real>& operator*=(const Real& rhs);
	TVector3<Real>& operator/=(const Real& rhs);

	template<concepts::Numerical N> friend constexpr TVector3<N> operator+(const TVector3<N>& lhs, const TVector3<N>& rhs) noexcept;
	template<concepts::Numerical N> friend constexpr TVector3<N> operator-(const TVector3<N>& lhs, const TVector3<N>& rhs) noexcept;
	template<concepts::Numerical N> friend constexpr TVector3<N> operator*(const TVector3<N>& lhs, const TVector3<N>& rhs) noexcept;
	template<concepts::Numerical N> friend constexpr TVector3<N> operator/(const TVector3<N>& lhs, const TVector3<N>& rhs) noexcept;
	template<concepts::Numerical L, concepts::Numerical R> constexpr friend TVector3<L> operator+(const TVector3<L>& lhs, const R& rhs) noexcept;
	template<concepts::Numerical L, concepts::Numerical R> constexpr friend TVector3<L> operator-(const TVector3<L>& lhs, const R& rhs) noexcept;
	template<concepts::Numerical L, concepts::Numerical R> constexpr friend TVector3<L> operator*(const TVector3<L>& lhs, const R& rhs) noexcept;
	template<concepts::Numerical L, concepts::Numerical R> constexpr friend TVector3<L> operator/(const TVector3<L>& lhs, const R& rhs) noexcept;
	template<concepts::Numerical L, concepts::Numerical R> constexpr friend TVector3<R> operator+(const L& lhs, const TVector3<R>& rhs) noexcept;
	template<concepts::Numerical L, concepts::Numerical R> constexpr friend TVector3<R> operator-(const L& lhs, const TVector3<R>& rhs) noexcept;
	template<concepts::Numerical L, concepts::Numerical R> constexpr friend TVector3<R> operator*(const L& lhs, const TVector3<R>& rhs) noexcept;

	TVector3<T>& NormalizeUnsafe();
	TVector3<T> GetNormalizedUnsafe() const;

	Real Dot(const TVector3<Real>& rhs);

	// dot product between this vector and rhs, but both are normalized before
	Real CosDegrees(const TVector3<Real>& rhs);
	// dot product between this vector and rhs, but both are normalized before
	Real CosRadians(const TVector3<Real>& rhs);

	// dot product between this vector and normalized Rhs
	Real ProjectOnTo(const TVector3<Real>& rhs);
	// dot product between this vector and Rhs which is assumed to be normalized
	Real ProjectOnToNormal(const TVector3<Real>& rhs);

	static Real Dot(const TVector3<Real>& lhs, const TVector3<Real>& rhs);
	static Real Cos(const TVector3<Real>& lhs, const TVector3<Real>& rhs);

	TVector3<Real> Cross(const TVector3<Real>& rhs);
	static TVector3<Real> Cross(const TVector3<Real>& lhs, const TVector3<Real>& rhs);

	Real Size() const;
	Real SizeSquared() const;

	Real Dist(const TVector3<Real>& rhs) const;
	Real DistSquared(const TVector3<Real>& rhs) const;
	static Real Dist(const TVector3<Real>& lhs, const TVector3<Real>& rhs);
	static Real DistSquared(const TVector3<Real>& lhs, const TVector3<Real>& rhs);

	static const TVector3<Real> ZeroVector;
	static const TVector3<Real> OneVector;
	static const TVector3<Real> UpVector;
	static const TVector3<Real> DownVector;
	static const TVector3<Real> ForwardVector;
	static const TVector3<Real> DownwardVector;
	static const TVector3<Real> RightVector;
	static const TVector3<Real> LeftVector;
	static const TVector3<Real> XAxisVector;
	static const TVector3<Real> YAxisVector;
	static const TVector3<Real> ZAxisVector;

};

template <typename Real>
TVector3<Real>& TVector3<Real>::operator-()
{
	return TVector3<Real>(-x, -y, -z);
}

template <typename Real>
TVector3<Real>& TVector3<Real>::operator+=(const TVector3<Real>& rhs)
{
	x += rhs.x;
	y += rhs.y;
	z += rhs.z;
	return *this;
}

template <typename Real>
TVector3<Real>& TVector3<Real>::operator-=(const TVector3<Real>& rhs)
{
	x -= rhs.x;
	y -= rhs.y;
	z -= rhs.z;
	return *this;
}

template <typename Real>
TVector3<Real>& TVector3<Real>::operator*=(const TVector3<Real>& rhs)
{
	x *= rhs.x;
	y *= rhs.y;
	z *= rhs.z;
	return *this;
}

template <typename Real>
TVector3<Real>& TVector3<Real>::operator/=(const TVector3<Real>& rhs)
{
	x /= rhs.x;
	y /= rhs.y;
	z /= rhs.z;
	return *this;
}

template <typename Real>
TVector3<Real>& TVector3<Real>::operator+=(const Real& rhs)
{
	x += rhs;
	y += rhs;
	z += rhs;
	return *this;
}

template <typename Real>
TVector3<Real>& TVector3<Real>::operator-=(const Real& rhs)
{
	x -= rhs;
	y -= rhs;
	z -= rhs;
	return *this;
}

template <typename Real>
TVector3<Real>& TVector3<Real>::operator*=(const Real& rhs)
{
	x *= rhs;
	y *= rhs;
	z *= rhs;
	return *this;
}

template <typename Real>
TVector3<Real>& TVector3<Real>::operator/=(const Real& rhs)
{
	x /= rhs;
	y /= rhs;
	z /= rhs;
	return *this;
}

template<concepts::Numerical N>
constexpr TVector3<N> operator+(const TVector3<N>& lhs, const TVector3<N>& rhs) noexcept
{
	return TVector3<N>(lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z);
}

template<concepts::Numerical N>
constexpr TVector3<N> operator-(const TVector3<N>& lhs, const TVector3<N>& rhs) noexcept
{
	return TVector3<N>(lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z);
}

template<concepts::Numerical N>
constexpr TVector3<N> operator*(const TVector3<N>& lhs, const TVector3<N>& rhs) noexcept
{
	return TVector3<N>(lhs.x * rhs.x, lhs.y * rhs.y, lhs.z * rhs.z);
}

template<concepts::Numerical N>
constexpr TVector3<N> operator/(const TVector3<N>& lhs, const TVector3<N>& rhs) noexcept
{
	return TVector3<N>(lhs.x / rhs.x, lhs.y / rhs.y, lhs.z / rhs.z);
}

template<concepts::Numerical L, concepts::Numerical R>
constexpr TVector3<L> operator+(const TVector3<L>& lhs, const R& rhs) noexcept
{
	return TVector3<L>(lhs.x + rhs, lhs.y + rhs, lhs.z + rhs);
}

template<concepts::Numerical L, concepts::Numerical R>
constexpr TVector3<L> operator-(const TVector3<L>& lhs, const R& rhs) noexcept
{
	return TVector3<L>(lhs.x - rhs, lhs.y - rhs, lhs.z - rhs);
}

template<concepts::Numerical L, concepts::Numerical R>
constexpr TVector3<L> operator*(const TVector3<L>& lhs, const R& rhs) noexcept
{
	return TVector3<L>(lhs.x * rhs, lhs.y * rhs, lhs.z * rhs);
}

template<concepts::Numerical L, concepts::Numerical R>
constexpr TVector3<L> operator/(const TVector3<L>& lhs, const R& rhs) noexcept
{
	return TVector3<L>(lhs.x / rhs, lhs.y / rhs, lhs.z / rhs);
}

template<concepts::Numerical L, concepts::Numerical R>
constexpr TVector3<R> operator+(const L& lhs, const TVector3<R>& rhs) noexcept
{
	return rhs + lhs;
}

template<concepts::Numerical L, concepts::Numerical R>
constexpr TVector3<R> operator-(const L& lhs, const TVector3<R>& rhs) noexcept
{
	return -rhs + lhs;
}

template<concepts::Numerical L, concepts::Numerical R>
constexpr TVector3<R> operator*(const L& lhs, const TVector3<R>& rhs) noexcept
{
	return rhs * lhs;
}

template <typename T>
TVector3<T>& TVector3<T>::NormalizeUnsafe()
{
	const float magnitude = Size();
	x /= magnitude;
	y /= magnitude;
	z /= magnitude;
	return *this;
}

template <typename T>
TVector3<T> TVector3<T>::GetNormalizedUnsafe() const
{
	return TVector3<T>(*this).NormalizeUnsafe();
}

template <typename Real>
Real TVector3<Real>::Dot(const TVector3<Real>& rhs)
{
	return x * rhs.x + y * rhs.y + z * rhs.z;
}

template <typename T>
typename TVector3<T>::Real TVector3<T>::CosDegrees(const TVector3<Real>& rhs)
{
	return frt::math::RadiansToDegrees(CosRadians(rhs));
}

template <typename T>
typename TVector3<T>::Real TVector3<T>::CosRadians(const TVector3<Real>& rhs)
{
	return Dot(GetNormalizedUnsafe(), rhs.GetNormalizedUnsafe());
}

template <typename T>
typename TVector3<T>::Real TVector3<T>::ProjectOnTo(const TVector3<Real>& rhs)
{
	return Dot(rhs);
}

template <typename T>
typename TVector3<T>::Real TVector3<T>::ProjectOnToNormal(const TVector3<Real>& rhs)
{
	return Dot(rhs.GetNormalizedUnsafe());
}

template <typename Real>
TVector3<Real> TVector3<Real>::Cross(const TVector3<Real>& rhs)
{
	return TVector3<Real>
		(
		y * rhs.z - z * rhs.y,
		z * rhs.x - x * rhs.z,
		x * rhs.y - y * rhs.x
		);
}

template <typename Real>
Real TVector3<Real>::Dot(const TVector3<Real>& lhs, const TVector3<Real>& rhs)
{
	return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z;
}

template <typename Real>
Real TVector3<Real>::Cos(const TVector3<Real>& lhs, const TVector3<Real>& rhs)
{
	return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z;
}

template <typename Real>
TVector3<Real> TVector3<Real>::Cross(const TVector3<Real>& lhs, const TVector3<Real>& rhs)
{
	return lhs.Cross(rhs);
}

template <typename Real>
Real TVector3<Real>::Size() const
{
	return std::sqrt(x * x + y * y + z * z);
}

template <typename Real>
Real TVector3<Real>::SizeSquared() const
{
	return x * x + y * y + z * z;
}

template <typename Real>
Real TVector3<Real>::Dist(const TVector3<Real>& rhs) const
{
	const Real dx = x - rhs.x;
	const Real dy = y - rhs.y;
	const Real dz = z - rhs.z;
	return std::sqrt(dx * dx + dy * dy + dz * dz);
}

template <typename Real>
Real TVector3<Real>::DistSquared(const TVector3<Real>& rhs) const
{
	const Real dx = x - rhs.x;
	const Real dy = y - rhs.y;
	const Real dz = z - rhs.z;
	return dx * dx + dy * dy + dz * dz;
}

template <typename Real>
Real TVector3<Real>::Dist(const TVector3<Real>& lhs, const TVector3<Real>& rhs)
{
	return lhs.Dist(rhs);
}

template <typename Real>
Real TVector3<Real>::DistSquared(const TVector3<Real>& lhs, const TVector3<Real>& rhs)
{
	return lhs.DistSquared(rhs);
}

}

NAMESPACE_FRT_END
