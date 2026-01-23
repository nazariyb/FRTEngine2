#pragma once

#include <variant>
#include <vector>

#include "Core.h"
#include "Input/InputTypes.h"


namespace frt::input
{
enum class EInputEventType : uint8
{
	Keyboard = 0u,
	MouseButton,
	MouseMove,
	MouseWheel,
	GamepadButton,
	GamepadAxis,
	DeviceConnection
};

using FInputEventData = std::variant<
	SKeyboardEventData,
	SMouseButtonEventData,
	SMouseMoveEventData,
	SMouseWheelEventData,
	SGamepadButtonEventData,
	SGamepadAxisEventData,
	SDeviceConnectionEventData>;

struct SInputEvent
{
	EInputEventType Type = EInputEventType::Keyboard;
	FInputEventData Data;
};


class FRT_CORE_API CPlatformInputQueue
{
public:
	static void Enqueue (const SKeyboardEventData& Data);
	static void Enqueue (const SMouseButtonEventData& Data);
	static void Enqueue (const SMouseMoveEventData& Data);
	static void Enqueue (const SMouseWheelEventData& Data);
	static void Enqueue (const SGamepadButtonEventData& Data);
	static void Enqueue (const SGamepadAxisEventData& Data);
	static void Enqueue (const SDeviceConnectionEventData& Data);

	static void Drain (std::vector<SInputEvent>& OutEvents);
	static void Clear ();
};
}
