#pragma once

#include "Core.h"
#include "Math/Math.h"
#include <DirectXMath.h>


namespace frt::graphics
{
	class FRT_CORE_API CCamera
	{
	public:
		void Tick(float DeltaSeconds);

		DirectX::XMMATRIX GetViewMatrix() const;
		DirectX::XMMATRIX GetProjectionMatrix(float fov, float aspectRatio, float nearPlane = 1.0f, float farPlane = 1000.0f) const;

	public:
		Vector3f Position;
		Vector3f LookDirection;
	};
}
