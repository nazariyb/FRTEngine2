#include "Input/PlatformInputWin32.h"

#if defined(_WINDOWS)

#include <unordered_map>


namespace frt::input
{
namespace
{
	constexpr DeviceId KeyboardDeviceId = 1u;
	constexpr DeviceId MouseDeviceId = 2u;

	WindowId ToWindowId (HWND hWnd)
	{
		return static_cast<WindowId>(reinterpret_cast<uintptr_t>(hWnd));
	}

	float GetMessageTimestampSeconds ()
	{
		return static_cast<float>(::GetMessageTime()) / 1000.0f;
	}

	SFlags<EInputModifier> GetModifiers ()
	{
		SFlags<EInputModifier> modifiers = EInputModifier::None;

		if ((::GetKeyState(VK_SHIFT) & 0x8000) != 0)
		{
			modifiers += EInputModifier::Shift;
		}
		if ((::GetKeyState(VK_CONTROL) & 0x8000) != 0)
		{
			modifiers += EInputModifier::Ctrl;
		}
		if ((::GetKeyState(VK_MENU) & 0x8000) != 0)
		{
			modifiers += EInputModifier::Alt;
		}
		if ((::GetKeyState(VK_LWIN) & 0x8000) != 0 || (::GetKeyState(VK_RWIN) & 0x8000) != 0)
		{
			modifiers += EInputModifier::Super;
		}
		if ((::GetKeyState(VK_CAPITAL) & 0x0001) != 0)
		{
			modifiers += EInputModifier::CapsLock;
		}
		if ((::GetKeyState(VK_NUMLOCK) & 0x0001) != 0)
		{
			modifiers += EInputModifier::NumLock;
		}

		return modifiers;
	}

