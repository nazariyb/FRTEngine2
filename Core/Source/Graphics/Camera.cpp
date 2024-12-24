#include "Camera.h"

void frt::graphics::CCamera::Tick(float DeltaSeconds)
{
	static float Direction = 1.f;
	Position.x += Direction * DeltaSeconds;
	if ((Direction  > 0. && Position.x > 100.f) ||
		(Direction  < 0. && Position.x < -100.f))
	{
		Direction *= -1.f;
	}
}

DirectX::XMMATRIX frt::graphics::CCamera::GetViewMatrix() const
{
	const Vector3f Up = Vector3f(0, 0, 1);
	return DirectX::XMMatrixLookToRH(
		DirectX::XMLoadFloat3((DirectX::XMFLOAT3*)&Position),
		DirectX::XMLoadFloat3((DirectX::XMFLOAT3*)&LookAt),
		DirectX::XMLoadFloat3((DirectX::XMFLOAT3*)&Up));
}

DirectX::XMMATRIX frt::graphics::CCamera::GetProjectionMatrix(float fov, float aspectRatio, float nearPlane,
	float farPlane) const
{
	return DirectX::XMMatrixPerspectiveFovRH(fov, aspectRatio, nearPlane, farPlane);
}
