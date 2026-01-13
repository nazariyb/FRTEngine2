#pragma once

#include <DirectXMath.h>

#include "Core.h"
#include "Math.h"


namespace frt::math
{
struct FRT_CORE_API STransform
{
	using RawType = DirectX::XMFLOAT4X4;

private:
#pragma warning(push)
#pragma warning(disable: 4251)
	DirectX::XMFLOAT4X4 Matrix4x4;
	DirectX::XMMATRIX MatrixIntr;
#pragma warning(pop)

	Vector3f Translation;
	Vector3f Rotation;
	Vector3f Scale;

public:
	STransform ();

	const DirectX::XMFLOAT4X4& GetMatrix ();

	void SetTranslation (float X, float Y, float Z);
	void SetTranslation (const Vector3f& InTranslation);

	void SetRotation (float X, float Y, float Z);
	void SetRotation (const Vector3f& InRotation);

	void SetScale (float InScale);
	void SetScale (const Vector3f& InScale);
};


inline STransform::STransform ()
{
	MatrixIntr = DirectX::XMMatrixIdentity();
	DirectX::XMStoreFloat4x4(&Matrix4x4, MatrixIntr);
}

inline const DirectX::XMFLOAT4X4& STransform::GetMatrix ()
{
	using namespace DirectX;

	MatrixIntr = XMMatrixMultiply(
		XMMatrixMultiply(
			XMMatrixRotationRollPitchYaw(Rotation.x, Rotation.z, Rotation.y),
			XMMatrixScaling(Scale.x, Scale.y, Scale.z)),
		XMMatrixTranslation(Translation.x, Translation.z, Translation.y));

	XMStoreFloat4x4(&Matrix4x4, MatrixIntr);

	return Matrix4x4;
}

inline void STransform::SetTranslation (float X, float Y, float Z)
{
	Translation = Vector3f(X, Y, Z);
}

inline void STransform::SetTranslation (const Vector3f& InTranslation)
{
	Translation = InTranslation;
}

inline void STransform::SetRotation (float X, float Y, float Z)
{
	Rotation = Vector3f(X, Y, Z);
}

inline void STransform::SetRotation (const Vector3f& InRotation)
{
	Rotation = InRotation;
}

inline void STransform::SetScale (float InScale)
{
	Scale = Vector3f(InScale);
}

inline void STransform::SetScale (const Vector3f& InScale)
{
	Scale = InScale;
}
}
