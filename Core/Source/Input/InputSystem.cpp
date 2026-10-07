#include "Input/InputSystem.h"

#include <algorithm>
#include <vector>

#include "Input/PlatformInput.h"


namespace frt::input
{
void CInputSystem::Update (float FrameTimeSeconds)
{
	BeginFrame(FrameTimeSeconds);

	std::vector<SInputEvent> events;
	CPlatformInputQueue::Drain(events);

	for (const SInputEvent& event : events)
	{
		std::visit(
			[this] (const auto& data)
			{
				HandleEvent(data);
			}, event.Data);
	}
}

void CInputSystem::Clear ()
{
	WindowStates.clear();
	// Events queued before the focus change describe keys whose releases will never arrive
	// (an Alt that went down for Alt+Tab would stay held). The default window stays: it was
	// set by the owner, and the next event would only have to find it again.
	CPlatformInputQueue::Clear();
}

void CInputSystem::SetDefaultWindow (WindowId Window)
{
	DefaultWindow = Window;
}

WindowId CInputSystem::GetDefaultWindow () const
{
	return DefaultWindow;
}

bool CInputSystem::IsKeyDown (KeyCode Key, WindowId Window) const
{
	const WindowId resolved = ResolveWindow(Window);
	const SWindowInputState* state = FindState(resolved);
	if (!state)
	{
		return false;
	}

	const size_t index = static_cast<size_t>(Key);
	if (index >= KeyCodeCount)
	{
		return false;
	}

	return state->Current.Keyboard.Keys[index].bDown;
}

bool CInputSystem::WasKeyPressed (KeyCode Key, WindowId Window) const
{
	const WindowId resolved = ResolveWindow(Window);
	const SWindowInputState* state = FindState(resolved);
	if (!state)
	{
		return false;
	}

	const size_t index = static_cast<size_t>(Key);
	if (index >= KeyCodeCount)
	{
		return false;
	}

	return state->Current.Keyboard.Keys[index].PressCount > 0u;
}

bool CInputSystem::WasKeyReleased (KeyCode Key, WindowId Window) const
{
	const WindowId resolved = ResolveWindow(Window);
	const SWindowInputState* state = FindState(resolved);
	if (!state)
	{
		return false;
	}

	const size_t index = static_cast<size_t>(Key);
	if (index >= KeyCodeCount)
	{
		return false;
	}

	return state->Current.Keyboard.Keys[index].ReleaseCount > 0u;
}

float CInputSystem::GetKeyHeldSeconds (KeyCode Key, WindowId Window) const
{
	const WindowId resolved = ResolveWindow(Window);
	const SWindowInputState* state = FindState(resolved);
	if (!state)
	{
		return 0.0f;
	}

	const size_t index = static_cast<size_t>(Key);
	if (index >= KeyCodeCount)
	{
		return 0.0f;
	}

	const SButtonState& key = state->Current.Keyboard.Keys[index];
	if (!key.bDown)
	{
		return 0.0f;
	}

	return std::max(0.0f, CurrentTimeSeconds - key.LastPressedTime);
}

SFlags<EInputModifier> CInputSystem::GetModifiers (WindowId Window) const
{
	SFlags<EInputModifier> modifiers = EInputModifier::None;
	if (IsKeyDown(KeyCode::LeftShift, Window) || IsKeyDown(KeyCode::RightShift, Window))
	{
		modifiers += EInputModifier::Shift;
	}
	if (IsKeyDown(KeyCode::LeftCtrl, Window) || IsKeyDown(KeyCode::RightCtrl, Window))
	{
		modifiers += EInputModifier::Ctrl;
	}
	if (IsKeyDown(KeyCode::LeftAlt, Window) || IsKeyDown(KeyCode::RightAlt, Window))
	{
		modifiers += EInputModifier::Alt;
	}
	if (IsKeyDown(KeyCode::LeftMeta, Window) || IsKeyDown(KeyCode::RightMeta, Window))
	{
		modifiers += EInputModifier::Super;
	}
	return modifiers;
}

bool CInputSystem::IsMouseButtonDown (EMouseButton Button, WindowId Window) const
{
	const WindowId resolved = ResolveWindow(Window);
	const SWindowInputState* state = FindState(resolved);
	if (!state)
	{
		return false;
	}

	const size_t index = static_cast<size_t>(Button);
	if (index >= MouseButtonCount)
	{
		return false;
	}

	return state->Current.Mouse.Buttons[index].bDown;
}

bool CInputSystem::WasMouseButtonPressed (EMouseButton Button, WindowId Window) const
{
	const WindowId resolved = ResolveWindow(Window);
	const SWindowInputState* state = FindState(resolved);
	if (!state)
	{
		return false;
	}

	const size_t index = static_cast<size_t>(Button);
	if (index >= MouseButtonCount)
	{
		return false;
	}

	return state->Current.Mouse.Buttons[index].PressCount > 0u;
}

bool CInputSystem::WasMouseButtonReleased (EMouseButton Button, WindowId Window) const
{
	const WindowId resolved = ResolveWindow(Window);
	const SWindowInputState* state = FindState(resolved);
	if (!state)
	{
		return false;
	}

	const size_t index = static_cast<size_t>(Button);
	if (index >= MouseButtonCount)
	{
		return false;
	}

	return state->Current.Mouse.Buttons[index].ReleaseCount > 0u;
}

Vector2f CInputSystem::GetMousePosition (WindowId Window) const
{
	const WindowId resolved = ResolveWindow(Window);
	const SWindowInputState* state = FindState(resolved);
	if (!state)
	{
		return {};
	}

	return state->Current.Mouse.Position;
}

Vector2f CInputSystem::GetMouseDelta (WindowId Window) const
{
	const WindowId resolved = ResolveWindow(Window);
	const SWindowInputState* state = FindState(resolved);
	if (!state)
	{
		return {};
	}

	return state->Current.Mouse.Delta;
}

Vector2f CInputSystem::GetRawMouseDelta (WindowId Window) const
{
	if (!bRawMouseSeen)
	{
		return GetMouseDelta(Window);
	}
	const SWindowInputState* state = FindState(ResolveWindow(Window));
	return state ? state->Current.Mouse.RawDelta : Vector2f(0.0f, 0.0f);
}

float CInputSystem::GetMouseWheelDelta (WindowId Window) const
{
	const WindowId resolved = ResolveWindow(Window);
	const SWindowInputState* state = FindState(resolved);
	if (!state)
	{
		return 0.0f;
	}

	return state->Current.Mouse.WheelDelta;
}

float CInputSystem::GetMouseWheelDeltaX (WindowId Window) const
{
	const SWindowInputState* state = FindState(ResolveWindow(Window));
	return state ? state->Current.Mouse.WheelDeltaX : 0.0f;
}

bool CInputSystem::IsGamepadConnected (uint8 GamepadIndex, WindowId Window) const
{
	if (GamepadIndex >= MaxGamepads)
	{
		return false;
	}

	const WindowId resolved = ResolveWindow(Window);
	const SWindowInputState* state = FindState(resolved);
	if (!state)
	{
		return false;
	}

	return state->Current.Gamepads[GamepadIndex].bConnected;
}

bool CInputSystem::IsGamepadButtonDown (uint8 GamepadIndex, EGamepadButton Button, WindowId Window) const
{
	if (GamepadIndex >= MaxGamepads)
	{
		return false;
	}

	const size_t index = static_cast<size_t>(Button);
	if (index >= GamepadButtonCount)
	{
		return false;
	}

	const WindowId resolved = ResolveWindow(Window);
	const SWindowInputState* state = FindState(resolved);
	if (!state)
	{
		return false;
	}

	return state->Current.Gamepads[GamepadIndex].Buttons[index].bDown;
}

bool CInputSystem::WasGamepadButtonPressed (uint8 GamepadIndex, EGamepadButton Button, WindowId Window) const
{
	if (GamepadIndex >= MaxGamepads)
	{
		return false;
	}

	const size_t index = static_cast<size_t>(Button);
	if (index >= GamepadButtonCount)
	{
		return false;
	}

	const WindowId resolved = ResolveWindow(Window);
	const SWindowInputState* state = FindState(resolved);
	if (!state)
	{
		return false;
	}

	const SButtonState& prev = state->Previous.Gamepads[GamepadIndex].Buttons[index];
	const SButtonState& curr = state->Current.Gamepads[GamepadIndex].Buttons[index];
	return !prev.bDown && curr.bDown;
}

bool CInputSystem::WasGamepadButtonReleased (uint8 GamepadIndex, EGamepadButton Button, WindowId Window) const
{
	if (GamepadIndex >= MaxGamepads)
	{
		return false;
	}

	const size_t index = static_cast<size_t>(Button);
	if (index >= GamepadButtonCount)
	{
		return false;
	}

	const WindowId resolved = ResolveWindow(Window);
	const SWindowInputState* state = FindState(resolved);
	if (!state)
	{
		return false;
	}

	const SButtonState& prev = state->Previous.Gamepads[GamepadIndex].Buttons[index];
	const SButtonState& curr = state->Current.Gamepads[GamepadIndex].Buttons[index];
	return prev.bDown && !curr.bDown;
}

float CInputSystem::GetGamepadAxis (uint8 GamepadIndex, EGamepadAxis Axis, WindowId Window) const
{
	if (GamepadIndex >= MaxGamepads)
	{
		return 0.0f;
	}

	const size_t axisIndex = static_cast<size_t>(Axis);
	if (axisIndex >= GamepadAxisCount)
	{
		return 0.0f;
	}

	const WindowId resolved = ResolveWindow(Window);
	const SWindowInputState* state = FindState(resolved);
	if (!state)
	{
		return 0.0f;
	}

	return state->Current.Gamepads[GamepadIndex].Axes[axisIndex];
}

CInputSystem::SWindowInputState& CInputSystem::GetOrCreateState (WindowId Window)
{
	auto [it, inserted] = WindowStates.emplace(Window, SWindowInputState{});
	return it->second;
}

const CInputSystem::SWindowInputState* CInputSystem::FindState (WindowId Window) const
{
	auto it = WindowStates.find(Window);
	if (it == WindowStates.end())
	{
		return nullptr;
	}

	return &it->second;
}

WindowId CInputSystem::ResolveWindow (WindowId Window) const
{
	if (Window != InvalidWindowId)
	{
		return Window;
	}

	return DefaultWindow;
}

void CInputSystem::BeginFrame (float FrameTimeSeconds)
{
	CurrentTimeSeconds += FrameTimeSeconds;

	for (auto& [windowId, state] : WindowStates)
	{
		state.Previous = state.Current;
		for (SButtonState& key : state.Current.Keyboard.Keys)
		{
			key.PressCount = key.ReleaseCount = 0u;
		}
		for (SButtonState& button : state.Current.Mouse.Buttons)
		{
			button.PressCount = button.ReleaseCount = 0u;
		}
		state.Current.Mouse.Delta = Vector2f(0.0f, 0.0f);
		state.Current.Mouse.RawDelta = Vector2f(0.0f, 0.0f);
		state.Current.Mouse.WheelDelta = 0.0f;
		state.Current.Mouse.WheelDeltaX = 0.0f;
	}
}

void CInputSystem::HandleEvent (const SKeyboardEventData& Data)
{
	const WindowId window = (Data.Header.Window != InvalidWindowId) ? Data.Header.Window : DefaultWindow;
	if (window == InvalidWindowId)
	{
		return;
	}

	if (DefaultWindow == InvalidWindowId)
	{
		DefaultWindow = window;
	}

	SWindowInputState& state = GetOrCreateState(window);
	const size_t index = static_cast<size_t>(Data.Key);
	if (index >= KeyCodeCount)
	{
		return;
	}

	SButtonState& keyState = state.Current.Keyboard.Keys[index];
	switch (Data.Action)
	{
		case EInputAction::Pressed: if (!keyState.bDown)
			{
				keyState.LastPressedTime = CurrentTimeSeconds;
			}
			keyState.bDown = true;
			keyState.PressCount = static_cast<uint8>(std::min(keyState.PressCount + 1, 255));
			break;
		case EInputAction::Repeat: keyState.bDown = true;
			break;
		case EInputAction::Released: if (keyState.bDown)
			{
				keyState.ReleaseCount = static_cast<uint8>(std::min(keyState.ReleaseCount + 1, 255));
			}
			keyState.bDown = false;
			keyState.LastReleasedTime = CurrentTimeSeconds;
			break;
		default: break;
	}

	OnKeyboardEvent.Invoke(Data);
}

void CInputSystem::HandleEvent (const SMouseButtonEventData& Data)
{
	const WindowId window = (Data.Header.Window != InvalidWindowId) ? Data.Header.Window : DefaultWindow;
	if (window == InvalidWindowId)
	{
		return;
	}

	if (DefaultWindow == InvalidWindowId)
	{
		DefaultWindow = window;
	}

	SWindowInputState& state = GetOrCreateState(window);
	const size_t index = static_cast<size_t>(Data.Button);
	if (index >= MouseButtonCount)
	{
		return;
	}

	SButtonState& buttonState = state.Current.Mouse.Buttons[index];
	switch (Data.Action)
	{
		case EInputAction::Pressed: if (!buttonState.bDown)
			{
				buttonState.LastPressedTime = CurrentTimeSeconds;
			}
			buttonState.bDown = true;
			buttonState.PressCount = static_cast<uint8>(std::min(buttonState.PressCount + 1, 255));
			break;
		case EInputAction::Repeat: buttonState.bDown = true;
			break;
		case EInputAction::Released: if (buttonState.bDown)
			{
				buttonState.ReleaseCount = static_cast<uint8>(std::min(buttonState.ReleaseCount + 1, 255));
			}
			buttonState.bDown = false;
			buttonState.LastReleasedTime = CurrentTimeSeconds;
			break;
		default: break;
	}

	state.Current.Mouse.Position = Data.Position;
	OnMouseButtonEvent.Invoke(Data);
}

void CInputSystem::HandleEvent (const SMouseMoveEventData& Data)
{
	const WindowId window = (Data.Header.Window != InvalidWindowId) ? Data.Header.Window : DefaultWindow;
	if (window == InvalidWindowId)
	{
		return;
	}

	if (DefaultWindow == InvalidWindowId)
	{
		DefaultWindow = window;
	}

	SWindowInputState& state = GetOrCreateState(window);
	if (Data.bRaw)
	{
		state.Current.Mouse.RawDelta += Data.Delta;
		bRawMouseSeen = true;
		return;
	}
	state.Current.Mouse.Position = Data.Position;
	state.Current.Mouse.Delta += Data.Delta;

	OnMouseMoveEvent.Invoke(Data);
}

void CInputSystem::HandleEvent (const SMouseWheelEventData& Data)
{
	const WindowId window = (Data.Header.Window != InvalidWindowId) ? Data.Header.Window : DefaultWindow;
	if (window == InvalidWindowId)
	{
		return;
	}

	if (DefaultWindow == InvalidWindowId)
	{
		DefaultWindow = window;
	}

	SWindowInputState& state = GetOrCreateState(window);
	(Data.bHorizontal ? state.Current.Mouse.WheelDeltaX : state.Current.Mouse.WheelDelta) += Data.Delta;

	OnMouseWheelEvent.Invoke(Data);
}

void CInputSystem::HandleEvent (const SGamepadButtonEventData& Data)
{
	const WindowId window = (Data.Header.Window != InvalidWindowId) ? Data.Header.Window : DefaultWindow;
	if (window == InvalidWindowId)
	{
		return;
	}

	if (DefaultWindow == InvalidWindowId)
	{
		DefaultWindow = window;
	}

	const uint8 gamepadIndex = static_cast<uint8>(Data.Header.Device);
	if (gamepadIndex >= MaxGamepads)
	{
		return;
	}

	SWindowInputState& state = GetOrCreateState(window);
	const size_t index = static_cast<size_t>(Data.Button);
	if (index >= GamepadButtonCount)
	{
		return;
	}

	SButtonState& buttonState = state.Current.Gamepads[gamepadIndex].Buttons[index];
	switch (Data.Action)
	{
		case EInputAction::Pressed: if (!buttonState.bDown)
			{
				buttonState.LastPressedTime = CurrentTimeSeconds;
			}
			buttonState.bDown = true;
			break;
		case EInputAction::Repeat: buttonState.bDown = true;
			break;
		case EInputAction::Released: buttonState.bDown = false;
			buttonState.LastReleasedTime = CurrentTimeSeconds;
			break;
		default: break;
	}

	OnGamepadButtonEvent.Invoke(Data);
}

void CInputSystem::HandleEvent (const SGamepadAxisEventData& Data)
{
	const WindowId window = (Data.Header.Window != InvalidWindowId) ? Data.Header.Window : DefaultWindow;
	if (window == InvalidWindowId)
	{
		return;
	}

	if (DefaultWindow == InvalidWindowId)
	{
		DefaultWindow = window;
	}

	const uint8 gamepadIndex = static_cast<uint8>(Data.Header.Device);
	if (gamepadIndex >= MaxGamepads)
	{
		return;
	}

	const size_t axisIndex = static_cast<size_t>(Data.Axis);
	if (axisIndex >= GamepadAxisCount)
	{
		return;
	}

	SWindowInputState& state = GetOrCreateState(window);
	state.Current.Gamepads[gamepadIndex].Axes[axisIndex] = Data.Value;

	OnGamepadAxisEvent.Invoke(Data);
}

void CInputSystem::HandleEvent (const SDeviceConnectionEventData& Data)
{
	const WindowId window = (Data.Header.Window != InvalidWindowId) ? Data.Header.Window : DefaultWindow;
	if (window == InvalidWindowId)
	{
		return;
	}

	if (DefaultWindow == InvalidWindowId)
	{
		DefaultWindow = window;
	}

	if (Data.Header.DeviceType == EInputDeviceType::Gamepad)
	{
		const uint8 gamepadIndex = static_cast<uint8>(Data.Header.Device);
		if (gamepadIndex < MaxGamepads)
		{
			SWindowInputState& state = GetOrCreateState(window);
			state.Current.Gamepads[gamepadIndex].bConnected = (Data.State == EDeviceConnectionState::Connected);
		}
	}

	OnDeviceConnectionEvent.Invoke(Data);
}
}
