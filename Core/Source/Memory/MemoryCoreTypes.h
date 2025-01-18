#pragma once

#include "Asserts.h"
#include "CoreTypes.h"

namespace frt::memory
{
	static constexpr uint64 KiloByte = 1024LL;
	static constexpr uint64 MegaByte = KiloByte * 1024LL;
	static constexpr uint64 GigaByte = MegaByte * 1024LL;
	static constexpr uint64 TeraByte = GigaByte * 1024LL;

	class PoolAllocator;

	//*************************//
	//    TMemoryHandle decl   //
	//*************************//
	template <typename T, typename TAllocator = PoolAllocator>
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

		uint64 GetSize() const;
		uint64 GetRealSize() const;

		void Release();

	private:
		void RegisterInAllocator();

		template<bool bTAuthority>
		void UnregisterFromAllocator();

	protected:
		T* Ptr;
		TAllocator* Allocator;

		friend TAllocator;
	};

	//******************************//
	//    TMemoryHandleArray decl   //
	//******************************//
	template <typename T, typename TAllocator>
	struct TMemoryHandleArray : public TMemoryHandle<T, TAllocator>
	{
		TMemoryHandleArray();
		TMemoryHandleArray(const TMemoryHandleArray& Other);
		TMemoryHandleArray(TMemoryHandleArray&& Other) noexcept;
		TMemoryHandleArray& operator=(const TMemoryHandleArray& Other);
		TMemoryHandleArray& operator=(TMemoryHandleArray&& Other) noexcept;

		TMemoryHandleArray(T* Data, TAllocator* InAllocator, uint64 InNum);
		~TMemoryHandleArray();

		T& operator[](int64 Index) const;

		uint64 GetNum() const;
		uint64 GetSize() const;
		uint64 GetRealSize() const;

		void Release();

	private:
		void RegisterInAllocator();

		template<bool bTAuthority>
		void UnregisterFromAllocator();

	private:
		using Super = TMemoryHandle<T, TAllocator>;
		uint64 Num;
	};


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


	//**************************//
	//    TMemoryHandle impl    //
	//**************************//

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
	uint64 TMemoryHandle<T, TAllocator>::GetSize() const
	{
		return sizeof(T);
	}

	template <typename T, typename TAllocator>
	uint64 TMemoryHandle<T, TAllocator>::GetRealSize() const
	{
		return GetSize() + TAllocator::GetShiftForAlignedPtr(reinterpret_cast<uint8*>(Ptr));
	}

	template <typename T, typename TAllocator>
	void TMemoryHandle<T, TAllocator>::Release()
	{
		UnregisterFromAllocator<true>();
		Ptr = nullptr;
		Allocator = nullptr;
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

		Other.~TMemoryHandle();
	}

	template <typename T, typename TAllocator>
	TMemoryHandle<T, TAllocator>::~TMemoryHandle()
	{
		UnregisterFromAllocator<false>();
		Ptr = nullptr;
		Allocator = nullptr;
	}

	template <typename T, typename TAllocator>
	TMemoryHandle<T, TAllocator>& TMemoryHandle<T, TAllocator>::operator=(const TMemoryHandle& Other)
	{
		if (this == &Other)
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

		Other.~TMemoryHandle();

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
			Allocator->template RemoveRef<TMemoryHandle<T, TAllocator>, bTAuthority>(*this);
		}
	}


	//*******************************//
	//    TMemoryHandleArray impl    //
	//*******************************//

	template <typename T, typename TAllocator>
	TMemoryHandleArray<T, TAllocator>::TMemoryHandleArray()
		: TMemoryHandle<T, TAllocator>()
		, Num(0)
	{
	}

	template <typename T, typename TAllocator>
	TMemoryHandleArray<T, TAllocator>::TMemoryHandleArray(const TMemoryHandleArray& Other)
		: Num(Other.Num)
	{
		Super::Ptr = Other.Ptr;
		Super::Allocator = Other.Allocator;
		RegisterInAllocator();
	}

	template <typename T, typename TAllocator>
	TMemoryHandleArray<T, TAllocator>::TMemoryHandleArray(TMemoryHandleArray&& Other) noexcept
		: Num(Other.Num)
	{
		Super::Ptr = Other.Ptr;
		Super::Allocator = Other.Allocator;
		RegisterInAllocator();

		Other.~TMemoryHandleArray();
	}

	template <typename T, typename TAllocator>
	TMemoryHandleArray<T, TAllocator>& TMemoryHandleArray<T, TAllocator>::operator=(const TMemoryHandleArray& Other)
	{
		if (this == &Other)
		{
			return *this;
		}

		UnregisterFromAllocator<false>();

		Super::Ptr = Other.Ptr;
		Super::Allocator = Other.Allocator;

		RegisterInAllocator();

		return *this;
	}

	template <typename T, typename TAllocator>
	TMemoryHandleArray<T, TAllocator>& TMemoryHandleArray<T, TAllocator>::operator=(TMemoryHandleArray&& Other) noexcept
	{
		UnregisterFromAllocator<false>();

		Super::Ptr = Other.Ptr;
		Super::Allocator = Other.Allocator;
		Num = Other.Num;
		RegisterInAllocator();

		Other.~TMemoryHandleArray();

		return *this;
	}

	template <typename T, typename TAllocator>
	TMemoryHandleArray<T, TAllocator>::TMemoryHandleArray(T* Data, TAllocator* InAllocator, uint64 InNum)
		: Num(InNum)
	{
		Super::Ptr = Data;
		Super::Allocator = InAllocator;
		RegisterInAllocator();
	}

	template <typename T, typename TAllocator>
	TMemoryHandleArray<T, TAllocator>::~TMemoryHandleArray()
	{
		UnregisterFromAllocator<false>();
		Super::Ptr = nullptr;
		Super::Allocator = nullptr;
		Num = 0;
	}

	template <typename T, typename TAllocator>
	uint64 TMemoryHandleArray<T, TAllocator>::GetNum() const
	{
		return Num;
	}

	template <typename T, typename TAllocator>
	uint64 TMemoryHandleArray<T, TAllocator>::GetSize() const
	{
		return sizeof(T) * GetNum();
	}

	template <typename T, typename TAllocator>
	uint64 TMemoryHandleArray<T, TAllocator>::GetRealSize() const
	{
		return GetSize() + TAllocator::GetShiftForAlignedPtr(reinterpret_cast<uint8*>(Super::Ptr));
	}

	template <typename T, typename TAllocator>
	void TMemoryHandleArray<T, TAllocator>::Release()
	{
		UnregisterFromAllocator<true>();
		Super::Ptr = nullptr;
		Super::Allocator = nullptr;
		Num = 0;
	}

	template <typename T, typename TAllocator>
	T& TMemoryHandleArray<T, TAllocator>::operator[](int64 Index) const
	{
		frt_assert(Super::Ptr);
		frt_assert(0 <= Index);
		frt_assert(Index < GetNum());
		return *(TMemoryHandle<T, TAllocator>::Ptr + Index);
	}

	template <typename T, typename TAllocator>
	void TMemoryHandleArray<T, TAllocator>::RegisterInAllocator()
	{
		if (Super::Ptr)
		{
			frt_assert(Super::Allocator);
			Super::Allocator->AddRef(*this);
		}
	}

	template <typename T, typename TAllocator>
	template<bool bTAuthority>
	void TMemoryHandleArray<T, TAllocator>::UnregisterFromAllocator()
	{
		if (Super::Ptr)
		{
			frt_assert(Super::Allocator);
			Super::Allocator->template RemoveRef<TMemoryHandleArray<T, TAllocator>, bTAuthority>(*this);
		}
	}
}
