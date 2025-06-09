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
* 
*/
class FRT_CORE_API CMemoryPool : public IAllocator
{
public:
	FRT_DELETE_COPY_OPS(CMemoryPool);
	CMemoryPool (CMemoryPool&& Other) noexcept;
	CMemoryPool& operator= (CMemoryPool&& Other) noexcept;

	CMemoryPool ();
	CMemoryPool (uint64 InSize);
	CMemoryPool (void* InMemory, uint64 InSize);

	virtual ~CMemoryPool () override;

	void MakeThisPrimaryInstance ();
	static CMemoryPool* GetPrimaryInstance ();

	virtual void* Allocate (uint64 Size) override;
	virtual void* ReAllocate (void* Memory, uint64 Size);
	virtual void Free (void* MemoryToFree) override;

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
	uint64 MemorySize = 0ull;
	uint8* Memory = nullptr;

	TLSF* Tlsf = nullptr;

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
