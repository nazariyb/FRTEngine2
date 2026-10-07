#pragma once

#include <DirectXMath.h>
#include "Core.h"
#include "Math/Math.h"
#include "Math/Transform.h"


namespace frt::graphics
{
class FRT_CORE_API CCamera
{
public:
	void Tick (float DeltaSeconds);

	DirectX::XMMATRIX GetViewMatrix () const;
	DirectX::XMMATRIX GetProjectionMatrix (
		float fov,
		float aspectRatio,
		float nearPlane = 1.0f,
		float farPlane = 1000.0f) const;
	Vector3f GetLookDirection () const;

public:
	math::STransform Transform;
	float FieldOfViewY = 1.5707963f;     // radians (vertical)
	float NearZ = 0.05f;
	float MovementSpeed = 5.0f;
	float RotationSpeed = 0.5f;
};
}