	KeyCode TranslateScanCode (uint16 ScanCode, bool bExtended)
	{
		const uint16 code = static_cast<uint16>(ScanCode | (bExtended ? 0x0100u : 0u));
		switch (code)
		{
			case 0x0001: return KeyCode::Escape;
			case 0x0002: return KeyCode::Digit1;
			case 0x0003: return KeyCode::Digit2;
			case 0x0004: return KeyCode::Digit3;
			case 0x0005: return KeyCode::Digit4;
			case 0x0006: return KeyCode::Digit5;
			case 0x0007: return KeyCode::Digit6;
			case 0x0008: return KeyCode::Digit7;
			case 0x0009: return KeyCode::Digit8;
			case 0x000A: return KeyCode::Digit9;
			case 0x000B: return KeyCode::Digit0;
			case 0x000C: return KeyCode::Minus;
			case 0x000D: return KeyCode::Equals;
			case 0x000E: return KeyCode::Backspace;
			case 0x000F: return KeyCode::Tab;
			case 0x0010: return KeyCode::Q;
			case 0x0011: return KeyCode::W;
			case 0x0012: return KeyCode::E;
			case 0x0013: return KeyCode::R;
			case 0x0014: return KeyCode::T;
			case 0x0015: return KeyCode::Y;
			case 0x0016: return KeyCode::U;
			case 0x0017: return KeyCode::I;
			case 0x0018: return KeyCode::O;
			case 0x0019: return KeyCode::P;
			case 0x001A: return KeyCode::LeftBracket;
			case 0x001B: return KeyCode::RightBracket;
			case 0x001C: return KeyCode::Enter;
			case 0x011C: return KeyCode::NumpadEnter;
			case 0x001D: return KeyCode::LeftCtrl;
			case 0x011D: return KeyCode::RightCtrl;
			case 0x001E: return KeyCode::A;
			case 0x001F: return KeyCode::S;
			case 0x0020: return KeyCode::D;
			case 0x0021: return KeyCode::F;
			case 0x0022: return KeyCode::G;
			case 0x0023: return KeyCode::H;
			case 0x0024: return KeyCode::J;
			case 0x0025: return KeyCode::K;
			case 0x0026: return KeyCode::L;
			case 0x0027: return KeyCode::Semicolon;
			case 0x0028: return KeyCode::Apostrophe;
			case 0x0029: return KeyCode::Grave;
			case 0x002A: return KeyCode::LeftShift;
			case 0x002B: return KeyCode::Backslash;
			case 0x002C: return KeyCode::Z;
			case 0x002D: return KeyCode::X;
			case 0x002E: return KeyCode::C;
			case 0x002F: return KeyCode::V;
			case 0x0030: return KeyCode::B;
			case 0x0031: return KeyCode::N;
			case 0x0032: return KeyCode::M;
			case 0x0033: return KeyCode::Comma;
			case 0x0034: return KeyCode::Period;
			case 0x0035: return KeyCode::Slash;
			case 0x0036: return KeyCode::RightShift;
			case 0x0037: return KeyCode::NumpadMultiply;
			case 0x0135: return KeyCode::NumpadDivide;
			case 0x0038: return KeyCode::LeftAlt;
			case 0x0138: return KeyCode::RightAlt;
			case 0x0039: return KeyCode::Space;
			case 0x003A: return KeyCode::CapsLock;
			case 0x003B: return KeyCode::F1;
			case 0x003C: return KeyCode::F2;
			case 0x003D: return KeyCode::F3;
			case 0x003E: return KeyCode::F4;
			case 0x003F: return KeyCode::F5;
			case 0x0040: return KeyCode::F6;
			case 0x0041: return KeyCode::F7;
			case 0x0042: return KeyCode::F8;
			case 0x0043: return KeyCode::F9;
			case 0x0044: return KeyCode::F10;
			case 0x0057: return KeyCode::F11;
			case 0x0058: return KeyCode::F12;
			case 0x0045: return KeyCode::NumLock;
			case 0x0046: return KeyCode::ScrollLock;
			case 0x0047: return KeyCode::Numpad7;
			case 0x0048: return KeyCode::Numpad8;
			case 0x0049: return KeyCode::Numpad9;
			case 0x004A: return KeyCode::NumpadMinus;
			case 0x004B: return KeyCode::Numpad4;
			case 0x004C: return KeyCode::Numpad5;
			case 0x004D: return KeyCode::Numpad6;
			case 0x004E: return KeyCode::NumpadPlus;
			case 0x004F: return KeyCode::Numpad1;
			case 0x0050: return KeyCode::Numpad2;
			case 0x0051: return KeyCode::Numpad3;
			case 0x0052: return KeyCode::Numpad0;
			case 0x0053: return KeyCode::NumpadDecimal;
			case 0x0056: return KeyCode::NonUSBackslash;
			case 0x0147: return KeyCode::Home;
			case 0x0148: return KeyCode::ArrowUp;
			case 0x0149: return KeyCode::PageUp;
			case 0x014B: return KeyCode::ArrowLeft;
			case 0x014D: return KeyCode::ArrowRight;
			case 0x014F: return KeyCode::End;
			case 0x0150: return KeyCode::ArrowDown;
			case 0x0151: return KeyCode::PageDown;
			case 0x0152: return KeyCode::Insert;
			case 0x0153: return KeyCode::Delete;
			case 0x015B: return KeyCode::LeftMeta;
			case 0x015C: return KeyCode::RightMeta;
			case 0x015D: return KeyCode::Menu;
			default: break;
		}

		return KeyCode::Unknown;
	}

	KeyCode TranslateKey (WPARAM wParam, LPARAM lParam)
	{
		const uint16 scanCode = static_cast<uint16>((lParam >> 16) & 0xFF);
		const bool bExtended = (lParam & (1 << 24)) != 0;
		KeyCode key = TranslateScanCode(scanCode, bExtended);
		if (key != KeyCode::Unknown)
		{
			return key;
		}

		switch (wParam)
		{
			case VK_SNAPSHOT: return KeyCode::PrintScreen;
			case VK_PAUSE: return KeyCode::Pause;
			default: break;
		}

		return KeyCode::Unknown;
	}

	Vector2f ToMousePosition (LPARAM lParam)
	{
		const int32 x = static_cast<int32>(static_cast<int16>(LOWORD(lParam)));
		const int32 y = static_cast<int32>(static_cast<int16>(HIWORD(lParam)));
		return Vector2f(static_cast<float>(x), static_cast<float>(y));
	}

	Vector2f ToMousePositionFromScreen (HWND hWnd, LPARAM lParam)
	{
		POINT point = {};
		point.x = static_cast<LONG>(static_cast<int16>(LOWORD(lParam)));
		point.y = static_cast<LONG>(static_cast<int16>(HIWORD(lParam)));
		::ScreenToClient(hWnd, &point);
		return Vector2f(static_cast<float>(point.x), static_cast<float>(point.y));
	}

