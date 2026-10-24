#include "Platform/VirtualMemory.h"

#include "Platform/Windows/WindowsCommon.h"


namespace frt::platform
{
namespace
{
struct SSystemMemoryInfo
{
	uint64 PageSize = 4096u;
	uint64 AllocationGranularity = 65536u;
};

// Asked once: neither value changes while the process runs.
const SSystemMemoryInfo& GetSystemMemoryInfo () noexcept
{
	static const SSystemMemoryInfo info = []
	{
		SYSTEM_INFO system = {};
		GetSystemInfo(&system);
		SSystemMemoryInfo result;
		result.PageSize = system.dwPageSize;
		result.AllocationGranularity = system.dwAllocationGranularity;
		return result;
	}();
	return info;
}

bool IsPageAligned (uint64 Value) noexcept
{
	return (Value & (GetPageSize() - 1u)) == 0u;
}
}


uint64 GetPageSize () noexcept
{
	return GetSystemMemoryInfo().PageSize;
}

uint64 GetAllocationGranularity () noexcept
{
	return GetSystemMemoryInfo().AllocationGranularity;
}

void* ReserveVirtual (uint64 Bytes) noexcept
{
	if (Bytes == 0u)
	{
		return nullptr;
	}
	return VirtualAlloc(nullptr, static_cast<SIZE_T>(Bytes), MEM_RESERVE, PAGE_NOACCESS);
}

bool CommitVirtual (void* Address, uint64 Bytes) noexcept
{
	if (Address == nullptr || Bytes == 0u)
	{
		return false;
	}
	// VirtualAlloc rounds the range out to whole pages itself, and fails for any page outside a
	// reservation.
	return VirtualAlloc(Address, static_cast<SIZE_T>(Bytes), MEM_COMMIT, PAGE_READWRITE) != nullptr;
}

bool DecommitVirtual (void* Address, uint64 Bytes) noexcept
{
	// VirtualFree would round a partial page out and decommit its neighbour's bytes too.
	if (Address == nullptr || Bytes == 0u || !IsPageAligned(reinterpret_cast<uint64>(Address)) || !IsPageAligned(Bytes))
	{
		return false;
	}
	// The analyzer flags MEM_DECOMMIT as a possible leak of the address space (C6250): it is the
	// point here; ReleaseVirtual frees the reservation.
#pragma warning(suppress: 6250)
	return VirtualFree(Address, static_cast<SIZE_T>(Bytes), MEM_DECOMMIT) != 0;
}

bool ReleaseVirtual (void* Base, uint64 ReservedBytes) noexcept
{
	(void)ReservedBytes;   // MEM_RELEASE frees the whole reservation and requires a size of 0
	if (Base == nullptr)
	{
		return false;
	}
	return VirtualFree(Base, 0u, MEM_RELEASE) != 0;
}
}
