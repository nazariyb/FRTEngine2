#include "MemoryPool.h"

#include <cstdio>
#include <new>

#include "Containers/String.h"
#include "Platform/Platform.h"
#include "Platform/Thread.h"

#if _WINDOWS
// #include <intrin.h>
#include <windows.h>
#include <memoryapi.h>
#elif _UNIX
#include <sys/mman.h>
#endif


namespace
{
// A thread that used a pool it does not own has been reported: once per thread, whichever pool.
thread_local bool bForeignThreadReported = false;

uint64 GetBlockSize (void* Memory)
{
	return frt::memory::TLSF::SBlockHeader::Cast(Memory)->GetSize();
}
}


namespace frt::memory
{
CMemoryPool::CMemoryPool (CMemoryPool&& Other) noexcept
{
	*this = std::move(Other);
}

CMemoryPool& CMemoryPool::operator= (CMemoryPool&& Other) noexcept
{
	if (this == &Other)
	{
		return *this;
	}

	// Without this, move-assigning onto a live pool silently leaks its whole arena -
	// gigabytes, in the case of GameInstance's.
	ReleaseMemory();

	MemorySize = Other.MemorySize;
	Memory = Other.Memory;
	Tlsf = Other.Tlsf;
	OwnerThreadId = Other.OwnerThreadId;
	AllocatedBytes = Other.AllocatedBytes;
	PeakAllocatedBytes = Other.PeakAllocatedBytes;
	HighWaterThreshold = Other.HighWaterThreshold;
	HighWaterFunction = Other.HighWaterFunction;
	HighWaterUserData = Other.HighWaterUserData;
	Other.MemorySize = 0;
	Other.Memory = nullptr;
	Other.Tlsf = nullptr;
	Other.AllocatedBytes = 0ull;
	Other.PeakAllocatedBytes = 0ull;
	Other.HighWaterThreshold = 0ull;
	Other.HighWaterFunction = nullptr;
	Other.HighWaterUserData = nullptr;

	// The arena moved, so the primary pointer has to follow it. Otherwise every holder
	// of GetPrimaryInstance() is left pointing at a pool that no longer owns anything.
	if (PrimaryInstance == &Other)
	{
		PrimaryInstance = this;
	}

	return *this;
}

CMemoryPool::CMemoryPool ()
	: OwnerThreadId(platform::GetCurrentThreadId())
{}

CMemoryPool::CMemoryPool (uint64 InSize)
	: MemorySize(TLSF::GetSize() + TLSF::GetPoolOverhead() + TLSF::AlignUp(InSize, TLSF::AlignSize))
	, OwnerThreadId(platform::GetCurrentThreadId())
{
#if _WINDOWS
	// Memory = new uint8[MemorySize];
	Memory = (uint8*)VirtualAlloc(nullptr, MemorySize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#elif _UNIX
		Memory = mmap(nullptr, MemorySize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		// munmap(pool, PoolSize);
#endif
	frt_assert(Memory);
	Tlsf = new(Memory) TLSF(MemorySize);
}

CMemoryPool::CMemoryPool (void* InMemory, uint64 InSize)
	: MemorySize(InSize)
	, OwnerThreadId(platform::GetCurrentThreadId())
{
	frt_assert((uint64)InMemory % TLSF::AlignSize == 0);

	Memory = static_cast<uint8*>(InMemory);
	Tlsf = new(Memory) TLSF(MemorySize);
}

CMemoryPool::~CMemoryPool ()
{
	ReleaseMemory();
}

void CMemoryPool::ReleaseMemory ()
{
	// Clear the primary pointer BEFORE releasing the arena. Leaving it dangling turns
	// every later allocation into a use-after-free that reads as random corruption;
	// nulling it makes the misuse fail immediately and visibly instead.
	if (PrimaryInstance == this)
	{
		PrimaryInstance = nullptr;
	}

	if (Memory)
	{
		frt_assert(Tlsf);
		Tlsf->~TLSF();
#if _WINDOWS
		VirtualFree(Memory, 0, MEM_RELEASE);
#elif _UNIX
		munmap(Memory, MemorySize);
#endif
	}

	Memory = nullptr;
	Tlsf = nullptr;
	MemorySize = 0ull;
	// The counters describe the arena, which is gone.
	AllocatedBytes = 0ull;
	PeakAllocatedBytes = 0ull;
	HighWaterThreshold = 0ull;
	HighWaterFunction = nullptr;
	HighWaterUserData = nullptr;
}

void CMemoryPool::MakeThisPrimaryInstance ()
{
	PrimaryInstance = this;
	OwnerThreadId = platform::GetCurrentThreadId();
}

CMemoryPool* CMemoryPool::GetPrimaryInstance ()
{
	return PrimaryInstance;
}

void* CMemoryPool::Allocate (uint64 Size)
{
	frt_assert(Tlsf);
	CheckOwnerThread("Allocate");

	void* memory = Tlsf->Malloc(Size);
	if (memory)
	{
		ApplyAllocatedDelta(0ull, GetBlockSize(memory));
	}
	return memory;
}

void* CMemoryPool::ReAllocate (void* InMemory, uint64 Size)
{
	frt_assert(Tlsf);
	CheckOwnerThread("ReAllocate");

	const uint64 oldBytes = InMemory ? GetBlockSize(InMemory) : 0ull;
	void* memory = Tlsf->Realloc(InMemory, Size);
	if (memory)
	{
		ApplyAllocatedDelta(oldBytes, GetBlockSize(memory));
	}
	else if (InMemory && Size == 0ull)
	{
		// Realloc to zero frees; a failed Realloc (nullptr for a non-zero size) keeps the old block.
		ApplyAllocatedDelta(oldBytes, 0ull);
	}
	return memory;
}

void CMemoryPool::Free (void* MemoryToFree)
{
	frt_assert(Tlsf);

	if (MemoryToFree)
	{
		CheckOwnerThread("Free");
		ApplyAllocatedDelta(GetBlockSize(MemoryToFree), 0ull);
		Tlsf->Free(MemoryToFree);
	}
}

void CMemoryPool::SetHighWaterCallback (uint64 ThresholdBytes, HighWaterCallback Callback, void* UserData)
{
	HighWaterThreshold = ThresholdBytes;
	HighWaterFunction = Callback;
	HighWaterUserData = Callback ? UserData : nullptr;
}

void CMemoryPool::ApplyAllocatedDelta (uint64 OldBytes, uint64 NewBytes)
{
	// Subtracting the old block first and testing the crossing afterwards would measure from the
	// reduced count: every reallocation of a block over the threshold would look like a crossing.
	const uint64 before = AllocatedBytes;
	AllocatedBytes = AllocatedBytes - OldBytes + NewBytes;
	PeakAllocatedBytes = (std::max)(PeakAllocatedBytes, AllocatedBytes);
	if (HighWaterFunction && before < HighWaterThreshold && AllocatedBytes >= HighWaterThreshold)
	{
		HighWaterFunction(*this, AllocatedBytes, HighWaterUserData);
	}
}

void CMemoryPool::CheckOwnerThread (const char* Operation)
{
	const uint64 threadId = platform::GetCurrentThreadId();
	if (threadId != OwnerThreadId) [[unlikely]]
	{
		// TODO(M1): frt_assert in Debug builds as well (AssetManager-Design I8, "assert in Debug from
		// M1"), once the worker pool has moved every off-thread allocation to the CRT heap.
		if (!bForeignThreadReported)
		{
			bForeignThreadReported = true;
			ReportForeignThread(Operation, threadId);
		}
	}
}

void CMemoryPool::ReportForeignThread (const char* Operation, uint64 ThreadId) const
{
	const FString message = FString::Printf(
		"[memory] CMemoryPool %p: %s on thread %llu, but the pool belongs to thread %llu. TLSF is single-threaded: "
		"code off the owner thread uses std::vector and the CRT heap (reported once per thread)\n",
		static_cast<const void*>(this), Operation, ThreadId, OwnerThreadId);
	std::fputs(message.CStr(), stderr);
	platform::OutputDebugText(message);
}
}
