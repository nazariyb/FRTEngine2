#pragma once

#include <memory>
#include <new>
#include <utility>

#include "Allocator.h"
#include "Core.h"
#include "CoreTypes.h"
#include "CoreUtils.h"
#include "Ref.h"
#include "TLSF.h"


namespace frt::memory
{
/**
 * A TLSF arena (Memory/TLSF.h): the primary instance is what TArray, component pools and managed
 * objects allocate from.
 *
 * TLSF is single-threaded, and so is the pool: it belongs to one thread, the one that constructed
 * it or last made it the primary instance. Allocate, ReAllocate or Free on any other thread is a
 * data race on the free lists; code that runs on worker threads or before the pool exists uses
 * std::vector and the CRT heap instead. The pool compares thread ids on every call (one compare,
 * AssetManager-Design I8) and reports the first offence of each thread through frt::log (category
 * "memory", a warning); Debug builds then assert, Development builds carry on.
 *
 * It also counts the bytes in its allocated blocks, with their peak, and can call a function when
 * that count first reaches a threshold: plain integers, never atomics, for the same reason.
 */
class FRT_CORE_API CMemoryPool : public IAllocator
{
public:
	// Called by the allocation that takes the allocated bytes from below the threshold to the
	// threshold or more (again only after they fall below it and climb back), on the owner thread,
	// inside Allocate or ReAllocate. A reallocation counts by its net change, the count before the
	// call against the count after it, so growing, shrinking or keeping a block while the pool is
	// already over the threshold never calls it. It may allocate, and may set a new threshold.
	using HighWaterFunction = void (*) (CMemoryPool& Pool, uint64 AllocatedBytes, void* Context);

	FRT_DELETE_COPY_OPS(CMemoryPool);
	// Moves the arena with its owner thread, counters and high-water callback.
	CMemoryPool (CMemoryPool&& Other) noexcept;
	CMemoryPool& operator= (CMemoryPool&& Other) noexcept;

	CMemoryPool ();
	CMemoryPool (uint64 InSize);
	CMemoryPool (void* InMemory, uint64 InSize);

	virtual ~CMemoryPool () override;

	// Also makes the calling thread the owner.
	void MakeThisPrimaryInstance ();
	static CMemoryPool* GetPrimaryInstance ();

	virtual void* Allocate (uint64 Size) override;
	virtual void* ReAllocate (void* Memory, uint64 Size);
	virtual void Free (void* MemoryToFree) override;

	// The OS id (platform::GetCurrentThreadId) of the thread the pool belongs to.
	[[nodiscard]] uint64 GetOwnerThreadId () const { return OwnerThreadId; }

	// Bytes in allocated blocks: TLSF's block sizes (each request rounded up to 8 bytes, at least
	// 24), without the 8-byte block headers. The peak is the highest value since construction or
	// the last ResetPeakAllocatedBytes.
	[[nodiscard]] uint64 GetAllocatedBytes () const { return AllocatedBytes; }
	[[nodiscard]] uint64 GetPeakAllocatedBytes () const { return PeakAllocatedBytes; }
	void ResetPeakAllocatedBytes () { PeakAllocatedBytes = AllocatedBytes; }

	// One callback per pool; a null Function removes it. Setting it never calls it, even when the
	// count is already at or above the threshold. Context is passed to it as it is.
	void SetHighWaterCallback (uint64 ThresholdBytes, HighWaterFunction Function, void* Context = nullptr);

	template <typename T, typename... Args>
	[[nodiscard]] T* NewUnmanaged (Args&&... InArgs);

	template <typename T, typename... Args>
	[[nodiscard]] TRefControlBlock<T>* NewManaged (Args&&... InArgs);

	template <typename T, typename... Args>
	[[nodiscard]] TRefUnique<T> NewUnique (Args&&... InArgs);

	template <typename T, typename... Args>
	[[nodiscard]] TRefShared<T> NewShared (Args&&... InArgs);

	void DeleteUnmanaged (void* MemoryToDelete);
	virtual void DeleteManaged (void* MemoryToDelete) override;

private:
	/** Releases the arena and clears PrimaryInstance if it points here. Idempotent. */
	void ReleaseMemory ();

	// Operation: "Allocate", "ReAllocate" or "Free", for the report.
	void CheckOwnerThread (const char* Operation);
	void ReportForeignThread (const char* Operation, uint64 ThreadId) const;
	// One allocation, reallocation or free: OldBytes leave the count and NewBytes join it in one
	// step, so the high-water test compares the count before the call with the count after it.
	void ApplyAllocatedDelta (uint64 OldBytes, uint64 NewBytes);

	uint64 MemorySize = 0ull;
	uint8* Memory = nullptr;

	TLSF* Tlsf = nullptr;

	uint64 OwnerThreadId = 0ull;
	uint64 AllocatedBytes = 0ull;
	uint64 PeakAllocatedBytes = 0ull;
	uint64 HighWaterThreshold = 0ull;
	HighWaterFunction OnHighWater = nullptr;
	void* HighWaterContext = nullptr;

	// Just for convenience, so we don't have to access GameInstance each time
	static inline CMemoryPool* PrimaryInstance = nullptr;
};
}


namespace frt::memory
{
template <typename T, typename... Args>
T* CMemoryPool::NewUnmanaged (Args&&... InArgs)
{
	constexpr uint64 size = sizeof(T);
	void* memory = Allocate(size);
	T* object = new(memory) T(std::forward<Args>(InArgs)...);
	return object;
}

template <typename T, typename... Args>
TRefControlBlock<T>* CMemoryPool::NewManaged (Args&&... InArgs)
{
	constexpr uint64 size = TRefControlBlock<T>::GetFullSize();
	void* memory = Allocate(size);

	auto* control = new(memory) TRefControlBlock<T>(std::forward<Args>(InArgs)...);
	control->Allocator = PrimaryInstance;
	return control;
}

template <typename T, typename... Args>
TRefUnique<T> CMemoryPool::NewUnique (Args&&... InArgs)
{
	return TRefUnique<T>(NewManaged<T>(std::forward<Args>(InArgs)...));
}

template <typename T, typename... Args>
TRefShared<T> CMemoryPool::NewShared (Args&&... InArgs)
{
	return TRefShared<T>(NewManaged<T>(std::forward<Args>(InArgs)...));
}

inline void CMemoryPool::DeleteUnmanaged (void* MemoryToDelete)
{
	Free(MemoryToDelete);
}

inline void CMemoryPool::DeleteManaged (void* MemoryToDelete)
{
	Free(MemoryToDelete);
}
}
