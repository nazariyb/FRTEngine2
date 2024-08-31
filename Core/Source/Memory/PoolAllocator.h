#pragma once

#include <algorithm>
#include <cstring>
#include <vector>
#include <map>
#include <ranges>

#include "Asserts.h"
#include "Core.h"
#include "CoreTypes.h"
#include "MemoryUtility.h"


namespace frt::memory
{
	struct MemoryHandleRef
	{
		MemoryHandleRef() = delete;

		template <typename T, typename TAllocator>
		MemoryHandleRef(const TMemoryHandle<T, TAllocator>& Handle)
		{
			MemoryHandlePtr = reinterpret_cast<uint64>(&Handle);
			TypeSize = sizeof(T);
		}

		bool operator==(const MemoryHandleRef& Other) const
		{
			return MemoryHandlePtr == Other.MemoryHandlePtr && TypeSize == Other.TypeSize;
		}

		uint64 MemoryHandlePtr;
		uint64 TypeSize; // this should be part of a key in _handles
	};


	//**************************//
	//    PoolAllocator decl    //
	//**************************//

	// TODO: Memory defragmentation!!
	class FRT_CORE_API PoolAllocator
	{
	public:
		PoolAllocator() : _memory(nullptr), _memorySize(0), _memoryUsed(0) {}
		PoolAllocator(const PoolAllocator&) = delete;
		PoolAllocator& operator=(const PoolAllocator&) = delete;
		PoolAllocator(PoolAllocator&&) noexcept = default;
		PoolAllocator& operator=(PoolAllocator&&) = default;

		PoolAllocator(uint64 Size);
		~PoolAllocator();

		static PoolAllocator& InitMasterInstance(uint64 Size);
		static void ReleaseMasterInstance();

		uint64 GetMemoryUsed() const;
		static PoolAllocator& GetMasterInstance();

		void* Allocate(uint64 Size);
		void Free(void* Memory, uint64 Size); // should not use Size?

		template <typename T>
		void Free(T* Memory);

		template <typename T>
		void AddRef(const TMemoryHandle<T, PoolAllocator>& MemoryHandle);

		template <typename T, bool bTAuthority = false>
		void RemoveRef(const TMemoryHandle<T, PoolAllocator>& MemoryHandle);

		static constexpr uint64 AlignmentSize = 8;

	private:
		void* FindFreeMemoryBlock(uint64 Size);
		static int64 GetShiftForAlignedPtr(uint8* AlignedPtr);

	private:
		uint8* _memory = nullptr;
		uint64 _memorySize = 0;
		uint64 _memoryUsed = 0;

		// TODO: use more optimized container for this?
		std::map<uint64, std::vector<MemoryHandleRef>> _handles;

		static PoolAllocator* _masterInstance;
	};


	//**************************//
	//    PoolAllocator impl    //
	//**************************//

	inline PoolAllocator::PoolAllocator(uint64 Size)
		: _memory(nullptr)
		, _memorySize(Size)
		, _memoryUsed(0)
	{
		_memory = new uint8[_memorySize];
	}

	inline PoolAllocator::~PoolAllocator()
	{
		delete[] _memory;
	}

	inline PoolAllocator& PoolAllocator::InitMasterInstance(uint64 Size)
	{
		_masterInstance = new PoolAllocator(Size);
		return *_masterInstance;
	}

	inline void PoolAllocator::ReleaseMasterInstance()
	{
		delete _masterInstance;
		_masterInstance = nullptr;
	}

	inline void* PoolAllocator::Allocate(uint64 Size)
	{
		uint64 actualSize = Size + AlignmentSize;

		uint8* rawPtr = reinterpret_cast<uint8*>(FindFreeMemoryBlock(actualSize));
		memset(rawPtr, 0, actualSize);

		uint8* alignedPtr = AlignPointer(rawPtr, AlignmentSize);
		if (alignedPtr == rawPtr)
		{
			alignedPtr += AlignmentSize;
		}

		int64 shift = alignedPtr - rawPtr;

		_memoryUsed += Size + AlignmentSize;

		// we always guarantee only (and at least) 1 extra byte of allocated memory,
		// so we can store value not more than 128; and up to 256 if we treat 0 as 256
		frt_assert(shift > 0 && shift <= 256);

		alignedPtr[-1] = static_cast<uint8>(shift & 0xFF);

		return alignedPtr;

	}

