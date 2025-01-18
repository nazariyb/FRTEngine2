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
		STransform();

		const DirectX::XMFLOAT4X4& GetMatrix();

		void SetTranslation(float x, float y, float z);
		void SetRotation(float x, float y, float z);
		void SetScale(float scale);
	};


	inline STransform::STransform()
	{
		MatrixIntr = DirectX::XMMatrixIdentity();
		DirectX::XMStoreFloat4x4(&Matrix4x4, MatrixIntr);
	}

	inline const DirectX::XMFLOAT4X4& STransform::GetMatrix()
	{
		using namespace DirectX;

		MatrixIntr = XMMatrixMultiply(
			XMMatrixMultiply(
				XMMatrixRotationRollPitchYaw(Rotation.x, Rotation.z, Rotation.y),
				XMMatrixScaling(Scale.x, Scale.y, Scale.z)),
			XMMatrixTranslation(Translation.x, Translation.y, Translation.z));

		XMStoreFloat4x4(&Matrix4x4, MatrixIntr);

		return Matrix4x4;
	}

	inline void STransform::SetTranslation(float x, float y, float z)
	{
		Translation = Vector3f(x, y, z);
	}

	inline void STransform::SetRotation(float x, float y, float z)
	{
		Rotation = Vector3f(x, y, z);
	}

	inline void STransform::SetScale(float scale)
	{
		Scale = Vector3f(scale);
	}
}
