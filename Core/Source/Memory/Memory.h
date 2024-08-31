#pragma once

#include "MemoryUtility.h"
#include "PoolAllocator.h"


namespace frt::memory
{
	class PoolAllocator;
	using DefaultAllocator = PoolAllocator;

	template<typename T, typename ... Args>
	TMemoryHandle<T, DefaultAllocator> New(Args&&... InArgs);

	template<typename T, typename TAllocator, typename ... Args>
	TMemoryHandle<T, TAllocator> New(TAllocator* AllocatorInstance, Args&&... InArgs);

	template <typename T, typename ... Args>
	[[nodiscard]] TMemoryHandle<T, DefaultAllocator> New(Args&&... InArgs)
	{
		return New<T, DefaultAllocator>(static_cast<DefaultAllocator*>(nullptr), std::forward<Args>(InArgs)...);
	}

	template <typename T, typename TAllocator, typename ... Args>
	[[nodiscard]] TMemoryHandle<T, TAllocator> New(TAllocator* AllocatorInstance, Args&&... InArgs)
	{
		TAllocator& allocator = AllocatorInstance ? *AllocatorInstance : TAllocator::GetMasterInstance();
		void* mem = allocator.Allocate(sizeof(T));
		T* inst = new (mem) T(std::forward<Args>(InArgs)...);
		return TMemoryHandle<T, TAllocator> (inst, AllocatorInstance);
	}

}