	inline void PoolAllocator::Free(void* Memory, uint64 Size)
	{
		if (!Memory)
		{
			return;
		}

		_memoryUsed -= (Size + AlignmentSize);
	}

	inline void* PoolAllocator::FindFreeMemoryBlock(const uint64 Size)
	{
		frt_assert(Size < (_memorySize - _memoryUsed));

		const auto usedAddresses = std::views::keys(_handles);
		uint64 allocatedBlocksNum = usedAddresses.size();
		if (allocatedBlocksNum == 0)
		{
			return _memory;
		}

		const std::vector<uint64> addrs{ usedAddresses.begin(), usedAddresses.end() };
		uint64 prevFreeBlock = reinterpret_cast<uint64>(_memory);
		for (uint64 i = 0; i < allocatedBlocksNum; ++i)
		{
			const uint64 alignedPtr = addrs[i];
			const uint64 nextUsedBlock = alignedPtr - GetShiftForAlignedPtr(reinterpret_cast<uint8*>(alignedPtr));
			if (nextUsedBlock - prevFreeBlock >= Size)
			{
				return reinterpret_cast<void*>(prevFreeBlock);
			}
			prevFreeBlock = nextUsedBlock + _handles.at(addrs[i])[0].TypeSize + AlignmentSize;
		}

		if (_memorySize - prevFreeBlock >= Size)
		{
			return reinterpret_cast<void*>(prevFreeBlock);
		}

		return nullptr;
	}

	inline int64 PoolAllocator::GetShiftForAlignedPtr(uint8* AlignedPtr)
	{
		int64 shift = AlignedPtr[-1];
		if (shift == 0)
		{
			shift = 256;
		}
		return shift;
	}

	template <typename T>
	void PoolAllocator::Free(T* Memory)
	{
		if (!Memory)
		{
			return;
		}

		Memory->~T();
		Free(reinterpret_cast<void*>(Memory), sizeof(T));
	}

	template <typename T>
	void PoolAllocator::AddRef(const TMemoryHandle<T, PoolAllocator>& MemoryHandle)
	{
		MemoryHandleRef MemoryRef (MemoryHandle);
		uint64 memPtr = reinterpret_cast<uint64>(MemoryHandle.Ptr);
		if (_handles.contains(memPtr))
		{
			_handles.at(memPtr).push_back(MemoryRef);
		}
		else
		{
			_handles.insert({memPtr, std::vector{MemoryRef}});
		}
	}

	template <typename T, bool bTAuthority = false>
	void PoolAllocator::RemoveRef(const TMemoryHandle<T, PoolAllocator>& MemoryHandle)
	{
		frt_assert(MemoryHandle.Ptr);

		uint64 memPtr = reinterpret_cast<uint64>(MemoryHandle.Ptr);
		if (!_handles.contains(memPtr))
		{
			return;
		}

		auto& memRefs = _handles.at(memPtr);

		if (bTAuthority)
		{
			Free(MemoryHandle.Ptr);
			for (MemoryHandleRef memRef : memRefs)
			{
				reinterpret_cast<TMemoryHandle<T, PoolAllocator>*>(memRef.MemoryHandlePtr)->Ptr = nullptr;
			}
			memRefs.clear();
			_handles.erase(memPtr);
		}
		else
		{
			std::erase(memRefs, MemoryHandleRef(MemoryHandle));
			if (memRefs.empty())
			{
				Free(MemoryHandle.Ptr);
				_handles.erase(memPtr);
			}
		}
	}

	inline uint64 PoolAllocator::GetMemoryUsed() const
	{
		return _memoryUsed;
	}

	inline PoolAllocator& PoolAllocator::GetMasterInstance()
	{
		frt_assert(_masterInstance);
		return *_masterInstance;
	}
}
