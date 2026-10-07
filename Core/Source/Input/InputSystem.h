#pragma once

#include <unordered_map>

#include "Core.h"
#include "Input/InputTypes.h"


namespace frt::input
{
class FRT_CORE_API CInputSystem
{
public:
	CInputSystem () = default;

	void Update (float FrameTimeSeconds);
	// Forgets every key and button held (focus loss: their releases will go to another window)
	// and drops input still queued from before.
	void Clear ();

	void SetDefaultWindow (WindowId Window);
	WindowId GetDefaultWindow () const;

	bool IsKeyDown (KeyCode Key, WindowId Window = InvalidWindowId) const;
	// Pressed / released this frame - also a tap that went down and up within the frame.
	// Auto-repeat does not count as a press.
	bool WasKeyPressed (KeyCode Key, WindowId Window = InvalidWindowId) const;
	bool WasKeyReleased (KeyCode Key, WindowId Window = InvalidWindowId) const;
	float GetKeyHeldSeconds (KeyCode Key, WindowId Window = InvalidWindowId) const;
	// Shift, Ctrl, Alt and Super held now, either side of the keyboard. Lock states are not
	// included.
	SFlags<EInputModifier> GetModifiers (WindowId Window = InvalidWindowId) const;

	bool IsMouseButtonDown (EMouseButton Button, WindowId Window = InvalidWindowId) const;
	bool WasMouseButtonPressed (EMouseButton Button, WindowId Window = InvalidWindowId) const;
	bool WasMouseButtonReleased (EMouseButton Button, WindowId Window = InvalidWindowId) const;

	Vector2f GetMousePosition (WindowId Window = InvalidWindowId) const;
	Vector2f GetMouseDelta (WindowId Window = InvalidWindowId) const;
	// Unaccelerated device motion this frame (raw input). Keeps reporting while the cursor is
	// clipped or hidden - what a captured camera should read. Falls back to GetMouseDelta()
	// where the platform delivers no raw input (e.g. remote sessions).
	Vector2f GetRawMouseDelta (WindowId Window = InvalidWindowId) const;
	// Wheel motion this frame, in notches (fractional for high-resolution wheels): vertical,
	// positive away from the user; horizontal (tilt), positive to the right.
	float GetMouseWheelDelta (WindowId Window = InvalidWindowId) const;
	float GetMouseWheelDeltaX (WindowId Window = InvalidWindowId) const;

	bool IsGamepadConnected (uint8 GamepadIndex, WindowId Window = InvalidWindowId) const;
	bool IsGamepadButtonDown (uint8 GamepadIndex, EGamepadButton Button, WindowId Window = InvalidWindowId) const;
	bool WasGamepadButtonPressed (uint8 GamepadIndex, EGamepadButton Button, WindowId Window = InvalidWindowId) const;
	bool WasGamepadButtonReleased (uint8 GamepadIndex, EGamepadButton Button, WindowId Window = InvalidWindowId) const;
	float GetGamepadAxis (uint8 GamepadIndex, EGamepadAxis Axis, WindowId Window = InvalidWindowId) const;

public:
	InputEventKeyboard OnKeyboardEvent;
	InputEventMouseButton OnMouseButtonEvent;
	InputEventMouseMove OnMouseMoveEvent;
	InputEventMouseWheel OnMouseWheelEvent;
	InputEventGamepadButton OnGamepadButtonEvent;
	InputEventGamepadAxis OnGamepadAxisEvent;
	InputEventDeviceConnection OnDeviceConnectionEvent;

private:
	struct SWindowInputState
	{
		SInputState Current = {};
		SInputState Previous = {};
	};

private:
	SWindowInputState& GetOrCreateState (WindowId Window);
	const SWindowInputState* FindState (WindowId Window) const;
	WindowId ResolveWindow (WindowId Window) const;

	void BeginFrame (float FrameTimeSeconds);

	void HandleEvent (const SKeyboardEventData& Data);
	void HandleEvent (const SMouseButtonEventData& Data);
	void HandleEvent (const SMouseMoveEventData& Data);
	void HandleEvent (const SMouseWheelEventData& Data);
	void HandleEvent (const SGamepadButtonEventData& Data);
	void HandleEvent (const SGamepadAxisEventData& Data);
	void HandleEvent (const SDeviceConnectionEventData& Data);

private:
	std::unordered_map<WindowId, SWindowInputState> WindowStates;
	WindowId DefaultWindow = InvalidWindowId;
	double CurrentTimeSeconds = 0.0;         // since the first Update; double so it keeps ticking in long sessions
	bool bRawMouseSeen = false;
};
}
