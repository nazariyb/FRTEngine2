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

	const Vector3f& GetTranslation () const { return Translation; }
	const Vector3f& GetRotation () const { return Rotation; }
	const Vector3f& GetScale () const { return Scale; }

	void SetTranslation (float X, float Y, float Z);
	void SetTranslation (const Vector3f& InTranslation);

	void SetRotation (float X, float Y, float Z);
	void SetRotation (const Vector3f& InRotation);

	void SetScale (float InScale);
	void SetScale (const Vector3f& InScale);

	void MoveBy (const Vector3f& Delta);
	void RotateBy (const Vector3f& Delta);
	void ScaleBy (float Delta);
};


inline STransform::STransform ()
{
	MatrixIntr = DirectX::XMMatrixIdentity();
	DirectX::XMStoreFloat4x4(&Matrix4x4, MatrixIntr);
}

inline const DirectX::XMFLOAT4X4& STransform::GetMatrix ()
{
	using namespace DirectX;

	const XMMATRIX rotation = XMMatrixRotationRollPitchYaw(Rotation.x, Rotation.y, Rotation.z);
	const XMMATRIX scale = XMMatrixScaling(Scale.x, Scale.y, Scale.z);
	const XMMATRIX translation = XMMatrixTranslation(Translation.x, Translation.y, Translation.z);
	const XMMATRIX engineMatrix = XMMatrixMultiply(XMMatrixMultiply(rotation, scale), translation);
	const XMMATRIX lufToDx = XMMatrixScaling(-1.f, 1.f, 1.f);
	MatrixIntr = XMMatrixMultiply(lufToDx, engineMatrix);

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

inline void STransform::MoveBy (const Vector3f& Delta)
{
	Translation += Delta;
}

inline void STransform::RotateBy (const Vector3f& Delta)
{
	Rotation += Delta;
}

inline void STransform::ScaleBy (float Delta)
{
	Scale *= Delta;
}
}
