#include "Input/PlatformInput.h"

#include <mutex>
#include <utility>


namespace frt::input
{
namespace
{
	std::vector<SInputEvent> gEventQueue;
	std::mutex gQueueMutex;

	void EnqueueInternal (EInputEventType Type, FInputEventData Data)
	{
		std::scoped_lock lock(gQueueMutex);
		gEventQueue.push_back(SInputEvent{ Type, std::move(Data) });
	}
}


void CPlatformInputQueue::Enqueue (const SKeyboardEventData& Data)
{
	EnqueueInternal(EInputEventType::Keyboard, Data);
}

void CPlatformInputQueue::Enqueue (const SMouseButtonEventData& Data)
{
	EnqueueInternal(EInputEventType::MouseButton, Data);
}

void CPlatformInputQueue::Enqueue (const SMouseMoveEventData& Data)
{
	EnqueueInternal(EInputEventType::MouseMove, Data);
}

void CPlatformInputQueue::Enqueue (const SMouseWheelEventData& Data)
{
	EnqueueInternal(EInputEventType::MouseWheel, Data);
}

void CPlatformInputQueue::Enqueue (const SGamepadButtonEventData& Data)
{
	EnqueueInternal(EInputEventType::GamepadButton, Data);
}

void CPlatformInputQueue::Enqueue (const SGamepadAxisEventData& Data)
{
	EnqueueInternal(EInputEventType::GamepadAxis, Data);
}

void CPlatformInputQueue::Enqueue (const SDeviceConnectionEventData& Data)
{
	EnqueueInternal(EInputEventType::DeviceConnection, Data);
}

void CPlatformInputQueue::Drain (std::vector<SInputEvent>& OutEvents)
{
	std::scoped_lock lock(gQueueMutex);
	if (gEventQueue.empty())
	{
		return;
	}

	OutEvents.insert(OutEvents.end(), gEventQueue.begin(), gEventQueue.end());
	gEventQueue.clear();
}

void CPlatformInputQueue::Clear ()
{
	std::scoped_lock lock(gQueueMutex);
	gEventQueue.clear();
}
}
