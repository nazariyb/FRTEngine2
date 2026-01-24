#include "Camera.h"

void frt::graphics::CCamera::Tick (float DeltaSeconds)
{}

DirectX::XMMATRIX frt::graphics::CCamera::GetViewMatrix () const
{
	const Vector3f up = Vector3f::UpVector;
	const Vector3f upDx = math::ToDirectXCoordinates(up);

	const Vector3f positionDx = math::ToDirectXCoordinates(Transform.GetTranslation());
	const Vector3f directionDx = math::ToDirectXCoordinates(GetLookDirection());

	return DirectX::XMMatrixLookToLH(
		DirectX::XMLoadFloat3(reinterpret_cast<const DirectX::XMFLOAT3*>(&positionDx)),
		DirectX::XMLoadFloat3(reinterpret_cast<const DirectX::XMFLOAT3*>(&directionDx)),
		DirectX::XMLoadFloat3(reinterpret_cast<const DirectX::XMFLOAT3*>(&upDx)));
}

DirectX::XMMATRIX frt::graphics::CCamera::GetProjectionMatrix (
	float fov,
	float aspectRatio,
	float nearPlane,
	float farPlane) const
{
	return DirectX::XMMatrixPerspectiveFovLH(fov, aspectRatio, nearPlane, farPlane);
}

Vector3f frt::graphics::CCamera::GetLookDirection () const
{
	using namespace DirectX;

	const Vector3f& rot = Transform.GetRotation(); // pitch, yaw, roll (radians)
	const XMMATRIX rotM = XMMatrixRotationRollPitchYaw(rot.x, rot.y, rot.z);

	const XMVECTOR f = XMLoadFloat3(reinterpret_cast<const XMFLOAT3*>(&Vector3f::ForwardVector));

	const XMVECTOR dir = XMVector3TransformNormal(f, rotM);

	Vector3f out{};
	XMStoreFloat3(reinterpret_cast<XMFLOAT3*>(&out), dir);
	return out;
}
