#include "Platform/Thread.h"

#include <Windows.h>


namespace frt::platform
{
uint64 GetCurrentThreadId () noexcept
{
	return static_cast<uint64>(::GetCurrentThreadId());
}
}
