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
	mutable DirectX::XMFLOAT4X4 Matrix4x4;
	mutable DirectX::XMMATRIX MatrixIntr;
#pragma warning(pop)

	Vector3f Translation;
	Vector3f Rotation;
	Vector3f Scale;

public:
	STransform ();

	const DirectX::XMFLOAT4X4& GetMatrix () const;
	DirectX::XMFLOAT3X4 GetRaytracingTransform () const;

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
	: Translation(Vector3f::ZeroVector)
	, Rotation(Vector3f::ZeroVector)
	, Scale(Vector3f::OneVector)
{
	MatrixIntr = DirectX::XMMatrixIdentity();
	DirectX::XMStoreFloat4x4(&Matrix4x4, MatrixIntr);
}

inline const DirectX::XMFLOAT4X4& STransform::GetMatrix () const
{
	using namespace DirectX;

	// Rotation * Scale * Translation * F, with F the engine (LUF) -> render (DirectX, RUF)
	// conversion: negating X of the WORLD result, v * M * F. This is the same conversion camera,
	// lights and portals use (ToDirectXCoordinates), so positions agree everywhere. Flipping the
	// local X instead (F * M) mirrors objects in place but leaves their translation unconverted,
	// which put meshes on the opposite side of X from the lights and the camera.
	//
	// Written out rather than as XMMatrixMultiply by scale, translation and flip matrices - the
	// same values exactly. MSVC 14.44's optimizer miscompiled the multiply chain in Release
	// (Core.dll's exported copy splatted the wrong lane for the translation row and dropped Z
	// translation), and the explicit form has nothing for it to fold.
	const XMMATRIX rotation = XMMatrixRotationRollPitchYaw(Rotation.x, Rotation.y, Rotation.z);
	const XMVECTOR rowScale = XMVectorSet(-Scale.x, Scale.y, Scale.z, 1.0f);
	MatrixIntr.r[0] = XMVectorMultiply(rotation.r[0], rowScale);
	MatrixIntr.r[1] = XMVectorMultiply(rotation.r[1], rowScale);
	MatrixIntr.r[2] = XMVectorMultiply(rotation.r[2], rowScale);
	MatrixIntr.r[3] = XMVectorSet(-Translation.x, Translation.y, Translation.z, 1.0f);

	XMStoreFloat4x4(&Matrix4x4, MatrixIntr);

	return Matrix4x4;
}

inline DirectX::XMFLOAT3X4 STransform::GetRaytracingTransform () const
{
	const DirectX::XMFLOAT4X4& m = GetMatrix();

	return DirectX::XMFLOAT3X4(
		m._11, m._21, m._31, m._41,
		m._12, m._22, m._32, m._42,
		m._13, m._23, m._33, m._43);
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
