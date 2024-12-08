#pragma once

#include <cmath>

#include "Core.h"
#include "MathUtility.h"


NAMESPACE_FRT_START

namespace math
{

template<typename T>
struct Vector3
{
	static_assert(std::is_floating_point_v<T>, "T must be a floating point number");

public:
	using Real = T;

	Real x;
	Real y;
	Real z;

	Vector3() : x(0), y(0), z(0) {}
	explicit Vector3(Real F) : x(F), y(F), z(F) {}
	Vector3(Real X, Real Y) : x(X), y(Y), z(0) {}
	Vector3(Real X, Real Y, Real Z) : x(X), y(Y), z(Z) {}

	Vector3(const Vector3<Real>&) = default;
	Vector3(Vector3<Real>&&) = default;

	Vector3<Real>& operator=(const Vector3<Real>&) = default;
	Vector3<Real>& operator=(Vector3<Real>&&) = default;

	Vector3<Real>& operator-();

	Vector3<Real>& operator+=(const Vector3<Real>& rhs);
	Vector3<Real>& operator-=(const Vector3<Real>& rhs);
	Vector3<Real>& operator*=(const Vector3<Real>& rhs);
	Vector3<Real>& operator/=(const Vector3<Real>& rhs);
	Vector3<Real>& operator+=(const Real& rhs);
	Vector3<Real>& operator-=(const Real& rhs);
	Vector3<Real>& operator*=(const Real& rhs);
	Vector3<Real>& operator/=(const Real& rhs);

	Vector3<Real>& operator+(const Vector3<Real>& rhs);
	Vector3<Real>& operator-(const Vector3<Real>& rhs);
	Vector3<Real>& operator*(const Vector3<Real>& rhs);
	Vector3<Real>& operator/(const Vector3<Real>& rhs);
	Vector3<Real>& operator+(const Real& rhs);
	Vector3<Real>& operator-(const Real& rhs);
	Vector3<Real>& operator*(const Real& rhs);
	Vector3<Real>& operator/(const Real& rhs);

	Vector3<T>& NormalizeUnsafe();
	Vector3<T> GetNormalizedUnsafe() const;

	Real Dot(const Vector3<Real>& rhs);

	// dot product between this vector and rhs, but both are normalized before
	Real CosDegrees(const Vector3<Real>& rhs);
	// dot product between this vector and rhs, but both are normalized before
	Real CosRadians(const Vector3<Real>& rhs);

	// dot product between this vector and normalized Rhs
	Real ProjectOnTo(const Vector3<Real>& rhs);
	// dot product between this vector and Rhs which is assumed to be normalized
	Real ProjectOnToNormal(const Vector3<Real>& rhs);

	static Real Dot(const Vector3<Real>& lhs, const Vector3<Real>& rhs);
	static Real Cos(const Vector3<Real>& lhs, const Vector3<Real>& rhs);

	Vector3<Real> Cross(const Vector3<Real>& rhs);
	static Vector3<Real> Cross(const Vector3<Real>& lhs, const Vector3<Real>& rhs);

	Real Size() const;
	Real SizeSquared() const;

	Real Dist(const Vector3<Real>& rhs) const;
	Real DistSquared(const Vector3<Real>& rhs) const;
	static Real Dist(const Vector3<Real>& lhs, const Vector3<Real>& rhs);
	static Real DistSquared(const Vector3<Real>& lhs, const Vector3<Real>& rhs);

