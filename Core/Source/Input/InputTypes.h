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
	// Raw device motion (WM_INPUT): Delta only - unaccelerated counts that keep coming while the
	// cursor is clipped, hidden or pinned against a screen edge. Position is not set.
	bool bRaw = false;
};


struct SMouseWheelEventData
{
	SInputEventHeader Header;
	float Delta = 0.0f;
	bool bHorizontal = false;          // a tilt wheel (WM_MOUSEHWHEEL), not the vertical wheel
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
	// Presses and releases this frame. A tap shorter than a frame still counts as pressed
	// (and released), where comparing the level against last frame's would miss it.
	uint8 PressCount = 0u;
	uint8 ReleaseCount = 0u;
	FRT_STRUCT_PADDING(1);
	double LastPressedTime = 0.0;          // CInputSystem's clock
	double LastReleasedTime = 0.0;
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
	Vector2f RawDelta = {};
	float WheelDelta = 0.0f;           // vertical, notches, positive away from the user
	float WheelDeltaX = 0.0f;          // horizontal (tilt), notches, positive to the right
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


// Key and mouse button names, as written in config files and binding text (see InputChord.h).
// Every key has one: the enum's own spelling, matched case-insensitively.
FRT_DECLARE_ENUM_REFLECTION(
	frt::input::KeyCode,
	FRT_ENUM_ENTRY(frt::input::KeyCode, Escape),
	FRT_ENUM_ENTRY(frt::input::KeyCode, F1),
	FRT_ENUM_ENTRY(frt::input::KeyCode, F2),
	FRT_ENUM_ENTRY(frt::input::KeyCode, F3),
	FRT_ENUM_ENTRY(frt::input::KeyCode, F4),
	FRT_ENUM_ENTRY(frt::input::KeyCode, F5),
	FRT_ENUM_ENTRY(frt::input::KeyCode, F6),
	FRT_ENUM_ENTRY(frt::input::KeyCode, F7),
	FRT_ENUM_ENTRY(frt::input::KeyCode, F8),
	FRT_ENUM_ENTRY(frt::input::KeyCode, F9),
	FRT_ENUM_ENTRY(frt::input::KeyCode, F10),
	FRT_ENUM_ENTRY(frt::input::KeyCode, F11),
	FRT_ENUM_ENTRY(frt::input::KeyCode, F12),
	FRT_ENUM_ENTRY(frt::input::KeyCode, PrintScreen),
	FRT_ENUM_ENTRY(frt::input::KeyCode, ScrollLock),
	FRT_ENUM_ENTRY(frt::input::KeyCode, Pause),
	FRT_ENUM_ENTRY(frt::input::KeyCode, Grave),
	FRT_ENUM_ENTRY(frt::input::KeyCode, Digit1),
	FRT_ENUM_ENTRY(frt::input::KeyCode, Digit2),
	FRT_ENUM_ENTRY(frt::input::KeyCode, Digit3),
	FRT_ENUM_ENTRY(frt::input::KeyCode, Digit4),
	FRT_ENUM_ENTRY(frt::input::KeyCode, Digit5),
	FRT_ENUM_ENTRY(frt::input::KeyCode, Digit6),
	FRT_ENUM_ENTRY(frt::input::KeyCode, Digit7),
	FRT_ENUM_ENTRY(frt::input::KeyCode, Digit8),
	FRT_ENUM_ENTRY(frt::input::KeyCode, Digit9),
	FRT_ENUM_ENTRY(frt::input::KeyCode, Digit0),
	FRT_ENUM_ENTRY(frt::input::KeyCode, Minus),
	FRT_ENUM_ENTRY(frt::input::KeyCode, Equals),
	FRT_ENUM_ENTRY(frt::input::KeyCode, Backspace),
	FRT_ENUM_ENTRY(frt::input::KeyCode, Tab),
	FRT_ENUM_ENTRY(frt::input::KeyCode, Q),
	FRT_ENUM_ENTRY(frt::input::KeyCode, W),
	FRT_ENUM_ENTRY(frt::input::KeyCode, E),
	FRT_ENUM_ENTRY(frt::input::KeyCode, R),
	FRT_ENUM_ENTRY(frt::input::KeyCode, T),
	FRT_ENUM_ENTRY(frt::input::KeyCode, Y),
	FRT_ENUM_ENTRY(frt::input::KeyCode, U),
	FRT_ENUM_ENTRY(frt::input::KeyCode, I),
	FRT_ENUM_ENTRY(frt::input::KeyCode, O),
	FRT_ENUM_ENTRY(frt::input::KeyCode, P),
	FRT_ENUM_ENTRY(frt::input::KeyCode, LeftBracket),
	FRT_ENUM_ENTRY(frt::input::KeyCode, RightBracket),
	FRT_ENUM_ENTRY(frt::input::KeyCode, Backslash),
	FRT_ENUM_ENTRY(frt::input::KeyCode, CapsLock),
	FRT_ENUM_ENTRY(frt::input::KeyCode, A),
	FRT_ENUM_ENTRY(frt::input::KeyCode, S),
	FRT_ENUM_ENTRY(frt::input::KeyCode, D),
	FRT_ENUM_ENTRY(frt::input::KeyCode, F),
	FRT_ENUM_ENTRY(frt::input::KeyCode, G),
	FRT_ENUM_ENTRY(frt::input::KeyCode, H),
	FRT_ENUM_ENTRY(frt::input::KeyCode, J),
	FRT_ENUM_ENTRY(frt::input::KeyCode, K),
	FRT_ENUM_ENTRY(frt::input::KeyCode, L),
	FRT_ENUM_ENTRY(frt::input::KeyCode, Semicolon),
	FRT_ENUM_ENTRY(frt::input::KeyCode, Apostrophe),
	FRT_ENUM_ENTRY(frt::input::KeyCode, Enter),
	FRT_ENUM_ENTRY(frt::input::KeyCode, LeftShift),
	FRT_ENUM_ENTRY(frt::input::KeyCode, Z),
	FRT_ENUM_ENTRY(frt::input::KeyCode, X),
	FRT_ENUM_ENTRY(frt::input::KeyCode, C),
	FRT_ENUM_ENTRY(frt::input::KeyCode, V),
	FRT_ENUM_ENTRY(frt::input::KeyCode, B),
	FRT_ENUM_ENTRY(frt::input::KeyCode, N),
	FRT_ENUM_ENTRY(frt::input::KeyCode, M),
	FRT_ENUM_ENTRY(frt::input::KeyCode, Comma),
	FRT_ENUM_ENTRY(frt::input::KeyCode, Period),
	FRT_ENUM_ENTRY(frt::input::KeyCode, Slash),
	FRT_ENUM_ENTRY(frt::input::KeyCode, RightShift),
	FRT_ENUM_ENTRY(frt::input::KeyCode, LeftCtrl),
	FRT_ENUM_ENTRY(frt::input::KeyCode, LeftAlt),
	FRT_ENUM_ENTRY(frt::input::KeyCode, LeftMeta),
	FRT_ENUM_ENTRY(frt::input::KeyCode, Space),
	FRT_ENUM_ENTRY(frt::input::KeyCode, RightAlt),
	FRT_ENUM_ENTRY(frt::input::KeyCode, RightMeta),
	FRT_ENUM_ENTRY(frt::input::KeyCode, Menu),
	FRT_ENUM_ENTRY(frt::input::KeyCode, RightCtrl),
	FRT_ENUM_ENTRY(frt::input::KeyCode, Insert),
	FRT_ENUM_ENTRY(frt::input::KeyCode, Delete),
	FRT_ENUM_ENTRY(frt::input::KeyCode, Home),
	FRT_ENUM_ENTRY(frt::input::KeyCode, End),
	FRT_ENUM_ENTRY(frt::input::KeyCode, PageUp),
	FRT_ENUM_ENTRY(frt::input::KeyCode, PageDown),
	FRT_ENUM_ENTRY(frt::input::KeyCode, ArrowUp),
	FRT_ENUM_ENTRY(frt::input::KeyCode, ArrowDown),
	FRT_ENUM_ENTRY(frt::input::KeyCode, ArrowLeft),
	FRT_ENUM_ENTRY(frt::input::KeyCode, ArrowRight),
	FRT_ENUM_ENTRY(frt::input::KeyCode, NumLock),
	FRT_ENUM_ENTRY(frt::input::KeyCode, NumpadDivide),
	FRT_ENUM_ENTRY(frt::input::KeyCode, NumpadMultiply),
	FRT_ENUM_ENTRY(frt::input::KeyCode, NumpadMinus),
	FRT_ENUM_ENTRY(frt::input::KeyCode, NumpadPlus),
	FRT_ENUM_ENTRY(frt::input::KeyCode, NumpadEnter),
	FRT_ENUM_ENTRY(frt::input::KeyCode, Numpad1),
	FRT_ENUM_ENTRY(frt::input::KeyCode, Numpad2),
	FRT_ENUM_ENTRY(frt::input::KeyCode, Numpad3),
	FRT_ENUM_ENTRY(frt::input::KeyCode, Numpad4),
	FRT_ENUM_ENTRY(frt::input::KeyCode, Numpad5),
	FRT_ENUM_ENTRY(frt::input::KeyCode, Numpad6),
	FRT_ENUM_ENTRY(frt::input::KeyCode, Numpad7),
	FRT_ENUM_ENTRY(frt::input::KeyCode, Numpad8),
	FRT_ENUM_ENTRY(frt::input::KeyCode, Numpad9),
	FRT_ENUM_ENTRY(frt::input::KeyCode, Numpad0),
	FRT_ENUM_ENTRY(frt::input::KeyCode, NumpadDecimal),
	FRT_ENUM_ENTRY(frt::input::KeyCode, NonUSBackslash),
	FRT_ENUM_ENTRY(frt::input::KeyCode, IntlRo),
	FRT_ENUM_ENTRY(frt::input::KeyCode, IntlYen)
);

FRT_DECLARE_ENUM_REFLECTION(
	frt::input::EMouseButton,
	FRT_ENUM_ENTRY(frt::input::EMouseButton, Left),
	FRT_ENUM_ENTRY(frt::input::EMouseButton, Right),
	FRT_ENUM_ENTRY(frt::input::EMouseButton, Middle),
	FRT_ENUM_ENTRY(frt::input::EMouseButton, X1),
	FRT_ENUM_ENTRY(frt::input::EMouseButton, X2)
);
