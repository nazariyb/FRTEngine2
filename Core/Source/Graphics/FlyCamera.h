#pragma once

#include "Core.h"
#include "Math/Math.h"


namespace frt::input
{
class CInputActionMap;
class CInputSystem;
}

namespace frt::graphics
{
class CCamera;

// One frame of fly-camera control, read from an action map (IA_EnableMove / IA_MoveForward /
// IA_MoveLeft / IA_MoveUp, as in the Demo template's Content/Input/IAM_Editor.frtinputmap).
struct SFlyCameraInput
{
	bool bActive = false;                       // IA_EnableMove held: look and move
	Vector2f LookDelta = Vector2f(0.0f, 0.0f);  // pixels
	Vector3f Move = Vector3f::ZeroVector;       // camera-local axes, [-1, 1]
	float Wheel = 0.0f;                         // notches: movement speed
};

FRT_CORE_API SFlyCameraInput ReadFlyCameraInput (const input::CInputActionMap& Actions, const input::CInputSystem& Device, const Vector2f& LookDelta);

// Mouse look (pitch clamped short of the poles) and camera-relative movement. The editor's
// scene camera and the demo game's camera share it.
FRT_CORE_API void UpdateFlyCamera (CCamera& Camera, const SFlyCameraInput& Input, float DeltaSeconds, float ViewAspect);
}
