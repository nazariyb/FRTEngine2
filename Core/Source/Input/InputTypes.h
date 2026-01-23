#pragma once

#include <array>

#include "CoreTypes.h"
#include "Enum.h"
#include "Event.h"
#include "Math/Math.h"


namespace frt::input
{
using WindowId = uint64;
using DeviceId = uint32;

static constexpr WindowId InvalidWindowId = 0u;
static constexpr DeviceId InvalidDeviceId = 0u;
static constexpr uint8 MaxGamepads = 4u;


enum class EInputDeviceType : uint8
{
	Unknown = 0u,
	Keyboard,
	Mouse,
	Gamepad
};


enum class EInputAction : uint8
{
	Pressed = 0u,
	Released,
	Repeat
};


enum class EInputModifier : uint8
{
	None     = 0u,
	Shift    = 1u << 0u,
	Ctrl     = 1u << 1u,
	Alt      = 1u << 2u,
	Super    = 1u << 3u,
	CapsLock = 1u << 4u,
	NumLock  = 1u << 5u
};


FRT_DECLARE_FLAG_ENUM(EInputModifier);


enum class KeyCode : uint16
{
	Unknown = 0u,

	Escape,
	F1,
	F2,
	F3,
	F4,
	F5,
	F6,
	F7,
	F8,
	F9,
	F10,
	F11,
	F12,
	PrintScreen,
	ScrollLock,
	Pause,

	Grave,
	Digit1,
	Digit2,
	Digit3,
	Digit4,
	Digit5,
	Digit6,
	Digit7,
	Digit8,
	Digit9,
	Digit0,
	Minus,
	Equals,
	Backspace,

	Tab,
	Q,
	W,
	E,
	R,
	T,
	Y,
	U,
	I,
	O,
	P,
	LeftBracket,
	RightBracket,
	Backslash,

	CapsLock,
	A,
	S,
	D,
	F,
	G,
	H,
	J,
	K,
	L,
	Semicolon,
	Apostrophe,
	Enter,

	LeftShift,
	Z,
	X,
	C,
	V,
	B,
	N,
	M,
	Comma,
	Period,
	Slash,
	RightShift,

	LeftCtrl,
	LeftAlt,
	LeftMeta,
	Space,
	RightAlt,
	RightMeta,
	Menu,
	RightCtrl,

	Insert,
	Delete,
	Home,
	End,
	PageUp,
	PageDown,

	ArrowUp,
	ArrowDown,
	ArrowLeft,
	ArrowRight,

	NumLock,
	NumpadDivide,
	NumpadMultiply,
	NumpadMinus,
	NumpadPlus,
	NumpadEnter,
	Numpad1,
	Numpad2,
	Numpad3,
	Numpad4,
	Numpad5,
	Numpad6,
	Numpad7,
	Numpad8,
	Numpad9,
	Numpad0,
	NumpadDecimal,

	NonUSBackslash,
	IntlRo,
	IntlYen,

	Count
};


constexpr std::size_t KeyCodeCount = static_cast<std::size_t>(KeyCode::Count);


enum class EMouseButton : uint8
{
	Left = 0u,
	Right,
	Middle,
	X1,
	X2,
	Count
};


constexpr std::size_t MouseButtonCount = static_cast<std::size_t>(EMouseButton::Count);


enum class EGamepadButton : uint8
{
	A = 0u,
	B,
	X,
	Y,
	LeftBumper,
	RightBumper,
	Back,
	Start,
	Guide,
	LeftStick,
	RightStick,
	DpadUp,
	DpadDown,
	DpadLeft,
	DpadRight,
	Count
};


constexpr std::size_t GamepadButtonCount = static_cast<std::size_t>(EGamepadButton::Count);


enum class EGamepadAxis : uint8
{
	LeftX = 0u,
	LeftY,
	RightX,
	RightY,
	LeftTrigger,
	RightTrigger,
	Count
};


constexpr std::size_t GamepadAxisCount = static_cast<std::size_t>(EGamepadAxis::Count);


enum class EDeviceConnectionState : uint8
{
	Connected = 0u,
	Disconnected
};


struct SInputEventHeader
{
	WindowId Window = InvalidWindowId;
	DeviceId Device = InvalidDeviceId;
	EInputDeviceType DeviceType = EInputDeviceType::Unknown;
	float TimestampSeconds = 0.0f;
};


struct SKeyboardEventData
{
	SInputEventHeader Header;
	KeyCode Key = KeyCode::Unknown;
	EInputAction Action = EInputAction::Pressed;
	SFlags<EInputModifier> Modifiers = EInputModifier::None;
};


struct SMouseButtonEventData
{
	SInputEventHeader Header;
	EMouseButton Button = EMouseButton::Left;
	EInputAction Action = EInputAction::Pressed;
	SFlags<EInputModifier> Modifiers = EInputModifier::None;
	Vector2f Position = {};
};


struct SMouseMoveEventData
{
	SInputEventHeader Header;
	Vector2f Position = {};
	Vector2f Delta = {};
};


struct SMouseWheelEventData
{
	SInputEventHeader Header;
	float Delta = 0.0f;
	Vector2f Position = {};
};


struct SGamepadButtonEventData
{
	SInputEventHeader Header;
	EGamepadButton Button = EGamepadButton::A;
	EInputAction Action = EInputAction::Pressed;
	float Value = 0.0f;
};


struct SGamepadAxisEventData
{
	SInputEventHeader Header;
	EGamepadAxis Axis = EGamepadAxis::LeftX;
	float Value = 0.0f;
	float Delta = 0.0f;
};


struct SDeviceConnectionEventData
{
	SInputEventHeader Header;
	EDeviceConnectionState State = EDeviceConnectionState::Connected;
};


using InputEventKeyboard = CEvent<SKeyboardEventData>;
using InputEventMouseButton = CEvent<SMouseButtonEventData>;
using InputEventMouseMove = CEvent<SMouseMoveEventData>;
using InputEventMouseWheel = CEvent<SMouseWheelEventData>;
using InputEventGamepadButton = CEvent<SGamepadButtonEventData>;
using InputEventGamepadAxis = CEvent<SGamepadAxisEventData>;
using InputEventDeviceConnection = CEvent<SDeviceConnectionEventData>;


struct SButtonState
{
	bool bDown = false;
	FRT_STRUCT_PADDING(3);
	float LastPressedTime = 0.0f;
	float LastReleasedTime = 0.0f;
};


struct SKeyboardState
{
	std::array<SButtonState, KeyCodeCount> Keys = {};
};


struct SMouseState
{
	std::array<SButtonState, MouseButtonCount> Buttons = {};
	Vector2f Position = {};
	Vector2f Delta = {};
	float WheelDelta = 0.0f;
};


struct SGamepadState
{
	bool bConnected = false;
	FRT_STRUCT_PADDING(3);
	std::array<SButtonState, GamepadButtonCount> Buttons = {};
	std::array<float, GamepadAxisCount> Axes = {};
};


struct SInputState
{
	SKeyboardState Keyboard;
	SMouseState Mouse;
	std::array<SGamepadState, MaxGamepads> Gamepads = {};
};
}
