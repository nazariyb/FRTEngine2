#include "Camera.h"

void frt::graphics::CCamera::Tick(float DeltaSeconds)
{

}

DirectX::XMMATRIX frt::graphics::CCamera::GetViewMatrix() const
{
	const Vector3f Up = Vector3f(0, 0, 1.f);
	const_cast<CCamera*>(this)->LookDirection = Vector3f(1.f, 0.f, 0.f);
	const_cast<CCamera*>(this)->Position = Vector3f(-1.f, 0.f, 0.f);
	return DirectX::XMMatrixLookToRH(
		DirectX::XMLoadFloat3((DirectX::XMFLOAT3*)&Position),
		DirectX::XMLoadFloat3((DirectX::XMFLOAT3*)&LookDirection),
		DirectX::XMLoadFloat3((DirectX::XMFLOAT3*)&Up));
}

DirectX::XMMATRIX frt::graphics::CCamera::GetProjectionMatrix(float fov, float aspectRatio, float nearPlane,
	float farPlane) const
{
	return DirectX::XMMatrixPerspectiveFovRH(fov, aspectRatio, nearPlane, farPlane);
}
