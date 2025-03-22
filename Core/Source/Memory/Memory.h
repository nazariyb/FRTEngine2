#pragma once

#include "MemoryPool.h"
#include "Ref.h"


namespace frt::memory
{
	//*****************//
	//    Constants    //
	//*****************//

	static constexpr uint64 KiloByte = 1024ull;
	static constexpr uint64 MegaByte = KiloByte * 1024ull;
	static constexpr uint64 GigaByte = MegaByte * 1024ull;
	static constexpr uint64 TeraByte = GigaByte * 1024ull;

	namespace literals
	{
		consteval uint64 operator""_Kb(unsigned long long V) { return V * KiloByte; }
		consteval uint64 operator""_Kb(long double V) { return (uint64)V * KiloByte; }
		consteval uint64 operator""_Mb(unsigned long long V) { return V * MegaByte; }
		consteval uint64 operator""_Mb(long double V) { return (uint64)V * MegaByte; }
		consteval uint64 operator""_Gb(unsigned long long V) { return V * GigaByte; }
		consteval uint64 operator""_Gb(long double V) { return (uint64)V * GigaByte; }
		consteval uint64 operator""_Tb(unsigned long long V) { return V * TeraByte; }
		consteval uint64 operator""_Tb(long double V) { return (uint64)V * TeraByte; }
	}
}


namespace frt::memory
{
	class PoolAllocator;
	using DefaultPool = CMemoryPool;

	template <typename T, typename... Args>
	[[nodiscard]] T* NewUnmanaged(Args&&... InArgs)
	{
		return DefaultPool::GetPrimaryInstance()->NewUnmanaged<T>(std::forward<Args>(InArgs)...);
	}

	template <typename T, typename... Args>
	[[nodiscard]] T* NewManaged(Args&&... InArgs)
	{
		return DefaultPool::GetPrimaryInstance()->NewManaged<T>(std::forward<Args>(InArgs)...);
	}

	template <typename T, typename... Args>
	[[nodiscard]] TRefUnique<T> NewUnique(Args&&... InArgs)
	{
		return DefaultPool::GetPrimaryInstance()->NewUnique<T>(std::forward<Args>(InArgs)...);
	}

	template <typename T, typename... Args>
	[[nodiscard]] TRefShared<T> NewShared(Args&&... InArgs)
	{
		return DefaultPool::GetPrimaryInstance()->NewShared<T>(std::forward<Args>(InArgs)...);
	}
}


namespace frt::memory
{
	//*****************//
	//      Utils      //
	//*****************//

	inline uint64 AlignAddress(const uint64 Address, const uint64 Align)
	{
		const uint64 mask = Align - 1;
		frt_assert((Align & mask) == 0);
		return (Address + mask) & ~mask;
	}

	template <typename T>
	inline T* AlignPointer(T* Ptr, uint64 Align)
	{
		const uint64 addr = reinterpret_cast<uint64>(Ptr);
		const uint64 addrAligned = AlignAddress(addr, Align);
		return reinterpret_cast<T*>(addrAligned);
	}
}
