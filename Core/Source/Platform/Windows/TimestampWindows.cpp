#include "Platform/Timestamp.h"

#include <Windows.h>


namespace frt::platform
{
uint64 ReadTimestamp () noexcept
{
	LARGE_INTEGER counter;
	// Cannot fail on Windows XP or later.
	::QueryPerformanceCounter(&counter);
	return static_cast<uint64>(counter.QuadPart);
}

uint64 GetTimestampFrequency () noexcept
{
	static const uint64 frequency = []
	{
		LARGE_INTEGER value;
		::QueryPerformanceFrequency(&value);
		return static_cast<uint64>(value.QuadPart);
	}();
	return frequency;
}
}
