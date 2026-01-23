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
	void Clear ();

	void SetDefaultWindow (WindowId Window);
	WindowId GetDefaultWindow () const;

	bool IsKeyDown (KeyCode Key, WindowId Window = InvalidWindowId) const;
	bool WasKeyPressed (KeyCode Key, WindowId Window = InvalidWindowId) const;
	bool WasKeyReleased (KeyCode Key, WindowId Window = InvalidWindowId) const;
	float GetKeyHeldSeconds (KeyCode Key, WindowId Window = InvalidWindowId) const;

	bool IsMouseButtonDown (EMouseButton Button, WindowId Window = InvalidWindowId) const;
	bool WasMouseButtonPressed (EMouseButton Button, WindowId Window = InvalidWindowId) const;
	bool WasMouseButtonReleased (EMouseButton Button, WindowId Window = InvalidWindowId) const;

	Vector2f GetMousePosition (WindowId Window = InvalidWindowId) const;
	Vector2f GetMouseDelta (WindowId Window = InvalidWindowId) const;
	float GetMouseWheelDelta (WindowId Window = InvalidWindowId) const;

	bool IsGamepadConnected (uint8 GamepadIndex, WindowId Window = InvalidWindowId) const;
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
	float CurrentTimeSeconds = 0.0f;
};
}
