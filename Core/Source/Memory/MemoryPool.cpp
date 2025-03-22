#include "MemoryPool.h"

#include <new>

#if _WINDOWS
// #include <intrin.h>
#include <windows.h>
#include <memoryapi.h>
#elif _UNIX
#include <sys/mman.h>
#endif


namespace frt::memory
{
	CMemoryPool::CMemoryPool(CMemoryPool&& Other) noexcept
	{
		*this = std::move(Other);
	}

	CMemoryPool& CMemoryPool::operator=(CMemoryPool&& Other) noexcept
	{
		MemorySize = Other.MemorySize;
		Memory = Other.Memory;
		Tlsf = Other.Tlsf;
		Other.MemorySize = 0;
		Other.Memory = nullptr;
		Other.Tlsf = nullptr;
		return *this;
	}

	CMemoryPool::CMemoryPool()
	{
	}

	CMemoryPool::CMemoryPool(uint64 InSize) : MemorySize(InSize)
	{
#if _WINDOWS
		// Memory = new uint8[MemorySize];
		Memory = (uint8*)VirtualAlloc(nullptr, MemorySize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#elif _UNIX
		Memory = mmap(nullptr, MemorySize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		// munmap(pool, PoolSize);
#endif
		Tlsf = new (Memory) TLSF(MemorySize);
	}

	CMemoryPool::CMemoryPool(void* InMemory, uint64 InSize) : MemorySize(InSize)
	{
		frt_assert((uint64)InMemory % TLSF::AlignSize == 0);

		Memory = static_cast<uint8*>(InMemory);
		Tlsf = new (Memory) TLSF(MemorySize);
	}

	CMemoryPool::~CMemoryPool()
	{
		if (Memory)
		{
			frt_assert(Tlsf);
			Tlsf->~TLSF();
#if _WINDOWS
			VirtualFree(Memory, 0, MEM_RELEASE);
#elif _UNIX
		
#endif
		}

		Memory = nullptr;
		Tlsf = nullptr;
	}

	void CMemoryPool::MakeThisPrimaryInstance()
	{
		PrimaryInstance = this;
	}

	CMemoryPool* CMemoryPool::GetPrimaryInstance()
	{
		return PrimaryInstance;
	}

	void* CMemoryPool::Allocate(uint64 Size)
	{
		frt_assert(Tlsf);

		return Tlsf->Malloc(Size);
	}

	void* CMemoryPool::ReAllocate(void* InMemory, uint64 Size)
	{
		frt_assert(Tlsf);
		frt_assert(InMemory);

		return Tlsf->Realloc(InMemory, Size);
	}

	void CMemoryPool::Free(void* MemoryToFree)
	{
		frt_assert(Tlsf);

		if (MemoryToFree)
		{
			Tlsf->Free(MemoryToFree);
		}
	}
}