	FRT_CORE_API static const Vector3<Real> ZeroVector;
	FRT_CORE_API static const Vector3<Real> OneVector;
	FRT_CORE_API static const Vector3<Real> UpVector;
	FRT_CORE_API static const Vector3<Real> DownVector;
	FRT_CORE_API static const Vector3<Real> ForwardVector;
	FRT_CORE_API static const Vector3<Real> DownwardVector;
	FRT_CORE_API static const Vector3<Real> RightVector;
	FRT_CORE_API static const Vector3<Real> LeftVector;
	FRT_CORE_API static const Vector3<Real> XAxisVector;
	FRT_CORE_API static const Vector3<Real> YAxisVector;
	FRT_CORE_API static const Vector3<Real> ZAxisVector;
	
};

template <typename Real>
Vector3<Real>& Vector3<Real>::operator-()
{
	return TVector3<Real>(-x, -y, -z);
}

template <typename Real>
Vector3<Real>& Vector3<Real>::operator+=(const Vector3<Real>& rhs)
{
	x += rhs.x;
	y += rhs.y;
	z += rhs.z;
	return *this;
}

template <typename Real>
Vector3<Real>& Vector3<Real>::operator-=(const Vector3<Real>& rhs)
{
	x -= rhs.x;
	y -= rhs.y;
	z -= rhs.z;
	return *this;
}

template <typename Real>
Vector3<Real>& Vector3<Real>::operator*=(const Vector3<Real>& rhs)
{
	x *= rhs.x;
	y *= rhs.y;
	z *= rhs.z;
	return *this;
}

template <typename Real>
Vector3<Real>& Vector3<Real>::operator/=(const Vector3<Real>& rhs)
{
	x /= rhs.x;
	y /= rhs.y;
	z /= rhs.z;
	return *this;
}

template <typename Real>
Vector3<Real>& Vector3<Real>::operator+=(const Real& rhs)
{
	x += rhs;
	y += rhs;
	z += rhs;
	return *this;
}

template <typename Real>
Vector3<Real>& Vector3<Real>::operator-=(const Real& rhs)
{
	x -= rhs;
	y -= rhs;
	z -= rhs;
	return *this;
}

template <typename Real>
Vector3<Real>& Vector3<Real>::operator*=(const Real& rhs)
{
	x *= rhs;
	y *= rhs;
	z *= rhs;
	return *this;
}

template <typename Real>
Vector3<Real>& Vector3<Real>::operator/=(const Real& rhs)
{
	x /= rhs;
	y /= rhs;
	z /= rhs;
	return *this;
}

template <typename Real>
Vector3<Real>& Vector3<Real>::operator+(const Vector3<Real>& rhs)
{
	return TVector3<Real>(x + rhs.x, y + rhs.y, z + rhs.z);
}

template <typename Real>
Vector3<Real>& Vector3<Real>::operator-(const Vector3<Real>& rhs)
{
	return TVector3<Real>(x - rhs.x, y - rhs.y, z - rhs.z);
}

template <typename Real>
Vector3<Real>& Vector3<Real>::operator*(const Vector3<Real>& rhs)
{
	return TVector3<Real>(x * rhs.x, y * rhs.y, z * rhs.z);
}

template <typename Real>
Vector3<Real>& Vector3<Real>::operator/(const Vector3<Real>& rhs)
{
	return TVector3<Real>(x / rhs.x, y / rhs.y, z / rhs.z);
}

template <typename Real>
Vector3<Real>& Vector3<Real>::operator+(const Real& rhs)
{
	return TVector3<Real>(x + rhs, y + rhs, z + rhs);
}

template <typename Real>
Vector3<Real>& Vector3<Real>::operator-(const Real& rhs)
{
	return TVector3<Real>(x - rhs, y - rhs, z - rhs);
}

template <typename Real>
Vector3<Real>& Vector3<Real>::operator*(const Real& rhs)
{
	return TVector3<Real>(x * rhs, y * rhs, z * rhs);
}

template <typename Real>
Vector3<Real>& Vector3<Real>::operator/(const Real& rhs)
{
	return TVector3<Real>(x / rhs, y / rhs, z / rhs);
}

template <typename T>
Vector3<T>& Vector3<T>::NormalizeUnsafe()
{
	const float magnitude = Size();
	x /= magnitude;
	y /= magnitude;
	z /= magnitude;
	return *this;
}

template <typename T>
Vector3<T> Vector3<T>::GetNormalizedUnsafe() const
{
	return TVector3<T>(NormalizeUnsafe());
}

template <typename Real>
Real Vector3<Real>::Dot(const Vector3<Real>& rhs)
{
	return x * rhs.x + y * rhs.y + z * rhs.z;
}

template <typename T>
typename Vector3<T>::Real Vector3<T>::CosDegrees(const Vector3<Real>& rhs)
{
	return frt::math::RadiansToDegrees(CosRadians(rhs));
}

template <typename T>
typename Vector3<T>::Real Vector3<T>::CosRadians(const Vector3<Real>& rhs)
{
	return Dot(GetNormalizedUnsafe(), rhs.GetNormalizedUnsafe());
}

template <typename T>
typename Vector3<T>::Real Vector3<T>::ProjectOnTo(const Vector3<Real>& rhs)
{
	return Dot(rhs);
}

template <typename T>
typename Vector3<T>::Real Vector3<T>::ProjectOnToNormal(const Vector3<Real>& rhs)
{
	return Dot(rhs.GetNormalizedUnsafe());
}

template <typename Real>
Vector3<Real> Vector3<Real>::Cross(const Vector3<Real>& rhs)
{
	return TVector3<Real>
		(
		y * rhs.z - z * rhs.y,
		z * rhs.x - x * rhs.z,
		x * rhs.y - y * rhs.x
		);
}

template <typename Real>
Real Vector3<Real>::Dot(const Vector3<Real>& lhs, const Vector3<Real>& rhs)
{
	return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z;
}

template <typename Real>
Real Vector3<Real>::Cos(const Vector3<Real>& lhs, const Vector3<Real>& rhs)
{
	return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z;
}

template <typename Real>
Vector3<Real> Vector3<Real>::Cross(const Vector3<Real>& lhs, const Vector3<Real>& rhs)
{
	return lhs.Cross(rhs);
}

template <typename Real>
Real Vector3<Real>::Size() const
{
	return std::sqrt(x * x + y * y + z * z);
}

template <typename Real>
Real Vector3<Real>::SizeSquared() const
{
	return x * x + y * y + z * z;
}

template <typename Real>
Real Vector3<Real>::Dist(const Vector3<Real>& rhs) const
{
	return std::sqrt(x * x + y * y + z * z);
}

template <typename Real>
Real Vector3<Real>::DistSquared(const Vector3<Real>& rhs) const
{
	return (*this - rhs).Size();
}

template <typename Real>
Real Vector3<Real>::Dist(const Vector3<Real>& lhs, const Vector3<Real>& rhs)
{
	return lhs.Dist(rhs);
}

template <typename Real>
Real Vector3<Real>::DistSquared(const Vector3<Real>& lhs, const Vector3<Real>& rhs)
{
	return lhs.DistSquared(rhs);
}

}

NAMESPACE_FRT_END
