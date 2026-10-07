#include "ViewportCamera.h"

#include <algorithm>
#include <cmath>

#include "Graphics/Camera.h"


namespace frt::editor
{
namespace
{
// Unreal's constants (FEditorViewportClient, FCameraControllerConfig, ULevelEditorViewportSettings),
// centimetres converted to metres.
constexpr float DegreesToRadians = 0.017453292519943295f;
constexpr float DragMetresPerCount = 0.01f;          // mouse translation at speed 1: 1 cm per count
constexpr float InitialLookAtDistance = 10.24f;      // 1024 cm ahead
constexpr float KeyAcceleration = 200.0f;            // m/s^2 at speed 1 (20000 cm/s^2)
constexpr float VelocityDamping = 10.0f;             // 1/s: about 20 m/s top speed at speed 1
// Unreal stops at 1e-6 m/s, which leaves the camera creeping invisibly for about 1.7 s after a
// key is released - and a progressive render restarting all that time. 1 cm/s stops in 0.8 s.
constexpr float StopSpeed = 0.01f;                   // m/s
constexpr float FovAcceleration = 1200.0f * DegreesToRadians;
constexpr float FovDamping = 10.0f;                  // 1/s
constexpr float FovRecoilRate = 10.0f;               // 1/s
constexpr float FovSnap = 0.1f * DegreesToRadians;
constexpr float MinFov = 0.1f;                       // the renderer's lower clamp (Unreal: 5 degrees)
constexpr float MaxFov = 170.0f * DegreesToRadians;
constexpr float WheelSpeedUp = 1.1f;                 // per notch while a button is held
constexpr float WheelSpeedDown = 0.9f;
// Short of straight up or down, where the view's up vector degenerates (Unreal: 90 - 1e-4 degrees).
constexpr float PitchLimit = 1.5707963f - 1e-3f;
// A dolly stops this far short of the pivot rather than passing through it.
constexpr float MinPivotDistance = 0.01f;

// The camera's axes in engine space (left-up-forward: +X left, +Y up, +Z forward), for
// Transform rotation (pitch, yaw, roll) as XMMatrixRotationRollPitchYaw reads it: +pitch looks
// down, +yaw turns left. Roll is never produced, so right stays level.
struct SBasis
{
	Vector3f Forward;
	Vector3f Right;
	Vector3f Up;
	Vector3f GroundForward;                          // forward flattened onto the ground
};

SBasis MakeBasis (float Pitch, float Yaw)
{
	const float sp = std::sin(Pitch);
	const float cp = std::cos(Pitch);
	const float sy = std::sin(Yaw);
	const float cy = std::cos(Yaw);
	SBasis basis;
	basis.Forward = Vector3f(cp * sy, -sp, cp * cy);
	basis.Right = Vector3f(-cy, 0.0f, sy);
	basis.Up = Vector3f(sp * sy, cp, sp * cy);
	basis.GroundForward = Vector3f(sy, 0.0f, cy);
	return basis;
}

const Vector3f WorldUp(0.0f, 1.0f, 0.0f);

float Dot (const Vector3f& A, const Vector3f& B)
{
	return A.x * B.x + A.y * B.y + A.z * B.z;
}

float Length (const Vector3f& V)
{
	return std::sqrt(Dot(V, V));
}

bool SameVector (const Vector3f& A, const Vector3f& B)
{
	return A.x == B.x && A.y == B.y && A.z == B.z;
}
}

void CViewportCameraController::Update (graphics::CCamera& Camera, const SViewportNavigationInput& Input, float DeltaSeconds)
{
	const float dt = std::clamp(DeltaSeconds, 0.0f, 1.0f);
	math::STransform& transform = Camera.Transform;
	Vector3f position = transform.GetTranslation();
	const Vector3f rotation = transform.GetRotation();
	float pitch = rotation.x;
	float yaw = rotation.y;

	// First use, or the camera was moved by something else: carry on from where it is now.
	if (!bHasState || !SameVector(position, LastPosition) || !SameVector(rotation, LastRotation))
	{
		const float distance = bHasState ? std::max(Length(LookAt - LastPosition), MinPivotDistance) : InitialLookAtDistance;
		LookAt = position + MakeBasis(pitch, yaw).Forward * distance;
		Velocity = Vector3f(0.0f, 0.0f, 0.0f);
		bHasState = true;
	}
	if (!bFovZooming)
	{
		RestFov = Camera.FieldOfViewY;
	}

	const bool bLeft = (Input.Buttons & viewport_button::Left) != 0u;
	const bool bRight = (Input.Buttons & viewport_button::Right) != 0u;
	const bool bMiddle = (Input.Buttons & viewport_button::Middle) != 0u;
	const bool bAnyButton = bLeft || bRight || bMiddle;
	const bool bOrbitControls = bAnyButton && Input.bAlt && !Input.bCtrl && !Input.bShift;
	// Unreal's "flight input": a button navigates and no modifier is held.
	const bool bFlight = bAnyButton && !Input.bAlt && !Input.bCtrl && !Input.bShift;

	// Mouse deltas: x to the right, y up (Unreal's convention).
	const float dx = Input.MouseDelta.x;
	const float dyUp = -Input.MouseDelta.y;
	const float turn = Settings.MouseSensitivity * DegreesToRadians;
	const float drag = DragMetresPerCount * Speed;
	const float pivotDistance = std::max(Length(LookAt - position), MinPivotDistance);
	bool bLookAtFollows = false;

	const SBasis before = MakeBasis(pitch, yaw);
	if (bOrbitControls)
	{
		if (bLeft && !bRight && !bMiddle)
		{
			// Orbit: turn, then put the camera back on its sphere around the pivot. A pivot that
			// a dolly left behind the camera is brought back in front first.
			if (Dot(LookAt - position, before.Forward) <= 0.0f)
			{
				LookAt = position + before.Forward * pivotDistance;
			}
			yaw -= dx * turn;
			pitch = std::clamp(pitch - dyUp * turn * (Settings.bInvertOrbitY ? -1.0f : 1.0f), -PitchLimit, PitchLimit);
			position = LookAt - MakeBasis(pitch, yaw).Forward * pivotDistance;
		}
		else if (bMiddle && !bLeft && !bRight)
		{
			// Track: camera and pivot move together, in the view plane.
			const Vector3f delta = (before.Right * dx + before.Up * dyUp) * (drag * (Settings.bInvertMiddleMousePan ? -1.0f : 1.0f));
			position = position + delta;
			LookAt = LookAt + delta;
		}
		else
		{
			// Dolly toward (mouse right or down) or away from the pivot, never through it.
			const float amount = Settings.bInvertRightMouseDollyY ? dx + dyUp : dx - dyUp;
			const float step = std::min(amount * drag, pivotDistance - MinPivotDistance);
			position = position + before.Forward * step;
		}
	}
	else if (bAnyButton && !Input.bCtrl)
	{
		// Unreal's order (ConvertMovementToDragRot): LMB alone, then anything with MMB, then
		// LMB+RMB, then RMB alone. Ctrl suspends mouse navigation altogether.
		if (bLeft && !bRight)
		{
			// Along the ground and turn; pitch never changes. LMB+MMB behaves the same.
			position = position + before.GroundForward * (dyUp * drag);
			yaw -= dx * turn;
		}
		else if (bMiddle)
		{
			position = position + (before.Right * dx + before.Up * dyUp) * (drag * (Settings.bInvertMiddleMousePan ? -1.0f : 1.0f));
		}
		else if (bLeft && bRight)
		{
			position = position + (Vector3f(before.Right.x, 0.0f, before.Right.z) * dx + WorldUp * dyUp) * drag;
		}
		else
		{
			yaw -= dx * turn;
			pitch = std::clamp(pitch - dyUp * turn * (Settings.bInvertMouseLookY ? -1.0f : 1.0f), -PitchLimit, PitchLimit);
		}
		bLookAtFollows = true;
	}

	SBasis basis = MakeBasis(pitch, yaw);

	// The wheel: with flight input (a drag, no modifier) it sets the speed; otherwise it
	// dollies - also during an Alt orbit, changing its distance. Ctrl+Wheel is the UI zoom.
	float wheelDolly = 0.0f;
	if (Input.Wheel != 0.0f && !Input.bCtrl)
	{
		if (bFlight)
		{
			const float factor = std::pow(Input.Wheel > 0.0f ? WheelSpeedUp : WheelSpeedDown, std::abs(Input.Wheel));
			Speed = std::clamp(Speed * factor, MinSpeed, MaxSpeed);
		}
		else
		{
			wheelDolly = Input.Wheel * Settings.WheelDollyDistance;
		}
	}

	// Navigation keys: an impulse per held direction (diagonals are faster, as in Unreal),
	// accelerated and damped. Exponential damping keeps the glide the same at any frame rate.
	const auto axis = [&Input] (EViewportKey Positive, EViewportKey Negative)
	{
		return (Input.IsKeyDown(Positive) ? 1.0f : 0.0f) - (Input.IsKeyDown(Negative) ? 1.0f : 0.0f);
	};
	const Vector3f impulse =
		basis.Forward * axis(EViewportKey::Forward, EViewportKey::Backward) +
		basis.Right * axis(EViewportKey::Right, EViewportKey::Left) +
		basis.Up * axis(EViewportKey::LocalUp, EViewportKey::LocalDown) +
		WorldUp * axis(EViewportKey::WorldUp, EViewportKey::WorldDown);
	Velocity = (Velocity + impulse * (KeyAcceleration * Speed * dt)) * std::exp(-VelocityDamping * dt);
	if (Dot(Velocity, Velocity) < StopSpeed * StopSpeed)
	{
		Velocity = Vector3f(0.0f, 0.0f, 0.0f);
	}
	else
	{
		position = position + Velocity * dt;
		bLookAtFollows = true;
	}

	if (bLookAtFollows)
	{
		LookAt = position + basis.Forward * pivotDistance;
	}
	// After the pivot is placed: a dolly moves the camera, never the point it orbits.
	position = position + basis.Forward * wheelDolly;

	// Key zoom: the field of view accelerates while a zoom key is held, and springs back to
	// where it was once no zoom key and no navigating button is held.
	const float fovImpulse = axis(EViewportKey::FovZoomOut, EViewportKey::FovZoomIn);
	if (fovImpulse != 0.0f && !bFovZooming)
	{
		RestFov = Camera.FieldOfViewY;
		bFovZooming = true;
	}
	FovVelocity = (FovVelocity + fovImpulse * FovAcceleration * dt) * std::exp(-FovDamping * dt);
	float fov = std::clamp(Camera.FieldOfViewY + FovVelocity * dt, MinFov, MaxFov);
	if (bFovZooming && fovImpulse == 0.0f && !bFlight)
	{
		FovVelocity = 0.0f;
		fov += (RestFov - fov) * std::min(FovRecoilRate * dt, 1.0f);
		if (std::abs(fov - RestFov) < FovSnap)
		{
			fov = RestFov;
			bFovZooming = false;
		}
	}
	Camera.FieldOfViewY = fov;

	transform.SetTranslation(position);
	transform.SetRotation(pitch, yaw, rotation.z);
	LastPosition = transform.GetTranslation();
	LastRotation = transform.GetRotation();
}
}
