#pragma once

#include "MemoryCoreTypes.h"
#include "PoolAllocator.h"


namespace frt::memory
{
	class PoolAllocator;
	using DefaultAllocator = PoolAllocator;


	//*****************//
	//    New decls    //
	//*****************//

	// New single object
	template<typename T, typename ... Args>
	TMemoryHandle<T, DefaultAllocator> New(Args&&... InArgs);

	template<typename T, typename TAllocator, typename ... Args>
	TMemoryHandle<T, TAllocator> New(TAllocator* AllocatorInstance, Args&&... InArgs);
	// ~New single object

	// New array
	template<typename T, bool bTInit = false, typename ... Args>
	TMemoryHandleArray<T, DefaultAllocator> NewArray(uint64 Num, Args&&... InArgs);

	template<typename T, typename TAllocator, bool bTInit = false, typename ... Args>
	TMemoryHandleArray<T, TAllocator> NewArray(uint64 Num, TAllocator* AllocatorInstance, Args&&... InArgs);
	// ~New array


	//*****************//
	//    New impls    //
	//*****************//

	// New single object
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
		return TMemoryHandle<T, TAllocator> (inst, &allocator);
	}
	// ~New single object

	// New array
	template <typename T, bool bTInit, typename ... Args>
	[[nodiscard]] TMemoryHandleArray<T, DefaultAllocator> NewArray(uint64 Num, Args&&... InArgs)
	{
		return NewArray<T, DefaultAllocator, bTInit>(Num, static_cast<DefaultAllocator*>(nullptr), std::forward<Args>(InArgs)...);
	}

	template <typename T, typename TAllocator, bool bTInit, typename ... Args>
	[[nodiscard]] TMemoryHandleArray<T, TAllocator> NewArray(uint64 Num, TAllocator* AllocatorInstance, Args&&... InArgs)
	{
		TAllocator& allocator = AllocatorInstance ? *AllocatorInstance : TAllocator::GetMasterInstance();
		void* mem = allocator.Allocate(sizeof(T) * Num);
		if (bTInit)
		{
			for (uint64 i = 0; i < Num; ++i)
			{
				new (reinterpret_cast<T*>(mem) + i) T(std::forward<Args>(InArgs)...);
			}
		}
		T* data = reinterpret_cast<T*>(mem);
		return TMemoryHandleArray<T, TAllocator> (data, &allocator, Num);
	}
	// ~New array
}
