#include "Graphics/FlyCamera.h"

#include <DirectXMath.h>

#include "Graphics/Camera.h"
#include "Input/InputActions.h"
#include "Input/InputSystem.h"
#include "Math/MathUtility.h"


namespace frt::graphics
{
namespace
{
float AxisValue (const input::CInputActionMap& Actions, const char* Name)
{
	const input::SInputActionState* state = Actions.FindActionState(Name);
	return state ? state->Value : 0.0f;
}
}

SFlyCameraInput ReadFlyCameraInput (const input::CInputActionMap& Actions, const input::CInputSystem& Device, const Vector2f& LookDelta)
{
	SFlyCameraInput out;
	const input::SInputActionState* enableMove = Actions.FindActionState("IA_EnableMove");
	out.bActive = enableMove && enableMove->bDown;
	if (!out.bActive)
	{
		return out;
	}
	out.LookDelta = LookDelta;
	out.Move = Vector3f::ForwardVector * AxisValue(Actions, "IA_MoveForward")
		+ Vector3f::LeftVector * AxisValue(Actions, "IA_MoveLeft")
		+ Vector3f::UpVector * AxisValue(Actions, "IA_MoveUp");
	out.Wheel = Device.GetMouseWheelDelta();
	return out;
}

void UpdateFlyCamera (CCamera& Camera, const SFlyCameraInput& Input, float DeltaSeconds, float ViewAspect)
{
	if (!Input.bActive)
	{
		return;
	}

	// Look
	const Vector2f look = Input.LookDelta * Camera.RotationSpeed;
	const float aspect = math::Max(ViewAspect, 1e-3f);
	Vector3f rotation = Camera.Transform.GetRotation();
	rotation += Vector3f::LeftVector * (look.y * DeltaSeconds);
	rotation += Vector3f::DownVector * ((look.x / aspect) * DeltaSeconds);
	constexpr float PitchLimit = math::PI_OVER_TWO - 0.001f;
	rotation.x = math::Clamp(rotation.x, -PitchLimit, PitchLimit);
	Camera.Transform.SetRotation(rotation);

	// Move, in camera space
	using namespace DirectX;
	const Vector3f rot = Camera.Transform.GetRotation(); // pitch, yaw, roll (radians)
	const XMMATRIX rotM = XMMatrixRotationRollPitchYaw(rot.x, rot.y, rot.z);
	const XMVECTOR local = XMLoadFloat3(reinterpret_cast<const XMFLOAT3*>(&Input.Move));
	Vector3f worldMove{};
	XMStoreFloat3(reinterpret_cast<XMFLOAT3*>(&worldMove), XMVector3TransformNormal(local, rotM));

	Camera.MovementSpeed = math::Max(Camera.MovementSpeed + Input.Wheel * 0.3f, 0.001f);
	Camera.Transform.MoveBy(worldMove * DeltaSeconds * Camera.MovementSpeed);
}
}
