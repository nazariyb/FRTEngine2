#include "Platform/Thread.h"

#include <vector>

#include <Windows.h>

#include "Containers/WideString.h"


namespace frt::platform
{
namespace
{
int ToWin32Priority (EThreadPriority Priority) noexcept
{
	switch (Priority)
	{
	case EThreadPriority::Lowest:      return THREAD_PRIORITY_LOWEST;
	case EThreadPriority::BelowNormal: return THREAD_PRIORITY_BELOW_NORMAL;
	case EThreadPriority::Normal:      return THREAD_PRIORITY_NORMAL;
	case EThreadPriority::AboveNormal: return THREAD_PRIORITY_ABOVE_NORMAL;
	case EThreadPriority::Highest:     return THREAD_PRIORITY_HIGHEST;
	}
	return THREAD_PRIORITY_NORMAL;
}

// The CPU set ids of the processors AffinityMask selects: bit i is the i-th processor (CPU set)
// GetSystemCpuSetInformation lists, across processor groups. Empty when none matches.
std::vector<ULONG> SelectCpuSets (uint64 AffinityMask)
{
	std::vector<ULONG> selected;
	ULONG length = 0u;
	// Asked with no buffer first: fails with ERROR_INSUFFICIENT_BUFFER and the size it needs.
	(void)GetSystemCpuSetInformation(nullptr, 0u, &length, GetCurrentProcess(), 0u);
	if (length == 0u)
	{
		return selected;
	}
	std::vector<uint8> buffer(length);
	if (!GetSystemCpuSetInformation(reinterpret_cast<SYSTEM_CPU_SET_INFORMATION*>(buffer.data()), length, &length,
		GetCurrentProcess(), 0u))
	{
		return selected;
	}

	uint32 index = 0u;
	for (ULONG offset = 0u; offset + sizeof(DWORD) * 2u <= length;)
	{
		const auto* info = reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(buffer.data() + offset);
		if (info->Size == 0u)
		{
			break;
		}
		if (info->Type == CpuSetInformation)
		{
			if (index < 64u && ((AffinityMask >> index) & 1u) != 0u)
			{
				selected.push_back(info->CpuSet.Id);
			}
			++index;
		}
		offset += info->Size;
	}
	return selected;
}
}


uint64 GetCurrentThreadId () noexcept
{
	return static_cast<uint64>(::GetCurrentThreadId());
}

bool ConfigureThread (FStringView Name, EThreadPriority Priority, uint64 AffinityMask) noexcept
{
	const HANDLE thread = GetCurrentThread();
	bool bApplied = SUCCEEDED(SetThreadDescription(thread, ToWide(Name).CStr()));
	bApplied = SetThreadPriority(thread, ToWin32Priority(Priority)) != 0 && bApplied;
	if (AffinityMask != 0u)
	{
		const std::vector<ULONG> cpuSets = SelectCpuSets(AffinityMask);
		bApplied = !cpuSets.empty()
			&& SetThreadSelectedCpuSets(thread, cpuSets.data(), static_cast<ULONG>(cpuSets.size())) != 0
			&& bApplied;
	}
	return bApplied;
}

FString GetCurrentThreadName ()
{
	PWSTR description = nullptr;
	if (FAILED(GetThreadDescription(GetCurrentThread(), &description)) || description == nullptr)
	{
		return {};
	}
	FString name = FString::FromWide(description);
	LocalFree(description);
	return name;
}
}
