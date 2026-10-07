#pragma once

#include <array>

#include "CoreTypes.h"
#include "Math/Math.h"

#include "EditorInput.h"


namespace frt::graphics
{
class CCamera;
}


namespace frt::editor
{
// Mouse buttons held in a drag that began on a viewport.
namespace viewport_button
{
constexpr uint8 Left = 1u << 0u;
constexpr uint8 Right = 1u << 1u;
constexpr uint8 Middle = 1u << 2u;
}

// One frame of navigation input for a viewport camera, already routed by the editor: only what
// this viewport may act on.
struct SViewportNavigationInput
{
	uint8 Buttons = 0u;                              // viewport_button bits
	bool bAlt = false;
	bool bCtrl = false;
	bool bShift = false;
	Vector2f MouseDelta = Vector2f(0.0f, 0.0f);      // raw device counts, +x right, +y down
	float Wheel = 0.0f;                              // notches, positive away from the user
	// Navigation keys held, already filtered to the sets that apply this frame.
	std::array<bool, static_cast<size_t>(EViewportKey::Count)> Keys = {};

	bool IsKeyDown (EViewportKey Key) const { return Keys[static_cast<size_t>(Key)]; }
};

/**
 * Unreal Engine's perspective level-viewport camera (FEditorViewportClient with its
 * FEditorCameraController), for one viewport, with Unreal's default settings:
 *
 *   RMB drag              look around; while held, the flight keys move (WASD, E/Q world
 *                         up/down, R/F camera up/down), Z/C widen/narrow the view and the
 *                         wheel changes the camera speed
 *   LMB drag              move along the ground and turn
 *   LMB+RMB drag          pan sideways and along the world's up
 *   MMB drag              pan along the camera's right and up (also with RMB)
 *   wheel                 dolly 0.96 m per notch along the view (also during an Alt drag)
 *   Alt+LMB / RMB / MMB   orbit / dolly / track around the look-at point
 *   arrows, PgUp/PgDn, numpad: move without a mouse button
 *
 * Mouse motion moves the camera directly; key motion accelerates and glides to a stop; a key
 * zoom springs back once keys and buttons are released. Speed scales every translation - not
 * rotation, and not the wheel dolly. Ctrl suspends mouse navigation, and any modifier the flight
 * keys (Unreal's rules).
 *
 * The look-at point, the orbit pivot, rides 10.24 m ahead of the camera; dollies and tracks move
 * the camera relative to it. Units: metres and radians (Unreal's centimetres converted).
 */
class CViewportCameraController
{
public:
	// Every frame the viewport exists, input or not: motion glides out and zoom springs back.
	void Update (graphics::CCamera& Camera, const SViewportNavigationInput& Input, float DeltaSeconds);

	SViewportCameraSettings Settings;
	float Speed = 1.0f;                              // Unreal's camera speed: 1 = its default

	static constexpr float MinSpeed = 0.0001f;
	static constexpr float MaxSpeed = 10000.0f;

	const Vector3f& GetLookAt () const { return LookAt; }
	const Vector3f& GetVelocity () const { return Velocity; }

private:
	Vector3f Velocity = Vector3f(0.0f, 0.0f, 0.0f);  // m/s, from the navigation keys
	Vector3f LookAt = Vector3f(0.0f, 0.0f, 0.0f);
	float FovVelocity = 0.0f;                        // rad/s
	float RestFov = 0.0f;                            // what a key zoom springs back to
	bool bFovZooming = false;

	// Where this controller left the camera: anything else moving it (the tools panel, Reset
	// Camera) is noticed, and the motion starts over from there.
	bool bHasState = false;
	Vector3f LastPosition = Vector3f(0.0f, 0.0f, 0.0f);
	Vector3f LastRotation = Vector3f(0.0f, 0.0f, 0.0f);
};
}
