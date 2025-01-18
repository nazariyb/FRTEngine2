#pragma once

#include <algorithm>
#include <cstring>
#include <vector>
#include <map>
#include <ranges>

#include "Asserts.h"
#include "Core.h"
#include "CoreTypes.h"
#include "MemoryCoreTypes.h"


namespace frt::memory
{
	struct MemoryHandleRef
	{
		MemoryHandleRef() = delete;

		template <typename THandle>
		MemoryHandleRef(const THandle& Handle)
		{
			MemoryHandlePtr = reinterpret_cast<uint64>(&Handle);
		}

		template <typename THandle>
		[[nodiscard]] THandle& Get() const
		{
			return *reinterpret_cast<THandle*>(MemoryHandlePtr);
		}

		bool operator==(const MemoryHandleRef& Other) const
		{
			return MemoryHandlePtr == Other.MemoryHandlePtr;
		}

		uint64 MemoryHandlePtr;
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

		template <typename T>
		void Free(T* Memory, uint64 Size);

		template <typename THandle>
		void AddRef(const THandle& MemoryHandle);

		template <typename THandle, bool bTAuthority = false>
		void RemoveRef(const THandle& MemoryHandle);

		static int64 GetShiftForAlignedPtr(uint8* AlignedPtr);

		static constexpr uint64 AlignmentSize = 8;

	private:
		void* FindFreeMemoryBlock(uint64 Size);

	private:
		uint8* _memory = nullptr;
		uint64 _memorySize = 0;
		uint64 _memoryUsed = 0;

		// TODO: use more optimized container for this?
		struct HandleKey
		{
			uint64 Ptr;
			uint64 Size;
			bool operator<(const HandleKey& Other) const
			{
				return Ptr < Other.Ptr;
			}
		};

#pragma warning(push)
#pragma warning(disable: 4251)
		std::map<HandleKey, std::vector<MemoryHandleRef>> _handles;
#pragma warning(pop)

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

	inline void* PoolAllocator::FindFreeMemoryBlock(const uint64 Size)
	{
		frt_assert(Size < (_memorySize - _memoryUsed));

		const auto usedAddresses = std::views::keys(_handles);
		const uint64 allocatedBlocksNum = usedAddresses.size();
		if (allocatedBlocksNum == 0)
		{
			return _memory;
		}

		const std::vector<HandleKey> addrs{ usedAddresses.begin(), usedAddresses.end() };
		uint64 prevFreeBlock = reinterpret_cast<uint64>(_memory);
		for (uint64 i = 0; i < allocatedBlocksNum; ++i)
		{
			const uint64 alignedPtr = addrs[i].Ptr;
			const uint64 nextUsedBlock = alignedPtr - GetShiftForAlignedPtr(reinterpret_cast<uint8*>(alignedPtr));
			if (nextUsedBlock - prevFreeBlock >= Size)
			{
				return reinterpret_cast<void*>(prevFreeBlock);
			}
			prevFreeBlock = nextUsedBlock + addrs[i].Size;
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
	void PoolAllocator::Free(T* Memory, uint64 Size)
	{
		if (!Memory)
		{
			return;
		}

		const uint64 elementsNum = Size / sizeof(T);
		for (uint64 i = 0; i < elementsNum; ++i)
		{
			(Memory + i)->~T();
		}

		_memoryUsed -= (Size + AlignmentSize);
	}

	template <typename THandle>
	void PoolAllocator::AddRef(const THandle& MemoryHandle)
	{
		MemoryHandleRef MemoryRef (MemoryHandle);
		uint64 memPtr = reinterpret_cast<uint64>(MemoryHandle.Ptr);

		HandleKey newKey{ memPtr, MemoryHandle.GetRealSize() };
		if (_handles.contains(newKey))
		{
			_handles.at(newKey).push_back(MemoryRef);
		}
		else
		{
			_handles.insert({ newKey, std::vector{ MemoryRef } });
		}
	}

	template <typename THandle, bool bTAuthority>
	void PoolAllocator::RemoveRef(const THandle& MemoryHandle)
	{
		frt_assert(MemoryHandle.Ptr);

		uint64 memPtr = reinterpret_cast<uint64>(MemoryHandle.Ptr);
		HandleKey handleKey{ memPtr, MemoryHandle.GetRealSize() };
		if (!_handles.contains(handleKey))
		{
			return;
		}

		auto& memRefs = _handles.at(handleKey);

		if (bTAuthority)
		{
			Free(MemoryHandle.Ptr, MemoryHandle.GetSize());
			for (MemoryHandleRef& memRef : memRefs)
			{
				memRef.Get<THandle>().Ptr = nullptr;
			}
			memRefs.clear();
			_handles.erase(handleKey);
		}
		else
		{
			std::erase(memRefs, MemoryHandleRef(MemoryHandle));
			if (memRefs.empty())
			{
				Free(MemoryHandle.Ptr, MemoryHandle.GetSize());
				_handles.erase(handleKey);
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