	EMouseButton TranslateMouseButton (UINT message, WPARAM wParam)
	{
		switch (message)
		{
			case WM_LBUTTONDOWN:
			case WM_LBUTTONUP: return EMouseButton::Left;
			case WM_RBUTTONDOWN:
			case WM_RBUTTONUP: return EMouseButton::Right;
			case WM_MBUTTONDOWN:
			case WM_MBUTTONUP: return EMouseButton::Middle;
			case WM_XBUTTONDOWN:
			case WM_XBUTTONUP:
			{
				const uint16 xButton = static_cast<uint16>(HIWORD(wParam));
				return (xButton == XBUTTON1) ? EMouseButton::X1 : EMouseButton::X2;
			}
			default: break;
		}

		return EMouseButton::Left;
	}

	void FillHeader (SInputEventHeader& Header, HWND hWnd, EInputDeviceType Type, DeviceId Device)
	{
		Header.Window = ToWindowId(hWnd);
		Header.DeviceType = Type;
		Header.Device = Device;
		Header.TimestampSeconds = GetMessageTimestampSeconds();
	}
}


void HandleWin32Message (HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	static std::unordered_map<WindowId, Vector2f> lastMousePositions;

	switch (message)
	{
		case WM_KEYDOWN:
		case WM_SYSKEYDOWN:
		case WM_KEYUP:
		case WM_SYSKEYUP:
		{
			const bool bIsDown = (message == WM_KEYDOWN) || (message == WM_SYSKEYDOWN);
			const bool bWasDown = (lParam & (1 << 30)) != 0;
			const EInputAction action = bIsDown
											? (bWasDown ? EInputAction::Repeat : EInputAction::Pressed)
											: EInputAction::Released;

			const KeyCode key = TranslateKey(wParam, lParam);
			if (key == KeyCode::Unknown)
			{
				return;
			}

			SKeyboardEventData data = {};
			FillHeader(data.Header, hWnd, EInputDeviceType::Keyboard, KeyboardDeviceId);
			data.Key = key;
			data.Action = action;
			data.Modifiers = GetModifiers();
			CPlatformInputQueue::Enqueue(data);
			break;
		}

		case WM_MOUSEMOVE:
		{
			const WindowId windowId = ToWindowId(hWnd);
			const Vector2f position = ToMousePosition(lParam);
			const auto lastIt = lastMousePositions.find(windowId);
			const Vector2f lastPos = (lastIt != lastMousePositions.end()) ? lastIt->second : position;
			lastMousePositions[windowId] = position;

			SMouseMoveEventData data = {};
			FillHeader(data.Header, hWnd, EInputDeviceType::Mouse, MouseDeviceId);
			data.Position = position;
			data.Delta = position - lastPos;
			CPlatformInputQueue::Enqueue(data);
			break;
		}

		case WM_LBUTTONDOWN:
		case WM_LBUTTONUP:
		case WM_RBUTTONDOWN:
		case WM_RBUTTONUP:
		case WM_MBUTTONDOWN:
		case WM_MBUTTONUP:
		case WM_XBUTTONDOWN:
		case WM_XBUTTONUP:
		{
			const bool bIsDown = (message == WM_LBUTTONDOWN) || (message == WM_RBUTTONDOWN)
								|| (message == WM_MBUTTONDOWN) || (message == WM_XBUTTONDOWN);
			const EInputAction action = bIsDown ? EInputAction::Pressed : EInputAction::Released;

			SMouseButtonEventData data = {};
			FillHeader(data.Header, hWnd, EInputDeviceType::Mouse, MouseDeviceId);
			data.Button = TranslateMouseButton(message, wParam);
			data.Action = action;
			data.Modifiers = GetModifiers();
			data.Position = ToMousePosition(lParam);
			CPlatformInputQueue::Enqueue(data);
			break;
		}

		case WM_MOUSEWHEEL:
		case WM_MOUSEHWHEEL:
		{
			const int16 wheelDelta = static_cast<int16>(HIWORD(wParam));
			const float delta = static_cast<float>(wheelDelta) / static_cast<float>(WHEEL_DELTA);

			SMouseWheelEventData data = {};
			FillHeader(data.Header, hWnd, EInputDeviceType::Mouse, MouseDeviceId);
			data.Delta = delta;
			data.Position = ToMousePositionFromScreen(hWnd, lParam);
			CPlatformInputQueue::Enqueue(data);
			break;
		}

		default: break;
	}
}
}

#endif
