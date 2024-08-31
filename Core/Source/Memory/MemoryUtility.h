#pragma once

#include "Asserts.h"
#include "CoreTypes.h"

namespace frt::memory
{
	static constexpr uint64 KiloByte = 1024LL;
	static constexpr uint64 MegaByte = KiloByte * 1024LL;
	static constexpr uint64 GigaByte = MegaByte * 1024LL;
	static constexpr uint64 TeraByte = GigaByte * 1024LL;

	template <typename T, typename TAllocator>
	struct TMemoryHandle
	{
		TMemoryHandle();
		TMemoryHandle(const TMemoryHandle& Other);
		TMemoryHandle(TMemoryHandle&& Other) noexcept;
		TMemoryHandle& operator=(const TMemoryHandle& Other);
		TMemoryHandle& operator=(TMemoryHandle&& Other) noexcept;

		TMemoryHandle(T* Data, TAllocator* InAllocator);
		~TMemoryHandle();

		T* Get() const;
		T* operator->() const;
		T& operator*() const;
		operator bool() const;
		operator T*() const;

		void Release();

	private:
		void RegisterInAllocator();

		template<bool bTAuthority>
		void UnregisterFromAllocator();

	private:
		T* Ptr;
		TAllocator* Allocator;

		friend TAllocator;
	};

	// template <typename T, typename TAllocator>
	// MemoryHandleRef TMemoryHandle<T, TAllocator>::GetRef() const
	// {
	// }


	inline uint64 AlignAddress(const uint64 Address, const uint64 Align)
	{
		const uint64 mask = Align - 1;
		frt_assert((Address & mask) == 0);
		return (Address + mask) & ~mask;
	}

	template <typename T>
	inline T* AlignPointer(T* Ptr, uint64 Align)
	{
		const uint64 addr = reinterpret_cast<uint64>(Ptr);
		const uint64 addrAligned = AlignAddress(addr, Align);
		return reinterpret_cast<T*>(addrAligned);
	}


	//
	//  TMemoryHandle impl
	//

	template <typename T, typename TAllocator>
	T* TMemoryHandle<T, TAllocator>::Get() const
	{
		frt_assert(Ptr);
		return Ptr;
	}

	template <typename T, typename TAllocator>
	T* TMemoryHandle<T, TAllocator>::operator->() const
	{
		frt_assert(Ptr);
		return Ptr;
	}

	template <typename T, typename TAllocator>
	T& TMemoryHandle<T, TAllocator>::operator*() const
	{
		frt_assert(Ptr);
		return *Ptr;
	}

	template <typename T, typename TAllocator>
	TMemoryHandle<T, TAllocator>::operator bool() const
	{
		return !!Ptr;
	}

	template <typename T, typename TAllocator>
	TMemoryHandle<T, TAllocator>::operator T*() const
	{
		frt_assert(Ptr);
		return Ptr;
	}

	template <typename T, typename TAllocator>
	void TMemoryHandle<T, TAllocator>::Release()
	{
		UnregisterFromAllocator<true>();
	}

	template <typename T, typename TAllocator>
	TMemoryHandle<T, TAllocator>::TMemoryHandle()
		: Ptr(nullptr), Allocator(nullptr)
	{
	}

	template <typename T, typename TAllocator>
	TMemoryHandle<T, TAllocator>::TMemoryHandle(T* Data, TAllocator* InAllocator)
		: Ptr(Data), Allocator(InAllocator)
	{
		RegisterInAllocator();
	}

	template <typename T, typename TAllocator>
	TMemoryHandle<T, TAllocator>::TMemoryHandle(const TMemoryHandle& Other)
		: Ptr(Other.Ptr), Allocator(Other.Allocator)
	{
		RegisterInAllocator();
	}

	template <typename T, typename TAllocator>
	TMemoryHandle<T, TAllocator>::TMemoryHandle(TMemoryHandle&& Other) noexcept
		: Ptr(Other.Ptr), Allocator(Other.Allocator)
	{
		RegisterInAllocator();

		Other.UnregisterFromAllocator<false>();
		Other.Ptr = nullptr;
		Other.Allocator = nullptr;
	}

	template <typename T, typename TAllocator>
	TMemoryHandle<T, TAllocator>::~TMemoryHandle()
	{
		UnregisterFromAllocator<false>();
	}

	template <typename T, typename TAllocator>
	TMemoryHandle<T, TAllocator>& TMemoryHandle<T, TAllocator>::operator=(const TMemoryHandle& Other)
	{
		if (Ptr == Other.Ptr || this == &Other)
		{
			return *this;
		}

		UnregisterFromAllocator<false>();

		Ptr = Other.Ptr;
		Allocator = Other.Allocator;

		RegisterInAllocator();

		return *this;
	}

	template <typename T, typename TAllocator>
	TMemoryHandle<T, TAllocator>& TMemoryHandle<T, TAllocator>::operator=(TMemoryHandle&& Other) noexcept
	{
		UnregisterFromAllocator<false>();

		Ptr = Other.Ptr;
		Allocator = Other.Allocator;
		RegisterInAllocator();

		Other.Ptr = nullptr;
		Other.Allocator = nullptr;
		Other.UnregisterFromAllocator<false>();

		return *this;
	}

	template <typename T, typename TAllocator>
	void TMemoryHandle<T, TAllocator>::RegisterInAllocator()
	{
		if (Ptr)
		{
			frt_assert(Allocator);
			Allocator->AddRef(*this);
		}
	}

	template <typename T, typename TAllocator>
	template<bool bTAuthority>
	void TMemoryHandle<T, TAllocator>::UnregisterFromAllocator()
	{
		if (Ptr)
		{
			frt_assert(Allocator);
			Allocator->template RemoveRef<T, bTAuthority>(*this);
		}
	}
}
